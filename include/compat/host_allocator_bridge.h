#ifndef MOCKTAIL_COMPAT_HOST_ALLOCATOR_BRIDGE_H_
#define MOCKTAIL_COMPAT_HOST_ALLOCATOR_BRIDGE_H_

#include <cstddef>

namespace mocktail::compat {

// Explicit smoke-only ownership boundary for allocations returned to the
// guest. Unknown/native pointers are never inspected or freed.
void* HostAllocate(size_t size) noexcept;
void* HostAlignedAllocate(size_t size, size_t alignment) noexcept;
void* HostReallocate(void* pointer, size_t size) noexcept;
void HostFree(void* pointer) noexcept;
size_t HostUsableSize(void* pointer) noexcept;
void* HostAllocatorObjectAllocate(void* object, size_t size,
                                  size_t alignment) noexcept;

}  // namespace mocktail::compat

#endif  // MOCKTAIL_COMPAT_HOST_ALLOCATOR_BRIDGE_H_
