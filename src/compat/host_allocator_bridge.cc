#include "compat/host_allocator_bridge.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace mocktail::compat {
namespace {

constexpr uint64_t kAllocationMagic = 0x4d4f434b5441494cULL;
constexpr size_t kFrontPadding = 0x80;
constexpr size_t kBackPadding = 0x200;
constexpr size_t kMaximumAllocation = size_t{1} << 30;

struct Allocation {
  uint64_t magic = kAllocationMagic;
  void* raw = nullptr;
  size_t requested_size = 0;
  size_t usable_size = 0;
};

std::mutex g_allocations_mutex;
std::unordered_map<void*, Allocation*> g_allocations;

bool IsPowerOfTwo(size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

Allocation* FindAllocationLocked(void* pointer) noexcept {
  auto found = g_allocations.find(pointer);
  return found == g_allocations.end() ? nullptr : found->second;
}

}  // namespace

void* HostAlignedAllocate(size_t size, size_t alignment) noexcept {
  if (size > kMaximumAllocation) {
    return nullptr;
  }
  if (!IsPowerOfTwo(alignment)) {
    alignment = 16;
  }
  alignment = std::max(alignment, alignof(Allocation));
  if (size == 0) {
    size = 1;
  }

  const size_t overhead = sizeof(Allocation) + kFrontPadding +
                          kBackPadding + alignment - 1;
  if (size > std::numeric_limits<size_t>::max() - overhead) {
    return nullptr;
  }
  void* raw = std::calloc(1, size + overhead);
  if (raw == nullptr) {
    return nullptr;
  }

  const uintptr_t first = reinterpret_cast<uintptr_t>(raw) +
                          sizeof(Allocation) + kFrontPadding;
  const uintptr_t aligned = (first + alignment - 1) & ~(alignment - 1);
  auto* allocation = reinterpret_cast<Allocation*>(
      aligned - kFrontPadding - sizeof(Allocation));
  allocation->magic = kAllocationMagic;
  allocation->raw = raw;
  allocation->requested_size = size;
  allocation->usable_size = size;
  void* user = reinterpret_cast<void*>(aligned);

  {
    std::lock_guard<std::mutex> lock(g_allocations_mutex);
    g_allocations[user] = allocation;
  }
  return user;
}

void* HostAllocate(size_t size) noexcept {
  return HostAlignedAllocate(size, 16);
}

void* HostReallocate(void* pointer, size_t size) noexcept {
  if (pointer == nullptr) {
    return HostAllocate(size);
  }
  if (size == 0) {
    HostFree(pointer);
    return nullptr;
  }

  size_t copy_size = 0;
  {
    std::lock_guard<std::mutex> lock(g_allocations_mutex);
    Allocation* allocation = FindAllocationLocked(pointer);
    if (allocation != nullptr && allocation->magic == kAllocationMagic) {
      copy_size = std::min(size, allocation->usable_size);
    }
  }
  void* replacement = HostAllocate(size);
  if (replacement == nullptr) {
    return nullptr;
  }
  if (copy_size != 0) {
    std::memcpy(replacement, pointer, copy_size);
    HostFree(pointer);
  }
  return replacement;
}

void HostFree(void* pointer) noexcept {
  if (pointer == nullptr) {
    return;
  }
  void* raw = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_allocations_mutex);
    auto found = g_allocations.find(pointer);
    if (found == g_allocations.end()) {
      return;
    }
    Allocation* allocation = found->second;
    if (allocation != nullptr && allocation->magic == kAllocationMagic) {
      allocation->magic = 0;
      raw = allocation->raw;
    }
    g_allocations.erase(found);
  }
  std::free(raw);
}

size_t HostUsableSize(void* pointer) noexcept {
  std::lock_guard<std::mutex> lock(g_allocations_mutex);
  Allocation* allocation = FindAllocationLocked(pointer);
  return allocation != nullptr && allocation->magic == kAllocationMagic
             ? allocation->usable_size
             : 0;
}

void* HostAllocatorObjectAllocate(void*, size_t size,
                                  size_t alignment) noexcept {
  return HostAlignedAllocate(size, alignment);
}

}  // namespace mocktail::compat
