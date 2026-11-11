#pragma once
// VanGFX stub — MWOn12's plugin ABI is not part of the shared translator.
// All calls compile to no-ops; a future VanGFX plugin system will replace this.

#include <windows.h>

struct ID3D12Resource;

namespace vangfx::d3d9to12 {
class D9Device12;
}

namespace vangfx::d3d9to12::sdk {
namespace PluginHost {

inline void NoteGameDirectory(const char*) noexcept {}
inline void EnsureLoaded()   noexcept {}
[[nodiscard]] inline bool WantsPresent() noexcept { return false; }
[[nodiscard]] inline bool Any()          noexcept { return false; }
inline void OnDeviceCreated (D9Device12*) noexcept {}
inline void OnResize        (D9Device12*) noexcept {}
inline void OnPresent       (D9Device12*, ID3D12Resource*, UINT64) noexcept {}
inline void OnDeviceDestroyed() noexcept {}
inline void OnShutdown()     noexcept {}

} // namespace PluginHost
} // namespace vangfx::d3d9to12::sdk
