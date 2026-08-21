#ifndef MOCKTAIL_AUDIO_OPENSL_RUNTIME_ABI_H_
#define MOCKTAIL_AUDIO_OPENSL_RUNTIME_ABI_H_

#include <cstdint>

#include "mocktail/audio/opensl_abi.h"

// This header describes the Android OpenSL ES subset used by Roblox's embedded
// FMOD backend. The layouts and numeric values mirror the NDK ABI. Host audio
// policy and playback live in Mocktail classes; no Android implementation
// details leak beyond this boundary.
namespace mocktail::audio::opensl_abi {

#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)

using Boolean = SLboolean;
using Int16 = SLint16;
using Int32 = SLint32;
using Char = SLchar;
using Millibel = SLmillibel;
using Permille = SLpermille;
using InterfaceIdValue = struct SLInterfaceID_;
using InterfaceId = SLInterfaceID;
using Object = SLObjectItf;
using ObjectTable = struct SLObjectItf_;
using Engine = SLEngineItf;
using EngineTable = struct SLEngineItf_;
using Play = SLPlayItf;
using PlayTable = struct SLPlayItf_;
using Volume = SLVolumeItf;
using VolumeTable = struct SLVolumeItf_;
using BufferQueue = SLBufferQueueItf;
using BufferQueueTable = struct SLBufferQueueItf_;
using AndroidConfiguration = SLAndroidConfigurationItf;
using AndroidConfigurationTable = struct SLAndroidConfigurationItf_;
using ObjectCallback = slObjectCallback;
using PlayCallback = slPlayCallback;
using DataSource = SLDataSource;
using DataSink = SLDataSink;
using DataLocatorAndroidSimpleBufferQueue =
    SLDataLocator_AndroidSimpleBufferQueue;
using DataLocatorBufferQueue = SLDataLocator_BufferQueue;
using DataLocatorOutputMix = SLDataLocator_OutputMix;
using DataFormatPcm = SLDataFormat_PCM;
using EngineOption = SLEngineOption;
using JavaProxyObject = jobject;

inline constexpr Result kResultContentUnsupported =
    SL_RESULT_CONTENT_UNSUPPORTED;
inline constexpr Uint32 kObjectStateUnrealized = SL_OBJECT_STATE_UNREALIZED;
inline constexpr Uint32 kObjectStateRealized = SL_OBJECT_STATE_REALIZED;
inline constexpr Uint32 kObjectStateSuspended = SL_OBJECT_STATE_SUSPENDED;
inline constexpr Uint32 kObjectIdEngine = SL_OBJECTID_ENGINE;
inline constexpr Uint32 kObjectIdAudioPlayer = SL_OBJECTID_AUDIOPLAYER;
inline constexpr Uint32 kObjectIdAudioRecorder = SL_OBJECTID_AUDIORECORDER;
inline constexpr Uint32 kObjectIdOutputMix = SL_OBJECTID_OUTPUTMIX;
inline constexpr Uint32 kPlayStateStopped = SL_PLAYSTATE_STOPPED;
inline constexpr Uint32 kPlayStatePaused = SL_PLAYSTATE_PAUSED;
inline constexpr Uint32 kPlayStatePlaying = SL_PLAYSTATE_PLAYING;
inline constexpr Uint32 kTimeUnknown = SL_TIME_UNKNOWN;
inline constexpr Uint32 kDataLocatorOutputMix = SL_DATALOCATOR_OUTPUTMIX;
inline constexpr Uint32 kDataLocatorBufferQueue = SL_DATALOCATOR_BUFFERQUEUE;
inline constexpr Uint32 kDataLocatorAndroidSimpleBufferQueue =
    SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
inline constexpr Uint32 kDataFormatPcm = SL_DATAFORMAT_PCM;
inline constexpr Uint32 kByteOrderBigEndian = SL_BYTEORDER_BIGENDIAN;
inline constexpr Uint32 kByteOrderLittleEndian = SL_BYTEORDER_LITTLEENDIAN;
inline constexpr Uint32 kPcmSampleFormatFixed8 = SL_PCMSAMPLEFORMAT_FIXED_8;
inline constexpr Uint32 kPcmSampleFormatFixed16 = SL_PCMSAMPLEFORMAT_FIXED_16;
inline constexpr Uint32 kPcmSampleFormatFixed32 = SL_PCMSAMPLEFORMAT_FIXED_32;
inline constexpr Uint32 kSpeakerFrontLeft = SL_SPEAKER_FRONT_LEFT;
inline constexpr Uint32 kSpeakerFrontRight = SL_SPEAKER_FRONT_RIGHT;
inline constexpr Uint32 kSpeakerFrontCenter = SL_SPEAKER_FRONT_CENTER;
inline constexpr Boolean kBooleanFalse = SL_BOOLEAN_FALSE;
inline constexpr Boolean kBooleanTrue = SL_BOOLEAN_TRUE;

#else

using Boolean = std::uint32_t;
using Int16 = std::int16_t;
using Int32 = std::int32_t;
using Char = std::uint8_t;
using Millibel = std::int16_t;
using Permille = std::int16_t;

struct InterfaceIdValue {
  std::uint32_t time_low;
  std::uint16_t time_mid;
  std::uint16_t time_hi_and_version;
  std::uint16_t clock_seq;
  std::uint8_t node[6];
};
using InterfaceId = const InterfaceIdValue*;

struct ObjectTable;
struct EngineTable;
struct PlayTable;
struct VolumeTable;
struct BufferQueueTable;
struct AndroidConfigurationTable;

using Object = const ObjectTable* const*;
using Engine = const EngineTable* const*;
using Play = const PlayTable* const*;
using Volume = const VolumeTable* const*;
using BufferQueue = const BufferQueueTable* const*;
using AndroidConfiguration = const AndroidConfigurationTable* const*;

using ObjectCallback = void (*)(Object caller, const void* context,
                                Uint32 event, Result result, Uint32 parameter,
                                void* interface_pointer);
using PlayCallback = void (*)(Play caller, void* context, Uint32 event);
using BufferQueueCallback = void (*)(BufferQueue caller, void* context);
using JavaProxyObject = void*;

struct DataSource {
  void* locator;
  void* format;
};

struct DataSink {
  void* locator;
  void* format;
};

struct DataLocatorAndroidSimpleBufferQueue {
  Uint32 locator_type;
  Uint32 num_buffers;
};

struct DataLocatorBufferQueue {
  Uint32 locator_type;
  Uint32 num_buffers;
};

struct DataLocatorOutputMix {
  Uint32 locator_type;
  Object output_mix;
};

struct DataFormatPcm {
  Uint32 format_type;
  Uint32 num_channels;
  Uint32 samples_per_second_millihertz;
  Uint32 bits_per_sample;
  Uint32 container_size;
  Uint32 channel_mask;
  Uint32 endianness;
};

struct EngineOption {
  Uint32 feature;
  Uint32 data;
};

struct ObjectTable {
  Result (*Realize)(Object self, Boolean async);
  Result (*Resume)(Object self, Boolean async);
  Result (*GetState)(Object self, Uint32* state);
  Result (*GetInterface)(Object self, InterfaceId iid, void* interface_out);
  Result (*RegisterCallback)(Object self, ObjectCallback callback,
                             void* context);
  void (*AbortAsyncOperation)(Object self);
  void (*Destroy)(Object self);
  Result (*SetPriority)(Object self, Int32 priority, Boolean preemptable);
  Result (*GetPriority)(Object self, Int32* priority, Boolean* preemptable);
  Result (*SetLossOfControlInterfaces)(Object self, Int16 num_interfaces,
                                       InterfaceId* ids, Boolean enabled);
};

struct EngineTable {
  Result (*CreateLedDevice)(Engine self, Object* device, Uint32 device_id,
                            Uint32 num_interfaces, const InterfaceId* ids,
                            const Boolean* required);
  Result (*CreateVibraDevice)(Engine self, Object* device, Uint32 device_id,
                              Uint32 num_interfaces, const InterfaceId* ids,
                              const Boolean* required);
  Result (*CreateAudioPlayer)(Engine self, Object* player, DataSource* source,
                              DataSink* sink, Uint32 num_interfaces,
                              const InterfaceId* ids,
                              const Boolean* required);
  Result (*CreateAudioRecorder)(Engine self, Object* recorder,
                                DataSource* source, DataSink* sink,
                                Uint32 num_interfaces, const InterfaceId* ids,
                                const Boolean* required);
  Result (*CreateMidiPlayer)(Engine self, Object* player,
                             DataSource* midi_source, DataSource* bank_source,
                             DataSink* audio_output, DataSink* vibra,
                             DataSink* led_array, Uint32 num_interfaces,
                             const InterfaceId* ids,
                             const Boolean* required);
  Result (*CreateListener)(Engine self, Object* listener,
                           Uint32 num_interfaces, const InterfaceId* ids,
                           const Boolean* required);
  Result (*Create3dGroup)(Engine self, Object* group, Uint32 num_interfaces,
                          const InterfaceId* ids, const Boolean* required);
  Result (*CreateOutputMix)(Engine self, Object* mix, Uint32 num_interfaces,
                            const InterfaceId* ids,
                            const Boolean* required);
  Result (*CreateMetadataExtractor)(Engine self, Object* extractor,
                                    DataSource* source, Uint32 num_interfaces,
                                    const InterfaceId* ids,
                                    const Boolean* required);
  Result (*CreateExtensionObject)(Engine self, Object* object,
                                  void* parameters, Uint32 object_id,
                                  Uint32 num_interfaces,
                                  const InterfaceId* ids,
                                  const Boolean* required);
  Result (*QueryNumSupportedInterfaces)(Engine self, Uint32 object_id,
                                        Uint32* count);
  Result (*QuerySupportedInterfaces)(Engine self, Uint32 object_id,
                                     Uint32 index, InterfaceId* iid);
  Result (*QueryNumSupportedExtensions)(Engine self, Uint32* count);
  Result (*QuerySupportedExtension)(Engine self, Uint32 index, Char* name,
                                    Int16* name_length);
  Result (*IsExtensionSupported)(Engine self, const Char* name,
                                 Boolean* supported);
};

struct PlayTable {
  Result (*SetPlayState)(Play self, Uint32 state);
  Result (*GetPlayState)(Play self, Uint32* state);
  Result (*GetDuration)(Play self, Uint32* milliseconds);
  Result (*GetPosition)(Play self, Uint32* milliseconds);
  Result (*RegisterCallback)(Play self, PlayCallback callback, void* context);
  Result (*SetCallbackEventsMask)(Play self, Uint32 event_flags);
  Result (*GetCallbackEventsMask)(Play self, Uint32* event_flags);
  Result (*SetMarkerPosition)(Play self, Uint32 milliseconds);
  Result (*ClearMarkerPosition)(Play self);
  Result (*GetMarkerPosition)(Play self, Uint32* milliseconds);
  Result (*SetPositionUpdatePeriod)(Play self, Uint32 milliseconds);
  Result (*GetPositionUpdatePeriod)(Play self, Uint32* milliseconds);
};

struct VolumeTable {
  Result (*SetVolumeLevel)(Volume self, Millibel level);
  Result (*GetVolumeLevel)(Volume self, Millibel* level);
  Result (*GetMaxVolumeLevel)(Volume self, Millibel* level);
  Result (*SetMute)(Volume self, Boolean mute);
  Result (*GetMute)(Volume self, Boolean* mute);
  Result (*EnableStereoPosition)(Volume self, Boolean enable);
  Result (*IsEnabledStereoPosition)(Volume self, Boolean* enabled);
  Result (*SetStereoPosition)(Volume self, Permille position);
  Result (*GetStereoPosition)(Volume self, Permille* position);
};

struct BufferQueueState {
  Uint32 count;
  Uint32 play_index;
};

struct BufferQueueTable {
  Result (*Enqueue)(BufferQueue self, const void* buffer, Uint32 size);
  Result (*Clear)(BufferQueue self);
  Result (*GetState)(BufferQueue self, BufferQueueState* state);
  Result (*RegisterCallback)(BufferQueue self, BufferQueueCallback callback,
                             void* context);
};

struct AndroidConfigurationTable {
  Result (*SetConfiguration)(AndroidConfiguration self, const Char* key,
                             const void* value, Uint32 value_size);
  Result (*GetConfiguration)(AndroidConfiguration self, const Char* key,
                             Uint32* value_size, void* value);
  Result (*AcquireJavaProxy)(AndroidConfiguration self, Uint32 proxy_type,
                             JavaProxyObject* proxy_object);
  Result (*ReleaseJavaProxy)(AndroidConfiguration self, Uint32 proxy_type);
};

inline constexpr Result kResultContentUnsupported = 9;
inline constexpr Uint32 kObjectStateUnrealized = 1;
inline constexpr Uint32 kObjectStateRealized = 2;
inline constexpr Uint32 kObjectStateSuspended = 3;
inline constexpr Uint32 kObjectIdEngine = 0x00001001;
inline constexpr Uint32 kObjectIdAudioPlayer = 0x00001004;
inline constexpr Uint32 kObjectIdAudioRecorder = 0x00001005;
inline constexpr Uint32 kObjectIdOutputMix = 0x00001009;
inline constexpr Uint32 kPlayStateStopped = 1;
inline constexpr Uint32 kPlayStatePaused = 2;
inline constexpr Uint32 kPlayStatePlaying = 3;
inline constexpr Uint32 kTimeUnknown = 0xffffffffU;
inline constexpr Uint32 kDataLocatorOutputMix = 0x00000004;
inline constexpr Uint32 kDataLocatorBufferQueue = 0x00000006;
inline constexpr Uint32 kDataLocatorAndroidSimpleBufferQueue = 0x800007bdU;
inline constexpr Uint32 kDataFormatPcm = 0x00000002;
inline constexpr Uint32 kByteOrderBigEndian = 1;
inline constexpr Uint32 kByteOrderLittleEndian = 2;
inline constexpr Uint32 kPcmSampleFormatFixed8 = 8;
inline constexpr Uint32 kPcmSampleFormatFixed16 = 16;
inline constexpr Uint32 kPcmSampleFormatFixed32 = 32;
inline constexpr Uint32 kSpeakerFrontLeft = 0x00000001;
inline constexpr Uint32 kSpeakerFrontRight = 0x00000002;
inline constexpr Uint32 kSpeakerFrontCenter = 0x00000004;
inline constexpr Boolean kBooleanFalse = 0;
inline constexpr Boolean kBooleanTrue = 1;

#endif

inline const void* DataSourceLocator(const DataSource& source) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return source.pLocator;
#else
  return source.locator;
#endif
}

inline const void* DataSourceFormat(const DataSource& source) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return source.pFormat;
#else
  return source.format;
#endif
}

inline const void* DataSinkLocator(const DataSink& sink) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return sink.pLocator;
#else
  return sink.locator;
#endif
}

inline Uint32 QueueLocatorType(
    const DataLocatorAndroidSimpleBufferQueue& locator) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return locator.locatorType;
#else
  return locator.locator_type;
#endif
}

inline Uint32 QueueLocatorCapacity(
    const DataLocatorAndroidSimpleBufferQueue& locator) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return locator.numBuffers;
#else
  return locator.num_buffers;
#endif
}

inline Uint32 OutputMixLocatorType(const DataLocatorOutputMix& locator) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return locator.locatorType;
#else
  return locator.locator_type;
#endif
}

inline Object OutputMixObject(const DataLocatorOutputMix& locator) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return locator.outputMix;
#else
  return locator.output_mix;
#endif
}

inline Uint32 PcmFormatType(const DataFormatPcm& format) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return format.formatType;
#else
  return format.format_type;
#endif
}

inline Uint32 PcmChannelCount(const DataFormatPcm& format) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return format.numChannels;
#else
  return format.num_channels;
#endif
}

inline Uint32 PcmSampleRateMillihertz(const DataFormatPcm& format) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return format.samplesPerSec;
#else
  return format.samples_per_second_millihertz;
#endif
}

inline Uint32 PcmBitsPerSample(const DataFormatPcm& format) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return format.bitsPerSample;
#else
  return format.bits_per_sample;
#endif
}

inline Uint32 PcmContainerSize(const DataFormatPcm& format) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return format.containerSize;
#else
  return format.container_size;
#endif
}

inline Uint32 PcmChannelMask(const DataFormatPcm& format) {
#if defined(MOCKTAIL_USE_SYSTEM_OPENSL_HEADERS)
  return format.channelMask;
#else
  return format.channel_mask;
#endif
}

inline Uint32 PcmEndianness(const DataFormatPcm& format) {
  return format.endianness;
}

inline constexpr const char kAndroidPlaybackStreamTypeKey[] =
    "androidPlaybackStreamType";
inline constexpr const char kAndroidPerformanceModeKey[] =
    "androidPerformanceMode";
inline constexpr Int32 kAndroidStreamMedia = 3;
inline constexpr Uint32 kAndroidPerformanceNone = 0;

}  // namespace mocktail::audio::opensl_abi

#endif  // MOCKTAIL_AUDIO_OPENSL_RUNTIME_ABI_H_
