#pragma once

#include <string_view>
#include <functional>
#include <cstddef>

// ─────────────────────────────────────────────────────────────────────────────
//  VanGFX proxy layer
//
//  A proxy DLL built with the proxy kit:
//    - Exports every symbol the original DLL does (by name and ordinal).
//    - Forwards to the real DLL, loaded lazily on first call.
//    - Lets you override individual exports via spec.json "implement" entries.
//    - Supports chain loading: set [Proxy] Chain= in vgx.ini to load another
//      proxy before the system DLL.
//
//  Most proxy projects include only this header.
// ─────────────────────────────────────────────────────────────────────────────

namespace vangfx::proxy {

// ── SystemModule ──────────────────────────────────────────────────────────────
//
// Owns the handle to the proxied system DLL.  Loaded once, on first call.
//
// - 64-bit process: loads from System32.
// - 32-bit process: loads from SysWOW64 (System32 is redirected; we use the
//   WOW64 path explicitly to avoid the redirector).
// - If [Proxy] Chain= is set in vgx.ini, the chain DLL is loaded first and
//   becomes the "real" module.  It is responsible for loading the system DLL.
//
class SystemModule {
public:
    SystemModule() = default;
    ~SystemModule();

    // Load the system (or chain) DLL for dll_name (without extension).
    // Safe to call from multiple threads; loads only once.
    // Returns false if the DLL could not be loaded.
    bool load(std::string_view dll_name);

    // Returns the HMODULE, or nullptr if not loaded.
    [[nodiscard]] void* handle() const noexcept;

    // Look up a named export from the loaded module.
    [[nodiscard]] void* proc(std::string_view name) const noexcept;

    // Look up an ordinal-only export.
    [[nodiscard]] void* proc_by_ordinal(unsigned short ordinal) const noexcept;

    // True after a successful load().
    [[nodiscard]] bool loaded() const noexcept;

private:
    void*       handle_  = nullptr;
    mutable int once_    = 0;   // guards load(); 0=unloaded, 1=loading, 2=loaded

    void do_load(std::string_view dll_name);
};

// ── Slot resolution ───────────────────────────────────────────────────────────
//
// Called by the generated exports_table.cpp / asm resolver to fill a slot.
// You do not call this directly.
//
// slot     — pointer to the void* that holds the lazy-resolver address.
// dll_name — name of the proxied DLL (e.g. "d3d9.dll").
//
// Returns the resolved function pointer and fills *slot so subsequent calls
// bypass the resolver entirely.
[[nodiscard]] void* resolve_slot(void** slot, std::string_view dll_name);

// Named and ordinal variants called from generated exports_table.cpp.
[[nodiscard]] void* resolve_named_slot  (void** slot, std::string_view dll_name,
                                          std::string_view   export_name);
[[nodiscard]] void* resolve_ordinal_slot(void** slot, std::string_view dll_name,
                                          unsigned short     ordinal);

// ── Logging ───────────────────────────────────────────────────────────────────
void log(std::string_view message);

// ── Typed real() helper ───────────────────────────────────────────────────────
//
// Retrieve the original function pointer for an export you have overridden.
//
//   auto real = vangfx::proxy::real<decltype(&Direct3DCreate9)>("Direct3DCreate9");
//   IDirect3D9* d3d = real(sdk);
//
template<typename FnPtr>
[[nodiscard]] FnPtr real(std::string_view name);

// ── On-first-call registration ────────────────────────────────────────────────
//
// Register a callback to run once, on the first call into any export,
// before the resolver fills the slot.  You call this via the macro below.
void register_first_call(std::function<void()> fn);

} // namespace vangfx::proxy

// ── VGX_PROXY_ON_FIRST_CALL() ─────────────────────────────────────────────────
//
// Convenience macro.  Place in proxy_main.cpp:
//
//   VGX_PROXY_ON_FIRST_CALL() {
//       vangfx::proxy::log("hello");
//   }
//
#define VGX_PROXY_ON_FIRST_CALL()                                               \
    static void vgx_first_call_body_();                                         \
    namespace {                                                                  \
        const int vgx_first_call_reg_ = (                                       \
            vangfx::proxy::register_first_call(&vgx_first_call_body_), 0);     \
    }                                                                            \
    static void vgx_first_call_body_()
