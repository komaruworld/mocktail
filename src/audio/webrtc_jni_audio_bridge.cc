#include "mocktail/audio/webrtc_jni_audio_bridge.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jnivm/jnivm.h"
#include "mocktail/audio/sdl_audio_capture.h"
#include "mocktail/audio/sdl_audio_sink.h"

namespace mocktail::audio {
namespace {

struct CaptureSession {
  const void* identity = nullptr;
  jnivm::WebRtcAudioRecordDataCallback data_callback = nullptr;
  void* data_context = nullptr;
  std::unique_ptr<AudioCapture> capture;
  std::shared_ptr<std::atomic<bool>> microphone_muted;
};

struct PlayoutSession {
  const void* identity = nullptr;
  jnivm::WebRtcAudioTrackDataCallback data_callback = nullptr;
  void* data_context = nullptr;
  std::unique_ptr<AudioSink> sink;
  std::vector<std::uint8_t> buffer;
  std::size_t frames_per_buffer = 0;
  int buffer_size_frames = 0;
  std::mutex operation_mutex;
  std::mutex mutex;
  bool running = false;
  bool stop_requested = false;
  bool in_data_callback = false;
  std::thread::id data_callback_thread;
};

struct WebRtcJniAudioBridgeContext {
  std::mutex mutex;
  std::unordered_set<const void*> initialized_managers;
  std::shared_ptr<std::atomic<bool>> microphone_muted =
      std::make_shared<std::atomic<bool>>(false);
  std::unordered_map<const void*, std::shared_ptr<CaptureSession>>
      capture_sessions;
  std::unordered_map<const void*, std::shared_ptr<PlayoutSession>>
      playout_sessions;
  bool recording_enabled = true;
  bool shutting_down = false;
};

bool GetManagerParameters(void* opaque_context,
                          jnivm::WebRtcAudioManagerParameters* parameters) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr || parameters == nullptr) return false;
  std::lock_guard<std::mutex> lock(context->mutex);
  if (context->shutting_down) return false;
  // SDL converts between this WebRTC-facing format and the actual device.
  // Match WebRtcAudioManager's default mono input/output and 10 ms frames.
  // Advertising the format never opens a capture device, including when the
  // microphone is disabled. Recording init remains the permission boundary.
  *parameters = {};
  parameters->sample_rate_hz = 48000;
  parameters->output_channels = 1;
  parameters->input_channels = 1;
  parameters->output_buffer_size_frames = 480;
  parameters->input_buffer_size_frames = 480;
  // Android hardware effects, low-latency paths and AAudio are not emulated;
  // WebRTC must retain its software processing and Java audio transport.
  return true;
}

bool InitManager(void* opaque_context, const void* identity) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr || identity == nullptr) return false;
  bool inserted = false;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->shutting_down) return false;
    inserted = context->initialized_managers.insert(identity).second;
  }
  if (inserted) {
    std::cout << "  [mocktail][audio] WebRTC audio manager initialized\n"
              << std::flush;
  }
  return true;
}

void DisposeManager(void* opaque_context, const void* identity) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr) return;
  std::lock_guard<std::mutex> lock(context->mutex);
  if (context->initialized_managers.erase(identity) != 0) {
    std::cout << "  [mocktail][audio] WebRTC audio manager disposed\n"
              << std::flush;
  }
  // Record/track objects own their streams and stop them independently.
}

void SetMicrophoneMute(void* opaque_context, bool muted) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr) return;
  context->microphone_muted->store(muted, std::memory_order_release);
}

void OnCapturedAudio(void* opaque_session, std::size_t size_bytes) {
  auto* session = static_cast<CaptureSession*>(opaque_session);
  if (session != nullptr && session->data_callback != nullptr) {
    if (session->microphone_muted != nullptr &&
        session->microphone_muted->load(std::memory_order_acquire)) {
      if (session->capture == nullptr ||
          size_bytes > session->capture->buffer_size_bytes()) {
        return;
      }
      std::memset(session->capture->buffer_data(), 0, size_bytes);
    }
    session->data_callback(session->data_context, session->identity,
                           size_bytes);
  }
}

void CloseCaptureSession(const std::shared_ptr<CaptureSession>& session) {
  if (session == nullptr || session->capture == nullptr) {
    return;
  }
  (void)session->capture->Stop();
  session->capture->Shutdown();
}

int Init(void* opaque_context, const void* identity, int sample_rate_hz,
         int channels, jnivm::WebRtcAudioRecordDataCallback data_callback,
         void* data_context, void** direct_buffer,
         std::size_t* direct_buffer_capacity) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr || identity == nullptr || data_callback == nullptr ||
      direct_buffer == nullptr || direct_buffer_capacity == nullptr) {
    return -1;
  }
  *direct_buffer = nullptr;
  *direct_buffer_capacity = 0;

  std::shared_ptr<CaptureSession> previous;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    if (!context->recording_enabled || context->shutting_down) {
      return -1;
    }
    const auto found = context->capture_sessions.find(identity);
    if (found != context->capture_sessions.end()) {
      previous = std::move(found->second);
      context->capture_sessions.erase(found);
    }
  }
  CloseCaptureSession(previous);

  auto session = std::shared_ptr<CaptureSession>(
      new (std::nothrow) CaptureSession());
  if (session == nullptr) {
    return -1;
  }
  session->identity = identity;
  session->data_callback = data_callback;
  session->data_context = data_context;
  session->microphone_muted = context->microphone_muted;

  SdlAudioCaptureOptions options;
  options.output_spec.sample_rate_hz = sample_rate_hz;
  options.output_spec.channels =
      channels > 0 && channels <= 255 ? static_cast<std::uint8_t>(channels) : 0;
  options.output_spec.format = PcmSampleFormat::kSigned16LittleEndian;
  options.frame_duration_ms = 10;
  options.data_callback = &OnCapturedAudio;
  options.data_context = session.get();
  const Status status = CreateSdlAudioCapture(options, &session->capture);
  if (!status.ok()) {
    std::cerr << "  [mocktail][audio] WebRTC microphone init_failed reason="
              << status.message() << '\n';
    return -1;
  }
  if (session->capture->frames_per_buffer() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    session->capture->Shutdown();
    return -1;
  }
  *direct_buffer = session->capture->buffer_data();
  *direct_buffer_capacity = session->capture->buffer_size_bytes();
  const int frames =
      static_cast<int>(session->capture->frames_per_buffer());
  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->recording_enabled && !context->shutting_down) {
      context->capture_sessions[identity] = session;
      accepted = true;
    }
  }
  if (!accepted) {
    *direct_buffer = nullptr;
    *direct_buffer_capacity = 0;
    CloseCaptureSession(session);
    return -1;
  }
  std::cout << "  [mocktail][audio] WebRTC microphone initialized "
            << "sample_rate_hz=" << sample_rate_hz
            << " channels=" << channels << " frames_per_buffer=" << frames
            << '\n'
            << std::flush;
  return frames;
}

bool Start(void* opaque_context, const void* identity) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr || identity == nullptr) {
    return false;
  }
  std::shared_ptr<CaptureSession> session;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    if (!context->recording_enabled || context->shutting_down) {
      return false;
    }
    const auto found = context->capture_sessions.find(identity);
    if (found != context->capture_sessions.end()) {
      session = found->second;
    }
  }
  if (session == nullptr || session->capture == nullptr) {
    return false;
  }
  const Status status = session->capture->Start();
  if (!status.ok()) {
    std::cerr << "  [mocktail][audio] WebRTC microphone start_failed reason="
              << status.message() << '\n';
    return false;
  }
  std::cout << "  [mocktail][audio] WebRTC microphone recording\n"
            << std::flush;
  return true;
}

bool Stop(void* opaque_context, const void* identity) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr || identity == nullptr) {
    return false;
  }
  std::shared_ptr<CaptureSession> session;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    const auto found = context->capture_sessions.find(identity);
    if (found == context->capture_sessions.end()) {
      return true;
    }
    session = std::move(found->second);
    context->capture_sessions.erase(found);
  }
  CloseCaptureSession(session);
  std::cout << "  [mocktail][audio] WebRTC microphone stopped\n"
            << std::flush;
  return true;
}

void Close(void* opaque_context, const void* identity) {
  (void)Stop(opaque_context, identity);
}

bool OnPlayoutDataNeeded(void* opaque_session, const void** data,
                         std::size_t* size_bytes) {
  auto* session = static_cast<PlayoutSession*>(opaque_session);
  if (data == nullptr || size_bytes == nullptr) {
    return false;
  }
  *data = nullptr;
  *size_bytes = 0;
  if (session == nullptr || session->buffer.empty()) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->running || session->stop_requested) {
      return false;
    }
    session->in_data_callback = true;
    session->data_callback_thread = std::this_thread::get_id();
  }

  session->data_callback(session->data_context, session->identity,
                         session->buffer.size());

  bool ready = false;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    ready = session->running && !session->stop_requested;
    session->in_data_callback = false;
    session->data_callback_thread = {};
  }
  if (ready) {
    *data = session->buffer.data();
    *size_bytes = session->buffer.size();
  }
  return ready;
}

void ClosePlayoutSession(const std::shared_ptr<PlayoutSession>& session) {
  if (session == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> operation_lock(session->operation_mutex);
  bool called_from_callback = false;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->running = false;
    session->stop_requested = true;
    called_from_callback =
        session->in_data_callback &&
        session->data_callback_thread == std::this_thread::get_id();
  }
  // SDL does not permit destroying a stream from its own get callback. The
  // session stays in the context and is closed by re-init or bridge shutdown.
  if (called_from_callback || session->sink == nullptr) {
    return;
  }
  (void)session->sink->Pause();
  (void)session->sink->Clear();
  session->sink->Shutdown();
  session->sink.reset();
}

int InitPlayout(void* opaque_context, const void* identity,
                int sample_rate_hz, int channels, double buffer_size_factor,
                jnivm::WebRtcAudioTrackDataCallback data_callback,
                void* data_context, void** direct_buffer,
                std::size_t* direct_buffer_capacity) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr || identity == nullptr || data_callback == nullptr ||
      direct_buffer == nullptr || direct_buffer_capacity == nullptr) {
    return -1;
  }
  *direct_buffer = nullptr;
  *direct_buffer_capacity = 0;
  if (!std::isfinite(buffer_size_factor) || buffer_size_factor <= 0.0 ||
      buffer_size_factor > 16.0 || (channels != 1 && channels != 2) ||
      sample_rate_hz <= 0 || sample_rate_hz % 100 != 0) {
    return -1;
  }

  const PcmSpec spec{sample_rate_hz, static_cast<std::uint8_t>(channels),
                     PcmSampleFormat::kSigned16LittleEndian};
  Status status = ValidatePcmSpec(spec);
  if (!status.ok()) {
    return -1;
  }
  const std::size_t frames_per_buffer =
      static_cast<std::size_t>(sample_rate_hz / 100);
  const std::size_t bytes_per_frame = BytesPerFrame(spec);
  if (frames_per_buffer >
      static_cast<std::size_t>(std::numeric_limits<int>::max()) /
          bytes_per_frame) {
    return -1;
  }
  const std::size_t callback_bytes = frames_per_buffer * bytes_per_frame;
  const double scaled_frames =
      std::ceil(static_cast<double>(frames_per_buffer) * buffer_size_factor);
  if (scaled_frames > static_cast<double>(std::numeric_limits<int>::max())) {
    return -1;
  }
  const std::size_t buffer_size_frames = std::max(
      frames_per_buffer, static_cast<std::size_t>(scaled_frames));
  if (buffer_size_frames >
      static_cast<std::size_t>(std::numeric_limits<int>::max()) /
          bytes_per_frame) {
    return -1;
  }
  const std::size_t buffer_size_bytes = buffer_size_frames * bytes_per_frame;

  std::shared_ptr<PlayoutSession> previous;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->shutting_down) {
      return -1;
    }
    const auto found = context->playout_sessions.find(identity);
    if (found != context->playout_sessions.end()) {
      previous = std::move(found->second);
      context->playout_sessions.erase(found);
    }
  }
  ClosePlayoutSession(previous);

  auto session = std::shared_ptr<PlayoutSession>(
      new (std::nothrow) PlayoutSession());
  if (session == nullptr) {
    return -1;
  }
  session->identity = identity;
  session->data_callback = data_callback;
  session->data_context = data_context;
  session->frames_per_buffer = frames_per_buffer;
  session->buffer_size_frames = static_cast<int>(buffer_size_frames);
  session->buffer.resize(callback_bytes);

  SdlAudioSinkOptions options;
  options.source_spec = spec;
  options.start_paused = true;
  options.data_needed_callback = &OnPlayoutDataNeeded;
  options.data_needed_context = session.get();
  status = CreateSdlAudioSink(options, &session->sink);
  if (!status.ok()) {
    std::cerr << "  [mocktail][audio] WebRTC playout init_failed reason="
              << status.message() << '\n';
    return -1;
  }

  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    if (!context->shutting_down) {
      context->playout_sessions[identity] = session;
      accepted = true;
    }
  }
  if (!accepted) {
    ClosePlayoutSession(session);
    return -1;
  }

  *direct_buffer = session->buffer.data();
  *direct_buffer_capacity = session->buffer.size();
  std::cout << "  [mocktail][audio] WebRTC playout initialized "
            << "sample_rate_hz=" << sample_rate_hz
            << " channels=" << channels
            << " frames_per_buffer=" << frames_per_buffer << '\n'
            << std::flush;
  return static_cast<int>(buffer_size_bytes);
}

int PlayoutBufferSizeFrames(void* opaque_context, const void* identity) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr || identity == nullptr) {
    return 0;
  }
  std::shared_ptr<PlayoutSession> session;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    const auto found = context->playout_sessions.find(identity);
    if (found != context->playout_sessions.end()) {
      session = found->second;
    }
  }
  return session != nullptr ? session->buffer_size_frames : 0;
}

bool StartPlayout(void* opaque_context, const void* identity) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr || identity == nullptr) {
    return false;
  }
  std::shared_ptr<PlayoutSession> session;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->shutting_down) {
      return false;
    }
    const auto found = context->playout_sessions.find(identity);
    if (found != context->playout_sessions.end()) {
      session = found->second;
    }
  }
  if (session == nullptr || session->sink == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> operation_lock(session->operation_mutex);
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->running) {
      return true;
    }
    if (session->stop_requested) {
      return false;
    }
    session->running = true;
  }
  const Status status = session->sink->Resume();
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->running = false;
    std::cerr << "  [mocktail][audio] WebRTC playout start_failed reason="
              << status.message() << '\n';
    return false;
  }
  std::cout << "  [mocktail][audio] WebRTC playout playing\n" << std::flush;
  return true;
}

bool StopPlayout(void* opaque_context, const void* identity) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr || identity == nullptr) {
    return false;
  }
  std::shared_ptr<PlayoutSession> session;
  bool called_from_callback = false;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    const auto found = context->playout_sessions.find(identity);
    if (found == context->playout_sessions.end()) {
      return true;
    }
    session = found->second;
    {
      std::lock_guard<std::mutex> session_lock(session->mutex);
      called_from_callback =
          session->in_data_callback &&
          session->data_callback_thread == std::this_thread::get_id();
    }
    if (!called_from_callback) {
      context->playout_sessions.erase(found);
    }
  }
  ClosePlayoutSession(session);
  std::cout << "  [mocktail][audio] WebRTC playout stopped\n" << std::flush;
  return true;
}

void ClosePlayout(void* opaque_context, const void* identity) {
  (void)StopPlayout(opaque_context, identity);
}

void Shutdown(void* opaque_context) {
  auto* context = static_cast<WebRtcJniAudioBridgeContext*>(opaque_context);
  if (context == nullptr) {
    return;
  }
  std::unordered_map<const void*, std::shared_ptr<CaptureSession>>
      capture_sessions;
  std::unordered_map<const void*, std::shared_ptr<PlayoutSession>>
      playout_sessions;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    context->recording_enabled = false;
    context->shutting_down = true;
    context->initialized_managers.clear();
    capture_sessions.swap(context->capture_sessions);
    playout_sessions.swap(context->playout_sessions);
  }
  for (auto& [identity, session] : playout_sessions) {
    (void)identity;
    ClosePlayoutSession(session);
  }
  for (auto& [identity, session] : capture_sessions) {
    (void)identity;
    CloseCaptureSession(session);
  }
}

}  // namespace

Status InstallWebRtcJniAudioBridge(jnivm::VM* vm) {
  if (vm == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "WebRTC JNI audio bridge requires a VM");
  }
  Status status = InitializeSdlAudioSubsystem();
  if (!status.ok()) {
    return status;
  }

  const char* configured = std::getenv("MOCKTAIL_AUDIO_INPUT_DEVICE");
  const std::string requested =
      configured != nullptr && configured[0] != '\0' ? configured : "default";
  auto context = std::shared_ptr<WebRtcJniAudioBridgeContext>(
      new (std::nothrow) WebRtcJniAudioBridgeContext());
  if (context == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "unable to allocate WebRTC JNI audio bridge");
  }

  if (requested == "disabled") {
    context->recording_enabled = false;
    std::cout << "  [mocktail][audio] microphone capture disabled\n"
              << std::flush;
  } else {
    std::vector<SdlRecordingDevice> devices;
    std::string resolved_name;
    status = ConfigureSdlRecordingDevice(requested, &devices, &resolved_name);
    if (!status.ok()) {
      return status;
    }
    std::cout << "  [mocktail][audio] SDL recording devices:\n"
              << "    - default (follows the host system default)\n";
    for (const SdlRecordingDevice& device : devices) {
      std::cout << "    - id:" << device.id << " " << device.name << '\n';
    }
    std::cout << "  [mocktail][audio] selected input=" << resolved_name
              << '\n'
              << std::flush;
  }

  jnivm::WebRtcAudioRecordCallbacks record_callbacks;
  record_callbacks.init = &Init;
  record_callbacks.start = &Start;
  record_callbacks.stop = &Stop;
  record_callbacks.close = &Close;
  record_callbacks.shutdown = &Shutdown;
  vm->SetWebRtcAudioRecordCallbacks(context, record_callbacks);

  jnivm::WebRtcAudioManagerCallbacks manager_callbacks;
  manager_callbacks.get_parameters = &GetManagerParameters;
  manager_callbacks.init = &InitManager;
  manager_callbacks.dispose = &DisposeManager;
  manager_callbacks.set_microphone_mute = &SetMicrophoneMute;
  vm->SetWebRtcAudioManagerCallbacks(context, manager_callbacks);

  jnivm::WebRtcAudioTrackCallbacks track_callbacks;
  track_callbacks.init = &InitPlayout;
  track_callbacks.buffer_size_frames = &PlayoutBufferSizeFrames;
  track_callbacks.start = &StartPlayout;
  track_callbacks.stop = &StopPlayout;
  track_callbacks.close = &ClosePlayout;
  vm->SetWebRtcAudioTrackCallbacks(std::move(context), track_callbacks);
  return Status::Ok();
}

Status ShutdownWebRtcJniAudioBridge(jnivm::VM* vm) {
  if (vm == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "WebRTC JNI audio bridge requires a VM");
  }
  // Drop dispatch first, then let the recording binding's shared context stop
  // both playout workers and capture streams.
  vm->ClearWebRtcAudioManagerCallbacks();
  vm->ClearWebRtcAudioTrackCallbacks();
  vm->ClearWebRtcAudioRecordCallbacks();
  return Status::Ok();
}

}  // namespace mocktail::audio
