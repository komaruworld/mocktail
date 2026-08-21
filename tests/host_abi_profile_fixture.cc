#include <cstddef>
#include <cstdlib>

extern "C" {

__attribute__((visibility("default"), noinline, used)) void*
MocktailFixtureSmallAllocate(std::size_t size) {
  return std::malloc(size);
}

__attribute__((visibility("default"), noinline, used)) void*
MocktailFixtureAllocate(std::size_t size) {
  return std::malloc(size);
}

__attribute__((visibility("default"), noinline, used)) void*
MocktailFixtureReallocate(void* pointer, std::size_t size) {
  return std::realloc(pointer, size);
}

__attribute__((visibility("default"), noinline, used)) void*
MocktailFixtureAlignedAllocate(std::size_t alignment, std::size_t size) {
  void* pointer = nullptr;
  return posix_memalign(&pointer, alignment, size) == 0 ? pointer : nullptr;
}

__attribute__((visibility("default"), noinline, used)) void MocktailFixtureFree(
    void* pointer) {
  std::free(pointer);
}

__attribute__((visibility("default"), noinline, used)) std::size_t
MocktailFixtureUsableSize(void*) {
  return 1;
}

__attribute__((visibility("default"), noinline, used)) void
MocktailFixtureArenaInitialize() {}

__attribute__((visibility("default"), noinline, used)) void
MocktailFixtureThreadInitialize() {}

__attribute__((visibility("default"), noinline, used)) void
MocktailFixtureRegistryInitialize() {}

__attribute__((visibility("default"), constructor(101), used)) void
MocktailFixtureConstructorTwo() {}

__attribute__((visibility("default"), constructor(102), used)) void
MocktailFixtureConstructorThree() {}

__attribute__((visibility("default"), constructor(103), used)) void
MocktailFixtureConstructorFour() {}

__attribute__((visibility("default"), constructor(104), used)) void
MocktailFixtureConstructorFive() {}

__attribute__((visibility("default"),
               used)) void* mocktail_fixture_allocator_slot = nullptr;
__attribute__((visibility("default"),
               used)) void* mocktail_fixture_empty_string_slot = nullptr;
__attribute__((visibility("default"), used)) void* mocktail_fixture_jni_slot =
    nullptr;
__attribute__((visibility("default"),
               used)) void* mocktail_fixture_arena_guard_slot = nullptr;
__attribute__((visibility("default"),
               used)) void* mocktail_fixture_arena_table_slot = nullptr;
__attribute__((visibility("default"),
               used)) void* mocktail_fixture_registry_slot = nullptr;

}  // extern "C"

namespace {

struct FixtureConstructor {
  FixtureConstructor() { mocktail_fixture_arena_guard_slot = nullptr; }
};

FixtureConstructor g_fixture_constructor;

}  // namespace
