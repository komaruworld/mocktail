#include "compat/bionic_pthread_key_runtime.h"

#include <errno.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace mocktail::compat {
namespace {

constexpr uint64_t kSequenceInUseBit = 1;
constexpr int kBionicDestructorIterations = 4;

struct KeySlot {
  std::atomic<uint64_t> sequence{0};
  std::atomic<BionicPthreadKeyDestructor> destructor{nullptr};
};

static_assert(std::atomic<BionicPthreadKeyDestructor>::is_always_lock_free,
              "Bionic pthread key destructor metadata must stay lock-free");

struct KeyValue {
  uint64_t sequence = 0;
  void* value = nullptr;
};

class RuntimeState;

struct ThreadKeyData {
  std::array<KeyValue, kBionicPthreadKeyCount> values{};
};

struct ThreadKeyCleanup {
  ~ThreadKeyCleanup();
};

class RuntimeState {
 public:
  RuntimeState() {
    // Bionic reserves two keys for libc and jemalloc before application code
    // can allocate a key. Keep the externally visible allocation range and
    // capacity identical without routing those internal slots to the host.
    slots_[0].sequence.store(kSequenceInUseBit, std::memory_order_relaxed);
    slots_[1].sequence.store(kSequenceInUseBit, std::memory_order_relaxed);
  }

  int Create(pthread_key_t* key,
             BionicPthreadKeyDestructor destructor) noexcept {
    if (key == nullptr) {
      return EINVAL;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t index = 0; index < slots_.size(); ++index) {
      KeySlot& slot = slots_[index];
      const uint64_t sequence =
          slot.sequence.load(std::memory_order_relaxed);
      if (IsInUse(sequence)) {
        continue;
      }
      slot.destructor.store(destructor, std::memory_order_relaxed);
      slot.sequence.store(sequence + 1, std::memory_order_release);
      *key = static_cast<pthread_key_t>(kBionicPthreadKeyValidFlag | index);
      return 0;
    }
    return EAGAIN;
  }

  int Delete(pthread_key_t key) noexcept {
    size_t index = 0;
    if (!DecodeKey(key, &index)) {
      return EINVAL;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    KeySlot& slot = slots_[index];
    const uint64_t sequence =
        slot.sequence.load(std::memory_order_relaxed);
    if (!IsInUse(sequence)) {
      return EINVAL;
    }
    slot.sequence.store(sequence + 1, std::memory_order_release);
    slot.destructor.store(nullptr, std::memory_order_relaxed);
    return 0;
  }

  void* Get(pthread_key_t key, ThreadKeyData* thread_data) noexcept {
    size_t index = 0;
    if (!DecodeKey(key, &index) || thread_data == nullptr) {
      return nullptr;
    }

    const uint64_t sequence =
        slots_[index].sequence.load(std::memory_order_acquire);
    KeyValue& value = thread_data->values[index];
    if (IsInUse(sequence) && value.sequence == sequence) {
      return value.value;
    }
    value.value = nullptr;
    return nullptr;
  }

  int Set(pthread_key_t key, const void* value,
          ThreadKeyData* thread_data) noexcept {
    size_t index = 0;
    if (!DecodeKey(key, &index) || thread_data == nullptr) {
      return EINVAL;
    }

    const uint64_t sequence =
        slots_[index].sequence.load(std::memory_order_acquire);
    if (!IsInUse(sequence)) {
      return EINVAL;
    }
    thread_data->values[index] = {sequence, const_cast<void*>(value)};
    return 0;
  }

  void Clean(ThreadKeyData* thread_data) noexcept {
    if (thread_data == nullptr) {
      return;
    }

    for (int round = 0; round < kBionicDestructorIterations; ++round) {
      int called_destructors = 0;
      for (size_t index = 0; index < slots_.size(); ++index) {
        const KeySlot& slot = slots_[index];
        const uint64_t sequence =
            slot.sequence.load(std::memory_order_acquire);
        KeyValue& thread_value = thread_data->values[index];
        if (!IsInUse(sequence) || thread_value.sequence != sequence ||
            thread_value.value == nullptr) {
          continue;
        }
        const BionicPthreadKeyDestructor destructor =
            slot.destructor.load(std::memory_order_relaxed);
        const uint64_t confirmed_sequence =
            slot.sequence.load(std::memory_order_acquire);
        if (sequence != confirmed_sequence || destructor == nullptr) {
          continue;
        }
        void* value = thread_value.value;
        thread_value.value = nullptr;
        destructor(value);
        ++called_destructors;
      }
      if (called_destructors == 0) {
        return;
      }
    }
  }

 private:
  static bool IsInUse(uint64_t sequence) noexcept {
    return (sequence & kSequenceInUseBit) != 0;
  }

  static bool DecodeKey(pthread_key_t key, size_t* index) noexcept {
    if (index == nullptr) {
      return false;
    }
    const uint32_t encoded = static_cast<uint32_t>(key);
    if ((encoded & kBionicPthreadKeyValidFlag) == 0) {
      return false;
    }
    const uint32_t candidate = encoded & ~kBionicPthreadKeyValidFlag;
    if (candidate >= kBionicPthreadKeyCount) {
      return false;
    }
    *index = candidate;
    return true;
  }

  std::mutex mutex_;
  std::array<KeySlot, kBionicPthreadKeyCount> slots_{};
};

// The process-global Bionic ABI runtime intentionally outlives TLS destructors
// so host thread teardown cannot observe a destroyed registry. Eager creation
// also removes a C++ guard-variable check from every getspecific hot-path call.
RuntimeState* const g_runtime_state = new RuntimeState();

RuntimeState& State() noexcept { return *g_runtime_state; }

// Keep the read-mostly storage trivially destructible. A non-trivial TLS
// object makes GCC emit a guard check and a possible __cxa_thread_atexit call
// in every getspecific invocation, even after the thread has been initialized.
// Roblox calls getspecific from its scheduler and HttpClient inner loops, so
// register cleanup only on the first setspecific call instead.
thread_local ThreadKeyData g_thread_key_data;
thread_local ThreadKeyCleanup g_thread_key_cleanup;

void* GetSpecificForCurrentThread(pthread_key_t key) noexcept {
  return State().Get(key, &g_thread_key_data);
}

int SetSpecificForCurrentThread(pthread_key_t key, const void* value) noexcept {
  static_cast<void>(g_thread_key_cleanup);
  return State().Set(key, value, &g_thread_key_data);
}

ThreadKeyCleanup::~ThreadKeyCleanup() {
  State().Clean(&g_thread_key_data);
}

}  // namespace

BionicPthreadKeyRuntime& BionicPthreadKeyRuntime::Instance() noexcept {
  static BionicPthreadKeyRuntime* const runtime = new BionicPthreadKeyRuntime;
  return *runtime;
}

int BionicPthreadKeyRuntime::Create(
    pthread_key_t* key, BionicPthreadKeyDestructor destructor) noexcept {
  return State().Create(key, destructor);
}

int BionicPthreadKeyRuntime::Delete(pthread_key_t key) noexcept {
  return State().Delete(key);
}

void* BionicPthreadKeyRuntime::Get(pthread_key_t key) noexcept {
  return GetSpecificForCurrentThread(key);
}

int BionicPthreadKeyRuntime::Set(pthread_key_t key,
                                  const void* value) noexcept {
  return SetSpecificForCurrentThread(key, value);
}

bool IsBionicPthreadKey(pthread_key_t key) noexcept {
  const uint32_t encoded = static_cast<uint32_t>(key);
  const uint32_t index = encoded & ~kBionicPthreadKeyValidFlag;
  return (encoded & kBionicPthreadKeyValidFlag) != 0 &&
         index < kBionicPthreadKeyCount;
}

}  // namespace mocktail::compat

extern "C" int mocktail_bionic_pthread_key_create(
    pthread_key_t* key, void (*destructor)(void*)) {
  return mocktail::compat::BionicPthreadKeyRuntime::Instance().Create(
      key, destructor);
}

extern "C" int mocktail_bionic_pthread_key_delete(pthread_key_t key) {
  return mocktail::compat::BionicPthreadKeyRuntime::Instance().Delete(key);
}

extern "C" void* mocktail_bionic_pthread_getspecific(pthread_key_t key) {
  return mocktail::compat::GetSpecificForCurrentThread(key);
}

extern "C" int mocktail_bionic_pthread_setspecific(pthread_key_t key,
                                                    const void* value) {
  return mocktail::compat::SetSpecificForCurrentThread(key, value);
}
