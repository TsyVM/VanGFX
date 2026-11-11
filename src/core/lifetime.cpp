/**
 * src/core/lifetime.cpp — see compat/lifetime.hpp.
 */

#include <compat/lifetime.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vangfx::lifetime {

namespace {

enum class Holder : uint8_t { D3D12, D3D11, DXGI, D3D9 };

struct ListenerRec {
    Listener fn;
    void*    user;
    uint64_t tag;
};

// The private-data key. Derived from the address this code is loaded at, so two
// copies of VanGFX in one process (two plugins, each linking the library) keep
// separate sentinels: with one shared key the second would replace the first,
// and the first would report the object destroyed when it was not.
const GUID& private_data_key() noexcept {
    static const GUID key = [] {
        GUID g{ 0x5f1c0a7e, 0x2b94, 0x4c61, { 0 } };
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&private_data_key), &self);
        const auto base = reinterpret_cast<uint64_t>(self);
        for (int i = 0; i < 8; ++i)
            g.Data4[i] = static_cast<unsigned char>(base >> (8 * i)) ^ static_cast<unsigned char>(0xA5 + i);
        return g;
    }();
    return key;
}

class Sentinel;

std::mutex                                     g_mutex;
std::unordered_map<const void*, Sentinel*>     g_sentinels;

class Sentinel final : public IUnknown {
public:
    const void*              object  = nullptr;
    void*                    holder  = nullptr;   // the interface the sentinel was set on (weak)
    Holder                   kind    = Holder::D3D12;
    std::vector<ListenerRec> listeners;           // guarded by g_mutex
    std::atomic<bool>        detached{ false };

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0) {
            if (!detached.load(std::memory_order_acquire)) fire();
            delete this;
        }
        return left;
    }

private:
    // The object is being destroyed: its runtime is dropping private data.
    void fire() noexcept {
        std::vector<ListenerRec> to_call;
        {
            std::lock_guard lk(g_mutex);
            auto it = g_sentinels.find(object);
            if (it != g_sentinels.end() && it->second == this) g_sentinels.erase(it);
            to_call.swap(listeners);
        }
        for (const auto& l : to_call) l.fn(l.user, object, l.tag);
    }

    std::atomic<ULONG> refs_{ 1 };
};

// Attach `s` to `object` through whichever private-data slot it has.
bool attach(IUnknown* object, Sentinel* s) noexcept {
    const GUID& key = private_data_key();
    {
        ID3D12Object* o = nullptr;
        if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D12Object), reinterpret_cast<void**>(&o))) && o) {
            const HRESULT hr = o->SetPrivateDataInterface(key, s);
            s->holder = o; s->kind = Holder::D3D12;
            o->Release();
            return SUCCEEDED(hr);
        }
    }
    {
        ID3D11DeviceChild* o = nullptr;
        if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D11DeviceChild), reinterpret_cast<void**>(&o))) && o) {
            const HRESULT hr = o->SetPrivateDataInterface(key, s);
            s->holder = o; s->kind = Holder::D3D11;
            o->Release();
            return SUCCEEDED(hr);
        }
    }
    {
        IDXGIObject* o = nullptr;
        if (SUCCEEDED(object->QueryInterface(__uuidof(IDXGIObject), reinterpret_cast<void**>(&o))) && o) {
            const HRESULT hr = o->SetPrivateDataInterface(key, s);
            s->holder = o; s->kind = Holder::DXGI;
            o->Release();
            return SUCCEEDED(hr);
        }
    }
    {
        IDirect3DResource9* o = nullptr;
        if (SUCCEEDED(object->QueryInterface(__uuidof(IDirect3DResource9), reinterpret_cast<void**>(&o))) && o) {
            // With D3DSPD_IUNKNOWN the data pointer *is* the interface (the runtime
            // AddRefs it and Releases it when the resource is freed); the size is
            // that of the pointer.
            const HRESULT hr = o->SetPrivateData(key, static_cast<IUnknown*>(s), sizeof(IUnknown*), D3DSPD_IUNKNOWN);
            s->holder = o; s->kind = Holder::D3D9;
            o->Release();
            return SUCCEEDED(hr);
        }
    }
    return false;
}

void remove_from(Sentinel* s) noexcept {
    const GUID& key = private_data_key();
    switch (s->kind) {
    case Holder::D3D12: static_cast<ID3D12Object*>(s->holder)->SetPrivateDataInterface(key, nullptr); break;
    case Holder::D3D11: static_cast<ID3D11DeviceChild*>(s->holder)->SetPrivateDataInterface(key, nullptr); break;
    case Holder::DXGI:  static_cast<IDXGIObject*>(s->holder)->SetPrivateDataInterface(key, nullptr); break;
    case Holder::D3D9:  static_cast<IDirect3DResource9*>(s->holder)->FreePrivateData(key); break;
    }
}

} // namespace

bool watch(IUnknown* object, Listener fn, void* user, uint64_t tag) noexcept {
    if (!object || !fn) return false;
    std::lock_guard lk(g_mutex);

    if (auto it = g_sentinels.find(object); it != g_sentinels.end()) {
        for (auto& l : it->second->listeners)
            if (l.user == user) { l.fn = fn; l.tag = tag; return true; }
        it->second->listeners.push_back({ fn, user, tag });
        return true;
    }

    // The lock is held across the attach. The only calls it can make back into
    // this file are the sentinel's AddRef (lock-free), since the object had no
    // sentinel of ours to replace.
    auto* s = new (std::nothrow) Sentinel;
    if (!s) return false;
    s->object = object;
    s->listeners.push_back({ fn, user, tag });
    if (!attach(object, s)) {
        s->detached.store(true, std::memory_order_release);
        s->Release();
        return false;
    }
    g_sentinels.emplace(object, s);
    s->Release();   // the object holds the only reference now
    return true;
}

// Removing a listener leaves the sentinel in place, listening for nobody, until
// the object dies or the module unloads. Taking it off now would mean calling
// into the object, which the application may be destroying on another thread
// at this very moment; a sentinel with no listeners costs a few bytes.
void unwatch(const void* object, void* user) noexcept {
    std::lock_guard lk(g_mutex);
    auto it = g_sentinels.find(object);
    if (it == g_sentinels.end()) return;
    auto& ls = it->second->listeners;
    for (size_t i = 0; i < ls.size(); ++i)
        if (ls[i].user == user) { ls.erase(ls.begin() + static_cast<ptrdiff_t>(i)); break; }
}

void forget(void* user) noexcept {
    std::lock_guard lk(g_mutex);
    for (auto& [obj, s] : g_sentinels) {
        auto& ls = s->listeners;
        for (size_t i = 0; i < ls.size();)
            if (ls[i].user == user) ls.erase(ls.begin() + static_cast<ptrdiff_t>(i)); else ++i;
    }
}

bool watched(const void* object) noexcept {
    std::lock_guard lk(g_mutex);
    return g_sentinels.count(object) != 0;
}

uint32_t watched_count() noexcept {
    std::lock_guard lk(g_mutex);
    return static_cast<uint32_t>(g_sentinels.size());
}

void detach_all() noexcept {
    std::vector<Sentinel*> all;
    {
        std::lock_guard lk(g_mutex);
        all.reserve(g_sentinels.size());
        for (auto& [obj, s] : g_sentinels) {
            s->detached.store(true, std::memory_order_release);
            s->listeners.clear();
            s->AddRef();   // kept alive across remove_from below
            all.push_back(s);
        }
        g_sentinels.clear();
    }
    // Each object was alive a moment ago: it would have released its sentinel
    // (and taken it out of the map) had it been destroyed. Taking the private
    // data off makes the runtime release the sentinel, which then goes quietly.
    for (Sentinel* s : all) {
        remove_from(s);
        s->Release();
    }
}

namespace {

// When this code is in a DLL being unloaded, every sentinel still attached is
// a pointer into code about to disappear, released whenever its object dies.
// So they come off here. Not at process exit (RtlDllShutdownInProgress): the
// runtimes' objects are being torn down in no particular order then, and
// nothing will outlive the process to call a sentinel anyway.
struct UnloadGuard {
    ~UnloadGuard() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&watched_count), &self);
        if (!self || self == GetModuleHandleW(nullptr)) return;   // in the executable: never unloaded early
        using ShutdownFn = BOOLEAN(NTAPI*)();
        auto in_shutdown = reinterpret_cast<ShutdownFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress")));
        if (in_shutdown && in_shutdown()) return;
        detach_all();
    }
} g_unload_guard;

} // namespace

} // namespace vangfx::lifetime
