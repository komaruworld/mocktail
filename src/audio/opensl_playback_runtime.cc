#include "mocktail/audio/opensl_playback_runtime.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "mocktail/audio/audio_sink.h"
#include "mocktail/audio/opensl_simple_buffer_queue.h"
#include "mocktail/audio/sdl_audio_sink.h"

namespace mocktail::audio {
namespace {

namespace abi = opensl_abi;

constexpr std::uint64_t kRuntimeObjectMagic = 0x4d4f434b534c4553ULL;
constexpr abi::Millibel kMaxVolumeLevel = 0;
constexpr abi::Millibel kMinVolumeLevel =
    std::numeric_limits<abi::Millibel>::min();

constexpr abi::InterfaceIdValue kAndroidConfigurationId = {
    0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c,
    {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
constexpr abi::InterfaceIdValue kAndroidSimpleBufferQueueId = {
    0x198e4940, 0xc5d7, 0x11df, 0xa2a6,
    {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
constexpr abi::InterfaceIdValue kBufferQueueId = {
    0x2bc99cc0, 0xddd4, 0x11db, 0x8d99,
    {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
constexpr abi::InterfaceIdValue kEngineId = {
    0x8d97c260, 0xddd4, 0x11db, 0x958f,
    {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
constexpr abi::InterfaceIdValue kPlayId = {
    0xef0bd9c0, 0xddd7, 0x11db, 0xbf49,
    {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
constexpr abi::InterfaceIdValue kRecordId = {
    0xc5657aa0, 0xdddb, 0x11db, 0x82f7,
    {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
constexpr abi::InterfaceIdValue kVolumeId = {
    0x09e8ede0, 0xddde, 0x11db, 0xb4f6,
    {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};

std::atomic<std::uint64_t> g_submitted_buffers{0};
std::atomic<std::uint64_t> g_consumed_buffers{0};
std::atomic<std::uint64_t> g_clean_player_shutdowns{0};

enum class RuntimeObjectKind {
  kEngine,
  kOutputMix,
  kPlayer,
};

struct RuntimeObject;

struct ObjectHandle {
  const abi::ObjectTable* table = nullptr;
  RuntimeObject* object = nullptr;
};

struct EngineHandle {
  const abi::EngineTable* table = nullptr;
  RuntimeObject* object = nullptr;
};

struct PlayHandle {
  const abi::PlayTable* table = nullptr;
  RuntimeObject* object = nullptr;
};

struct VolumeHandle {
  const abi::VolumeTable* table = nullptr;
  RuntimeObject* object = nullptr;
};

struct ConfigurationHandle {
  const abi::AndroidConfigurationTable* table = nullptr;
  RuntimeObject* object = nullptr;
};

extern const abi::ObjectTable kObjectTable;
extern const abi::EngineTable kEngineTable;
extern const abi::PlayTable kPlayTable;
extern const abi::VolumeTable kVolumeTable;
extern const abi::AndroidConfigurationTable kConfigurationTable;

struct RuntimeObject {
  explicit RuntimeObject(RuntimeObjectKind object_kind) : kind(object_kind) {
    object_handle = ObjectHandle{&kObjectTable, this};
    engine_handle = EngineHandle{&kEngineTable, this};
    play_handle = PlayHandle{&kPlayTable, this};
    volume_handle = VolumeHandle{&kVolumeTable, this};
    configuration_handle = ConfigurationHandle{&kConfigurationTable, this};
  }

  abi::Object ObjectInterface() {
    return reinterpret_cast<abi::Object>(&object_handle.table);
  }

  abi::Engine EngineInterface() {
    return reinterpret_cast<abi::Engine>(&engine_handle.table);
  }

  abi::Play PlayInterface() {
    return reinterpret_cast<abi::Play>(&play_handle.table);
  }

  abi::Volume VolumeInterface() {
    return reinterpret_cast<abi::Volume>(&volume_handle.table);
  }

  abi::AndroidConfiguration ConfigurationInterface() {
    return reinterpret_cast<abi::AndroidConfiguration>(
        &configuration_handle.table);
  }

  std::uint64_t magic = kRuntimeObjectMagic;
  RuntimeObjectKind kind;
  ObjectHandle object_handle;
  EngineHandle engine_handle;
  PlayHandle play_handle;
  VolumeHandle volume_handle;
  ConfigurationHandle configuration_handle;
  mutable std::mutex mutex;
  abi::Uint32 state = abi::kObjectStateUnrealized;
  abi::ObjectCallback object_callback = nullptr;
  void* object_callback_context = nullptr;
  abi::Int32 priority = 0;
  abi::Boolean preemptable = abi::kBooleanFalse;

  PcmSpec pcm_spec;
  abi::Uint32 queue_capacity = 0;
  bool expose_queue = false;
  bool expose_volume = false;
  bool expose_configuration = false;
  abi::Uint32 play_state = abi::kPlayStateStopped;
  abi::Millibel volume_level = kMaxVolumeLevel;
  abi::Boolean muted = abi::kBooleanFalse;
  abi::Int32 stream_type = abi::kAndroidStreamMedia;
  abi::Uint32 performance_mode = abi::kAndroidPerformanceNone;
  std::unique_ptr<OpenSlSimpleBufferQueueAdapter> queue_adapter;
  std::atomic<std::uint64_t> submitted_buffers{0};
  std::atomic<std::uint64_t> consumed_buffers{0};
  std::atomic<bool> submission_reported{false};
  std::atomic<bool> consumption_reported{false};
};

RuntimeObject* FromObject(abi::Object self) {
  if (self == nullptr) {
    return nullptr;
  }
  const auto* handle = reinterpret_cast<const ObjectHandle*>(self);
  RuntimeObject* object = handle->object;
  return object != nullptr && object->magic == kRuntimeObjectMagic ? object
                                                                  : nullptr;
}

RuntimeObject* FromEngine(abi::Engine self) {
  if (self == nullptr) {
    return nullptr;
  }
  const auto* handle = reinterpret_cast<const EngineHandle*>(self);
  RuntimeObject* object = handle->object;
  return object != nullptr && object->magic == kRuntimeObjectMagic ? object
                                                                  : nullptr;
}

RuntimeObject* FromPlay(abi::Play self) {
  if (self == nullptr) {
    return nullptr;
  }
  const auto* handle = reinterpret_cast<const PlayHandle*>(self);
  RuntimeObject* object = handle->object;
  return object != nullptr && object->magic == kRuntimeObjectMagic ? object
                                                                  : nullptr;
}

RuntimeObject* FromVolume(abi::Volume self) {
  if (self == nullptr) {
    return nullptr;
  }
  const auto* handle = reinterpret_cast<const VolumeHandle*>(self);
  RuntimeObject* object = handle->object;
  return object != nullptr && object->magic == kRuntimeObjectMagic ? object
                                                                  : nullptr;
}

RuntimeObject* FromConfiguration(abi::AndroidConfiguration self) {
  if (self == nullptr) {
    return nullptr;
  }
  const auto* handle = reinterpret_cast<const ConfigurationHandle*>(self);
  RuntimeObject* object = handle->object;
  return object != nullptr && object->magic == kRuntimeObjectMagic ? object
                                                                  : nullptr;
}

bool InterfaceIdEquals(abi::InterfaceId left, abi::InterfaceId right) {
  return left != nullptr && right != nullptr &&
         (left == right ||
          std::memcmp(left, right, sizeof(abi::InterfaceIdValue)) == 0);
}

bool IsQueueInterface(abi::InterfaceId iid) {
  return InterfaceIdEquals(iid, SL_IID_BUFFERQUEUE) ||
         InterfaceIdEquals(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE);
}

abi::Result ToOpenSlResult(const Status& status) {
  switch (status.code()) {
    case StatusCode::kOk:
      return abi::kResultSuccess;
    case StatusCode::kInvalidArgument:
      return abi::kResultParameterInvalid;
    case StatusCode::kFailedPrecondition:
      return abi::kResultPreconditionsViolated;
    case StatusCode::kUnavailable:
      return abi::kResultResourceError;
    case StatusCode::kUnsupported:
      return abi::kResultFeatureUnsupported;
    case StatusCode::kPlatformError:
      return abi::kResultIoError;
  }
  return abi::kResultInternalError;
}

bool IsObjectRealized(RuntimeObject* object) {
  std::lock_guard<std::mutex> lock(object->mutex);
  return object->state == abi::kObjectStateRealized;
}

abi::Result ValidateInterfaceArray(abi::Uint32 count,
                                   const abi::InterfaceId* ids,
                                   const abi::Boolean* required) {
  if (count == 0) {
    return abi::kResultSuccess;
  }
  if (ids == nullptr || required == nullptr) {
    return abi::kResultParameterInvalid;
  }
  for (abi::Uint32 index = 0; index < count; ++index) {
    if (ids[index] == nullptr) {
      return abi::kResultParameterInvalid;
    }
  }
  return abi::kResultSuccess;
}

abi::Result ValidatePcmSource(const abi::DataSource* source,
                              PcmSpec* pcm_spec,
                              abi::Uint32* queue_capacity) {
  if (source == nullptr || pcm_spec == nullptr || queue_capacity == nullptr ||
      abi::DataSourceLocator(*source) == nullptr ||
      abi::DataSourceFormat(*source) == nullptr) {
    return abi::kResultParameterInvalid;
  }

  const auto* locator =
      static_cast<const abi::DataLocatorAndroidSimpleBufferQueue*>(
          abi::DataSourceLocator(*source));
  const abi::Uint32 locator_type = abi::QueueLocatorType(*locator);
  const abi::Uint32 locator_capacity = abi::QueueLocatorCapacity(*locator);
  if (locator_type != abi::kDataLocatorAndroidSimpleBufferQueue &&
      locator_type != abi::kDataLocatorBufferQueue) {
    return abi::kResultContentUnsupported;
  }
  if (locator_capacity == 0 || locator_capacity > 1024) {
    return abi::kResultParameterInvalid;
  }

  const auto* format = static_cast<const abi::DataFormatPcm*>(
      abi::DataSourceFormat(*source));
  const abi::Uint32 sample_rate_millihertz =
      abi::PcmSampleRateMillihertz(*format);
  const abi::Uint32 channel_count = abi::PcmChannelCount(*format);
  const abi::Uint32 bits_per_sample = abi::PcmBitsPerSample(*format);
  if (abi::PcmFormatType(*format) != abi::kDataFormatPcm ||
      sample_rate_millihertz == 0 || sample_rate_millihertz % 1000 != 0) {
    return abi::kResultContentUnsupported;
  }
  if (channel_count != 1 && channel_count != 2) {
    return abi::kResultContentUnsupported;
  }

  const abi::Uint32 expected_channel_mask =
      channel_count == 1
          ? abi::kSpeakerFrontCenter
          : abi::kSpeakerFrontLeft | abi::kSpeakerFrontRight;
  if (abi::PcmChannelMask(*format) != 0 &&
      abi::PcmChannelMask(*format) != expected_channel_mask) {
    return abi::kResultContentUnsupported;
  }

  PcmSampleFormat sample_format;
  switch (bits_per_sample) {
    case abi::kPcmSampleFormatFixed8:
      sample_format = PcmSampleFormat::kUnsigned8;
      break;
    case abi::kPcmSampleFormatFixed16:
      sample_format = PcmSampleFormat::kSigned16LittleEndian;
      break;
    case abi::kPcmSampleFormatFixed32:
      sample_format = PcmSampleFormat::kSigned32LittleEndian;
      break;
    default:
      return abi::kResultContentUnsupported;
  }
  if (abi::PcmContainerSize(*format) != bits_per_sample) {
    return abi::kResultContentUnsupported;
  }
  if (bits_per_sample != abi::kPcmSampleFormatFixed8 &&
      abi::PcmEndianness(*format) != abi::kByteOrderLittleEndian) {
    return abi::kResultContentUnsupported;
  }

  const abi::Uint32 sample_rate_hz =
      sample_rate_millihertz / 1000;
  if (sample_rate_hz >
      static_cast<abi::Uint32>(std::numeric_limits<int>::max())) {
    return abi::kResultContentUnsupported;
  }
  const PcmSpec candidate{static_cast<int>(sample_rate_hz),
                          static_cast<std::uint8_t>(channel_count),
                          sample_format};
  if (!ValidatePcmSpec(candidate).ok()) {
    return abi::kResultContentUnsupported;
  }
  *pcm_spec = candidate;
  *queue_capacity = locator_capacity;
  return abi::kResultSuccess;
}

abi::Result ValidateOutputMixSink(const abi::DataSink* sink) {
  if (sink == nullptr || abi::DataSinkLocator(*sink) == nullptr) {
    return abi::kResultParameterInvalid;
  }
  const auto* locator =
      static_cast<const abi::DataLocatorOutputMix*>(
          abi::DataSinkLocator(*sink));
  if (abi::OutputMixLocatorType(*locator) != abi::kDataLocatorOutputMix ||
      abi::OutputMixObject(*locator) == nullptr) {
    return abi::kResultContentUnsupported;
  }
  RuntimeObject* output_mix = FromObject(abi::OutputMixObject(*locator));
  if (output_mix == nullptr ||
      output_mix->kind != RuntimeObjectKind::kOutputMix ||
      !IsObjectRealized(output_mix)) {
    return abi::kResultPreconditionsViolated;
  }
  return abi::kResultSuccess;
}

void OnQueueEvent(void* context, OpenSlBufferQueueEvent event,
                  std::size_t /*size_bytes*/) {
  auto* player = static_cast<RuntimeObject*>(context);
  if (player == nullptr || player->magic != kRuntimeObjectMagic) {
    return;
  }
  switch (event) {
    case OpenSlBufferQueueEvent::kSubmitted:
      player->submitted_buffers.fetch_add(1, std::memory_order_relaxed);
      g_submitted_buffers.fetch_add(1, std::memory_order_relaxed);
      {
        bool expected = false;
        if (player->submission_reported.compare_exchange_strong(
                expected, true, std::memory_order_relaxed)) {
          std::fprintf(stderr,
                       "[mocktail][audio] OpenSL playback activity: "
                       "submitted_buffers=1\n");
        }
      }
      break;
    case OpenSlBufferQueueEvent::kConsumed: {
      const std::uint64_t consumed =
          player->consumed_buffers.fetch_add(1, std::memory_order_relaxed) + 1;
      g_consumed_buffers.fetch_add(1, std::memory_order_relaxed);
      bool expected = false;
      if (player->consumption_reported.compare_exchange_strong(
              expected, true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[mocktail][audio] OpenSL playback evidence: "
                     "consumed_buffers=%llu\n",
                     static_cast<unsigned long long>(consumed));
      }
      break;
    }
    case OpenSlBufferQueueEvent::kDiscarded:
      break;
  }
}

const char* PcmFormatName(PcmSampleFormat format) {
  switch (format) {
    case PcmSampleFormat::kUnsigned8:
      return "u8";
    case PcmSampleFormat::kSigned16LittleEndian:
      return "s16le";
    case PcmSampleFormat::kSigned32LittleEndian:
      return "s32le";
    case PcmSampleFormat::kFloat32LittleEndian:
      return "f32le";
  }
  return "unknown";
}

float LinearGain(abi::Millibel level, abi::Boolean muted) {
  if (muted != abi::kBooleanFalse || level == kMinVolumeLevel) {
    return 0.0F;
  }
  return std::pow(10.0F, static_cast<float>(level) / 2000.0F);
}

abi::Result ObjectRealize(abi::Object self, abi::Boolean async) {
  RuntimeObject* object = FromObject(self);
  if (object == nullptr) {
    return abi::kResultParameterInvalid;
  }
  if (async != abi::kBooleanFalse) {
    return abi::kResultFeatureUnsupported;
  }

  std::lock_guard<std::mutex> lock(object->mutex);
  if (object->state != abi::kObjectStateUnrealized) {
    return abi::kResultPreconditionsViolated;
  }
  if (object->kind == RuntimeObjectKind::kPlayer) {
    SdlAudioSinkOptions sink_options;
    sink_options.source_spec = object->pcm_spec;
    sink_options.start_paused = true;
    std::unique_ptr<AudioSink> sink;
    Status status = CreateSdlAudioSink(sink_options, &sink);
    if (!status.ok()) {
      std::fprintf(stderr, "[mocktail][audio] OpenSL player realize failed: %s\n",
                   status.message().c_str());
      return ToOpenSlResult(status);
    }

    OpenSlSimpleBufferQueueOptions queue_options;
    queue_options.max_buffers = object->queue_capacity;
    queue_options.event_callback = &OnQueueEvent;
    queue_options.event_context = object;
    status = OpenSlSimpleBufferQueueAdapter::Create(
        std::move(sink), queue_options, &object->queue_adapter);
    if (!status.ok()) {
      std::fprintf(stderr, "[mocktail][audio] OpenSL queue realize failed: %s\n",
                   status.message().c_str());
      return ToOpenSlResult(status);
    }
    std::fprintf(stderr,
                 "[mocktail][audio] OpenSL player realized: rate_hz=%d "
                 "channels=%u format=%s queue_capacity=%u\n",
                 object->pcm_spec.sample_rate_hz,
                 static_cast<unsigned>(object->pcm_spec.channels),
                 PcmFormatName(object->pcm_spec.format),
                 static_cast<unsigned>(object->queue_capacity));
  }
  object->state = abi::kObjectStateRealized;
  return abi::kResultSuccess;
}

abi::Result ObjectResume(abi::Object self, abi::Boolean /*async*/) {
  RuntimeObject* object = FromObject(self);
  if (object == nullptr) {
    return abi::kResultParameterInvalid;
  }
  return abi::kResultPreconditionsViolated;
}

abi::Result ObjectGetState(abi::Object self, abi::Uint32* state) {
  RuntimeObject* object = FromObject(self);
  if (object == nullptr || state == nullptr) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(object->mutex);
  *state = object->state;
  return abi::kResultSuccess;
}

abi::Result ObjectGetInterface(abi::Object self, abi::InterfaceId iid,
                               void* interface_out) {
  RuntimeObject* object = FromObject(self);
  if (object == nullptr || iid == nullptr || interface_out == nullptr) {
    return abi::kResultParameterInvalid;
  }

  std::lock_guard<std::mutex> lock(object->mutex);
  if (object->kind == RuntimeObjectKind::kPlayer &&
      object->expose_configuration &&
      InterfaceIdEquals(iid, SL_IID_ANDROIDCONFIGURATION)) {
    *static_cast<abi::AndroidConfiguration*>(interface_out) =
        object->ConfigurationInterface();
    return abi::kResultSuccess;
  }
  if (object->state != abi::kObjectStateRealized) {
    *static_cast<void**>(interface_out) = nullptr;
    return abi::kResultPreconditionsViolated;
  }
  if (object->kind == RuntimeObjectKind::kEngine &&
      InterfaceIdEquals(iid, SL_IID_ENGINE)) {
    *static_cast<abi::Engine*>(interface_out) = object->EngineInterface();
    return abi::kResultSuccess;
  }
  if (object->kind == RuntimeObjectKind::kPlayer &&
      InterfaceIdEquals(iid, SL_IID_PLAY)) {
    *static_cast<abi::Play*>(interface_out) = object->PlayInterface();
    return abi::kResultSuccess;
  }
  if (object->kind == RuntimeObjectKind::kPlayer && object->expose_volume &&
      InterfaceIdEquals(iid, SL_IID_VOLUME)) {
    *static_cast<abi::Volume*>(interface_out) = object->VolumeInterface();
    return abi::kResultSuccess;
  }
  if (object->kind == RuntimeObjectKind::kPlayer && object->expose_queue &&
      IsQueueInterface(iid) && object->queue_adapter != nullptr) {
    if (InterfaceIdEquals(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE)) {
      *static_cast<abi::AndroidSimpleBufferQueue*>(interface_out) =
          object->queue_adapter->interface();
    } else {
      *static_cast<abi::BufferQueue*>(interface_out) =
          reinterpret_cast<abi::BufferQueue>(
              object->queue_adapter->interface());
    }
    return abi::kResultSuccess;
  }
  *static_cast<void**>(interface_out) = nullptr;
  return abi::kResultFeatureUnsupported;
}

abi::Result ObjectRegisterCallback(abi::Object self,
                                   abi::ObjectCallback callback,
                                   void* context) {
  RuntimeObject* object = FromObject(self);
  if (object == nullptr) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(object->mutex);
  object->object_callback = callback;
  object->object_callback_context = context;
  return abi::kResultSuccess;
}

void ObjectAbortAsyncOperation(abi::Object /*self*/) {}

void ObjectDestroy(abi::Object self) {
  RuntimeObject* object = FromObject(self);
  if (object == nullptr) {
    return;
  }

  std::unique_ptr<OpenSlSimpleBufferQueueAdapter> queue_adapter;
  const bool is_player = object->kind == RuntimeObjectKind::kPlayer;
  {
    std::lock_guard<std::mutex> lock(object->mutex);
    queue_adapter = std::move(object->queue_adapter);
    object->state = abi::kObjectStateUnrealized;
  }
  if (queue_adapter != nullptr) {
    queue_adapter->Shutdown();
    const OpenSlSimpleBufferQueueStats stats = queue_adapter->GetStats();
    const Status shutdown_status = queue_adapter->last_error();
    const bool clean = shutdown_status.ok() && stats.pending_buffers == 0;
    if (clean) {
      g_clean_player_shutdowns.fetch_add(1, std::memory_order_relaxed);
    }
    std::fprintf(
        stderr,
        "[mocktail][audio] OpenSL player shutdown: submitted=%llu "
        "consumed=%llu discarded=%llu clean=%s\n",
        static_cast<unsigned long long>(stats.submitted_buffers),
        static_cast<unsigned long long>(stats.consumed_buffers),
        static_cast<unsigned long long>(stats.discarded_buffers),
        clean ? "true" : "false");
  } else if (is_player) {
    std::fprintf(stderr,
                 "[mocktail][audio] OpenSL unrealized player destroyed\n");
  }
  object->magic = 0;
  delete object;
}

abi::Result ObjectSetPriority(abi::Object self, abi::Int32 priority,
                              abi::Boolean preemptable) {
  RuntimeObject* object = FromObject(self);
  if (object == nullptr ||
      (preemptable != abi::kBooleanFalse &&
       preemptable != abi::kBooleanTrue)) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(object->mutex);
  object->priority = priority;
  object->preemptable = preemptable;
  return abi::kResultSuccess;
}

abi::Result ObjectGetPriority(abi::Object self, abi::Int32* priority,
                              abi::Boolean* preemptable) {
  RuntimeObject* object = FromObject(self);
  if (object == nullptr || priority == nullptr || preemptable == nullptr) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(object->mutex);
  *priority = object->priority;
  *preemptable = object->preemptable;
  return abi::kResultSuccess;
}

abi::Result ObjectSetLossOfControlInterfaces(
    abi::Object self, abi::Int16 /*num_interfaces*/, abi::InterfaceId* /*ids*/,
    abi::Boolean /*enabled*/) {
  return FromObject(self) == nullptr ? abi::kResultParameterInvalid
                                     : abi::kResultFeatureUnsupported;
}

abi::Result UnsupportedDeviceFactory(abi::Engine self, abi::Object* object,
                                     abi::Uint32 /*device_id*/,
                                     abi::Uint32 /*num_interfaces*/,
                                     const abi::InterfaceId* /*ids*/,
                                     const abi::Boolean* /*required*/) {
  if (object != nullptr) {
    *object = nullptr;
  }
  return FromEngine(self) == nullptr || object == nullptr
             ? abi::kResultParameterInvalid
             : abi::kResultFeatureUnsupported;
}

abi::Result EngineCreateAudioPlayer(
    abi::Engine self, abi::Object* player, abi::DataSource* source,
    abi::DataSink* sink, abi::Uint32 num_interfaces,
    const abi::InterfaceId* ids, const abi::Boolean* required) {
  if (player == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *player = nullptr;
  RuntimeObject* engine = FromEngine(self);
  if (engine == nullptr || engine->kind != RuntimeObjectKind::kEngine ||
      !IsObjectRealized(engine)) {
    return abi::kResultPreconditionsViolated;
  }
  abi::Result result =
      ValidateInterfaceArray(num_interfaces, ids, required);
  if (result != abi::kResultSuccess) {
    return result;
  }

  PcmSpec pcm_spec;
  abi::Uint32 queue_capacity = 0;
  result = ValidatePcmSource(source, &pcm_spec, &queue_capacity);
  if (result != abi::kResultSuccess) {
    return result;
  }
  result = ValidateOutputMixSink(sink);
  if (result != abi::kResultSuccess) {
    return result;
  }

  bool expose_queue = false;
  bool expose_volume = false;
  bool expose_configuration = false;
  for (abi::Uint32 index = 0; index < num_interfaces; ++index) {
    const bool supported = InterfaceIdEquals(ids[index], SL_IID_PLAY) ||
                           IsQueueInterface(ids[index]) ||
                           InterfaceIdEquals(ids[index], SL_IID_VOLUME) ||
                           InterfaceIdEquals(ids[index],
                                             SL_IID_ANDROIDCONFIGURATION);
    if (!supported && required[index] != abi::kBooleanFalse) {
      return abi::kResultFeatureUnsupported;
    }
    expose_queue = expose_queue || IsQueueInterface(ids[index]);
    expose_volume = expose_volume ||
                    InterfaceIdEquals(ids[index], SL_IID_VOLUME);
    expose_configuration =
        expose_configuration ||
        InterfaceIdEquals(ids[index], SL_IID_ANDROIDCONFIGURATION);
  }
  if (!expose_queue) {
    return abi::kResultFeatureUnsupported;
  }

  auto* object =
      new (std::nothrow) RuntimeObject(RuntimeObjectKind::kPlayer);
  if (object == nullptr) {
    return abi::kResultMemoryFailure;
  }
  object->pcm_spec = pcm_spec;
  object->queue_capacity = queue_capacity;
  object->expose_queue = expose_queue;
  object->expose_volume = expose_volume;
  object->expose_configuration = expose_configuration;
  *player = object->ObjectInterface();
  return abi::kResultSuccess;
}

abi::Result EngineCreateAudioRecorder(
    abi::Engine self, abi::Object* recorder, abi::DataSource* /*source*/,
    abi::DataSink* /*sink*/, abi::Uint32 /*num_interfaces*/,
    const abi::InterfaceId* /*ids*/, const abi::Boolean* /*required*/) {
  if (recorder != nullptr) {
    *recorder = nullptr;
  }
  if (FromEngine(self) == nullptr || recorder == nullptr) {
    return abi::kResultParameterInvalid;
  }
  return abi::kResultFeatureUnsupported;
}

abi::Result EngineCreateMidiPlayer(
    abi::Engine self, abi::Object* player, abi::DataSource* /*midi_source*/,
    abi::DataSource* /*bank_source*/, abi::DataSink* /*audio_output*/,
    abi::DataSink* /*vibra*/, abi::DataSink* /*led_array*/,
    abi::Uint32 /*num_interfaces*/, const abi::InterfaceId* /*ids*/,
    const abi::Boolean* /*required*/) {
  if (player != nullptr) {
    *player = nullptr;
  }
  return FromEngine(self) == nullptr || player == nullptr
             ? abi::kResultParameterInvalid
             : abi::kResultFeatureUnsupported;
}

abi::Result UnsupportedObjectFactory(abi::Engine self, abi::Object* object,
                                     abi::Uint32 /*num_interfaces*/,
                                     const abi::InterfaceId* /*ids*/,
                                     const abi::Boolean* /*required*/) {
  if (object != nullptr) {
    *object = nullptr;
  }
  return FromEngine(self) == nullptr || object == nullptr
             ? abi::kResultParameterInvalid
             : abi::kResultFeatureUnsupported;
}

abi::Result EngineCreateOutputMix(abi::Engine self, abi::Object* mix,
                                  abi::Uint32 num_interfaces,
                                  const abi::InterfaceId* ids,
                                  const abi::Boolean* required) {
  if (mix == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *mix = nullptr;
  RuntimeObject* engine = FromEngine(self);
  if (engine == nullptr || engine->kind != RuntimeObjectKind::kEngine ||
      !IsObjectRealized(engine)) {
    return abi::kResultPreconditionsViolated;
  }
  const abi::Result validation =
      ValidateInterfaceArray(num_interfaces, ids, required);
  if (validation != abi::kResultSuccess) {
    return validation;
  }
  for (abi::Uint32 index = 0; index < num_interfaces; ++index) {
    if (required[index] != abi::kBooleanFalse) {
      return abi::kResultFeatureUnsupported;
    }
  }
  auto* object =
      new (std::nothrow) RuntimeObject(RuntimeObjectKind::kOutputMix);
  if (object == nullptr) {
    return abi::kResultMemoryFailure;
  }
  *mix = object->ObjectInterface();
  return abi::kResultSuccess;
}

abi::Result EngineCreateMetadataExtractor(
    abi::Engine self, abi::Object* extractor, abi::DataSource* /*source*/,
    abi::Uint32 num_interfaces, const abi::InterfaceId* ids,
    const abi::Boolean* required) {
  return UnsupportedObjectFactory(self, extractor, num_interfaces, ids,
                                  required);
}

abi::Result EngineCreateExtensionObject(
    abi::Engine self, abi::Object* object, void* /*parameters*/,
    abi::Uint32 /*object_id*/, abi::Uint32 num_interfaces,
    const abi::InterfaceId* ids, const abi::Boolean* required) {
  return UnsupportedObjectFactory(self, object, num_interfaces, ids, required);
}

abi::Uint32 SupportedInterfaceCount(abi::Uint32 object_id) {
  switch (object_id) {
    case abi::kObjectIdEngine:
      return 1;
    case abi::kObjectIdAudioPlayer:
      return 4;
    case abi::kObjectIdAudioRecorder:
    case abi::kObjectIdOutputMix:
      return 0;
    default:
      return 0;
  }
}

abi::InterfaceId SupportedInterface(abi::Uint32 object_id,
                                    abi::Uint32 index) {
  if (object_id == abi::kObjectIdEngine && index == 0) {
    return SL_IID_ENGINE;
  }
  if (object_id == abi::kObjectIdAudioPlayer) {
    switch (index) {
      case 0:
        return SL_IID_PLAY;
      case 1:
        return SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
      case 2:
        return SL_IID_VOLUME;
      case 3:
        return SL_IID_ANDROIDCONFIGURATION;
      default:
        break;
    }
  }
  return nullptr;
}

abi::Result EngineQueryNumSupportedInterfaces(abi::Engine self,
                                              abi::Uint32 object_id,
                                              abi::Uint32* count) {
  if (FromEngine(self) == nullptr || count == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *count = SupportedInterfaceCount(object_id);
  return abi::kResultSuccess;
}

abi::Result EngineQuerySupportedInterfaces(abi::Engine self,
                                           abi::Uint32 object_id,
                                           abi::Uint32 index,
                                           abi::InterfaceId* iid) {
  if (FromEngine(self) == nullptr || iid == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *iid = SupportedInterface(object_id, index);
  return *iid != nullptr ? abi::kResultSuccess
                         : abi::kResultParameterInvalid;
}

abi::Result EngineQueryNumSupportedExtensions(abi::Engine self,
                                              abi::Uint32* count) {
  if (FromEngine(self) == nullptr || count == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *count = 0;
  return abi::kResultSuccess;
}

abi::Result EngineQuerySupportedExtension(abi::Engine self,
                                          abi::Uint32 /*index*/,
                                          abi::Char* name,
                                          abi::Int16* name_length) {
  if (FromEngine(self) == nullptr || name_length == nullptr) {
    return abi::kResultParameterInvalid;
  }
  if (name != nullptr && *name_length > 0) {
    name[0] = 0;
  }
  *name_length = 0;
  return abi::kResultFeatureUnsupported;
}

abi::Result EngineIsExtensionSupported(abi::Engine self,
                                       const abi::Char* name,
                                       abi::Boolean* supported) {
  if (FromEngine(self) == nullptr || name == nullptr || supported == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *supported = abi::kBooleanFalse;
  return abi::kResultSuccess;
}

abi::Result PlaySetState(abi::Play self, abi::Uint32 state) {
  RuntimeObject* player = FromPlay(self);
  if (player == nullptr || player->kind != RuntimeObjectKind::kPlayer) {
    return abi::kResultParameterInvalid;
  }
  if (state != abi::kPlayStateStopped && state != abi::kPlayStatePaused &&
      state != abi::kPlayStatePlaying) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->state != abi::kObjectStateRealized ||
      player->queue_adapter == nullptr) {
    return abi::kResultPreconditionsViolated;
  }
  const Status status = player->queue_adapter->SetPlaying(
      state == abi::kPlayStatePlaying);
  if (!status.ok()) {
    return ToOpenSlResult(status);
  }
  player->play_state = state;
  return abi::kResultSuccess;
}

abi::Result PlayGetState(abi::Play self, abi::Uint32* state) {
  RuntimeObject* player = FromPlay(self);
  if (player == nullptr || state == nullptr ||
      player->kind != RuntimeObjectKind::kPlayer) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  *state = player->play_state;
  return abi::kResultSuccess;
}

abi::Result PlayGetDuration(abi::Play self, abi::Uint32* milliseconds) {
  if (FromPlay(self) == nullptr || milliseconds == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *milliseconds = abi::kTimeUnknown;
  return abi::kResultSuccess;
}

abi::Result PlayGetPosition(abi::Play self, abi::Uint32* milliseconds) {
  RuntimeObject* player = FromPlay(self);
  if (player == nullptr || milliseconds == nullptr ||
      player->kind != RuntimeObjectKind::kPlayer) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->queue_adapter == nullptr) {
    return abi::kResultPreconditionsViolated;
  }
  const OpenSlSimpleBufferQueueStats stats = player->queue_adapter->GetStats();
  const std::size_t frame_bytes = BytesPerFrame(player->pcm_spec);
  if (frame_bytes == 0 || player->pcm_spec.sample_rate_hz <= 0) {
    return abi::kResultInternalError;
  }
  const std::uint64_t frames = stats.consumed_bytes / frame_bytes;
  const std::uint64_t position =
      frames * 1000 / static_cast<std::uint64_t>(player->pcm_spec.sample_rate_hz);
  *milliseconds = static_cast<abi::Uint32>(
      std::min<std::uint64_t>(position, std::numeric_limits<abi::Uint32>::max()));
  return abi::kResultSuccess;
}

abi::Result UnsupportedPlayCallback(abi::Play self,
                                    abi::PlayCallback /*callback*/,
                                    void* /*context*/) {
  return FromPlay(self) == nullptr ? abi::kResultParameterInvalid
                                   : abi::kResultFeatureUnsupported;
}

abi::Result UnsupportedPlayValue(abi::Play self, abi::Uint32 /*value*/) {
  return FromPlay(self) == nullptr ? abi::kResultParameterInvalid
                                   : abi::kResultFeatureUnsupported;
}

abi::Result UnsupportedPlayOutput(abi::Play self, abi::Uint32* output) {
  if (FromPlay(self) == nullptr || output == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *output = 0;
  return abi::kResultFeatureUnsupported;
}

abi::Result UnsupportedPlayOperation(abi::Play self) {
  return FromPlay(self) == nullptr ? abi::kResultParameterInvalid
                                   : abi::kResultFeatureUnsupported;
}

abi::Result ApplyPlayerGainLocked(RuntimeObject* player) {
  if (player->queue_adapter == nullptr) {
    return abi::kResultPreconditionsViolated;
  }
  return ToOpenSlResult(player->queue_adapter->SetGain(
      LinearGain(player->volume_level, player->muted)));
}

abi::Result VolumeSetLevel(abi::Volume self, abi::Millibel level) {
  RuntimeObject* player = FromVolume(self);
  if (player == nullptr || player->kind != RuntimeObjectKind::kPlayer) {
    return abi::kResultParameterInvalid;
  }
  if (level > kMaxVolumeLevel) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  const abi::Millibel previous = player->volume_level;
  player->volume_level = level;
  const abi::Result result = ApplyPlayerGainLocked(player);
  if (result != abi::kResultSuccess) {
    player->volume_level = previous;
  }
  return result;
}

abi::Result VolumeGetLevel(abi::Volume self, abi::Millibel* level) {
  RuntimeObject* player = FromVolume(self);
  if (player == nullptr || level == nullptr ||
      player->kind != RuntimeObjectKind::kPlayer) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  *level = player->volume_level;
  return abi::kResultSuccess;
}

abi::Result VolumeGetMaxLevel(abi::Volume self, abi::Millibel* level) {
  if (FromVolume(self) == nullptr || level == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *level = kMaxVolumeLevel;
  return abi::kResultSuccess;
}

abi::Result VolumeSetMute(abi::Volume self, abi::Boolean muted) {
  RuntimeObject* player = FromVolume(self);
  if (player == nullptr || player->kind != RuntimeObjectKind::kPlayer ||
      (muted != abi::kBooleanFalse && muted != abi::kBooleanTrue)) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  const abi::Boolean previous = player->muted;
  player->muted = muted;
  const abi::Result result = ApplyPlayerGainLocked(player);
  if (result != abi::kResultSuccess) {
    player->muted = previous;
  }
  return result;
}

abi::Result VolumeGetMute(abi::Volume self, abi::Boolean* muted) {
  RuntimeObject* player = FromVolume(self);
  if (player == nullptr || muted == nullptr ||
      player->kind != RuntimeObjectKind::kPlayer) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  *muted = player->muted;
  return abi::kResultSuccess;
}

abi::Result VolumeEnableStereoPosition(abi::Volume self,
                                       abi::Boolean enabled) {
  if (FromVolume(self) == nullptr ||
      (enabled != abi::kBooleanFalse && enabled != abi::kBooleanTrue)) {
    return abi::kResultParameterInvalid;
  }
  return enabled == abi::kBooleanFalse ? abi::kResultSuccess
                                       : abi::kResultFeatureUnsupported;
}

abi::Result VolumeIsStereoPositionEnabled(abi::Volume self,
                                          abi::Boolean* enabled) {
  if (FromVolume(self) == nullptr || enabled == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *enabled = abi::kBooleanFalse;
  return abi::kResultSuccess;
}

abi::Result VolumeSetStereoPosition(abi::Volume self,
                                    abi::Permille /*position*/) {
  return FromVolume(self) == nullptr ? abi::kResultParameterInvalid
                                     : abi::kResultFeatureUnsupported;
}

abi::Result VolumeGetStereoPosition(abi::Volume self,
                                    abi::Permille* position) {
  if (FromVolume(self) == nullptr || position == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *position = 0;
  return abi::kResultSuccess;
}

bool ConfigurationKeyEquals(const abi::Char* key, const char* expected) {
  return key != nullptr &&
         std::strcmp(reinterpret_cast<const char*>(key), expected) == 0;
}

abi::Result ConfigurationSet(abi::AndroidConfiguration self,
                             const abi::Char* key, const void* value,
                             abi::Uint32 value_size) {
  RuntimeObject* player = FromConfiguration(self);
  if (player == nullptr || player->kind != RuntimeObjectKind::kPlayer ||
      key == nullptr || value == nullptr) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->state != abi::kObjectStateUnrealized) {
    return abi::kResultPreconditionsViolated;
  }
  if (ConfigurationKeyEquals(key, abi::kAndroidPlaybackStreamTypeKey)) {
    if (value_size != sizeof(abi::Int32)) {
      return abi::kResultParameterInvalid;
    }
    const abi::Int32 stream_type = *static_cast<const abi::Int32*>(value);
    if (stream_type != abi::kAndroidStreamMedia) {
      return abi::kResultFeatureUnsupported;
    }
    player->stream_type = stream_type;
    return abi::kResultSuccess;
  }
  if (ConfigurationKeyEquals(key, abi::kAndroidPerformanceModeKey)) {
    if (value_size != sizeof(abi::Uint32)) {
      return abi::kResultParameterInvalid;
    }
    const abi::Uint32 performance_mode =
        *static_cast<const abi::Uint32*>(value);
    if (performance_mode != abi::kAndroidPerformanceNone) {
      return abi::kResultFeatureUnsupported;
    }
    player->performance_mode = performance_mode;
    return abi::kResultSuccess;
  }
  return abi::kResultFeatureUnsupported;
}

template <typename Value>
abi::Result CopyConfigurationValue(const Value& source, abi::Uint32* value_size,
                                   void* value) {
  if (value_size == nullptr) {
    return abi::kResultParameterInvalid;
  }
  const abi::Uint32 required_size = sizeof(Value);
  if (value == nullptr) {
    *value_size = required_size;
    return abi::kResultSuccess;
  }
  if (*value_size < required_size) {
    *value_size = required_size;
    return abi::kResultBufferInsufficient;
  }
  std::memcpy(value, &source, required_size);
  *value_size = required_size;
  return abi::kResultSuccess;
}

abi::Result ConfigurationGet(abi::AndroidConfiguration self,
                             const abi::Char* key, abi::Uint32* value_size,
                             void* value) {
  RuntimeObject* player = FromConfiguration(self);
  if (player == nullptr || player->kind != RuntimeObjectKind::kPlayer ||
      key == nullptr) {
    return abi::kResultParameterInvalid;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (ConfigurationKeyEquals(key, abi::kAndroidPlaybackStreamTypeKey)) {
    return CopyConfigurationValue(player->stream_type, value_size, value);
  }
  if (ConfigurationKeyEquals(key, abi::kAndroidPerformanceModeKey)) {
    return CopyConfigurationValue(player->performance_mode, value_size, value);
  }
  return abi::kResultFeatureUnsupported;
}

abi::Result ConfigurationAcquireJavaProxy(abi::AndroidConfiguration self,
                                          abi::Uint32 /*proxy_type*/,
                                          abi::JavaProxyObject* proxy_object) {
  if (proxy_object != nullptr) {
    *proxy_object = nullptr;
  }
  return FromConfiguration(self) == nullptr || proxy_object == nullptr
             ? abi::kResultParameterInvalid
             : abi::kResultFeatureUnsupported;
}

abi::Result ConfigurationReleaseJavaProxy(abi::AndroidConfiguration self,
                                          abi::Uint32 /*proxy_type*/) {
  return FromConfiguration(self) == nullptr ? abi::kResultParameterInvalid
                                            : abi::kResultFeatureUnsupported;
}

const abi::ObjectTable kObjectTable = {
    &ObjectRealize,
    &ObjectResume,
    &ObjectGetState,
    &ObjectGetInterface,
    &ObjectRegisterCallback,
    &ObjectAbortAsyncOperation,
    &ObjectDestroy,
    &ObjectSetPriority,
    &ObjectGetPriority,
    &ObjectSetLossOfControlInterfaces,
};

const abi::EngineTable kEngineTable = {
    &UnsupportedDeviceFactory,
    &UnsupportedDeviceFactory,
    &EngineCreateAudioPlayer,
    &EngineCreateAudioRecorder,
    &EngineCreateMidiPlayer,
    &UnsupportedObjectFactory,
    &UnsupportedObjectFactory,
    &EngineCreateOutputMix,
    &EngineCreateMetadataExtractor,
    &EngineCreateExtensionObject,
    &EngineQueryNumSupportedInterfaces,
    &EngineQuerySupportedInterfaces,
    &EngineQueryNumSupportedExtensions,
    &EngineQuerySupportedExtension,
    &EngineIsExtensionSupported,
};

const abi::PlayTable kPlayTable = {
    &PlaySetState,
    &PlayGetState,
    &PlayGetDuration,
    &PlayGetPosition,
    &UnsupportedPlayCallback,
    &UnsupportedPlayValue,
    &UnsupportedPlayOutput,
    &UnsupportedPlayValue,
    &UnsupportedPlayOperation,
    &UnsupportedPlayOutput,
    &UnsupportedPlayValue,
    &UnsupportedPlayOutput,
};

const abi::VolumeTable kVolumeTable = {
    &VolumeSetLevel,
    &VolumeGetLevel,
    &VolumeGetMaxLevel,
    &VolumeSetMute,
    &VolumeGetMute,
    &VolumeEnableStereoPosition,
    &VolumeIsStereoPositionEnabled,
    &VolumeSetStereoPosition,
    &VolumeGetStereoPosition,
};

const abi::AndroidConfigurationTable kConfigurationTable = {
    &ConfigurationSet,
    &ConfigurationGet,
    &ConfigurationAcquireJavaProxy,
    &ConfigurationReleaseJavaProxy,
};

abi::Result QuerySupportedEngineInterface(abi::Uint32 index,
                                          abi::InterfaceId* iid) {
  if (iid == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *iid = index == 0 ? SL_IID_ENGINE : nullptr;
  return *iid != nullptr ? abi::kResultSuccess
                         : abi::kResultParameterInvalid;
}

}  // namespace
}  // namespace mocktail::audio

extern "C" {

MOCKTAIL_OPENSL_EXPORT extern const
    mocktail::audio::opensl_abi::InterfaceId SL_IID_ANDROIDCONFIGURATION =
        &mocktail::audio::kAndroidConfigurationId;
MOCKTAIL_OPENSL_EXPORT extern const
    mocktail::audio::opensl_abi::InterfaceId SL_IID_ANDROIDSIMPLEBUFFERQUEUE =
        &mocktail::audio::kAndroidSimpleBufferQueueId;
MOCKTAIL_OPENSL_EXPORT extern const
    mocktail::audio::opensl_abi::InterfaceId SL_IID_BUFFERQUEUE =
        &mocktail::audio::kBufferQueueId;
MOCKTAIL_OPENSL_EXPORT extern const
    mocktail::audio::opensl_abi::InterfaceId SL_IID_ENGINE =
        &mocktail::audio::kEngineId;
MOCKTAIL_OPENSL_EXPORT extern const
    mocktail::audio::opensl_abi::InterfaceId SL_IID_PLAY =
        &mocktail::audio::kPlayId;
MOCKTAIL_OPENSL_EXPORT extern const
    mocktail::audio::opensl_abi::InterfaceId SL_IID_RECORD =
        &mocktail::audio::kRecordId;
MOCKTAIL_OPENSL_EXPORT extern const
    mocktail::audio::opensl_abi::InterfaceId SL_IID_VOLUME =
        &mocktail::audio::kVolumeId;

MOCKTAIL_OPENSL_EXPORT mocktail::audio::opensl_abi::Result
MOCKTAIL_OPENSL_API_ENTRY slCreateEngine(
    mocktail::audio::opensl_abi::Object* engine,
    mocktail::audio::opensl_abi::Uint32 num_options,
    const mocktail::audio::opensl_abi::EngineOption* options,
    mocktail::audio::opensl_abi::Uint32 num_interfaces,
    const mocktail::audio::opensl_abi::InterfaceId* interface_ids,
    const mocktail::audio::opensl_abi::Boolean* interface_required) {
  namespace abi = mocktail::audio::opensl_abi;
  if (engine == nullptr) {
    return abi::kResultParameterInvalid;
  }
  *engine = nullptr;
  if ((num_options > 0 && options == nullptr) || num_options > 1) {
    return abi::kResultParameterInvalid;
  }
  // THREADSAFE is the only engine option this mutex-backed runtime can claim.
  if (num_options == 1 && (options[0].feature != 1 || options[0].data == 0)) {
    return abi::kResultFeatureUnsupported;
  }
  const abi::Result validation = mocktail::audio::ValidateInterfaceArray(
      num_interfaces, interface_ids, interface_required);
  if (validation != abi::kResultSuccess) {
    return validation;
  }
  for (abi::Uint32 index = 0; index < num_interfaces; ++index) {
    if (!mocktail::audio::InterfaceIdEquals(interface_ids[index],
                                            SL_IID_ENGINE) &&
        interface_required[index] != abi::kBooleanFalse) {
      return abi::kResultFeatureUnsupported;
    }
  }

  auto* object = new (std::nothrow)
      mocktail::audio::RuntimeObject(
          mocktail::audio::RuntimeObjectKind::kEngine);
  if (object == nullptr) {
    return abi::kResultMemoryFailure;
  }
  *engine = object->ObjectInterface();
  return abi::kResultSuccess;
}

MOCKTAIL_OPENSL_EXPORT mocktail::audio::opensl_abi::Result
MOCKTAIL_OPENSL_API_ENTRY slQueryNumSupportedEngineInterfaces(
    mocktail::audio::opensl_abi::Uint32* count) {
  if (count == nullptr) {
    return mocktail::audio::opensl_abi::kResultParameterInvalid;
  }
  *count = 1;
  return mocktail::audio::opensl_abi::kResultSuccess;
}

MOCKTAIL_OPENSL_EXPORT mocktail::audio::opensl_abi::Result
MOCKTAIL_OPENSL_API_ENTRY slQuerySupportedEngineInterfaces(
    mocktail::audio::opensl_abi::Uint32 index,
    mocktail::audio::opensl_abi::InterfaceId* iid) {
  return mocktail::audio::QuerySupportedEngineInterface(index, iid);
}

MOCKTAIL_OPENSL_EXPORT mocktail::audio::opensl_abi::Result
mocktailOpenSlGetRuntimeStats(MocktailOpenSlRuntimeStats* stats,
                              std::uint32_t stats_size) {
  namespace abi = mocktail::audio::opensl_abi;
  if (stats == nullptr || stats_size < sizeof(MocktailOpenSlRuntimeStats)) {
    return abi::kResultParameterInvalid;
  }
  *stats = MocktailOpenSlRuntimeStats{
      mocktail::audio::g_submitted_buffers.load(std::memory_order_relaxed),
      mocktail::audio::g_consumed_buffers.load(std::memory_order_relaxed),
      mocktail::audio::g_clean_player_shutdowns.load(
          std::memory_order_relaxed)};
  return abi::kResultSuccess;
}

}  // extern "C"
