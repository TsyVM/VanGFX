#pragma once

/**
 * function_hook.hpp — x64 inline hook for named-export functions.
 *
 * Patches the first 14 bytes of a function with an absolute indirect JMP,
 * then allocates a small executable trampoline so callers can still invoke
 * the original code:
 *
 *   Target:
 *     FF 25 00 00 00 00          JMP QWORD PTR [RIP+0]
 *     <detour address, 8 bytes>
 *
 *   Trampoline (VirtualAlloc'd):
 *     <saved 14 bytes from target>
 *     FF 25 00 00 00 00          JMP QWORD PTR [RIP+0]
 *     <target+14, 8 bytes>
 *
 * Limitation: the saved 14 bytes must not contain relative branches or
 * RIP-relative memory references.  Well-known SDK exports (XAudio2Create,
 * DirectSoundCreate8) start with stack-frame setup that is always
 * position-independent, so this is safe in practice.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "../../../include/vangfx/core/error.hpp"
#include <cstdint>
#include <cstring>

namespace vangfx::function_hook {

static constexpr uint32_t kPatchSize = 14;

struct InlinePatch {
    void*   target    = nullptr; // original function address
    void*   trampoline = nullptr; // executable stub; call this to run original
    uint8_t saved[kPatchSize]{};
    bool    installed = false;

    // Install a jmp from target to detour.  The original code is callable
    // through the trampoline pointer afterwards.
    Result<void> install(void* detour) noexcept {
        if (!target || !detour)
            return gfx_err<void>(ShaderError::NotInitialized, "null target or detour");

        // Allocate executable trampoline memory.
        trampoline = ::VirtualAlloc(nullptr, 32,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
        if (!trampoline)
            return gfx_err<void>(ShaderError::OutOfMemory, "VirtualAlloc failed for trampoline");

        // Save original bytes.
        DWORD old_prot = 0;
        ::VirtualProtect(target, kPatchSize, PAGE_EXECUTE_READWRITE, &old_prot);
        std::memcpy(saved, target, kPatchSize);

        // Build trampoline: saved bytes + absolute jmp to target+kPatchSize.
        auto* stub = static_cast<uint8_t*>(trampoline);
        std::memcpy(stub, saved, kPatchSize);
        stub[kPatchSize + 0] = 0xFF;
        stub[kPatchSize + 1] = 0x25;
        stub[kPatchSize + 2] = stub[kPatchSize + 3] =
        stub[kPatchSize + 4] = stub[kPatchSize + 5] = 0x00; // [RIP+0]
        uintptr_t cont = reinterpret_cast<uintptr_t>(target) + kPatchSize;
        std::memcpy(stub + kPatchSize + 6, &cont, 8);
        ::FlushInstructionCache(::GetCurrentProcess(), trampoline, 28);

        // Patch the target function.
        auto* fn = static_cast<uint8_t*>(target);
        fn[0] = 0xFF; fn[1] = 0x25;
        fn[2] = fn[3] = fn[4] = fn[5] = 0x00; // [RIP+0]
        uintptr_t det = reinterpret_cast<uintptr_t>(detour);
        std::memcpy(fn + 6, &det, 8);
        ::FlushInstructionCache(::GetCurrentProcess(), target, kPatchSize);
        ::VirtualProtect(target, kPatchSize, old_prot, &old_prot);

        installed = true;
        return {};
    }

    void remove() noexcept {
        if (!installed) return;
        DWORD old_prot = 0;
        ::VirtualProtect(target, kPatchSize, PAGE_EXECUTE_READWRITE, &old_prot);
        std::memcpy(target, saved, kPatchSize);
        ::FlushInstructionCache(::GetCurrentProcess(), target, kPatchSize);
        ::VirtualProtect(target, kPatchSize, old_prot, &old_prot);
        ::VirtualFree(trampoline, 0, MEM_RELEASE);
        trampoline = nullptr;
        installed  = false;
    }

    ~InlinePatch() noexcept { remove(); }

    InlinePatch()                              = default;
    InlinePatch(const InlinePatch&)            = delete;
    InlinePatch& operator=(const InlinePatch&) = delete;
    InlinePatch(InlinePatch&& o) noexcept
        : target(o.target), trampoline(o.trampoline), installed(o.installed)
    {
        std::memcpy(saved, o.saved, kPatchSize);
        o.installed  = false;
        o.trampoline = nullptr;
    }
};

} // namespace vangfx::function_hook
