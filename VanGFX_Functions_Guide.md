# VanGFX Functions Guide

Full API reference, architecture notes, and worked examples for VanGFX.

---

## Table of Contents

- [ContextBuilder](#contextbuilder)
- [Context](#context)
- [FrameContext](#framecontext)
- [ShaderBuilder](#shaderbuilder)
- [Effect API](#effect-api)
  - [replace_ps / replace_vs / replace_effect](#replace_ps--replace_vs--replace_effect)
  - [inject_ps_post / inject_ps_pre](#inject_ps_post--inject_ps_pre)
  - [ScopedShaderReplace](#scopedshaderreplace)
  - [ScopedInjection](#scopedinjection)
  - [ReplaceFilter](#replacefilter)
  - [ReplaceOptions](#replaceoptions)
- [Uniforms](#uniforms)
- [CaptureSession](#capturesession)
- [GBufferCapture](#gbuffercapture)
- [D3D12 Extensions](#d3d12-extensions)
  - [PSOCreateEvent](#psocreateevent)
  - [CommandListEvent](#commandlistevent)
  - [ResourceBarrier](#resourcebarrier)
  - [DescriptorHeap & RootBinding](#descriptorhea--rootbinding)
- [Event Structs](#event-structs)
  - [DrawEvent](#drawevent)
  - [RenderTargetEvent](#rendertargetevent)
  - [PresentEvent](#presentevent)
  - [ResourceEvent](#resourceevent)
  - [StateChangeEvent](#statechangeevent)
  - [QueryEvent](#queryevent)
- [Error Handling](#error-handling)
- [Handle Types](#handle-types)
- [Compat Layer](#compat-layer)

---

## ContextBuilder

Fluent builder for constructing a `Context`. Call `.build()` exactly once at startup.

```cpp
#include <vangfx/vangfx.hpp>

auto ctx = vangfx::ContextBuilder{}
    .backend(vangfx::BackendKind::Auto)       // Auto | D3D9 | D3D11 | D3D12
    .log_level(vangfx::LogLevel::Info)        // Off | Error | Warn | Info | Debug | Trace
    .log_callback([](vangfx::LogLevel lvl, std::string_view msg) { /* ... */ })
    .d3d12_device(ptr)                        // required on D3D12: ID3D12Device*
    .d3d12_command_queue(ptr)                 // required on D3D12: ID3D12CommandQueue*
    .d3d12_swap_chain(ptr)                    // optional: IDXGISwapChain* for on_present
    .d3d12_min_shader_model(60)               // fail-fast if device is below SM 6.0
    .auto_capture(true)                       // begin CaptureSession on first Present
    .build();                                 // Result<unique_ptr<Context>>

if (!ctx) {
    printf("init failed: %s\n", ctx.error().message.c_str());
}
```

**The device is not optional.** VanGFX does not go looking for one, because on
D3D11 and D3D12 there is nothing to look in: neither API keeps a global the way
Direct3D 9 did, and a device cannot be asked which queues or swap chains were
created from it. Supply them from wherever you already have them — your own
hook, a proxy DLL, or the host that loaded you. `build()` returns
`ShaderError::NotInitialized` with a message naming what is missing.

What each backend needs:

| Backend | Required | Optional |
|---|---|---|
| D3D9  | `d3d9_device` | — |
| D3D11 | `d3d11_device`, `d3d11_swap_chain` | — |
| D3D12 | `d3d12_device`, `d3d12_command_queue` | `d3d12_swap_chain` — without it `on_present` never fires |

**`BackendKind::Auto` detection order:** D3D12 → D3D11 → D3D9 (first loaded DLL wins).

---

## Context

The root object returned by `ContextBuilder::build()`. Owns all vtable patches — patches are removed when the `Context` is destroyed.

### Callback registration

```cpp
ctx->on_draw(fn);            // DrawCallback
ctx->on_render_target(fn);   // RenderTargetCallback
ctx->on_present(fn);         // PresentCallback
ctx->on_resource(fn);        // ResourceCallback
ctx->on_state_change(fn);    // StateCallback
ctx->on_query(fn);           // QueryCallback
ctx->on_device_lost(fn);     // void()
ctx->on_device_reset(fn);    // void(FrameContext&)

// D3D12 only
ctx->on_command_list(fn);    // CommandListCallback
ctx->on_pso_create(fn);      // PSOCreateCallback
ctx->on_resource_barrier(fn); // ResourceBarrierCallback
```

### Shader / effect management

```cpp
auto shader = ctx->compile_shader(builder);
auto effect = ctx->link_effect(vs, ps);
auto cs_eff = ctx->link_compute_effect(cs);
ctx->destroy(effect->handle);
ctx->destroy(shader->handle);
```

### Capture control

```cpp
ctx->start_capture();
ctx->stop_capture();
bool active = ctx->capture_active();
```

### Backend queries

```cpp
vangfx::BackendKind bk  = ctx->backend();
uint32_t d9ver           = ctx->d3d9_ver();
uint32_t d11fl           = ctx->d3d11_feature_level();  // e.g. 0xB100
uint32_t d12sm           = ctx->d3d12_shader_model();   // e.g. 66
uint32_t d12rt           = ctx->d3d12_raytracing_tier();
uint32_t d12ms           = ctx->d3d12_mesh_shader_tier();
```

---

## FrameContext

Passed to every callback. Exposes per-frame state. Never stored across frames.

```cpp
frame.backend();           // BackendKind
frame.frame_index();       // uint64_t — monotonic present counter
frame.surfaces();          // FrameSurfaces { width, height, back_buffer_format }
frame.pipeline();          // PipelineState snapshot
frame.format_of(rt);       // SurfaceFormat of a live RenderTargetHandle
frame.format_of(db);       // SurfaceFormat of a live DepthBufferHandle
frame.format_of(tex);      // SurfaceFormat of a live TextureHandle
frame.capture_active();    // bool
frame.latest_capture();    // Result<GBufferCapture>

// Resource creation
auto tex = frame.create_texture(desc);         // Result<TextureHandle>
auto rt  = frame.create_render_target(desc);   // Result<RenderTargetHandle>
auto db  = frame.create_depth_buffer(desc);    // Result<DepthBufferHandle>
frame.destroy(tex.value());
frame.destroy(rt.value());
frame.destroy(db.value());

// Effect binding
frame.bind_effect(effect_handle);
frame.unbind_effect();
frame.update_constant_buffer(cb, ptr, size);

// CPU readback — images
auto rb = frame.readback(rt_handle);  // Result<ReadbackData>
// rb->pixels    — std::span<const uint8_t>
// rb->row_pitch — bytes per scanline

// CPU readback — buffers (all three backends)
// Overloads for VertexBufferHandle, IndexBufferHandle, ConstantBufferHandle,
// StructuredBufferHandle, RawBufferHandle and UAVHandle. Handles come from
// ResourceEvent. offset/size are bytes; size 0 reads to the end, and both are
// clamped to the buffer.
auto bd = frame.readback(ib_handle, 0, 64);   // Result<BufferData>
// bd->bytes       — std::span<const uint8_t>
// bd->offset      — where in the source it came from
// bd->buffer_size — how big the whole source is
//
// Each call blocks until the copy has finished. Ask for the window you need:
// copying a 4 MB vertex buffer to read 64 bytes costs the same stall as
// copying the 64 bytes.

// From a draw to the geometry behind it (D3D12 only)
// DrawEvent carries vertex/index buffer VIEWS -- GPU virtual addresses, not
// handles. buffer_at resolves one to the buffer containing it.
auto ref = frame.buffer_at(ev.d3d12_ib.view.gpu_virtual_address);
if (ref) {                                  // ref.id == 0 means not found
    auto r = frame.readback(ref.as_index_buffer(), ref.offset, 96);
}
// ref.offset is where the address sits inside the buffer, usually not zero:
// a game packs many meshes into one buffer, so id+offset together identify a
// mesh and are stable across frames -- the pair to cache.
auto r2 = frame.readback_at(addr, 96);      // resolve + read in one call
```

---

## ShaderBuilder

Compiles HLSL or wraps pre-compiled bytecode.

```cpp
// HLSL from string
auto s = vangfx::ShaderBuilder{}
    .source(hlsl_string)
    .entry("PSMain")              // default "main"
    .profile("ps_5_0")            // auto-selected if omitted
    .define("MY_DEFINE", "1")
    .include_dir("shaders/")
    .optimization(3)              // 0 = off, 1–3 = O1–O3
    .debug_info(false)
    .use_dxc(false)               // force DXC for SM 6.x
    .validate_dxil(true)          // DXIL validation pass (D3D12 only)
    .root_signature_define("RS")  // -rootsig-define forwarded to DXC
    .build(*ctx);                 // Result<Shader>

// HLSL from file
auto s2 = vangfx::ShaderBuilder{}
    .source_file("shaders/vs.hlsl")
    .entry("VSMain")
    .profile("vs_5_0")
    .build(*ctx);

// Pre-compiled blob
auto s3 = vangfx::ShaderBuilder{}
    .bytecode(blob_ptr, blob_size)
    .stage(vangfx::ShaderStage::Pixel)
    .build(*ctx);

if (!s) {
    printf("%s\n", s.error().message.c_str());  // full compiler diagnostic
}
```

**Compiler selection:**
- D3D9 / D3D11 → `d3dcompiler_47.dll` (runtime-loaded via `LoadLibrary`)
- D3D12, `ps_6_*` / `vs_6_*` etc. → `dxcompiler.dll` + `dxil.dll` (runtime-loaded)
- Both DLLs must be present on the system; VanGFX does not ship them.

---

## Effect API

### `replace_ps` / `replace_vs` / `replace_effect`

```cpp
vangfx::EffectHandle h = vangfx::replace_ps(*ctx, hlsl_source);
vangfx::EffectHandle h = vangfx::replace_ps(*ctx, hlsl_source, filter);
vangfx::EffectHandle h = vangfx::replace_ps(*ctx, hlsl_source, filter, opts);

vangfx::EffectHandle h = vangfx::replace_vs(*ctx, hlsl_source);
vangfx::EffectHandle h = vangfx::replace_effect(*ctx, vs_hlsl, ps_hlsl);

vangfx::remove_replacement(*ctx, h);
```

### `inject_ps_post` / `inject_ps_pre`

```cpp
vangfx::InjectionHandle h = vangfx::inject_ps_post(*ctx, snippet);
vangfx::InjectionHandle h = vangfx::inject_ps_post(*ctx, snippet, filter);
vangfx::InjectionHandle h = vangfx::inject_ps_pre(*ctx, snippet);

vangfx::remove_injection(*ctx, h);
```

**`inject_ps_post` snippet environment:**

| Symbol | Type | Description |
|---|---|---|
| `color` | `float4` | Pixel output — modify in-place |
| `tex` | `Texture2D` | Game's bound texture (slot t0) |
| `samp` | `SamplerState` | Game's bound sampler (slot s0) |
| `vangfx_uv` | `float2` | Screen UV [0,1]² |
| `vangfx_time` | `float` | Seconds since context creation |
| `vangfx_delta` | `float` | Seconds since last frame |
| `vangfx_resolution` | `float2` | Back-buffer width × height |
| `vangfx_float4[8]` | `float4[]` | User uniform slots 0–7 |

**`inject_ps_pre` snippet environment:** same except `color` is not available; modify `vangfx_uv` to warp UV before sampling.

### `ScopedShaderReplace`

```cpp
// RAII — replacement removed when guard goes out of scope or reset() is called.
auto g = vangfx::ScopedShaderReplace::ps(*ctx, hlsl, filter);
auto g = vangfx::ScopedShaderReplace::vs(*ctx, hlsl, filter);
auto g = vangfx::ScopedShaderReplace::effect(*ctx, vs_hlsl, ps_hlsl, filter);

g.valid();   // bool
g.reset();   // explicit restore
```

### `ScopedInjection`

```cpp
auto g = vangfx::ScopedInjection::post(*ctx, snippet, filter);
auto g = vangfx::ScopedInjection::pre(*ctx, snippet, filter);
g.reset();
```

### `ReplaceFilter`

```cpp
vangfx::ReplaceFilter f;
f.min_verts   = 1000;                    // minimum vertex count
f.max_verts   = 0;                       // 0 = no upper limit
f.rt_width    = 1920;                    // exact RT width match (0 = any)
f.rt_height   = 1080;                    // exact RT height match (0 = any)
f.predicate   = [](const vangfx::FrameContext& frame,
                   const vangfx::DrawEvent& ev) -> bool {
    return ev.instance_count > 1;
};
```

### `ReplaceOptions`

```cpp
struct ReplaceOptions {
    std::string entry   = "main";
    std::string profile = "";       // "" = auto-select for backend + stage
    bool        use_dxc = false;
    bool        debug   = false;
};
```

---

## Uniforms

Drive live values from C++ into every shader installed via the Effect API.

```cpp
vangfx::Uniforms u(*ctx);  // one instance per context, shared across all effects

u.set(0, 0.75f);                        // float → vangfx_float4[0].x
u.set(1, 1.0f, 0.5f, 0.2f);            // xyz → vangfx_float4[1].xyz
u.set(2, vangfx::Float4{r, g, b, a});  // full float4

vangfx::Float4 v = u.get(0);           // CPU-side readback (no GPU transfer)
u.flush();                              // force immediate GPU upload
u.reset();                              // zero all 8 slots
```

**HLSL — always available in replacement / injection shaders:**

```hlsl
cbuffer vangfx_params : register(b0) {
    float4 vangfx_float4[8];
    float  vangfx_time;
    float  vangfx_delta;
    float2 vangfx_resolution;
};
```

Slot indices 0–7 correspond to `vangfx_float4[0]`–`vangfx_float4[7]`.

---

## CaptureSession

```cpp
vangfx::CaptureConfig cfg;
cfg.max_frames    = 5;
cfg.cpu_readback  = true;
cfg.output_dir    = "captures/";
cfg.output_format = vangfx::CaptureFormat::PNG;  // PNG | EXR* | DDS | Raw

cfg.capture_layer(vangfx::GBufferLayerKind::Albedo)
   .capture_layer(vangfx::GBufferLayerKind::Normal)
   .capture_layer(vangfx::GBufferLayerKind::Depth);

cfg.on_frame = [](const vangfx::GBufferCapture& cap) {
    for (auto& layer : cap.layers) {
        printf("layer %d: %ux%u  %zu bytes\n",
            (int)layer.kind, layer.width, layer.height,
            layer.pixels.size_bytes());
    }
};

auto session = vangfx::CaptureSession::begin(*ctx, cfg).value();
// RAII: destructor calls stop_capture()

session.stop();  // explicit stop
```

\* EXR output falls back to Raw with a logged warning — tinyexr integration is deferred.

---

## GBufferCapture

```cpp
const vangfx::GBufferCapture& cap;

cap.frame_index;    // uint64_t
cap.layers;         // std::vector<GBufferLayer>

// Find a specific layer
const vangfx::GBufferLayer* albedo = cap.find(vangfx::GBufferLayerKind::Albedo);
if (albedo && albedo->available) {
    std::span<const uint8_t> px = albedo->pixels;
    uint32_t pitch = albedo->row_pitch;
    uint32_t w     = albedo->width;
    uint32_t h     = albedo->height;
    vangfx::SurfaceFormat fmt = albedo->format;
}
```

| `GBufferLayerKind` | Typical D3D format | Description |
|---|---|---|
| `Albedo` | R8G8B8A8_UNORM | Base colour at backbuffer resolution |
| `Normal` | R16G16_SNORM / R16G16B16A16_SNORM | World- or view-space normals |
| `Depth` | R32_FLOAT | Linearised scene depth |
| `DepthRaw` | D24_UNORM_S8 / D32_FLOAT | Raw hardware depth-stencil |
| `Specular` | R8G8B8A8_UNORM | Roughness / metallic pack |
| `Emissive` | R16G16B16A16_FLOAT | Glow / HDR emission |
| `Motion` | R16G16_FLOAT | Screen-space motion vectors |
| `AmbientOcclusion` | R8_UNORM | SSAO / baked AO (often at reduced res) |
| `Velocity` | R16G16_FLOAT | Per-pixel velocity for TAA |
| `Backbuffer` | — | Final resolved back-buffer |

---

## D3D12 Extensions

### PSOCreateEvent

Fired by `on_pso_create` before the PSO reaches the driver. Assign `.data` / `.size` on any `ShaderBytecodeView` field to redirect the blob — it must remain valid until `CreateGraphicsPipelineState` / `CreateComputePipelineState` returns.

```cpp
ctx->on_pso_create([](vangfx::FrameContext&, vangfx::d3d12::PSOCreateEvent& ev) {
    // ev.kind                — PSOKind::Graphics | Compute | MeshShader
    // ev.vs, ev.ps, ev.gs    — ShaderBytecodeView {data, size}
    // ev.hs, ev.ds, ev.cs    — ShaderBytecodeView
    // ev.root_signature      — RootSignatureHandle
    // ev.input_layout[]      — InputLayoutElement array

    if (!ev.ps.empty()) {
        ev.ps.data = replacement_dxil_ptr;
        ev.ps.size = replacement_dxil_size;
    }
});
```

### CommandListEvent

```cpp
ctx->on_command_list([](vangfx::FrameContext& f, vangfx::d3d12::CommandListEvent& ev) {
    // ev.action        — CommandListAction::Reset|Close|Submit|Signal|Wait
    // ev.command_list  — CommandListHandle (stable id, survives Reset)
    // ev.command_queue — CommandQueueHandle (Submit, Signal, Wait only)
    // ev.fence         — FenceHandle (Signal, Wait only)
});
```

### ResourceBarrier

```cpp
ctx->on_resource_barrier([](vangfx::FrameContext& f,
                             vangfx::d3d12::ResourceBarrier* barriers,
                             uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        auto& b = barriers[i];
        switch (b.type) {
        case vangfx::d3d12::BarrierType::Transition:
            // b.transition.resource, b.transition.subresource
            // b.transition.before_state, b.transition.after_state
            break;
        case vangfx::d3d12::BarrierType::Aliasing:
            // b.aliasing.before_resource, b.aliasing.after_resource
            break;
        case vangfx::d3d12::BarrierType::UAV:
            // b.uav.resource
            break;
        }
        // b.intercept = true → suppress this barrier (caller owns hazard)
    }
});
```

### DescriptorHeap & RootBinding

The descriptor heap registry tracks all `ID3D12DescriptorHeap` and `ID3D12RootSignature` objects created by the game. `DrawEvent::root_bindings` is a snapshot of the active root parameters at draw time.

```cpp
ctx->on_draw([](vangfx::FrameContext& f, vangfx::DrawEvent& ev) {
    for (auto& rb : ev.root_bindings) {
        // rb.root_index     — root parameter slot
        // rb.type           — RootBindingType::CBV|SRV|UAV|Table|Constants
        // rb.gpu_address    — GPU VA (CBV / SRV / UAV direct)
        // rb.heap           — DescriptorHeapHandle (Table)
        // rb.heap_offset    — index into the heap (Table)
    }
});
```

---

## Event Structs

### DrawEvent

```cpp
struct DrawEvent {
    DrawType      type;             // Draw | DrawIndexed | DrawInstanced |
                                    //   DrawIndexedInstanced | DrawAuto |
                                    //   DrawIndirect | DrawIndexedIndirect |
                                    //   Dispatch | DispatchIndirect
    uint32_t      vertex_count;
    uint32_t      index_count;
    uint32_t      instance_count;
    uint32_t      dispatch_x, dispatch_y, dispatch_z;  // Dispatch only

    // D3D12 only
    CommandListHandle              command_list;
    PSOHandle                      pso;
    std::span<const RootBinding>   root_bindings;
    D3D12VertexBufferBinding       d3d12_vb;
    D3D12IndexBufferBinding        d3d12_ib;

    bool intercept;  // set true to suppress the draw
};
```

### RenderTargetEvent

```cpp
struct RenderTargetEvent {
    RenderTargetHandle render_targets[8];
    DepthBufferHandle  depth_buffer;
    uint32_t           rt_count;

    // D3D12 only
    D3D12_CPU_DESCRIPTOR_HANDLE d3d12_rtv_handles[8];
    D3D12_CPU_DESCRIPTOR_HANDLE d3d12_dsv_handle;
    bool                        d3d12_single_handle_range;
    CommandListHandle            d3d12_command_list;

    bool intercept;
};
```

### PresentEvent

```cpp
struct PresentEvent {
    uint32_t sync_interval;
    uint32_t flags;
    bool     intercept;  // suppress the Present call (use with care)
};
```

### ResourceEvent

```cpp
struct ResourceEvent {
    ResourceAction action;   // Create | Update | Destroy
    ResourceKind   kind;     // Texture | RenderTarget | DepthBuffer |
                             //   VertexBuffer | IndexBuffer | ConstantBuffer |
                             //   StructuredBuffer | UAV | RawBuffer | AccelStructure
    uint64_t       id;       // stable handle id

    // D3D12 — populated on Create
    D3D12ResourceDesc d3d12;
};
```

### StateChangeEvent

```cpp
struct StateChangeEvent {
    StateDirtyFlags dirty;  // bitmask — Blend | Depth | Rasterizer |
                            //   Sampler | Topology | Viewport | Scissor
    PipelineState   state;  // full snapshot after the change
};
```

### QueryEvent

```cpp
struct QueryEvent {
    QueryAction action;  // Begin | End | GetData
    QueryType   type;    // Occlusion | Timestamp | PipelineStats | etc.
    uint64_t    id;

    // D3D12 only
    uint64_t d3d12_query_heap_id;
    uint32_t d3d12_query_index;
    uint64_t d3d12_resolve_dest_va;
};
```

---

## Error Handling

All VanGFX functions return `vangfx::Result<T>` (`std::expected<T, VanGFXError>`).

```cpp
struct VanGFXError {
    ShaderError  code;
    std::string  message;
};
```

| Code | Meaning |
|---|---|
| `FileNotFound` | `source_file()` path unreadable |
| `CompileFailed` | Compiler returned errors; `message` contains the full diagnostic |
| `LinkFailed` | Invalid stage combination for link |
| `Unsupported` | Feature unavailable on the active backend |
| `DeviceLost` | D3D device lost mid-operation |
| `InvalidFormat` | Surface format rejected by the backend |
| `InvalidHandle` | Null or already-destroyed handle |
| `ResourceLost` | D3D resource released during readback |
| `OutOfMemory` | GPU or CPU allocator exhausted |
| `CaptureNotActive` | `latest_capture()` with no active session |
| `ReadbackFailed` | Fence timeout or resource not in a readable state |
| `NotInitialized` | Operation attempted before `ContextBuilder::build()` |

---

## Handle Types

| Type | Description |
|---|---|
| `TextureHandle` | Non-owning id for a tracked texture |
| `RenderTargetHandle` | Non-owning id for a tracked render target view |
| `DepthBufferHandle` | Non-owning id for a tracked depth-stencil view |
| `VertexBufferHandle` | Non-owning id for a tracked vertex buffer |
| `IndexBufferHandle` | Non-owning id for a tracked index buffer |
| `ConstantBufferHandle` | Non-owning id for a tracked constant buffer |
| `StructuredBufferHandle` | Non-owning id for a tracked structured buffer |
| `UAVHandle` | Non-owning id for a tracked UAV |
| `ShaderHandle` | Owned by `Context::destroy(ShaderHandle)` |
| `EffectHandle` | Owned by `Context::destroy(EffectHandle)` |
| `d3d12::CommandListHandle` | Non-owning id (stable across Reset) |
| `d3d12::CommandQueueHandle` | Non-owning id |
| `d3d12::FenceHandle` | Non-owning id |
| `d3d12::PSOHandle` | Non-owning id; invalidated on game-side Release |
| `d3d12::DescriptorHeapHandle` | Non-owning id |
| `d3d12::RootSignatureHandle` | Non-owning id |

All ids are `uint64_t` under the hood. An id of `0` means null / not tracked.

---

## Compat Layer

`include/vangfx/compat/` exposes the internal vtable-patching primitives. Most mod authors will not need these directly — they are required by the VanGFX runtime itself, and are made available for mods that need raw COM or vtable access.

### `vtable.hpp` — `ScopedPatch<Fn>`

RAII vtable slot patcher. Lifts page protection, writes the replacement pointer, restores original on destruction.

```cpp
#include <vangfx/compat/vtable.hpp>

auto patch = vangfx::vtable::ScopedPatch<decltype(&IDXGISwapChain::Present)>(
    vtable_ptr, slot_index, &my_present_hook);
// original restored when patch goes out of scope
```

### `com.hpp` — COM helpers

Utility wrappers for `QueryInterface`, `AddRef`/`Release` counting, and safe COM pointer casts.

### `d3d9_slots.hpp` / `d3d11_slots.hpp` / `d3d12_slots.hpp`

Verified vtable slot constants for every hooked method. All values are `inline constexpr uint32_t`. Example:

```cpp
#include <vangfx/compat/d3d12_slots.hpp>

// vangfx::d3d12_slots::kDrawInstanced == 12
// vangfx::d3d12_slots::kCreateGraphicsPipelineState == 24
// vangfx::d3d12_slots::kExecuteCommandLists == 10 (on ID3D12CommandQueue)
```

---

*Built and maintained by [TsyVM](https://github.com/TsyVM) · [TeamVanilla](https://www.teamvanilla.org/)*
