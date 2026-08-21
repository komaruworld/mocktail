#include "mocktail/audio/opensl_simple_buffer_queue.h"

#include <pthread.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

namespace mocktail::audio {
namespace {

using opensl_abi::AndroidSimpleBufferQueue;
using opensl_abi::AndroidSimpleBufferQueueCallback;
using opensl_abi::AndroidSimpleBufferQueueState;
using opensl_abi::AndroidSimpleBufferQueueTable;
using opensl_abi::Result;
using opensl_abi::Uint32;

Result ToOpenSlResult(const Status& status) {
  switch (status.code()) {
    case StatusCode::kOk:
      return opensl_abi::kResultSuccess;
    case StatusCode::kInvalidArgument:
      return opensl_abi::kResultParameterInvalid;
    case StatusCode::kFailedPrecondition:
      return opensl_abi::kResultPreconditionsViolated;
    case StatusCode::kUnavailable:
      return opensl_abi::kResultResourceError;
    case StatusCode::kUnsupported:
      return opensl_abi::kResultFeatureUnsupported;
    case StatusCode::kPlatformError:
      return opensl_abi::kResultIoError;
  }
  return opensl_abi::kResultInternalError;
}

}  // namespace

struct OpenSlSimpleBufferQueueAdapter::State
    : public std::enable_shared_from_this<
          OpenSlSimpleBufferQueueAdapter::State> {
  struct Handle {
    const AndroidSimpleBufferQueueTable* table = nullptr;
    State* state = nullptr;
  };

  struct ReleaseTicket {
    std::shared_ptr<State> state;
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
  };

  struct CallbackJob {
    AndroidSimpleBufferQueueCallback callback = nullptr;
    void* context = nullptr;
  };

  explicit State(std::unique_ptr<AudioSink> owned_sink,
                 const OpenSlSimpleBufferQueueOptions& options)
      : sink(std::move(owned_sink)),
        max_buffers(options.max_buffers),
        event_callback(options.event_callback),
        event_context(options.event_context) {}

  static const AndroidSimpleBufferQueueTable kQueueTable;

  AndroidSimpleBufferQueue Interface() {
    return reinterpret_cast<AndroidSimpleBufferQueue>(&handle.table);
  }

  static State* FromInterface(AndroidSimpleBufferQueue self) {
    if (self == nullptr) {
      return nullptr;
    }
    auto* handle = reinterpret_cast<Handle*>(
        const_cast<AndroidSimpleBufferQueueTable**>(self));
    return handle->state;
  }

  static Result MOCKTAIL_OPENSL_API_ENTRY
  AbiEnqueue(AndroidSimpleBufferQueue self, const void* buffer, Uint32 size) {
    State* raw_state = FromInterface(self);
    if (raw_state == nullptr || buffer == nullptr || size == 0) {
      return opensl_abi::kResultParameterInvalid;
    }
    const std::shared_ptr<State> state = raw_state->shared_from_this();

    std::lock_guard<std::mutex> operation_lock(state->sink_operation_mutex);
    std::uint64_t id = 0;
    ReleaseTicket* ticket = nullptr;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->stopping) {
        return opensl_abi::kResultPreconditionsViolated;
      }
      if (state->pending.size() >= state->max_buffers) {
        return opensl_abi::kResultBufferInsufficient;
      }
      id = state->next_id++;
      ticket = new (std::nothrow) ReleaseTicket{state, id, state->generation};
      if (ticket == nullptr) {
        return opensl_abi::kResultMemoryFailure;
      }
      state->pending.emplace(id, ticket);
    }

    const PcmBuffer pcm{buffer, static_cast<std::size_t>(size),
                        &State::ReleaseBuffer, ticket};
    const Status status = state->sink->Enqueue(pcm);
    if (status.ok()) {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->submitted_buffers;
      }
      state->NotifyEvent(OpenSlBufferQueueEvent::kSubmitted,
                         static_cast<std::size_t>(size));
      return opensl_abi::kResultSuccess;
    }

    bool delete_ticket = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->last_status = status;
      const auto found = state->pending.find(id);
      if (found != state->pending.end() && found->second == ticket) {
        state->pending.erase(found);
        delete_ticket = true;
      }
    }
    if (delete_ticket) {
      delete ticket;
    }
    return ToOpenSlResult(status);
  }

  static Result MOCKTAIL_OPENSL_API_ENTRY
  AbiClear(AndroidSimpleBufferQueue self) {
    State* raw_state = FromInterface(self);
    if (raw_state == nullptr) {
      return opensl_abi::kResultParameterInvalid;
    }
    const std::shared_ptr<State> state = raw_state->shared_from_this();
    std::lock_guard<std::mutex> operation_lock(state->sink_operation_mutex);
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->stopping) {
        return opensl_abi::kResultPreconditionsViolated;
      }
      ++state->generation;
    }
    const Status status = state->sink->Clear();
    if (!status.ok()) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->last_status = status;
    }
    return ToOpenSlResult(status);
  }

  static Result MOCKTAIL_OPENSL_API_ENTRY AbiGetState(
      AndroidSimpleBufferQueue self, AndroidSimpleBufferQueueState* output) {
    State* state = FromInterface(self);
    if (state == nullptr || output == nullptr) {
      return opensl_abi::kResultParameterInvalid;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    output->count = static_cast<Uint32>(std::min<std::size_t>(
        state->pending.size(), std::numeric_limits<Uint32>::max()));
    output->index = static_cast<Uint32>(std::min<std::uint64_t>(
        state->processed_buffers, std::numeric_limits<Uint32>::max()));
    return opensl_abi::kResultSuccess;
  }

  static Result MOCKTAIL_OPENSL_API_ENTRY AbiRegisterCallback(
      AndroidSimpleBufferQueue self,
      AndroidSimpleBufferQueueCallback registered_callback, void* context) {
    State* state = FromInterface(self);
    if (state == nullptr) {
      return opensl_abi::kResultParameterInvalid;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopping) {
      return opensl_abi::kResultPreconditionsViolated;
    }
    state->callback = registered_callback;
    state->callback_context = context;
    return opensl_abi::kResultSuccess;
  }

  static void ReleaseBuffer(void* context, const void* /*data*/,
                            std::size_t size_bytes) {
    auto* ticket = static_cast<ReleaseTicket*>(context);
    if (ticket == nullptr) {
      return;
    }
    const std::shared_ptr<State> state = ticket->state;
    bool delete_ticket = false;
    bool notify_worker = false;
    bool consumed = false;
    bool discarded = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      const auto found = state->pending.find(ticket->id);
      if (found != state->pending.end() && found->second == ticket) {
        state->pending.erase(found);
        delete_ticket = true;
        if (!state->stopping && ticket->generation == state->generation) {
          ++state->processed_buffers;
          ++state->consumed_buffers;
          state->consumed_bytes += size_bytes;
          consumed = true;
          if (state->callback != nullptr) {
            state->callback_jobs.push_back(
                CallbackJob{state->callback, state->callback_context});
            notify_worker = true;
          }
        } else {
          ++state->discarded_buffers;
          discarded = true;
        }
      }
    }
    if (notify_worker) {
      state->callback_cv.notify_one();
    }
    if (delete_ticket) {
      delete ticket;
    }
    if (consumed) {
      state->NotifyEvent(OpenSlBufferQueueEvent::kConsumed, size_bytes);
    } else if (discarded) {
      state->NotifyEvent(OpenSlBufferQueueEvent::kDiscarded, size_bytes);
    }
  }

  void NotifyEvent(OpenSlBufferQueueEvent event, std::size_t size_bytes) {
    if (event_callback != nullptr) {
      event_callback(event_context, event, size_bytes);
    }
  }

  void RunCallbacks() {
    for (;;) {
      CallbackJob job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        callback_cv.wait(lock,
                         [this] { return stopping || !callback_jobs.empty(); });
        if (stopping) {
          callback_jobs.clear();
          return;
        }
        job = callback_jobs.front();
        callback_jobs.pop_front();
      }
      if (job.callback != nullptr) {
        job.callback(Interface(), job.context);
      }
    }
  }

  static void* CallbackThreadMain(void* context) {
    auto* state_holder = static_cast<std::shared_ptr<State>*>(context);
    const std::shared_ptr<State> state = std::move(*state_holder);
    delete state_holder;
    state->RunCallbacks();
    return nullptr;
  }

  Status StartCallbackThread() {
    auto* state_holder =
        new (std::nothrow) std::shared_ptr<State>(shared_from_this());
    if (state_holder == nullptr) {
      return Status::Error(StatusCode::kUnavailable,
                           "unable to allocate OpenSL callback thread state");
    }
    const int result = pthread_create(&callback_thread, nullptr,
                                      &State::CallbackThreadMain, state_holder);
    if (result != 0) {
      delete state_holder;
      return Status::Error(
          StatusCode::kPlatformError,
          std::string("unable to create OpenSL callback thread: ") +
              std::strerror(result));
    }
    callback_thread_started = true;
    return Status::Ok();
  }

  bool IsCallbackThreadLocked() const {
    return callback_thread_started &&
           pthread_equal(callback_thread, pthread_self()) != 0;
  }

  void JoinCallbackThread() {
    pthread_t thread{};
    bool is_current_thread = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!callback_thread_started) {
        return;
      }
      thread = callback_thread;
      is_current_thread = pthread_equal(thread, pthread_self()) != 0;
    }
    const int result = is_current_thread ? pthread_detach(thread)
                                         : pthread_join(thread, nullptr);
    {
      std::lock_guard<std::mutex> lock(mutex);
      callback_thread_started = false;
      if (result != 0) {
        last_status = Status::Error(
            StatusCode::kPlatformError,
            std::string(is_current_thread
                            ? "unable to detach OpenSL callback thread: "
                            : "unable to join OpenSL callback thread: ") +
                std::strerror(result));
      }
    }
  }

  mutable std::mutex mutex;
  std::condition_variable callback_cv;
  std::condition_variable shutdown_cv;
  std::mutex sink_operation_mutex;
  std::unique_ptr<AudioSink> sink;
  const std::uint32_t max_buffers;
  const OpenSlBufferQueueEventCallback event_callback;
  void* const event_context;
  Handle handle;
  pthread_t callback_thread{};
  std::unordered_map<std::uint64_t, ReleaseTicket*> pending;
  std::deque<CallbackJob> callback_jobs;
  AndroidSimpleBufferQueueCallback callback = nullptr;
  void* callback_context = nullptr;
  Status last_status;
  std::uint64_t next_id = 1;
  std::uint64_t generation = 1;
  std::uint64_t processed_buffers = 0;
  std::uint64_t submitted_buffers = 0;
  std::uint64_t consumed_buffers = 0;
  std::uint64_t discarded_buffers = 0;
  std::uint64_t consumed_bytes = 0;
  bool stopping = false;
  bool callback_thread_started = false;
  bool shutdown_started = false;
  bool shutdown_complete = false;
};

const AndroidSimpleBufferQueueTable
    OpenSlSimpleBufferQueueAdapter::State::kQueueTable = {
        &OpenSlSimpleBufferQueueAdapter::State::AbiEnqueue,
        &OpenSlSimpleBufferQueueAdapter::State::AbiClear,
        &OpenSlSimpleBufferQueueAdapter::State::AbiGetState,
        &OpenSlSimpleBufferQueueAdapter::State::AbiRegisterCallback,
};

OpenSlSimpleBufferQueueAdapter::OpenSlSimpleBufferQueueAdapter(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}

OpenSlSimpleBufferQueueAdapter::~OpenSlSimpleBufferQueueAdapter() {
  Shutdown();
}

Status OpenSlSimpleBufferQueueAdapter::Create(
    std::unique_ptr<AudioSink> sink,
    const OpenSlSimpleBufferQueueOptions& options,
    std::unique_ptr<OpenSlSimpleBufferQueueAdapter>* adapter) {
  if (adapter == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "OpenSL adapter output pointer is null");
  }
  adapter->reset();
  if (sink == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "OpenSL adapter requires an audio sink");
  }
  if (options.max_buffers == 0) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "OpenSL queue capacity must be non-zero");
  }

  auto state = std::shared_ptr<State>(
      new (std::nothrow) State(std::move(sink), options));
  if (state == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "unable to allocate OpenSL queue state");
  }
  state->handle.table = &State::kQueueTable;
  state->handle.state = state.get();
  Status status = state->StartCallbackThread();
  if (!status.ok()) {
    state->sink->Shutdown();
    return status;
  }

  auto* implementation =
      new (std::nothrow) OpenSlSimpleBufferQueueAdapter(state);
  if (implementation == nullptr) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->stopping = true;
    }
    state->callback_cv.notify_all();
    state->JoinCallbackThread();
    state->sink->Shutdown();
    return Status::Error(StatusCode::kUnavailable,
                         "unable to allocate OpenSL queue adapter");
  }
  adapter->reset(implementation);
  return Status::Ok();
}

AndroidSimpleBufferQueue OpenSlSimpleBufferQueueAdapter::interface() const {
  return state_ != nullptr ? state_->Interface() : nullptr;
}

Status OpenSlSimpleBufferQueueAdapter::SetPlaying(bool playing) {
  const std::shared_ptr<State> state = state_;
  if (state == nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "OpenSL adapter has no state");
  }
  std::lock_guard<std::mutex> operation_lock(state->sink_operation_mutex);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopping) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "OpenSL adapter is shut down");
    }
  }
  const Status status = playing ? state->sink->Resume() : state->sink->Pause();
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->last_status = status;
  }
  return status;
}

Status OpenSlSimpleBufferQueueAdapter::SetGain(float linear_gain) {
  const std::shared_ptr<State> state = state_;
  if (state == nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "OpenSL adapter has no state");
  }
  std::lock_guard<std::mutex> operation_lock(state->sink_operation_mutex);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopping) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "OpenSL adapter is shut down");
    }
  }
  const Status status = state->sink->SetGain(linear_gain);
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->last_status = status;
  }
  return status;
}

OpenSlSimpleBufferQueueStats OpenSlSimpleBufferQueueAdapter::GetStats() const {
  const std::shared_ptr<State> state = state_;
  if (state == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  return OpenSlSimpleBufferQueueStats{
      state->submitted_buffers, state->consumed_buffers,
      state->discarded_buffers, state->consumed_bytes, state->pending.size()};
}

Status OpenSlSimpleBufferQueueAdapter::last_error() const {
  const std::shared_ptr<State> state = state_;
  if (state == nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "OpenSL adapter has no state");
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->last_status;
}

void OpenSlSimpleBufferQueueAdapter::Shutdown() {
  const std::shared_ptr<State> state = state_;
  if (state == nullptr) {
    return;
  }

  {
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->shutdown_started) {
      if (state->IsCallbackThreadLocked()) {
        return;
      }
      state->shutdown_cv.wait(lock,
                              [&state] { return state->shutdown_complete; });
      return;
    }
    state->shutdown_started = true;
    state->stopping = true;
    ++state->generation;
    state->callback = nullptr;
    state->callback_context = nullptr;
    state->callback_jobs.clear();
  }
  state->callback_cv.notify_all();

  {
    std::lock_guard<std::mutex> operation_lock(state->sink_operation_mutex);
    state->sink->Shutdown();
  }
  state->JoinCallbackThread();

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->pending.empty()) {
      state->last_status = Status::Error(
          StatusCode::kPlatformError,
          "audio sink did not release all borrowed OpenSL buffers");
    }
    state->shutdown_complete = true;
  }
  state->shutdown_cv.notify_all();
}

Status OpenSlRecorderSupportStatus() {
  return Status::Error(StatusCode::kUnsupported,
                       "OpenSL audio recording is not implemented");
}

}  // namespace mocktail::audio
