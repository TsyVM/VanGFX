#include <vangfx/proxy/proxy.hpp>
#include <vangfx/core/config.hpp>

#include <windows.h>
#include <pathcch.h>

#include <atomic>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "pathcch.lib")

namespace vangfx::proxy {

// ── Global state ──────────────────────────────────────────────────────────────

namespace {

// One SystemModule per proxied DLL name.  In practice a proxy DLL has only one.
static SystemModule* s_system_module = nullptr;

static std::once_flag          s_first_call_flag;
static std::vector<std::function<void()>> s_first_call_fns;
static std::mutex              s_first_call_mutex;

// Simple log: OutputDebugStringA + optionally a file later.
void write_log(std::string_view msg) {
    std::string out = std::format("[VanGFX proxy] {}\n", msg);
    OutputDebugStringA(out.c_str());
}

// Return the directory of the current process executable.
std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathCchRemoveFileSpec(path, MAX_PATH);
    return path;
}

// Read [Proxy] Chain from <exe_dir>\VanGFX\vgx.ini.
std::string read_chain(const std::string& dll_name) {
    Config cfg;
    std::wstring ed = exe_dir();
    cfg.load(std::filesystem::path(ed) / L"VanGFX" / L"vgx.ini");
    return cfg.get("Proxy", "Chain");
}

} // namespace

// ── SystemModule ──────────────────────────────────────────────────────────────

SystemModule::~SystemModule() {
    if (handle_) FreeLibrary(static_cast<HMODULE>(handle_));
}

void SystemModule::do_load(std::string_view dll_name) {
    // 1. Check for a chain DLL first.
    std::string chain = read_chain(std::string(dll_name));
    if (!chain.empty()) {
        // Chain DLL is relative to the game directory (beside the proxy).
        wchar_t proxy_dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, proxy_dir, MAX_PATH);
        PathCchRemoveFileSpec(proxy_dir, MAX_PATH);
        std::filesystem::path chain_path =
            std::filesystem::path(proxy_dir) / chain;
        handle_ = LoadLibraryW(chain_path.c_str());
        if (handle_) {
            write_log(std::format("chain loaded: {}", chain));
            return;
        }
        write_log(std::format("chain load failed: {} (falling through to system)", chain));
    }

    // 2. Load from System32 / SysWOW64.
    wchar_t sys_dir[MAX_PATH] = {};
#ifdef _WIN64
    GetSystemDirectoryW(sys_dir, MAX_PATH);
#else
    // In a 32-bit process, GetSystemDirectoryW returns the redirected
    // System32 (which is actually SysWOW64).  Use the explicit WOW64 path
    // so we never load the wrong bitness of the system DLL.
    GetSystemWow64DirectoryW(sys_dir, MAX_PATH);
    if (sys_dir[0] == L'\0')
        GetSystemDirectoryW(sys_dir, MAX_PATH); // non-WOW64 machine (rare)
#endif

    std::wstring name_w(dll_name.begin(), dll_name.end());
    std::filesystem::path full = std::filesystem::path(sys_dir) / (name_w + L".dll");
    handle_ = LoadLibraryW(full.c_str());
    if (handle_) {
        write_log(std::format("system DLL loaded: {}", full.string()));
    } else {
        write_log(std::format("FAILED to load system DLL: {} (error {})",
                              full.string(), GetLastError()));
    }
}

bool SystemModule::load(std::string_view dll_name) {
    std::call_once(*reinterpret_cast<std::once_flag*>(&once_),
                   [this, dll_name]() { do_load(dll_name); });
    return handle_ != nullptr;
}

void*  SystemModule::handle()           const noexcept { return handle_; }
bool   SystemModule::loaded()           const noexcept { return handle_ != nullptr; }

void* SystemModule::proc(std::string_view name) const noexcept {
    if (!handle_) return nullptr;
    return GetProcAddress(static_cast<HMODULE>(handle_),
                          std::string(name).c_str());
}

void* SystemModule::proc_by_ordinal(unsigned short ordinal) const noexcept {
    if (!handle_) return nullptr;
    return GetProcAddress(static_cast<HMODULE>(handle_),
                          MAKEINTRESOURCEA(ordinal));
}

// ── Slot resolution ───────────────────────────────────────────────────────────

void* resolve_slot(void** slot, std::string_view dll_name) {
    // Trigger the on-first-call callbacks exactly once.
    std::call_once(s_first_call_flag, [&]() {
        std::lock_guard lock(s_first_call_mutex);
        for (auto& fn : s_first_call_fns) fn();
        s_first_call_fns.clear();
    });

    if (!s_system_module) {
        // Allocate on first use; never freed (intentional: proxy lives forever).
        static SystemModule mod;
        s_system_module = &mod;
        s_system_module->load(dll_name);
    }

    // The slot name encodes whether this is a named or ordinal export.
    // We find the right proc by walking the slot address back to its name.
    // (In the generated exports_table.cpp every slot is a named extern, and
    //  the resolver receives the slot's address.  We look it up by ordinal
    //  or name via the exports_table symbol name — but that requires
    //  per-slot metadata.  For now, resolve_slot is called with the DLL name
    //  only; the generated exports_table.cpp must call a per-slot overload.)
    //
    // The actual per-slot lookup is done in the generated exports_table.cpp:
    // it calls resolve_named_slot / resolve_ordinal_slot below.
    return nullptr; // replaced by the per-slot overloads
}

void* resolve_named_slot(void** slot, std::string_view dll_name,
                          std::string_view export_name) {
    std::call_once(s_first_call_flag, [&]() {
        std::lock_guard lock(s_first_call_mutex);
        for (auto& fn : s_first_call_fns) fn();
        s_first_call_fns.clear();
    });

    if (!s_system_module) {
        static SystemModule mod;
        s_system_module = &mod;
        s_system_module->load(dll_name);
    }

    void* fn = s_system_module ? s_system_module->proc(export_name) : nullptr;
    if (fn) *slot = fn;
    else {
        write_log(std::format("WARN: export not found: {}", export_name));
    }
    return fn;
}

void* resolve_ordinal_slot(void** slot, std::string_view dll_name,
                            unsigned short ordinal) {
    std::call_once(s_first_call_flag, [&]() {
        std::lock_guard lock(s_first_call_mutex);
        for (auto& fn : s_first_call_fns) fn();
        s_first_call_fns.clear();
    });

    if (!s_system_module) {
        static SystemModule mod;
        s_system_module = &mod;
        s_system_module->load(dll_name);
    }

    void* fn = s_system_module ? s_system_module->proc_by_ordinal(ordinal) : nullptr;
    if (fn) *slot = fn;
    else {
        write_log(std::format("WARN: ordinal not found: {}", ordinal));
    }
    return fn;
}

// ── Logging ───────────────────────────────────────────────────────────────────

void log(std::string_view message) { write_log(message); }

// ── On-first-call registration ────────────────────────────────────────────────

void register_first_call(std::function<void()> fn) {
    std::lock_guard lock(s_first_call_mutex);
    s_first_call_fns.push_back(std::move(fn));
}

} // namespace vangfx::proxy
