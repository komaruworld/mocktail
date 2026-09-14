#include "mocktail/vr/roblox_vr_device_bridge.h"

#include <elf.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include "mocktail/vr/openxr_backend.h"
#include "mocktail/vr/vr_pose_math.h"
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "vr/roblox_vr_device_bridge_internal.h"

namespace mocktail::vr {
namespace {

using internal::kConstructorContractSize;
using internal::kExpectedObjectType;
using internal::kEyeGetterContractSize;
using internal::kEyeGetterVtableSlot;
using internal::kEyeInitializerVtableSlot;
using internal::kFramebufferSlotOffset;
using internal::kGraphicsDeviceOffset;
using internal::kHeightOffset;
using internal::kInitializerContractSize;
using internal::kObjectNameOffset;
using internal::kObjectTypeOffset;
using internal::kReadyByteOffset;
using internal::kStateGetterContractSize;
using internal::kStateGetterVtableSlot;
using internal::kTextureSlotOffset;
using internal::kWidthOffset;

std::atomic<RobloxVrDeviceBridge *> g_active_bridge{nullptr};
// Evidence queries expose only the current native call's provenance. These
// pointers are identities for the layer, never retained/dereferenced by it.
thread_local void *g_initializing_object = nullptr;
thread_local void *g_eye_object = nullptr;
thread_local void *g_eye_framebuffer = nullptr;
thread_local int g_eye_index = -1;
thread_local bool g_thread_presented = false;
std::mutex g_native_pose_mutex;
ScriptedPoseSample g_native_pose;
using PointerFrameFn = void *(*)(void *, void *);
std::atomic<PointerFrameFn> g_pointer_original{nullptr};
constexpr std::uintptr_t kPointerFrameRva = 0x4d573ee;
// Whole instructions, no RIP-relative operands. 2998's sret prologue.
constexpr std::array<unsigned char, 15> kPointerPrologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x70};
std::uintptr_t g_pointer_target = 0;

void AbsoluteJump(unsigned char *code, std::uintptr_t target) {
  const unsigned char opcode[6] = {0xff, 0x25, 0, 0, 0, 0};
  std::memcpy(code, opcode, 6);
  std::memcpy(code + 6, &target, 8);
}

extern "C" void *mocktail_vr_pointer_frame(void *result, void *service) {
  auto original = g_pointer_original.load(std::memory_order_acquire);
  if (!original)
    return result;
  original(result, service);
  auto *bridge = g_active_bridge.load(std::memory_order_acquire);
  if (!bridge || !bridge->active()) return result;
  ScriptedPoseSample native_pose;
  {
    std::lock_guard<std::mutex> lock(g_native_pose_mutex);
    native_pose = g_native_pose;
  }
  if (!native_pose.valid) return result;
  auto *backend = ActiveVrBackend();
  if (!backend)
    return result;
  const auto current = backend->PublishedHeadPose();
  if (!current.valid)
    return result;
  int index = 0;
  std::memcpy(&index, static_cast<unsigned char *>(service) + 0x118, 4);
  if (index != 1 && index != 2)
    return result;
  if (!current.hands[index - 1].aim_valid)
    return result;
  const auto &hand = native_pose.hands[index - 1];
  if (!hand.grip_valid || !hand.aim_valid)
    return result;
  const auto base = bridge->image_base();
  // Same camera lookup as the original getter (see pointer-world-2998.asm).
  auto get_workspace = reinterpret_cast<void *(*)(void *)>(base + 0x263778c);
  void *workspace = get_workspace(service);
  if (!workspace)
    return result;
  auto *vtable = *static_cast<std::uintptr_t **>(workspace);
  void *camera =
      reinterpret_cast<void *(*)(void *)>(vtable[0x3a8 / 8])(workspace);
  if (!camera)
    return result;
  float scale = 0;
  int degrees = 0;
  std::memcpy(&scale, static_cast<unsigned char *>(camera) + 0x140, 4);
  std::memcpy(&degrees, reinterpret_cast<void *>(base + 0x73677a8), 4);
  if (internal::ApplyAimToWorldFrame(static_cast<float *>(result), hand, scale,
                                     -static_cast<float>(degrees))) {
    static std::atomic<bool> logged[2]{};
    if (!logged[index - 1].exchange(true))
      std::fprintf(
          stderr, "  [vr-input] native menu pointer uses OpenXR aim: hand=%d\n",
          index - 1);
  }
  return result;
}

bool SetCodeWritable(std::uintptr_t target, bool writable) {
  const auto page_size = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  if (!page_size)
    return false;
  const auto first = target & ~(page_size - 1);
  const auto end =
      (target + kPointerPrologue.size() + page_size - 1) & ~(page_size - 1);
  return mprotect(reinterpret_cast<void *>(first), end - first,
                  PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0)) == 0;
}

bool InstallPointerHook(std::uintptr_t base) {
  if (g_pointer_target)
    return false;
  auto *target = reinterpret_cast<unsigned char *>(base + kPointerFrameRva);
  if (std::memcmp(target, kPointerPrologue.data(), kPointerPrologue.size()) !=
      0)
    return false;
  const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  auto *code = static_cast<unsigned char *>(
      mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (code == MAP_FAILED)
    return false;
  std::memcpy(code, target, kPointerPrologue.size());
  AbsoluteJump(code + kPointerPrologue.size(),
               base + kPointerFrameRva + kPointerPrologue.size());
  if (mprotect(code, page_size, PROT_READ | PROT_EXEC) != 0 ||
      !SetCodeWritable(base + kPointerFrameRva, true)) {
    munmap(code, page_size);
    return false;
  }
  g_pointer_original.store(reinterpret_cast<PointerFrameFn>(code),
                           std::memory_order_release);
  AbsoluteJump(target,
               reinterpret_cast<std::uintptr_t>(&mocktail_vr_pointer_frame));
  target[14] = 0x90;
  if (!SetCodeWritable(base + kPointerFrameRva, false)) {
    std::memcpy(target, kPointerPrologue.data(), kPointerPrologue.size());
    (void)SetCodeWritable(base + kPointerFrameRva, false);
    return false;
  }
  // Retain this single RX page for process lifetime: an in-flight guest call
  // may still be returning through it when the entrypoint is restored.
  g_pointer_target = base + kPointerFrameRva;
  return true;
}

bool RestorePointerHook() {
  if (!g_pointer_target)
    return true;
  if (!SetCodeWritable(g_pointer_target, true))
    return false;
  std::memcpy(reinterpret_cast<void *>(g_pointer_target),
              kPointerPrologue.data(), kPointerPrologue.size());
  const bool restored = SetCodeWritable(g_pointer_target, false);
  if (restored)
    g_pointer_target = 0;
  return restored;
}

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

std::uint64_t CurrentThreadId() {
  return static_cast<std::uint64_t>(syscall(SYS_gettid));
}

bool IsValidElfImage(std::uintptr_t image_base) {
  if (image_base == 0) {
    return false;
  }
  const auto *header = reinterpret_cast<const Elf64_Ehdr *>(image_base);
  return std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 &&
         header->e_ident[EI_CLASS] == ELFCLASS64 && header->e_phoff != 0 &&
         header->e_phnum != 0;
}

bool IsProgramHeaderRange(std::uintptr_t image_base, std::uintptr_t rva,
                          std::size_t size, std::uint32_t type,
                          std::uint32_t required_flags) {
  if (!IsValidElfImage(image_base) || size == 0 ||
      rva > std::numeric_limits<std::uintptr_t>::max() - size) {
    return false;
  }
  const auto *header = reinterpret_cast<const Elf64_Ehdr *>(image_base);
  const auto *program_headers = reinterpret_cast<const Elf64_Phdr *>(
      image_base + static_cast<std::uintptr_t>(header->e_phoff));
  const std::uintptr_t range_end = rva + size;
  for (std::size_t index = 0; index < header->e_phnum; ++index) {
    const Elf64_Phdr &segment = program_headers[index];
    if (segment.p_type != type ||
        (segment.p_flags & required_flags) != required_flags ||
        segment.p_vaddr >
            std::numeric_limits<std::uintptr_t>::max() - segment.p_memsz) {
      continue;
    }
    const std::uintptr_t segment_end =
        static_cast<std::uintptr_t>(segment.p_vaddr + segment.p_memsz);
    if (rva >= segment.p_vaddr && range_end <= segment_end) {
      return true;
    }
  }
  return false;
}

bool IsExecutableImageRange(std::uintptr_t image_base, std::uintptr_t rva,
                            std::size_t size) {
  return IsProgramHeaderRange(image_base, rva, size, PT_LOAD, PF_R | PF_X);
}

bool IsWritableDataImageRange(std::uintptr_t image_base, std::uintptr_t rva,
                              std::size_t size) {
  return IsProgramHeaderRange(image_base, rva, size, PT_LOAD, PF_R | PF_W);
}

bool IsRelroImageRange(std::uintptr_t image_base, std::uintptr_t rva,
                       std::size_t size) {
  return IsProgramHeaderRange(image_base, rva, size, PT_GNU_RELRO, 0);
}

std::uintptr_t ImageSize(std::uintptr_t image_base) {
  const auto *header = reinterpret_cast<const Elf64_Ehdr *>(image_base);
  const auto *program_headers = reinterpret_cast<const Elf64_Phdr *>(
      image_base + static_cast<std::uintptr_t>(header->e_phoff));
  std::uintptr_t highest = 0;
  for (std::size_t index = 0; index < header->e_phnum; ++index) {
    const Elf64_Phdr &segment = program_headers[index];
    if (segment.p_type != PT_LOAD ||
        segment.p_vaddr >
            std::numeric_limits<std::uintptr_t>::max() - segment.p_memsz) {
      continue;
    }
    highest = std::max(highest, static_cast<std::uintptr_t>(segment.p_vaddr +
                                                            segment.p_memsz));
  }
  return highest;
}

bool IsCodePointerInImage(std::uintptr_t image_base, std::uintptr_t image_size,
                          std::uintptr_t address) {
  return address > image_base && image_size != 0 &&
         address - image_base < image_size;
}

bool SetVtableRangeWritable(std::uintptr_t *first_slot,
                            std::uintptr_t *last_slot_exclusive,
                            bool writable) {
  if (first_slot == nullptr || last_slot_exclusive <= first_slot) {
    return false;
  }
  const long page_size_value = sysconf(_SC_PAGESIZE);
  if (page_size_value <= 0) {
    return false;
  }
  const std::uintptr_t page_size = static_cast<std::uintptr_t>(page_size_value);
  const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(first_slot);
  const std::uintptr_t last =
      reinterpret_cast<std::uintptr_t>(last_slot_exclusive);
  const std::uintptr_t page_begin = first - first % page_size;
  const std::uintptr_t page_end =
      ((last + page_size - 1) / page_size) * page_size;
  const int protection = writable ? PROT_READ | PROT_WRITE : PROT_READ;
  return mprotect(reinterpret_cast<void *>(page_begin), page_end - page_begin,
                  protection) == 0;
}

bool ContainsBytes(const std::uint8_t *code, std::size_t size,
                   const std::uint8_t *expected, std::size_t expected_size) {
  if (code == nullptr || expected == nullptr || expected_size == 0 ||
      expected_size > size) {
    return false;
  }
  for (std::size_t index = 0; index + expected_size <= size; ++index) {
    if (std::memcmp(code + index, expected, expected_size) == 0) {
      return true;
    }
  }
  return false;
}

template <typename Function> std::uintptr_t FunctionAddress(Function function) {
  return reinterpret_cast<std::uintptr_t>(function);
}

void Log(const char *format, ...) {
  std::va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  va_end(arguments);
  std::fflush(stderr);
}

} // namespace

namespace internal {

EyeResourceState InspectEyeResources(const void *object) {
  if (object == nullptr)
    return EyeResourceState::kInvalid;
  const auto *bytes = static_cast<const std::uint8_t *>(object);
  std::uintptr_t fb[2], tex[2];
  std::memcpy(&fb[0], bytes + kFramebufferSlotOffset, sizeof(fb[0]));
  std::memcpy(&fb[1], bytes + kFramebufferSlotOffset + kEyeSlotStride,
              sizeof(fb[1]));
  std::memcpy(&tex[0], bytes + kTextureSlotOffset, sizeof(tex[0]));
  std::memcpy(&tex[1], bytes + kTextureSlotOffset + kEyeSlotStride,
              sizeof(tex[1]));
  std::uint32_t width, height;
  std::memcpy(&width, bytes + kWidthOffset, sizeof(width));
  std::memcpy(&height, bytes + kHeightOffset, sizeof(height));
  if (width == 0 || height == 0 || width > 16384 || height > 16384)
    return EyeResourceState::kInvalid;
  if (bytes[kReadyByteOffset] == 0 && !fb[0] && !fb[1] && !tex[0] && !tex[1])
    return EyeResourceState::kEmpty;
  if (bytes[kReadyByteOffset] == 1 && fb[0] && fb[1] && tex[0] && tex[1] &&
      fb[0] != fb[1] && tex[0] != tex[1])
    return EyeResourceState::kReady;
  return EyeResourceState::kInvalid;
}

bool HasExpectedVrStateGetterContract(const std::uint8_t *code,
                                      std::size_t size) {
  // push rbp; mov rbp,rsp; push rbx; push rax; mov rbx,rdi; add rsi,0x14;
  // mov edx,0x138; call memcpy; mov rax,rbx; add rsp,8; pop rbx; pop rbp; ret
  static constexpr std::array<std::uint8_t, kStateGetterContractSize>
      kExpected = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb,
                   0x48, 0x83, 0xc6, 0x14, 0xba, 0x38, 0x01, 0x00, 0x00,
                   0xe8, 0xd3, 0xb5, 0x36, 0x03, 0x48, 0x89, 0xd8, 0x48,
                   0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3};
  return code != nullptr && size >= kExpected.size() &&
         std::memcmp(code, kExpected.data(), kExpected.size()) == 0;
}

bool HasExpectedVrEyeGetterContract(const std::uint8_t *code,
                                    std::size_t size) {
  // movsxd rax,esi; shl rax,4; mov rax,[rdi+rax+0x150]; ret
  static constexpr std::array<std::uint8_t, kEyeGetterContractSize> kExpected =
      {0x48, 0x63, 0xc6, 0x48, 0xc1, 0xe0, 0x04, 0x48,
       0x8b, 0x84, 0x07, 0x50, 0x01, 0x00, 0x00, 0xc3};
  return code != nullptr && size >= kExpected.size() &&
         std::memcmp(code, kExpected.data(), kExpected.size()) == 0;
}

bool HasExpectedVrConstructorContract(const std::uint8_t *code,
                                      std::size_t size) {
  static constexpr std::array<std::uint8_t, 29> kPrologue = {
      0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
      0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28, 0x49, 0x89, 0xcf,
      0x41, 0x89, 0xd4, 0x41, 0x89, 0xf5, 0x48, 0x89, 0xfb};
  // lea rax,[rip -> DebugDeviceVR vtable]; mov [rdi],rax
  static constexpr std::array<std::uint8_t, 7> kVtableStore = {
      0x48, 0x8d, 0x05, 0xf2, 0x74, 0x47, 0x03};
  static constexpr std::array<std::uint8_t, 3> kVptrWrite = {0x48, 0x89, 0x07};
  // mov dword ptr [rdi+0x10], 6
  static constexpr std::array<std::uint8_t, 7> kTypeStore = {
      0xc7, 0x47, 0x10, 0x06, 0x00, 0x00, 0x00};
  // lea r14,[rdi+0x14]; mov edx,0x134 (state block memset)
  static constexpr std::array<std::uint8_t, 9> kStateMemset = {
      0x4c, 0x8d, 0x77, 0x14, 0xba, 0x34, 0x01, 0x00, 0x00};
  // mov [rbx+0x190],r13d ; mov [rbx+0x194],r12d
  static constexpr std::array<std::uint8_t, 14> kWidthHeightStore = {
      0x44, 0x89, 0xab, 0x90, 0x01, 0x00, 0x00,
      0x44, 0x89, 0xa3, 0x94, 0x01, 0x00, 0x00};
  // mov [rbx+0x198],r15 (graphics device captured from rcx)
  static constexpr std::array<std::uint8_t, 7> kDeviceStore = {
      0x4c, 0x89, 0xbb, 0x98, 0x01, 0x00, 0x00};
  if (code == nullptr || size < kConstructorContractSize) {
    return false;
  }
  return std::memcmp(code, kPrologue.data(), kPrologue.size()) == 0 &&
         ContainsBytes(code, size, kVtableStore.data(), kVtableStore.size()) &&
         ContainsBytes(code, size, kVptrWrite.data(), kVptrWrite.size()) &&
         ContainsBytes(code, size, kTypeStore.data(), kTypeStore.size()) &&
         ContainsBytes(code, size, kStateMemset.data(), kStateMemset.size()) &&
         ContainsBytes(code, size, kWidthHeightStore.data(),
                       kWidthHeightStore.size()) &&
         ContainsBytes(code, size, kDeviceStore.data(), kDeviceStore.size());
}

bool HasExpectedVrEyeInitializerContract(
    const std::uint8_t *code, std::size_t size,
    const compat::VrDebugDeviceBridgeProfile &profile) {
  static constexpr std::array<std::uint8_t, 24> kPrologue = {
      0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
      0x53, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x89, 0xb5, 0x50};
  // mov cl,1; mov [rdi+0x147],cl; mov [rdi+0x94],cl; mov [rdi+0x144],cl
  static constexpr std::array<std::uint8_t, 20> kReadyBytes = {
      0xb1, 0x01, 0x88, 0x8f, 0x47, 0x01, 0x00, 0x00, 0x88, 0x8f,
      0x94, 0x00, 0x00, 0x00, 0x88, 0x8f, 0x44, 0x01, 0x00, 0x00};
  // lea rax,[rdi+0x170] (texture slots)
  static constexpr std::array<std::uint8_t, 7> kTextureSlotBase = {
      0x48, 0x8d, 0x87, 0x70, 0x01, 0x00, 0x00};
  // lea rax,[rdi+0x150] (framebuffer slots)
  static constexpr std::array<std::uint8_t, 7> kFramebufferSlotBase = {
      0x48, 0x8d, 0x87, 0x50, 0x01, 0x00, 0x00};
  // mov rsi,[rbp-0xb0]; mov rax,[rsi]; mov rax,[rax+createSlot]; call rax
  std::array<std::uint8_t, 13> device_call = {0x48, 0x8b, 0x06, 0x48, 0x8b,
                                              0x80, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00};
  const std::uint32_t create_slot = static_cast<std::uint32_t>(
      profile.device_create_framebuffer_vtable_offset);
  device_call[6] = static_cast<std::uint8_t>(create_slot);
  device_call[7] = static_cast<std::uint8_t>(create_slot >> 8);
  device_call[8] = static_cast<std::uint8_t>(create_slot >> 16);
  device_call[9] = static_cast<std::uint8_t>(create_slot >> 24);
  // Two-iteration eye loop: mov r15d,1; test [rbp-0xa4],1; mov ecx,0; jne back
  static constexpr std::array<std::uint8_t, 15> kEyeLoopTail = {
      0x41, 0xbf, 0x01, 0x00, 0x00, 0x00, 0xf6, 0x85,
      0x5c, 0xff, 0xff, 0xff, 0x01, 0xb9, 0x00};
  if (code == nullptr || size < kInitializerContractSize) {
    return false;
  }
  return std::memcmp(code, kPrologue.data(), kPrologue.size()) == 0 &&
         ContainsBytes(code, size, kReadyBytes.data(), kReadyBytes.size()) &&
         ContainsBytes(code, size, kTextureSlotBase.data(),
                       kTextureSlotBase.size()) &&
         ContainsBytes(code, size, kFramebufferSlotBase.data(),
                       kFramebufferSlotBase.size()) &&
         ContainsBytes(code, size, device_call.data(), 10) &&
         ContainsBytes(code, size, kEyeLoopTail.data(), kEyeLoopTail.size());
}

bool HasExpectedVrDebugDeviceVtable(
    const std::uintptr_t *vtable, std::uintptr_t image_base,
    const compat::VrDebugDeviceBridgeProfile &profile) {
  return vtable != nullptr && image_base != 0 &&
         vtable[kStateGetterVtableSlot / sizeof(std::uintptr_t)] ==
             image_base + profile.state_getter_rva &&
         vtable[kEyeGetterVtableSlot / sizeof(std::uintptr_t)] ==
             image_base + profile.eye_getter_rva &&
         vtable[kEyeInitializerVtableSlot / sizeof(std::uintptr_t)] ==
             image_base + profile.eye_initializer_rva;
}

} // namespace internal

namespace {

// Guest-callable interposed vtable entries. The guest Android ABI for these
// methods is SysV amd64: the state getter is sret (rdi = hidden 0x138-byte
// result buffer, rsi = device object, returns the buffer) and the eye getter
// takes (rdi = device object, esi = eye index) and returns the framebuffer
// object pointer.
extern "C" void mocktail_vr_debug_device_haptics(void* object, int hand, float amplitude) {
  if (auto* bridge = g_active_bridge.load(std::memory_order_acquire))
    bridge->OnHapticsCall(object, hand, amplitude);
}

extern "C" void *mocktail_vr_debug_device_state_getter(void *result_buffer,
                                                       void *device_object) {
  RobloxVrDeviceBridge *bridge =
      g_active_bridge.load(std::memory_order_acquire);
  if (bridge != nullptr) {
    bridge->OnStateGetterCall(result_buffer, device_object);
  }
  return result_buffer;
}

extern "C" void *mocktail_vr_debug_device_eye_getter(void *device_object,
                                                     int eye_index) {
  RobloxVrDeviceBridge *bridge =
      g_active_bridge.load(std::memory_order_acquire);
  const auto original =
      bridge != nullptr ? bridge->original_eye_getter() : nullptr;
  void *framebuffer =
      original != nullptr ? original(device_object, eye_index) : nullptr;
  if (bridge != nullptr) {
    bridge->OnEyeGetterCall(device_object, eye_index, framebuffer);
  }
  return framebuffer;
}

} // namespace

extern "C" void *mocktail_vr_initializing_object() {
  return g_initializing_object;
}

extern "C" bool mocktail_vr_current_eye(void **self, int *eye,
                                        void **framebuffer) {
  *self = g_eye_object;
  *eye = g_eye_index;
  *framebuffer = g_eye_framebuffer;
  return g_eye_object != nullptr && g_eye_index >= 0 &&
         g_eye_framebuffer != nullptr;
}

extern "C" void mocktail_vr_consume_eye() {
  g_eye_object = nullptr;
  g_eye_framebuffer = nullptr;
  g_eye_index = -1;
}

extern "C" void mocktail_vr_note_host_present_begin() {
  RobloxVrDeviceBridge *bridge =
      g_active_bridge.load(std::memory_order_acquire);
  if (bridge != nullptr) {
    bridge->NoteHostPresent();
  }
}

RobloxVrDeviceBridge::~RobloxVrDeviceBridge() {
  Shutdown();
  RobloxVrDeviceBridge *expected = this;
  (void)g_active_bridge.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
}

Status RobloxVrDeviceBridge::Install(const compat::BuildProfile &profile) {
  if (installed_) {
    return FailedPrecondition("Roblox VR device bridge is already armed");
  }
  if (!profile.vr_debug_device_bridge.has_value()) {
    return FailedPrecondition(
        "experimental Roblox VR requires vr_debug_device_bridge metadata in "
        "the exact-build compatibility profile");
  }
  if (profile.elf_build_id != "ade08266c67aee88ec9c1d00902150e1684dad3a")
    return FailedPrecondition("VR controller/pointer/haptic contracts are verified only for Build 2998");
  if (!profile.allow_host_abi_bridges) {
    return FailedPrecondition(
        "Roblox VR device bridge profile requires host ABI bridges");
  }

  RobloxVrDeviceBridge *expected = nullptr;
  if (!g_active_bridge.compare_exchange_strong(expected, this,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
    return FailedPrecondition("another Roblox VR device bridge owns the "
                              "process");
  }
  profile_ = *profile.vr_debug_device_bridge;
  installed_ = true;
  std::fprintf(stderr,
               "  [vr-device] native stereo bridge armed for Build ID %s "
               "(vtable +0x28/+0x48 interposition; init phase: %s)\n",
               profile.elf_build_id.c_str(), "live render-thread state getter");
  return Status::Ok();
}

Status RobloxVrDeviceBridge::Activate(std::uintptr_t image_base) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!installed_) {
    return FailedPrecondition("Roblox VR device bridge is not armed");
  }
  if (active_.load(std::memory_order_acquire)) {
    return image_base == image_base_.load(std::memory_order_relaxed)
               ? Status::Ok()
               : FailedPrecondition("Roblox VR device bridge saw a second "
                                    "image");
  }
  if (!IsValidElfImage(image_base)) {
    return InvalidArgument("libroblox image base is not a valid ELF64 image");
  }
  const std::uintptr_t largest_rva = std::max(
      {profile_.vtable_rva + kEyeInitializerVtableSlot + sizeof(std::uintptr_t),
       profile_.constructor_rva + kConstructorContractSize,
       profile_.state_getter_rva + kStateGetterContractSize,
       profile_.eye_getter_rva + kEyeGetterContractSize,
       profile_.eye_initializer_rva + kInitializerContractSize,
       profile_.emulator_flag_storage_rva + 1});
  if (image_base > std::numeric_limits<std::uintptr_t>::max() - largest_rva) {
    return InvalidArgument("VR device profile address overflows");
  }

  const std::uintptr_t vtable_rva_size =
      kEyeInitializerVtableSlot + sizeof(std::uintptr_t);
  if (!IsRelroImageRange(image_base, profile_.vtable_rva, vtable_rva_size)) {
    return FailedPrecondition(
        "DebugDeviceVR vtable is not inside the verified RELRO range");
  }
  if (!IsExecutableImageRange(image_base, profile_.constructor_rva,
                              kConstructorContractSize) ||
      !IsExecutableImageRange(image_base, profile_.state_getter_rva,
                              kStateGetterContractSize) ||
      !IsExecutableImageRange(image_base, profile_.eye_getter_rva,
                              kEyeGetterContractSize) ||
      !IsExecutableImageRange(image_base, profile_.eye_initializer_rva,
                              kInitializerContractSize)) {
    return FailedPrecondition(
        "VR device profile functions are not in executable image ranges");
  }
  if (!IsWritableDataImageRange(image_base, profile_.emulator_flag_storage_rva,
                                1)) {
    return FailedPrecondition(
        "VR emulator flag storage is not in a writable data range");
  }

  const auto *vtable = reinterpret_cast<const std::uintptr_t *>(
      image_base + profile_.vtable_rva);
  if (!internal::HasExpectedVrDebugDeviceVtable(vtable, image_base, profile_)) {
    return FailedPrecondition(
        "DebugDeviceVR vtable does not match the Build-ID profile");
  }
  const auto *state_getter_code = reinterpret_cast<const std::uint8_t *>(
      image_base + profile_.state_getter_rva);
  const auto *eye_getter_code = reinterpret_cast<const std::uint8_t *>(
      image_base + profile_.eye_getter_rva);
  const auto *constructor_code = reinterpret_cast<const std::uint8_t *>(
      image_base + profile_.constructor_rva);
  const auto *initializer_code = reinterpret_cast<const std::uint8_t *>(
      image_base + profile_.eye_initializer_rva);
  if (!internal::HasExpectedVrStateGetterContract(state_getter_code,
                                                  kStateGetterContractSize) ||
      !internal::HasExpectedVrEyeGetterContract(eye_getter_code,
                                                kEyeGetterContractSize)) {
    return FailedPrecondition(
        "DebugDeviceVR getter machine contracts do not match the profile");
  }
  if (!internal::HasExpectedVrConstructorContract(constructor_code,
                                                  kConstructorContractSize) ||
      !internal::HasExpectedVrEyeInitializerContract(
          initializer_code, kInitializerContractSize, profile_)) {
    return FailedPrecondition(
        "DebugDeviceVR constructor/initializer machine contracts do not "
        "match the profile");
  }

  if (!IsExecutableImageRange(image_base, kPointerFrameRva, 0x1b2) ||
      std::memcmp(reinterpret_cast<void*>(image_base + kPointerFrameRva),
                  kPointerPrologue.data(), kPointerPrologue.size()) != 0)
    return FailedPrecondition("2998 VR pointer function contract mismatch");
  image_base_.store(image_base, std::memory_order_relaxed);
  image_size_.store(ImageSize(image_base), std::memory_order_relaxed);
  vtable_ =
      reinterpret_cast<std::uintptr_t *>(image_base + profile_.vtable_rva);
  // 2998's haptic sink is a verified no-op (ret at 0x1d48b70).
  // The caller at 0x28a767e passes self, hand 0/1 and float amplitude.
  if (vtable_[3] != image_base + 0x1d48b70 ||
      !IsExecutableImageRange(image_base, 0x1d48b70, 1) ||
      *reinterpret_cast<const unsigned char*>(image_base + 0x1d48b70) != 0xc3)
    { vtable_ = nullptr; return FailedPrecondition("2998 VR haptic vtable contract mismatch"); }
  original_haptic_slot_ = vtable_[3];
  original_state_slot_ = vtable_[kStateGetterVtableSlot / sizeof(void *)];
  original_eye_slot_ = vtable_[kEyeGetterVtableSlot / sizeof(void *)];

  std::uintptr_t *first_slot = vtable_ + 3;
  std::uintptr_t *last_slot_exclusive =
      vtable_ + kEyeInitializerVtableSlot / 8 + 1;
  if (!SetVtableRangeWritable(first_slot, last_slot_exclusive, true)) {
    vtable_ = nullptr;
    return Status::Error(StatusCode::kPlatformError,
                         "cannot make DebugDeviceVR vtable writable");
  }
  original_state_getter_.store(
      reinterpret_cast<StateGetterFn>(original_state_slot_),
      std::memory_order_release);
  original_eye_getter_.store(reinterpret_cast<EyeGetterFn>(original_eye_slot_),
                             std::memory_order_release);
  __atomic_store_n(&vtable_[3], FunctionAddress(&mocktail_vr_debug_device_haptics), __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kStateGetterVtableSlot / 8],
                   FunctionAddress(&mocktail_vr_debug_device_state_getter),
                   __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kEyeGetterVtableSlot / 8],
                   FunctionAddress(&mocktail_vr_debug_device_eye_getter),
                   __ATOMIC_RELEASE);
  if (!SetVtableRangeWritable(first_slot, last_slot_exclusive, false)) {
    __atomic_store_n(&vtable_[3], original_haptic_slot_, __ATOMIC_RELEASE);
    __atomic_store_n(&vtable_[kStateGetterVtableSlot / 8], original_state_slot_,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&vtable_[kEyeGetterVtableSlot / 8], original_eye_slot_,
                     __ATOMIC_RELEASE);
    (void)SetVtableRangeWritable(first_slot, last_slot_exclusive, false);
    original_state_getter_.store(nullptr, std::memory_order_release);
    original_eye_getter_.store(nullptr, std::memory_order_release);
    vtable_ = nullptr;
    return Status::Error(StatusCode::kPlatformError,
                         "cannot restore DebugDeviceVR vtable RELRO "
                         "protection");
  }

  if (!InstallPointerHook(image_base)) {
    (void)RestoreVtableLocked();
    return FailedPrecondition("could not install verified VR aim pointer hook");
  }
  {
    std::lock_guard<std::mutex> pose_lock(g_native_pose_mutex);
    g_native_pose = {};
  }
  active_.store(true, std::memory_order_release);
  Log("  [vr-device] activated: DebugDeviceVR input/eyes/haptics +0x18/+0x28/+0x48 and aim pointer "
      "interposed at image base 0x%" PRIxPTR "\n",
      image_base);
  return Status::Ok();
}

void RobloxVrDeviceBridge::Shutdown() {
  if (!installed_) {
    return;
  }
  bool restored = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_.load(std::memory_order_acquire)) {
      active_.store(false, std::memory_order_release);
      restored = RestoreVtableLocked();
      Log("  [vr-device] shutdown: captured=%d initialized=%d "
          "eye0_calls=%" PRIu64 " eye1_calls=%" PRIu64 " presents=%" PRIu64
          " vtable_restored=%d\n",
          captured_device_count(), initialized_device_count(),
          eye_getter_call_count(0), eye_getter_call_count(1),
          present_count_.load(std::memory_order_relaxed), restored ? 1 : 0);
      vtable_ = nullptr;
      image_base_.store(0, std::memory_order_relaxed);
      image_size_.store(0, std::memory_order_relaxed);
    }
  }
  if (!restored) {
    std::fprintf(stderr, "  [vr-device] failed to restore DebugDeviceVR vtable "
                         "protection\n");
  }
  installed_ = false;
}

bool RobloxVrDeviceBridge::RestoreVtableLocked() {
  if (!RestorePointerHook()) return false;
  if (vtable_ == nullptr) {
    return false;
  }
  std::uintptr_t *first_slot = vtable_ + 3;
  std::uintptr_t *last_slot_exclusive =
      vtable_ + kEyeInitializerVtableSlot / 8 + 1;
  if (!SetVtableRangeWritable(first_slot, last_slot_exclusive, true)) {
    return false;
  }
  __atomic_store_n(&vtable_[3], original_haptic_slot_, __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kStateGetterVtableSlot / 8], original_state_slot_,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kEyeGetterVtableSlot / 8], original_eye_slot_,
                   __ATOMIC_RELEASE);
  return SetVtableRangeWritable(first_slot, last_slot_exclusive, false);
}

void RobloxVrDeviceBridge::NoteHostPresent() {
  if (!active_.load(std::memory_order_acquire)) {
    return;
  }
  const std::uint64_t thread_id = CurrentThreadId();
  std::uint64_t expected_thread = 0;
  if (present_thread_.compare_exchange_strong(expected_thread, thread_id,
                                              std::memory_order_acq_rel)) {
    Log("  [vr-device] render thread observed: tid=%" PRIu64 "\n", thread_id);
  }
  const std::uint64_t presents =
      present_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (presents == 1) {
    Log("  [vr-device] first host present completed on tid=%" PRIu64 "\n",
        thread_id);
  }
  g_thread_presented = true;
  mocktail_vr_consume_eye();
}

bool RobloxVrDeviceBridge::ValidateObjectHeader(void *self) const {
  const std::uintptr_t base = image_base_.load(std::memory_order_acquire);
  if (self == nullptr || base == 0) {
    return false;
  }
  auto *bytes = static_cast<std::uint8_t *>(self);
  const std::uintptr_t vptr = *reinterpret_cast<const std::uintptr_t *>(bytes);
  if (vptr != base + profile_.vtable_rva) {
    return false;
  }
  const std::uint32_t type =
      *reinterpret_cast<const std::uint32_t *>(bytes + kObjectTypeOffset);
  if (type != kExpectedObjectType) {
    return false;
  }
  const char *name =
      *reinterpret_cast<const char *const *>(bytes + kObjectNameOffset);
  return name != nullptr && std::strcmp(name, "DebugDevice") == 0;
}

void *RobloxVrDeviceBridge::ValidatedGraphicsDevice(void *self) const {
  auto *bytes = static_cast<std::uint8_t *>(self);
  void *device =
      *reinterpret_cast<void *const *>(bytes + kGraphicsDeviceOffset);
  if (device == nullptr) {
    return nullptr;
  }
  const std::uintptr_t base = image_base_.load(std::memory_order_acquire);
  const std::uintptr_t size = image_size_.load(std::memory_order_acquire);
  const std::uintptr_t device_vtable =
      *reinterpret_cast<const std::uintptr_t *>(device);
  if (!IsCodePointerInImage(base, size, device_vtable)) {
    return nullptr;
  }
  const std::uintptr_t create_framebuffer =
      *reinterpret_cast<const std::uintptr_t *>(
          device_vtable + profile_.device_create_framebuffer_vtable_offset);
  if (!IsCodePointerInImage(base, size, create_framebuffer)) {
    return nullptr;
  }
  return device;
}

bool internal::ApplyAimToWorldFrame(float *frame, const VrHandPose &hand,
                                    float head_scale, float degrees) {
  if (!frame || !hand.grip_valid || !hand.aim_valid ||
      !std::isfinite(head_scale) || head_scale <= 0 ||
      !std::isfinite(degrees) || std::abs(degrees) > 360)
    return false;
  for (int i = 0; i < 12; ++i)
    if (!std::isfinite(frame[i]))
      return false;
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(hand.grip_position[i]) ||
        !std::isfinite(hand.aim_position[i]))
      return false;
  pose::Quat grip, aim;
  if (!pose::QuatNormalize({hand.grip_orientation[0], hand.grip_orientation[1],
                            hand.grip_orientation[2], hand.grip_orientation[3]},
                           &grip) ||
      !pose::QuatNormalize({hand.aim_orientation[0], hand.aim_orientation[1],
                            hand.aim_orientation[2], hand.aim_orientation[3]},
                           &aim))
    return false;
  const float angle = degrees * pose::kPi / 360.f;
  grip = pose::QuatMultiply(grip, {std::sin(angle), 0, 0, std::cos(angle)});
  const auto inverse = pose::QuatConjugate(grip);
  const auto delta = pose::QuatMultiply(inverse, aim);
  auto translation =
      pose::QuatRotate(inverse, {hand.aim_position[0] - hand.grip_position[0],
                                 hand.aim_position[1] - hand.grip_position[1],
                                 hand.aim_position[2] - hand.grip_position[2]});
  const float scale = (10.f / 3.f) * head_scale;
  const float offset[3] = {translation.x * scale, translation.y * scale,
                           translation.z * scale};
  float out[12];
  for (int col = 0; col < 3; ++col) {
    const auto axis =
        pose::QuatRotate(delta, {col == 0 ? 1.f : 0.f, col == 1 ? 1.f : 0.f,
                                 col == 2 ? 1.f : 0.f});
    const float component[3] = {axis.x, axis.y, axis.z};
    for (int row = 0; row < 3; ++row) {
      out[row * 3 + col] = 0;
      for (int k = 0; k < 3; ++k)
        out[row * 3 + col] += frame[row * 3 + k] * component[k];
    }
  }
  for (int row = 0; row < 3; ++row) {
    out[9 + row] = frame[9 + row];
    for (int k = 0; k < 3; ++k)
      out[9 + row] += frame[row * 3 + k] * offset[k];
  }
  for (float value : out)
    if (!std::isfinite(value))
      return false;
  std::memcpy(frame, out, sizeof(out));
  return true;
}

bool internal::ApplyXrPose(void *state, const ScriptedPoseSample &pose) {
  if (!state)
    return false;
  auto *bytes = static_cast<std::uint8_t *>(state);
  bytes[0] = 0;
  bytes[0x80] = bytes[0x132] = 0;
  const std::uint32_t changed = 0x0fffffff;
  std::memcpy(bytes + 0x84, &changed, sizeof(changed));
  std::memset(bytes + 0x88, 0, 28 * sizeof(float));
  // Hand records are cleared unless a tracked grip pose is supplied below.
  // Clearing the valid byte (never the whole record) makes the guest report
  // the hand as untracked instead of holding a stale pose. The extra index-3
  // record is unused by this backend and stays invalid.
  bytes[kExtraRecordOffset + kRecordValidOffset] = 0;
  bytes[kLeftHandRecordOffset + kRecordValidOffset] = 0;
  bytes[kRightHandRecordOffset + kRecordValidOffset] = 0;
  if (!pose.valid)
    return false;
  float norm = 0;
  for (float v : pose.orientation) {
    if (!std::isfinite(v))
      return false;
    norm += v * v;
  }
  if (norm < 0.99f || norm > 1.01f)
    return false;
  for (float v : pose.position)
    if (!std::isfinite(v))
      return false;
  for (int eye = 0; eye < 2; ++eye) {
    for (float v : pose.eye_offset[eye])
      if (!std::isfinite(v))
        return false;
    for (float v : pose.eye_fov[eye])
      if (!std::isfinite(v) || std::abs(v) >= 1.56f)
        return false;
  }
  bytes[0x80] = pose.valid && pose.controllers_connected;
  for (int channel = 0; channel < 28; ++channel) {
    const float raw = pose.controller_channels[channel];
    const float value =
        pose.valid && pose.controllers_connected && std::isfinite(raw)
            ? std::clamp(raw, -1.f, 1.f)
            : 0.f;
    std::memcpy(bytes + 0x88 + channel * 4, &value, sizeof(value));
  }
  bytes[0x132] = pose.valid && pose.controllers_connected;

  // 2998's 0x37eacb8 converts metres to studs itself (factor 10/3).
  std::memcpy(bytes + 4, pose.position, sizeof(pose.position));
  std::memcpy(bytes + 0x10, pose.orientation, sizeof(pose.orientation));
  // Tracked hands go into the same state copy the head uses, so the guest's
  // own conversion (10/3 studs) and -20 deg hand pitch offset apply once,
  // exactly as they do for the head. Grip is the held-object pose; a hand
  // without a valid grip pose stays marked untracked rather than being
  // fabricated at the origin.
  for (int hand = 0; hand < 2; ++hand) {
    const auto &source = pose.hands[hand];
    if (!source.grip_valid)
      continue;
    float hand_norm = 0;
    bool finite = true;
    for (float v : source.grip_position)
      finite = finite && std::isfinite(v);
    for (float v : source.grip_orientation) {
      finite = finite && std::isfinite(v);
      hand_norm += v * v;
    }
    if (!finite || hand_norm < 0.99f || hand_norm > 1.01f)
      continue;
    const std::size_t record =
        hand == 0 ? kLeftHandRecordOffset : kRightHandRecordOffset;
    std::memcpy(bytes + record + kRecordPositionOffset, source.grip_position,
                sizeof(source.grip_position));
    std::memcpy(bytes + record + kRecordOrientationOffset,
                source.grip_orientation, sizeof(source.grip_orientation));
    bytes[record + kRecordValidOffset] = 1;
  }
  for (int eye = 0; eye < 2; ++eye) {
    std::memcpy(bytes + 0xf8 + eye * 12, pose.eye_offset[eye], 12);
    const auto *fov = pose.eye_fov[eye];
    const float tangents[4] = {std::tan(fov[2]), -std::tan(fov[3]),
                               -std::tan(fov[0]), std::tan(fov[1])};
    std::memcpy(bytes + 0x110 + eye * 16, tangents, sizeof(tangents));
  }
  bytes[0] = 1;
  return true;
}

void RobloxVrDeviceBridge::OnHapticsCall(void *device_object, int hand,
                                         float amplitude) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active() || !ValidateObjectHeader(device_object) || hand < 0 || hand > 1)
    return;
  if (auto *backend = ActiveVrBackend()) {
    static std::atomic<bool> logged[2]{};
    if (!logged[hand].exchange(true))
      Log("  [vr-input] native haptic callback observed: hand=%d "
          "amplitude=%.3f\n",
          hand, amplitude);
    if (!std::isfinite(amplitude) || amplitude <= 0)
      backend->StopControllerHaptics(hand);
    // Native pull refreshes once per engine frame. A short lease prevents a
    // stalled or disconnected engine from leaving the controller vibrating.
    else
      backend->RequestControllerHaptics(hand, amplitude, 50000000, 0);
  }
}

void RobloxVrDeviceBridge::OnStateGetterCall(void *result_buffer,
                                             void *device_object) {
  // Serialize initialization with all copies of readiness: the initializer
  // raises the guest ready byte before its resources have been allocated.
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_.load(std::memory_order_acquire) && g_thread_presented &&
      ValidateObjectHeader(device_object) &&
      internal::InspectEyeResources(device_object) ==
          internal::EyeResourceState::kEmpty) {
    TryInitializeEyes(device_object);
  }
  const auto original = original_state_getter();
  if (original != nullptr)
    original(result_buffer, device_object);
  if (active_.load(std::memory_order_acquire) && result_buffer && original) {
    if (auto* backend = ActiveVrBackend()) {
      const auto pose = backend->PublishedHeadPose();
      {
        std::lock_guard<std::mutex> pose_lock(g_native_pose_mutex);
        g_native_pose = pose;
      }
      if (internal::ApplyXrPose(result_buffer, pose)) {
        backend->NotePoseApplied(device_object, pose.frame);
        static thread_local std::uint64_t last_logged = 0;
        if (pose.frame != last_logged && (pose.frame == 1 || pose.frame % 20 == 0)) {
          last_logged = pose.frame;
          Log("  [vr-device][evidence] pose-applied frame=%llu owner=%p "
              "pos=(%.4f,%.4f,%.4f) ori=(%.4f,%.4f,%.4f,%.4f) hands=%d/%d input=%d\n",
              static_cast<unsigned long long>(pose.frame), device_object,
              pose.position[0], pose.position[1], pose.position[2],
              pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3],
              pose.hands[0].grip_valid, pose.hands[1].grip_valid, pose.controllers_connected);
        }
      }
    }
  }
  if (active_.load(std::memory_order_acquire) && result_buffer != nullptr &&
      internal::InspectEyeResources(device_object) !=
          internal::EyeResourceState::kReady) {
    // Fail closed in the returned state, without changing guest ownership or
    // manufacturing readiness. Partial/failed resources must never be rendered.
    static_cast<std::uint8_t *>(
        result_buffer)[internal::kStateReadyOffsetInCopy] = 0;
  }
}

void RobloxVrDeviceBridge::TryInitializeEyes(void *self) {
  if (self == nullptr || !ValidateObjectHeader(self)) {
    Log("  [vr-device] eye-init aborted: object header no longer valid "
        "(self=%p)\n",
        self);
    return;
  }
  const auto *flag = reinterpret_cast<const std::uint8_t *>(
      image_base_.load(std::memory_order_acquire) +
      profile_.emulator_flag_storage_rva);
  if (*flag == 0) {
    bool expected = false;
    if (flag_warning_logged_.compare_exchange_strong(expected, true)) {
      Log("  [vr-device] eye-init deferred: FFlagDebugEnableVREmulator "
          "storage byte is 0\n");
    }
    return;
  }
  void *device = ValidatedGraphicsDevice(self);
  if (device == nullptr) {
    Log("  [vr-device] eye-init failed: graphics device at self+0x198 did "
        "not pass the machine contract (self=%p)\n",
        self);
    return;
  }
  captured_count_.fetch_add(1, std::memory_order_relaxed);
  auto *bytes = static_cast<std::uint8_t *>(self);
  Log("  [vr-device] captured DebugDeviceVR object self=%p device=%p\n", self,
      device);
  const auto initializer = reinterpret_cast<EyeInitializerFn>(
      image_base_.load(std::memory_order_acquire) +
      profile_.eye_initializer_rva);

  if (auto* backend = ActiveVrBackend()) {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (backend->RecommendedEyeExtent(&width, &height)) {
      *reinterpret_cast<std::uint32_t*>(bytes + kWidthOffset) = width;
      *reinterpret_cast<std::uint32_t*>(bytes + kHeightOffset) = height;
      Log("  [vr-device][evidence] using OpenXR recommended eye extent %ux%u\n",
          width, height);
    }
  }

  Log("  [vr-device][evidence] eye-init-begin self=%p device=%p tid=%" PRIu64
      " presents=%" PRIu64 "\n",
      self, device, CurrentThreadId(),
      present_count_.load(std::memory_order_relaxed));
  g_initializing_object = self;
  initializer(self, device);
  g_initializing_object = nullptr;
  Log("  [vr-device][evidence] eye-init-returned self=%p\n", self);

  const std::uint8_t ready = bytes[kReadyByteOffset];
  const void *framebuffer0 =
      *reinterpret_cast<void *const *>(bytes + kFramebufferSlotOffset);
  const void *framebuffer1 = *reinterpret_cast<void *const *>(
      bytes + kFramebufferSlotOffset + internal::kEyeSlotStride);
  const void *texture0 =
      *reinterpret_cast<void *const *>(bytes + kTextureSlotOffset);
  const void *texture1 = *reinterpret_cast<void *const *>(
      bytes + kTextureSlotOffset + internal::kEyeSlotStride);
  const std::uint32_t width =
      *reinterpret_cast<const std::uint32_t *>(bytes + kWidthOffset);
  const std::uint32_t height =
      *reinterpret_cast<const std::uint32_t *>(bytes + kHeightOffset);
  const bool slots_valid = framebuffer0 != nullptr && framebuffer1 != nullptr &&
                           texture0 != nullptr && texture1 != nullptr &&
                           framebuffer0 != framebuffer1 && texture0 != texture1;
  if (ready == 1 && slots_valid &&
      internal::InspectEyeResources(self) ==
          internal::EyeResourceState::kReady) {
    initialized_count_.fetch_add(1, std::memory_order_relaxed);
    Log("  [vr-device][evidence] eye-init-complete self=%p ready=1 fb0=%p "
        "fb1=%p tex0=%p tex1=%p width=%u height=%u\n",
        self, framebuffer0, framebuffer1, texture0, texture1, width, height);
    const auto *eye0_view =
        reinterpret_cast<const float *>(bytes + internal::kEye0ViewOffset);
    const auto *eye1_view =
        reinterpret_cast<const float *>(bytes + internal::kEye1ViewOffset);
    const auto *eye0_projection = reinterpret_cast<const float *>(
        bytes + internal::kEye0ProjectionOffset);
    const auto *eye1_projection = reinterpret_cast<const float *>(
        bytes + internal::kEye1ProjectionOffset);
    const auto *field_of_view =
        reinterpret_cast<const float *>(bytes + internal::kFieldOfViewOffset);
    const float half_ipd = *reinterpret_cast<const float *>(
        bytes + internal::kHalfInterpupillaryOffset);
    Log("  [vr-device][evidence] eye-params self=%p eye0_view=(%.4f,%.4f,"
        "%.4f) eye1_view=(%.4f,%.4f,%.4f) eye0_proj=(%.4f,%.4f,%.4f,%.4f) "
        "eye1_proj=(%.4f,%.4f,%.4f,%.4f) fov_deg=(%.1f,%.1f,%.1f,%.1f) "
        "half_ipd=%.4f\n",
        self, eye0_view[0], eye0_view[1], eye0_view[2], eye1_view[0],
        eye1_view[1], eye1_view[2], eye0_projection[0], eye0_projection[1],
        eye0_projection[2], eye0_projection[3], eye1_projection[0],
        eye1_projection[1], eye1_projection[2], eye1_projection[3],
        field_of_view[0], field_of_view[1], field_of_view[2], field_of_view[3],
        half_ipd);
  } else {
    Log("  [vr-device][evidence] eye-init-failed self=%p ready=%u fb0=%p "
        "fb1=%p tex0=%p tex1=%p width=%u height=%u\n",
        self, ready, framebuffer0, framebuffer1, texture0, texture1, width,
        height);
  }
}

void RobloxVrDeviceBridge::OnEyeGetterCall(void *device_object, int eye_index,
                                           void *framebuffer) {
  if (!active_.load(std::memory_order_acquire)) {
    return;
  }
  if (eye_index < 0 || eye_index > 1) {
    Log("  [vr-device] eye getter called with unexpected index %d self=%p\n",
        eye_index, device_object);
    return;
  }
  g_eye_object = device_object;
  g_eye_framebuffer = framebuffer;
  g_eye_index = eye_index;
  const std::uint64_t calls =
      eye_call_totals_[eye_index].fetch_add(1, std::memory_order_relaxed) + 1;
  if (calls <= 4) {
    Log("  [vr-device][evidence] eye-getter self=%p eye=%d framebuffer=%p "
        "call=%" PRIu64 " tid=%" PRIu64 "\n",
        device_object, eye_index, framebuffer, calls, CurrentThreadId());
  } else if (calls % kEyeGetterSummaryInterval == 0) {
    Log("  [vr-device][evidence] eye-getter-summary self=%p eye0=%" PRIu64
        " eye1=%" PRIu64 " last_framebuffer=%p\n",
        device_object, eye_getter_call_count(0), eye_getter_call_count(1),
        framebuffer);
  }
}

int RobloxVrDeviceBridge::captured_device_count() const {
  return captured_count_.load(std::memory_order_relaxed);
}

int RobloxVrDeviceBridge::initialized_device_count() const {
  return initialized_count_.load(std::memory_order_relaxed);
}

std::uint64_t RobloxVrDeviceBridge::eye_getter_call_count(int eye_index) const {
  if (eye_index < 0 || eye_index > 1) {
    return 0;
  }
  return eye_call_totals_[eye_index].load(std::memory_order_relaxed);
}

Status NotifyRobloxVrImageLoaded(std::uintptr_t image_base) {
  RobloxVrDeviceBridge *bridge =
      g_active_bridge.load(std::memory_order_acquire);
  if (bridge == nullptr) {
    return Status::Ok();
  }
  if (image_base == 0) {
    return FailedPrecondition(
        "Roblox VR device bridge is armed but libroblox base is unknown");
  }
  return bridge->Activate(image_base);
}

bool NativeControllerInputActive() {
  auto* bridge = g_active_bridge.load(std::memory_order_acquire);
  return bridge && bridge->active() && ActiveVrBackend();
}

} // namespace mocktail::vr

extern "C" bool mocktail_vr_native_controller_input_active() {
  // Query through the bridge's public ownership API below.
  return mocktail::vr::NativeControllerInputActive();
}
