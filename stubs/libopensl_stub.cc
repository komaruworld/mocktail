// Test fixture for libOpenSLES.so and libOpenMAXAL.so. Engine creation requires
// MOCKTAIL_ENABLE_TEST_AUDIO_STUBS=1; audio capture is not implemented.

#include <cstdint>
#include <cstdlib>
#include <cstring>

using SLresult = uint32_t;
using SLboolean = uint32_t;
using SLuint32 = uint32_t;
using SLint32 = int32_t;
using SLInterfaceID = const void*;

static constexpr SLresult SL_RESULT_SUCCESS = 0;
static constexpr SLresult SL_RESULT_PARAMETER_INVALID = 2;
static constexpr SLresult SL_RESULT_FEATURE_UNSUPPORTED = 12;
static constexpr SLuint32 SL_OBJECT_STATE_REALIZED = 2;
static constexpr SLuint32 SL_PLAYSTATE_STOPPED = 1;
static constexpr SLuint32 SL_RECORDSTATE_STOPPED = 1;

struct SLObjectItf_;
struct SLEngineItf_;
struct SLPlayItf_;
struct SLRecordItf_;
struct SLBufferQueueItf_;
struct SLAndroidSimpleBufferQueueItf_;
struct SLVolumeItf_;
struct SLAndroidConfigurationItf_;

using SLObjectItf = const SLObjectItf_* const*;
using SLEngineItf = const SLEngineItf_* const*;
using SLPlayItf = const SLPlayItf_* const*;
using SLRecordItf = const SLRecordItf_* const*;
using SLBufferQueueItf = const SLBufferQueueItf_* const*;
using SLAndroidSimpleBufferQueueItf =
    const SLAndroidSimpleBufferQueueItf_* const*;
using SLVolumeItf = const SLVolumeItf_* const*;
using SLAndroidConfigurationItf = const SLAndroidConfigurationItf_* const*;

using SLObjectCallback = void (*)(SLObjectItf, void*, SLuint32, SLresult,
                                  SLuint32, void*);
using SLBufferQueueCallback = void (*)(SLBufferQueueItf, void*);
using SLAndroidSimpleBufferQueueCallback =
    void (*)(SLAndroidSimpleBufferQueueItf, void*);
using SLPlayCallback = void (*)(SLPlayItf, void*, SLuint32);
using SLRecordCallback = void (*)(SLRecordItf, void*, SLuint32);

struct SLObjectItf_ {
  void (*Destroy)(SLObjectItf self);
  SLresult (*Realize)(SLObjectItf self, SLboolean async);
  SLresult (*Resume)(SLObjectItf self, SLboolean async);
  SLresult (*GetState)(SLObjectItf self, SLuint32* state);
  SLresult (*GetInterface)(SLObjectItf self, SLInterfaceID iid,
                           void* interface_out);
  SLresult (*RegisterCallback)(SLObjectItf self, SLObjectCallback callback,
                               void* context);
  void (*AbortAsyncOperation)(SLObjectItf self);
  SLresult (*SetPriority)(SLObjectItf self, SLint32 priority,
                          SLboolean preemptable);
  SLresult (*GetPriority)(SLObjectItf self, SLint32* priority,
                          SLboolean* preemptable);
  SLresult (*SetLossOfControlInterfaces)(SLObjectItf self,
                                         SLuint32 num_interfaces,
                                         const SLInterfaceID* ids,
                                         SLboolean enabled);
};

struct SLEngineItf_ {
  SLresult (*CreateLEDDevice)(SLEngineItf self, SLObjectItf* device,
                              SLuint32 device_id, SLuint32 num_interfaces,
                              const SLInterfaceID* ids,
                              const SLboolean* required);
  SLresult (*CreateVibraDevice)(SLEngineItf self, SLObjectItf* device,
                                SLuint32 device_id, SLuint32 num_interfaces,
                                const SLInterfaceID* ids,
                                const SLboolean* required);
  SLresult (*CreateAudioPlayer)(SLEngineItf self, SLObjectItf* player,
                                const void* source, const void* sink,
                                SLuint32 num_interfaces,
                                const SLInterfaceID* ids,
                                const SLboolean* required);
  SLresult (*CreateAudioRecorder)(SLEngineItf self, SLObjectItf* recorder,
                                  const void* source, const void* sink,
                                  SLuint32 num_interfaces,
                                  const SLInterfaceID* ids,
                                  const SLboolean* required);
  SLresult (*CreateMidiPlayer)(SLEngineItf self, SLObjectItf* player,
                               const void* source, const void* sink,
                               const void* bank_source,
                               const void* bank_sink,
                               SLuint32 num_interfaces,
                               const SLInterfaceID* ids,
                               const SLboolean* required);
  SLresult (*CreateListener)(SLEngineItf self, SLObjectItf* listener,
                             SLuint32 num_interfaces, const SLInterfaceID* ids,
                             const SLboolean* required);
  SLresult (*Create3DGroup)(SLEngineItf self, SLObjectItf* group,
                            SLuint32 num_interfaces, const SLInterfaceID* ids,
                            const SLboolean* required);
  SLresult (*CreateOutputMix)(SLEngineItf self, SLObjectItf* mix,
                              SLuint32 num_interfaces, const SLInterfaceID* ids,
                              const SLboolean* required);
  SLresult (*CreateMetadataExtractor)(SLEngineItf self, SLObjectItf* extractor,
                                      const void* source,
                                      SLuint32 num_interfaces,
                                      const SLInterfaceID* ids,
                                      const SLboolean* required);
  SLresult (*CreateExtensionObject)(SLEngineItf self, SLObjectItf* object,
                                    void* parameters, SLuint32 object_id,
                                    SLuint32 num_interfaces,
                                    const SLInterfaceID* ids,
                                    const SLboolean* required);
  SLresult (*QueryNumSupportedInterfaces)(SLEngineItf self, SLuint32 object_id,
                                          SLuint32* count);
  SLresult (*QuerySupportedInterfaces)(SLEngineItf self, SLuint32 object_id,
                                       SLuint32 index, SLInterfaceID* iid);
  SLresult (*QueryNumSupportedExtensions)(SLEngineItf self, SLuint32* count);
  SLresult (*QuerySupportedExtension)(SLEngineItf self, SLuint32 index,
                                      char* name, SLint32* name_length);
  SLresult (*IsExtensionSupported)(SLEngineItf self, const char* name,
                                   SLboolean* supported);
};

struct SLPlayItf_ {
  SLresult (*SetPlayState)(SLPlayItf self, SLuint32 state);
  SLresult (*GetPlayState)(SLPlayItf self, SLuint32* state);
  SLresult (*GetDuration)(SLPlayItf self, SLuint32* duration);
  SLresult (*GetPosition)(SLPlayItf self, SLuint32* position);
  SLresult (*RegisterCallback)(SLPlayItf self, SLPlayCallback callback,
                               void* context);
  SLresult (*SetCallbackEventsMask)(SLPlayItf self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(SLPlayItf self, SLuint32* mask);
  SLresult (*SetMarkerPosition)(SLPlayItf self, SLuint32 position);
  SLresult (*ClearMarkerPosition)(SLPlayItf self);
  SLresult (*GetMarkerPosition)(SLPlayItf self, SLuint32* position);
  SLresult (*SetPositionUpdatePeriod)(SLPlayItf self, SLuint32 period);
  SLresult (*GetPositionUpdatePeriod)(SLPlayItf self, SLuint32* period);
};

struct SLRecordItf_ {
  SLresult (*SetRecordState)(SLRecordItf self, SLuint32 state);
  SLresult (*GetRecordState)(SLRecordItf self, SLuint32* state);
  SLresult (*SetDurationLimit)(SLRecordItf self, SLuint32 milliseconds);
  SLresult (*GetPosition)(SLRecordItf self, SLuint32* position);
  SLresult (*RegisterCallback)(SLRecordItf self, SLRecordCallback callback,
                               void* context);
  SLresult (*SetCallbackEventsMask)(SLRecordItf self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(SLRecordItf self, SLuint32* mask);
  SLresult (*SetMarkerPosition)(SLRecordItf self, SLuint32 position);
  SLresult (*ClearMarkerPosition)(SLRecordItf self);
  SLresult (*GetMarkerPosition)(SLRecordItf self, SLuint32* position);
  SLresult (*SetPositionUpdatePeriod)(SLRecordItf self, SLuint32 period);
  SLresult (*GetPositionUpdatePeriod)(SLRecordItf self, SLuint32* period);
};

struct SLBufferQueueState {
  SLuint32 count;
  SLuint32 play_index;
};

struct SLBufferQueueItf_ {
  SLresult (*Enqueue)(SLBufferQueueItf self, const void* buffer,
                      SLuint32 size);
  SLresult (*Clear)(SLBufferQueueItf self);
  SLresult (*GetState)(SLBufferQueueItf self, SLBufferQueueState* state);
  SLresult (*RegisterCallback)(SLBufferQueueItf self,
                               SLBufferQueueCallback callback,
                               void* context);
};

struct SLAndroidSimpleBufferQueueItf_ {
  SLresult (*Enqueue)(SLAndroidSimpleBufferQueueItf self, const void* buffer,
                      SLuint32 size);
  SLresult (*Clear)(SLAndroidSimpleBufferQueueItf self);
  SLresult (*GetState)(SLAndroidSimpleBufferQueueItf self,
                       SLBufferQueueState* state);
  SLresult (*RegisterCallback)(SLAndroidSimpleBufferQueueItf self,
                               SLAndroidSimpleBufferQueueCallback callback,
                               void* context);
};

struct SLVolumeItf_ {
  SLresult (*SetVolumeLevel)(SLVolumeItf self, SLint32 level);
  SLresult (*GetVolumeLevel)(SLVolumeItf self, SLint32* level);
  SLresult (*GetMaxVolumeLevel)(SLVolumeItf self, SLint32* level);
  SLresult (*SetMute)(SLVolumeItf self, SLboolean mute);
  SLresult (*GetMute)(SLVolumeItf self, SLboolean* mute);
  SLresult (*EnableStereoPosition)(SLVolumeItf self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(SLVolumeItf self, SLboolean* enabled);
  SLresult (*SetStereoPosition)(SLVolumeItf self, SLint32 position);
  SLresult (*GetStereoPosition)(SLVolumeItf self, SLint32* position);
};

struct SLAndroidConfigurationItf_ {
  SLresult (*SetConfiguration)(SLAndroidConfigurationItf self,
                               const char* key, const void* value,
                               SLuint32 value_size);
  SLresult (*GetConfiguration)(SLAndroidConfigurationItf self,
                               const char* key, SLuint32* value_size,
                               void* value);
};

struct FakeObject {
  const SLObjectItf_* table;
  const char* kind;
};

namespace {

SLuint32 g_play_state = SL_PLAYSTATE_STOPPED;
SLuint32 g_record_state = SL_RECORDSTATE_STOPPED;
SLBufferQueueState g_buffer_queue_state{};
SLBufferQueueCallback g_buffer_queue_callback = nullptr;
void* g_buffer_queue_context = nullptr;
SLBufferQueueState g_simple_queue_state{};
SLAndroidSimpleBufferQueueCallback g_simple_queue_callback = nullptr;
void* g_simple_queue_context = nullptr;

bool TestAudioStubsEnabled() {
  const char* enabled = std::getenv("MOCKTAIL_ENABLE_TEST_AUDIO_STUBS");
  return enabled != nullptr && std::strcmp(enabled, "1") == 0;
}

bool MatchesInterfaceId(SLInterfaceID iid, void* value, void* address) {
  return iid == value || iid == address;
}

SLresult SuccessObject(SLObjectItf* object, SLObjectItf fake_object) {
  if (object) {
    *object = fake_object;
  }
  return SL_RESULT_SUCCESS;
}

void ObjectDestroy(SLObjectItf) {}

SLresult ObjectRealize(SLObjectItf, SLboolean) {
  return SL_RESULT_SUCCESS;
}

SLresult ObjectResume(SLObjectItf, SLboolean) {
  return SL_RESULT_SUCCESS;
}

SLresult ObjectGetState(SLObjectItf, SLuint32* state) {
  if (state) {
    *state = SL_OBJECT_STATE_REALIZED;
  }
  return SL_RESULT_SUCCESS;
}

SLresult ObjectRegisterCallback(SLObjectItf,
                                SLObjectCallback,
                                void*) {
  return SL_RESULT_SUCCESS;
}

void ObjectAbortAsyncOperation(SLObjectItf) {}

SLresult ObjectSetPriority(SLObjectItf, SLint32,
                           SLboolean) {
  return SL_RESULT_SUCCESS;
}

SLresult ObjectGetPriority(SLObjectItf, SLint32* priority,
                           SLboolean* preemptable) {
  if (priority) {
    *priority = 0;
  }
  if (preemptable) {
    *preemptable = 0;
  }
  return SL_RESULT_SUCCESS;
}

SLresult ObjectSetLossOfControlInterfaces(SLObjectItf,
                                          SLuint32,
                                          const SLInterfaceID*,
                                          SLboolean) {
  return SL_RESULT_SUCCESS;
}

SLresult ObjectGetInterface(SLObjectItf self, SLInterfaceID iid,
                            void* interface_out);
SLresult CreateUnsupportedObject(SLEngineItf, SLObjectItf* object,
                                 SLuint32,
                                 SLuint32,
                                 const SLInterfaceID*,
                                 const SLboolean*);
SLresult CreateAudioPlayer(SLEngineItf self, SLObjectItf* player,
                           const void* source, const void* sink,
                           SLuint32 num_interfaces, const SLInterfaceID* ids,
                           const SLboolean* required);
SLresult CreateAudioRecorder(SLEngineItf self, SLObjectItf* recorder,
                             const void* source, const void* sink,
                             SLuint32 num_interfaces, const SLInterfaceID* ids,
                             const SLboolean* required);
SLresult CreateOutputMix(SLEngineItf self, SLObjectItf* mix,
                         SLuint32 num_interfaces, const SLInterfaceID* ids,
                         const SLboolean* required);

SLresult CreateMidiPlayer(SLEngineItf, SLObjectItf* player,
                          const void*, const void*,
                          const void*,
                          const void*,
                          SLuint32,
                          const SLInterfaceID*,
                          const SLboolean*);
SLresult CreateListener(SLEngineItf, SLObjectItf* listener,
                        SLuint32,
                        const SLInterfaceID*,
                        const SLboolean*);
SLresult Create3DGroup(SLEngineItf, SLObjectItf* group,
                       SLuint32, const SLInterfaceID*,
                       const SLboolean*);
SLresult CreateMetadataExtractor(SLEngineItf, SLObjectItf* extractor,
                                 const void*,
                                 SLuint32,
                                 const SLInterfaceID*,
                                 const SLboolean*);
SLresult CreateExtensionObject(SLEngineItf, SLObjectItf* object,
                               void*, SLuint32,
                               SLuint32,
                               const SLInterfaceID*,
                               const SLboolean*);
SLresult QueryNumSupportedInterfaces(SLEngineItf,
                                     SLuint32, SLuint32* count);
SLresult QuerySupportedInterfaces(SLEngineItf, SLuint32,
                                  SLuint32, SLInterfaceID* iid);
SLresult QueryNumSupportedExtensions(SLEngineItf, SLuint32* count);
SLresult QuerySupportedExtension(SLEngineItf, SLuint32,
                                 char* name, SLint32* name_length);
SLresult IsExtensionSupported(SLEngineItf, const char*,
                              SLboolean* supported);

SLresult PlaySetState(SLPlayItf, SLuint32 state) {
  g_play_state = state;
  return SL_RESULT_SUCCESS;
}

SLresult PlayGetState(SLPlayItf, SLuint32* state) {
  if (state) {
    *state = g_play_state;
  }
  return SL_RESULT_SUCCESS;
}

SLresult ReturnZeroU32(SLuint32* value) {
  if (value) {
    *value = 0;
  }
  return SL_RESULT_SUCCESS;
}

SLresult PlayGetDuration(SLPlayItf, SLuint32* duration) {
  return ReturnZeroU32(duration);
}

SLresult PlayGetPosition(SLPlayItf, SLuint32* position) {
  return ReturnZeroU32(position);
}

SLresult PlayRegisterCallback(SLPlayItf, SLPlayCallback,
                              void*) {
  return SL_RESULT_SUCCESS;
}

SLresult PlaySetEventsMask(SLPlayItf, SLuint32) {
  return SL_RESULT_SUCCESS;
}

SLresult PlayGetEventsMask(SLPlayItf, SLuint32* mask) {
  return ReturnZeroU32(mask);
}

SLresult PlaySetMarkerPosition(SLPlayItf, SLuint32) {
  return SL_RESULT_SUCCESS;
}

SLresult PlayClearMarkerPosition(SLPlayItf) {
  return SL_RESULT_SUCCESS;
}

SLresult PlayGetMarkerPosition(SLPlayItf, SLuint32* position) {
  return ReturnZeroU32(position);
}

SLresult PlaySetPositionUpdatePeriod(SLPlayItf, SLuint32) {
  return SL_RESULT_SUCCESS;
}

SLresult PlayGetPositionUpdatePeriod(SLPlayItf, SLuint32* period) {
  return ReturnZeroU32(period);
}

SLresult RecordSetState(SLRecordItf, SLuint32 state) {
  g_record_state = state;
  return SL_RESULT_SUCCESS;
}

SLresult RecordGetState(SLRecordItf, SLuint32* state) {
  if (state) {
    *state = g_record_state;
  }
  return SL_RESULT_SUCCESS;
}

SLresult RecordSetDurationLimit(SLRecordItf,
                                SLuint32) {
  return SL_RESULT_SUCCESS;
}

SLresult RecordGetPosition(SLRecordItf, SLuint32* position) {
  return ReturnZeroU32(position);
}

SLresult RecordRegisterCallback(SLRecordItf,
                                SLRecordCallback,
                                void*) {
  return SL_RESULT_SUCCESS;
}

SLresult RecordSetEventsMask(SLRecordItf, SLuint32) {
  return SL_RESULT_SUCCESS;
}

SLresult RecordGetEventsMask(SLRecordItf, SLuint32* mask) {
  return ReturnZeroU32(mask);
}

SLresult RecordSetMarkerPosition(SLRecordItf, SLuint32) {
  return SL_RESULT_SUCCESS;
}

SLresult RecordClearMarkerPosition(SLRecordItf) {
  return SL_RESULT_SUCCESS;
}

SLresult RecordGetMarkerPosition(SLRecordItf, SLuint32* position) {
  return ReturnZeroU32(position);
}

SLresult RecordSetPositionUpdatePeriod(SLRecordItf,
                                       SLuint32) {
  return SL_RESULT_SUCCESS;
}

SLresult RecordGetPositionUpdatePeriod(SLRecordItf, SLuint32* period) {
  return ReturnZeroU32(period);
}

SLresult BufferQueueEnqueue(SLBufferQueueItf, const void*,
                            SLuint32) {
  ++g_buffer_queue_state.count;
  return SL_RESULT_SUCCESS;
}

SLresult BufferQueueClear(SLBufferQueueItf) {
  g_buffer_queue_state = {};
  return SL_RESULT_SUCCESS;
}

SLresult BufferQueueGetState(SLBufferQueueItf,
                             SLBufferQueueState* state) {
  if (state) {
    *state = g_buffer_queue_state;
  }
  return SL_RESULT_SUCCESS;
}

SLresult BufferQueueRegisterCallback(SLBufferQueueItf,
                                     SLBufferQueueCallback callback,
                                     void* context) {
  g_buffer_queue_callback = callback;
  g_buffer_queue_context = context;
  return SL_RESULT_SUCCESS;
}

SLresult SimpleBufferQueueEnqueue(SLAndroidSimpleBufferQueueItf,
                                  const void*, SLuint32) {
  ++g_simple_queue_state.count;
  return SL_RESULT_SUCCESS;
}

SLresult SimpleBufferQueueClear(SLAndroidSimpleBufferQueueItf) {
  g_simple_queue_state = {};
  return SL_RESULT_SUCCESS;
}

SLresult SimpleBufferQueueGetState(SLAndroidSimpleBufferQueueItf,
                                   SLBufferQueueState* state) {
  if (state) {
    *state = g_simple_queue_state;
  }
  return SL_RESULT_SUCCESS;
}

SLresult SimpleBufferQueueRegisterCallback(
    SLAndroidSimpleBufferQueueItf,
    SLAndroidSimpleBufferQueueCallback callback, void* context) {
  g_simple_queue_callback = callback;
  g_simple_queue_context = context;
  return SL_RESULT_SUCCESS;
}

SLresult VolumeSetLevel(SLVolumeItf, SLint32) {
  return SL_RESULT_SUCCESS;
}

SLresult VolumeGetLevel(SLVolumeItf, SLint32* level) {
  if (level) {
    *level = 0;
  }
  return SL_RESULT_SUCCESS;
}

SLresult VolumeSetMute(SLVolumeItf, SLboolean) {
  return SL_RESULT_SUCCESS;
}

SLresult VolumeGetMute(SLVolumeItf, SLboolean* mute) {
  if (mute) {
    *mute = 0;
  }
  return SL_RESULT_SUCCESS;
}

SLresult VolumeEnableStereoPosition(SLVolumeItf, SLboolean) {
  return SL_RESULT_SUCCESS;
}

SLresult VolumeIsStereoPositionEnabled(SLVolumeItf,
                                       SLboolean* enabled) {
  if (enabled) {
    *enabled = 0;
  }
  return SL_RESULT_SUCCESS;
}

SLresult VolumeSetStereoPosition(SLVolumeItf, SLint32) {
  return SL_RESULT_SUCCESS;
}

SLresult VolumeGetStereoPosition(SLVolumeItf, SLint32* position) {
  if (position) {
    *position = 0;
  }
  return SL_RESULT_SUCCESS;
}

SLresult ConfigSet(SLAndroidConfigurationItf, const char*,
                   const void*, SLuint32) {
  return SL_RESULT_SUCCESS;
}

SLresult ConfigGet(SLAndroidConfigurationItf, const char*,
                   SLuint32* value_size, void* value) {
  if (value_size) {
    if (value && *value_size > 0) {
      std::memset(value, 0, *value_size);
    }
    *value_size = 0;
  }
  return SL_RESULT_SUCCESS;
}

const SLObjectItf_ kObjectTable = {
    ObjectDestroy,
    ObjectRealize,
    ObjectResume,
    ObjectGetState,
    ObjectGetInterface,
    ObjectRegisterCallback,
    ObjectAbortAsyncOperation,
    ObjectSetPriority,
    ObjectGetPriority,
    ObjectSetLossOfControlInterfaces,
};

const SLEngineItf_ kEngineTable = {
    CreateUnsupportedObject,
    CreateUnsupportedObject,
    CreateAudioPlayer,
    CreateAudioRecorder,
    CreateMidiPlayer,
    CreateListener,
    Create3DGroup,
    CreateOutputMix,
    CreateMetadataExtractor,
    CreateExtensionObject,
    QueryNumSupportedInterfaces,
    QuerySupportedInterfaces,
    QueryNumSupportedExtensions,
    QuerySupportedExtension,
    IsExtensionSupported,
};

const SLPlayItf_ kPlayTable = {
    PlaySetState,
    PlayGetState,
    PlayGetDuration,
    PlayGetPosition,
    PlayRegisterCallback,
    PlaySetEventsMask,
    PlayGetEventsMask,
    PlaySetMarkerPosition,
    PlayClearMarkerPosition,
    PlayGetMarkerPosition,
    PlaySetPositionUpdatePeriod,
    PlayGetPositionUpdatePeriod,
};

const SLRecordItf_ kRecordTable = {
    RecordSetState,
    RecordGetState,
    RecordSetDurationLimit,
    RecordGetPosition,
    RecordRegisterCallback,
    RecordSetEventsMask,
    RecordGetEventsMask,
    RecordSetMarkerPosition,
    RecordClearMarkerPosition,
    RecordGetMarkerPosition,
    RecordSetPositionUpdatePeriod,
    RecordGetPositionUpdatePeriod,
};

const SLBufferQueueItf_ kBufferQueueTable = {
    BufferQueueEnqueue,
    BufferQueueClear,
    BufferQueueGetState,
    BufferQueueRegisterCallback,
};

const SLAndroidSimpleBufferQueueItf_ kSimpleBufferQueueTable = {
    SimpleBufferQueueEnqueue,
    SimpleBufferQueueClear,
    SimpleBufferQueueGetState,
    SimpleBufferQueueRegisterCallback,
};

const SLVolumeItf_ kVolumeTable = {
    VolumeSetLevel,
    VolumeGetLevel,
    VolumeGetLevel,
    VolumeSetMute,
    VolumeGetMute,
    VolumeEnableStereoPosition,
    VolumeIsStereoPositionEnabled,
    VolumeSetStereoPosition,
    VolumeGetStereoPosition,
};

const SLAndroidConfigurationItf_ kConfigTable = {
    ConfigSet,
    ConfigGet,
};

const SLEngineItf_* g_engine_interface = &kEngineTable;
const SLPlayItf_* g_play_interface = &kPlayTable;
const SLRecordItf_* g_record_interface = &kRecordTable;
const SLBufferQueueItf_* g_buffer_queue_interface = &kBufferQueueTable;
const SLAndroidSimpleBufferQueueItf_* g_simple_queue_interface =
    &kSimpleBufferQueueTable;
const SLVolumeItf_* g_volume_interface = &kVolumeTable;
const SLAndroidConfigurationItf_* g_config_interface = &kConfigTable;

FakeObject g_engine_object = {&kObjectTable, "engine"};
FakeObject g_output_mix_object = {&kObjectTable, "output_mix"};
FakeObject g_player_object = {&kObjectTable, "player"};
FakeObject g_generic_object = {&kObjectTable, "generic"};

SLObjectItf EngineObject() {
  return reinterpret_cast<SLObjectItf>(&g_engine_object);
}

SLObjectItf OutputMixObject() {
  return reinterpret_cast<SLObjectItf>(&g_output_mix_object);
}

SLObjectItf PlayerObject() {
  return reinterpret_cast<SLObjectItf>(&g_player_object);
}

SLObjectItf GenericObject() {
  return reinterpret_cast<SLObjectItf>(&g_generic_object);
}

}  // namespace

extern "C" {

void* SL_IID_ANDROIDCONFIGURATION = reinterpret_cast<void*>(0x1001);
void* SL_IID_ANDROIDSIMPLEBUFFERQUEUE = reinterpret_cast<void*>(0x1002);
void* SL_IID_BUFFERQUEUE = reinterpret_cast<void*>(0x1003);
void* SL_IID_ENGINE = reinterpret_cast<void*>(0x1004);
void* SL_IID_PLAY = reinterpret_cast<void*>(0x1005);
void* SL_IID_RECORD = reinterpret_cast<void*>(0x1006);
void* SL_IID_VOLUME = reinterpret_cast<void*>(0x1007);

}  // extern "C"

namespace {

SLresult ObjectGetInterface(SLObjectItf, SLInterfaceID iid,
                            void* interface_out) {
  if (!interface_out) {
    return SL_RESULT_SUCCESS;
  }
  auto out = static_cast<const void***>(interface_out);
  if (MatchesInterfaceId(iid, SL_IID_ENGINE, &SL_IID_ENGINE)) {
    *out = reinterpret_cast<const void**>(&g_engine_interface);
    return SL_RESULT_SUCCESS;
  }
  if (MatchesInterfaceId(iid, SL_IID_PLAY, &SL_IID_PLAY)) {
    *out = reinterpret_cast<const void**>(&g_play_interface);
    return SL_RESULT_SUCCESS;
  }
  if (MatchesInterfaceId(iid, SL_IID_RECORD, &SL_IID_RECORD)) {
    *out = reinterpret_cast<const void**>(&g_record_interface);
    return SL_RESULT_SUCCESS;
  }
  if (MatchesInterfaceId(iid, SL_IID_BUFFERQUEUE, &SL_IID_BUFFERQUEUE)) {
    *out = reinterpret_cast<const void**>(&g_buffer_queue_interface);
    return SL_RESULT_SUCCESS;
  }
  if (MatchesInterfaceId(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                         &SL_IID_ANDROIDSIMPLEBUFFERQUEUE)) {
    *out = reinterpret_cast<const void**>(&g_simple_queue_interface);
    return SL_RESULT_SUCCESS;
  }
  if (MatchesInterfaceId(iid, SL_IID_VOLUME, &SL_IID_VOLUME)) {
    *out = reinterpret_cast<const void**>(&g_volume_interface);
    return SL_RESULT_SUCCESS;
  }
  if (MatchesInterfaceId(iid, SL_IID_ANDROIDCONFIGURATION,
                         &SL_IID_ANDROIDCONFIGURATION)) {
    *out = reinterpret_cast<const void**>(&g_config_interface);
    return SL_RESULT_SUCCESS;
  }
  *out = nullptr;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

SLresult CreateUnsupportedObject(SLEngineItf, SLObjectItf* object,
                                 SLuint32,
                                 SLuint32,
                                 const SLInterfaceID*,
                                 const SLboolean*) {
  return SuccessObject(object, GenericObject());
}

SLresult CreateAudioPlayer(SLEngineItf, SLObjectItf* player,
                           const void*, const void*,
                           SLuint32,
                           const SLInterfaceID*,
                           const SLboolean*) {
  g_play_state = SL_PLAYSTATE_STOPPED;
  g_buffer_queue_state = {};
  g_simple_queue_state = {};
  return SuccessObject(player, PlayerObject());
}

SLresult CreateAudioRecorder(SLEngineItf, SLObjectItf* recorder,
                             const void*, const void*,
                             SLuint32,
                             const SLInterfaceID*,
                             const SLboolean*) {
  if (recorder != nullptr) {
    *recorder = nullptr;
  }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

SLresult CreateMidiPlayer(SLEngineItf, SLObjectItf* player,
                          const void*, const void*,
                          const void*,
                          const void*,
                          SLuint32,
                          const SLInterfaceID*,
                          const SLboolean*) {
  return SuccessObject(player, PlayerObject());
}

SLresult CreateListener(SLEngineItf, SLObjectItf* listener,
                        SLuint32,
                        const SLInterfaceID*,
                        const SLboolean*) {
  return SuccessObject(listener, GenericObject());
}

SLresult Create3DGroup(SLEngineItf, SLObjectItf* group,
                       SLuint32,
                       const SLInterfaceID*,
                       const SLboolean*) {
  return SuccessObject(group, GenericObject());
}

SLresult CreateOutputMix(SLEngineItf, SLObjectItf* mix,
                         SLuint32,
                         const SLInterfaceID*,
                         const SLboolean*) {
  return SuccessObject(mix, OutputMixObject());
}

SLresult CreateMetadataExtractor(SLEngineItf, SLObjectItf* extractor,
                                 const void*,
                                 SLuint32,
                                 const SLInterfaceID*,
                                 const SLboolean*) {
  return SuccessObject(extractor, GenericObject());
}

SLresult CreateExtensionObject(SLEngineItf, SLObjectItf* object,
                               void*, SLuint32,
                               SLuint32,
                               const SLInterfaceID*,
                               const SLboolean*) {
  return SuccessObject(object, GenericObject());
}

SLresult QueryNumSupportedInterfaces(SLEngineItf,
                                     SLuint32, SLuint32* count) {
  if (count) {
    *count = 0;
  }
  return SL_RESULT_SUCCESS;
}

SLresult QuerySupportedInterfaces(SLEngineItf, SLuint32,
                                  SLuint32, SLInterfaceID* iid) {
  if (iid) {
    *iid = nullptr;
  }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

SLresult QueryNumSupportedExtensions(SLEngineItf, SLuint32* count) {
  if (count) {
    *count = 0;
  }
  return SL_RESULT_SUCCESS;
}

SLresult QuerySupportedExtension(SLEngineItf, SLuint32,
                                 char* name, SLint32* name_length) {
  if (name_length) {
    *name_length = 0;
  }
  if (name) {
    name[0] = '\0';
  }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

SLresult IsExtensionSupported(SLEngineItf, const char*,
                              SLboolean* supported) {
  if (supported) {
    *supported = 0;
  }
  return SL_RESULT_SUCCESS;
}

}  // namespace

extern "C" {

SLresult slCreateEngine(void** pEngine, uint32_t,
                         const void*,
                         uint32_t,
                         const void*,
                         const void*) {
  if (pEngine == nullptr) {
    return SL_RESULT_PARAMETER_INVALID;
  }
  *pEngine = nullptr;
  if (!TestAudioStubsEnabled()) {
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  *pEngine =
      const_cast<void*>(reinterpret_cast<const void*>(EngineObject()));
  return SL_RESULT_SUCCESS;
}

SLresult alCreateEngine(void** pEngine, uint32_t,
                         const void*,
                         uint32_t,
                         const void*,
                         const void*) {
  if (pEngine == nullptr) {
    return SL_RESULT_PARAMETER_INVALID;
  }
  *pEngine = nullptr;
  if (!TestAudioStubsEnabled()) {
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  *pEngine =
      const_cast<void*>(reinterpret_cast<const void*>(EngineObject()));
  return SL_RESULT_SUCCESS;
}

}  // extern "C"
