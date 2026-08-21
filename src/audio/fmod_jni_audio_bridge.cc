#include "mocktail/audio/fmod_jni_audio_bridge.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "jnivm/jnivm.h"
#include "mocktail/audio/fmod_java_audio_runtime.h"
#include "mocktail/audio/sdl_audio_sink.h"

namespace mocktail::audio {
namespace {

struct FmodJniAudioBridgeContext {
  FmodJavaAudioRuntime runtime;
  std::atomic<bool> logged_first_submission{false};
  std::atomic<std::uint64_t> rejected_writes{0};
  std::mutex log_mutex;
};

void LogRuntimeStats(const char* event, FmodJniAudioBridgeContext* context) {
  const FmodJavaAudioRuntimeStats stats = context->runtime.GetStats();
  std::lock_guard<std::mutex> lock(context->log_mutex);
  std::cout << "  [mocktail][audio] fmod_java " << event
            << " submitted_buffers=" << stats.submitted_buffers
            << " submitted_bytes=" << stats.submitted_bytes
            << " consumed_buffers=" << stats.consumed_buffers
            << " consumed_bytes=" << stats.consumed_bytes
            << " discarded_buffers=" << stats.discarded_buffers
            << " discarded_bytes=" << stats.discarded_bytes
            << " pending_buffers=" << stats.pending_buffers << '\n'
            << std::flush;
}

bool Init(void* opaque_context, const void* identity, int channels,
          int sample_rate_hz, int block_size_frames, int block_count) {
  auto* context = static_cast<FmodJniAudioBridgeContext*>(opaque_context);
  const Status status = context->runtime.Init(
      identity, channels, sample_rate_hz, block_size_frames, block_count);
  std::lock_guard<std::mutex> lock(context->log_mutex);
  if (!status.ok()) {
    std::cerr << "  [mocktail][audio] fmod_java init_failed reason="
              << status.message() << '\n';
    return false;
  }
  std::cout << "  [mocktail][audio] fmod_java initialized channels="
            << channels << " sample_rate_hz=" << sample_rate_hz
            << " block_size_frames=" << block_size_frames
            << " block_count=" << block_count << '\n'
            << std::flush;
  return true;
}

bool Write(void* opaque_context, const void* identity,
           const std::uint8_t* data, std::size_t size) {
  auto* context = static_cast<FmodJniAudioBridgeContext*>(opaque_context);
  const Status status = context->runtime.Write(identity, data, size);
  if (!status.ok()) {
    const std::uint64_t rejected =
        context->rejected_writes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rejected <= 3 || (rejected & (rejected - 1)) == 0) {
      std::lock_guard<std::mutex> lock(context->log_mutex);
      std::cerr << "  [mocktail][audio] fmod_java write_failed count="
                << rejected << " reason=" << status.message() << '\n';
    }
    return false;
  }
  if (!context->logged_first_submission.exchange(true,
                                                  std::memory_order_relaxed)) {
    LogRuntimeStats("first_submission", context);
  }
  return true;
}

bool Close(void* opaque_context, const void* identity) {
  auto* context = static_cast<FmodJniAudioBridgeContext*>(opaque_context);
  const Status status = context->runtime.Close(identity);
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(context->log_mutex);
    std::cerr << "  [mocktail][audio] fmod_java close_failed reason="
              << status.message() << '\n';
    return false;
  }
  LogRuntimeStats("closed", context);
  return true;
}

void Shutdown(void* opaque_context) {
  auto* context = static_cast<FmodJniAudioBridgeContext*>(opaque_context);
  context->runtime.Shutdown();
  LogRuntimeStats("shutdown", context);
}

}  // namespace

Status InstallFmodJniAudioBridge(jnivm::VM* vm) {
  if (vm == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "FMOD JNI audio bridge requires a VM");
  }
  Status status = InitializeSdlAudioSubsystem();
  if (!status.ok()) {
    return status;
  }
  const char* configured = std::getenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE");
  const std::string requested =
      configured != nullptr && configured[0] != '\0' ? configured : "default";
  std::vector<SdlPlaybackDevice> available_devices;
  std::string resolved_name;
  status =
      ConfigureSdlPlaybackDevice(requested, &available_devices, &resolved_name);
  if (!status.ok()) {
    (void)ShutdownSdlAudioSubsystem();
    return status;
  }
  std::cout << "  [mocktail][audio] SDL playback devices:\n"
            << "    - default (follows the host system default)\n";
  for (const SdlPlaybackDevice& device : available_devices) {
    std::cout << "    - " << device.name << '\n';
  }
  std::cout << "  [mocktail][audio] selected output=" << resolved_name << '\n'
            << std::flush;
  auto context = std::shared_ptr<FmodJniAudioBridgeContext>(
      new (std::nothrow) FmodJniAudioBridgeContext());
  if (context == nullptr) {
    (void)ShutdownSdlAudioSubsystem();
    return Status::Error(StatusCode::kUnavailable,
                         "unable to allocate FMOD JNI audio bridge");
  }

  jnivm::FmodAudioDeviceCallbacks callbacks;
  callbacks.init = &Init;
  callbacks.write = &Write;
  callbacks.close = &Close;
  callbacks.shutdown = &Shutdown;
  vm->SetFmodAudioDeviceCallbacks(std::move(context), callbacks);
  return Status::Ok();
}

Status ShutdownFmodJniAudioBridge(jnivm::VM* vm) {
  if (vm == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "FMOD JNI audio bridge requires a VM");
  }
  // Clear first: the retained bridge context shuts down all sinks and waits
  // for borrowed SDL buffers before the main thread releases SDL_INIT_AUDIO.
  vm->ClearFmodAudioDeviceCallbacks();
  return ShutdownSdlAudioSubsystem();
}

}  // namespace mocktail::audio
