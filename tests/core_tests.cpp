/**
 * tests/core_tests.cpp — the hook foundation on its own: hooks::Site against
 * a class whose vtable lives in another DLL (shared by every object, like a COM
 * class's in d3d11.dll), and the lifetime service against real D3D12 objects.
 */

#include "harness.hpp"
#include "fake_runtime.hpp"
#include <compat/hook_site.hpp>
#include <compat/lifetime.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using namespace vgx;

namespace {

using ValueFn = int (__stdcall*)(IWidget*, int);
vangfx::hooks::Site s_value{ 0, "IWidget::value", vangfx::hooks::Heal::SameModule };
vangfx::hooks::Site s_other{ 0, "IWidget::value (another tool)" };
int g_seen = 0, g_other_seen = 0;

int __stdcall hook_value(IWidget* self, int x) {
    ++g_seen;
    const int r = s_value.original<ValueFn>(self)(self, x);
    s_value.heal(self);
    return r;
}

// Another tool's hook, in a module other than the runtime's (this one).
int __stdcall other_value(IWidget* self, int x) {
    ++g_other_seen;
    return s_other.original<ValueFn>(self)(self, x) * 10;
}

int call(IWidget* w, int x) {
    IWidget* volatile p = w;   // through the vtable, as a game calls
    return p->value(x);
}

void** vt(IWidget* w) { return vangfx::hooks::vtable_of(w); }

void write(void** where, void* value) {
    DWORD old = 0;
    VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old);
    *reinterpret_cast<void* volatile*>(where) = value;
    VirtualProtect(where, sizeof(void*), old, &old);
}

struct Widgets {
    IWidget* a = vgx_make_widget();
    IWidget* b = vgx_make_widget();
    ~Widgets() { a->destroy(); b->destroy(); }
};

} // namespace

// Installed through one object, the hook serves every object of the class; a
// second install is a no-op rather than a hook that calls itself.
VGX_TEST(core_site_shared_vtable) {
    Widgets w;
    void* original = vt(w.a)[0];
    CHECK(vt(w.a) == vt(w.b));
    CHECK(s_value.install(w.a, &hook_value));
    CHECK(s_value.install(w.b, &hook_value));
    CHECK(s_value.install(w.a, &hook_value));
    CHECK_EQ(s_value.vtable_count(), 1);
    g_seen = 0;
    CHECK_EQ(call(w.a, 1), 2);
    CHECK_EQ(call(w.b, 5), 6);
    CHECK_EQ(g_seen, 2);
    s_value.uninstall();
    CHECK(vt(w.a)[0] == original);
    CHECK_EQ(call(w.b, 1), 2);
    CHECK_EQ(g_seen, 2);
    CHECK(s_value.install(w.b, &hook_value));   // and back again
    CHECK_EQ(call(w.a, 1), 2);
    CHECK_EQ(g_seen, 3);
    s_value.uninstall();
}

// The runtime swapping in another of its own functions is healed over and the
// swapped-in function adopted; a value from another module (another tool's
// hook) is left alone.
VGX_TEST(core_site_heal) {
    Widgets w;
    void* original = vt(w.a)[0];
    CHECK(s_value.install(w.a, &hook_value));
    write(&vt(w.a)[0], reinterpret_cast<void*>(&vgx_alternate_value));   // the runtime's swap
    g_seen = 0;
    CHECK_EQ(call(w.a, 1), 3);       // alternate ran, the hook did not see it...
    CHECK_EQ(g_seen, 0);
    s_value.heal(w.a);               // ...until healed (the present and flush hooks do this)
    CHECK(vt(w.a)[0] == reinterpret_cast<void*>(&hook_value));
    CHECK_EQ(call(w.a, 1), 3);       // the hook now chains to the adopted function
    CHECK_EQ(g_seen, 1);

    write(&vt(w.a)[0], reinterpret_cast<void*>(&GetTickCount));   // another module: someone else's
    s_value.heal(w.a);
    CHECK(vt(w.a)[0] == reinterpret_cast<void*>(&GetTickCount));
    write(&vt(w.a)[0], reinterpret_cast<void*>(&hook_value));
    s_value.uninstall();
    write(&vt(w.a)[0], original);
}

// Another tool hooks on top of ours. Taking ours out must not cut theirs off,
// and healing must not mistake theirs for the runtime's.
VGX_TEST(core_site_stacked) {
    Widgets w;
    void* original = vt(w.a)[0];
    CHECK(s_value.install(w.a, &hook_value));
    CHECK(s_other.install(w.a, &other_value));
    g_seen = g_other_seen = 0;
    CHECK_EQ(call(w.a, 1), 20);
    CHECK_EQ(g_seen, 1);
    CHECK_EQ(g_other_seen, 1);
    CHECK(vt(w.a)[0] == reinterpret_cast<void*>(&other_value));   // heal left theirs alone
    s_value.uninstall();
    CHECK(vt(w.a)[0] == reinterpret_cast<void*>(&other_value));
    CHECK_EQ(call(w.a, 1), 20);
    CHECK_EQ(g_other_seen, 2);
    s_other.uninstall();               // theirs goes: the slot is ours again, still passing through
    CHECK(vt(w.a)[0] == reinterpret_cast<void*>(&hook_value));
    CHECK_EQ(call(w.a, 1), 2);
    s_value.uninstall();               // and now ours can go
    CHECK(vt(w.a)[0] == original);
    CHECK_EQ(call(w.a, 1), 2);
}

namespace {
// A class of this module: its "runtime" and any hook on it share a module.
struct Local {
    virtual int __stdcall value(int x) { return x + 1; }
    virtual ~Local() = default;
};
int __stdcall local_other(Local*, int x) { return x + 5; }
using LocalFn = int (__stdcall*)(Local*, int);
vangfx::hooks::Site s_local{ 0, "Local::value", vangfx::hooks::Heal::SameModule };
int __stdcall hook_local(Local* self, int x) { return s_local.original<LocalFn>(self)(self, x); }
}

// A runtime in the same module as VanGFX is never healed over: there, a swapped
// slot cannot be told from a second hook, and adopting a hook loops forever.
VGX_TEST(core_site_heal_own_module) {
    Local l;
    // Volatile: this vtable is a constant object of this module, and the
    // optimiser folds plain loads from it to the functions it was built with.
    void* volatile* t = reinterpret_cast<void* volatile*>(vangfx::hooks::vtable_of(&l));
    void* original = t[0];
    CHECK(s_local.install(&l, &hook_local));
    CHECK(t[0] == reinterpret_cast<void*>(&hook_local));
    write(const_cast<void**>(&t[0]), reinterpret_cast<void*>(&local_other));
    s_local.heal(&l);
    CHECK(t[0] == reinterpret_cast<void*>(&local_other));
    write(const_cast<void**>(&t[0]), reinterpret_cast<void*>(&hook_local));
    s_local.uninstall();
    CHECK(t[0] == original);
}

// The lifetime service reports each object's destruction once, to each
// listener, with its tag, without hooking Release.
VGX_TEST(core_lifetime) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter> warp;
    factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    ComPtr<ID3D12Device> dev;
    CHECK(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))));

    struct Heard { const void* object; uint64_t tag; int n; };
    static Heard a{}, b{};
    auto listen_a = [](void*, const void* o, uint64_t t) noexcept { a.object = o; a.tag = t; ++a.n; };
    auto listen_b = [](void*, const void* o, uint64_t t) noexcept { b.object = o; b.tag = t; ++b.n; };
    char key_a = 0, key_b = 0;

    const uint32_t before = vangfx::lifetime::watched_count();
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    CHECK(vangfx::lifetime::watch(fence, listen_a, &key_a, 7));
    CHECK(vangfx::lifetime::watch(fence, listen_b, &key_b, 9));
    CHECK(vangfx::lifetime::watch(fence, listen_a, &key_a, 8));   // again: replaces the tag
    CHECK_EQ(vangfx::lifetime::watched_count(), before + 1);
    fence->AddRef();
    fence->Release();
    CHECK_EQ(a.n, 0);
    const void* addr = fence;
    fence->Release();
    CHECK_EQ(a.n, 1);
    CHECK_EQ(b.n, 1);
    CHECK(a.object == addr);
    CHECK_EQ(a.tag, 8ull);
    CHECK_EQ(b.tag, 9ull);
    CHECK_EQ(vangfx::lifetime::watched_count(), before);

    // forget(): no more reports to that listener.
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    CHECK(vangfx::lifetime::watch(fence, listen_a, &key_a, 1));
    vangfx::lifetime::forget(&key_a);
    fence->Release();
    CHECK_EQ(a.n, 1);
}
