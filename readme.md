<div align="center">

<img src="vangfx-logo.png" width="600" alt="VanGFX"/>

<p><em>Universal D3D9 · D3D11 · D3D12 Shader Authoring & Pipeline Interception Library</em></p>

<a href="#">
<img src="https://readme-typing-svg.demolab.com/?lines=D3D9+%C2%B7+D3D11+%C2%B7+D3D12+%E2%80%94+One+API%2C+every+backend.;Replace+shaders+in+one+line+of+C%2B%2B.;Inject+post-processing+without+touching+the+original.;Live+float+parameters.+No+recompile.;PSO+interception.+Root+sig+patching.+GBuffer+capture.;No+DLL+proxy.+No+exceptions.+RAII+first.;std%3A%3Aexpected+all+the+way+down.&font=Fira%20Code&center=true&width=700&height=45&color=107C10&vCenter=true&size=20&pause=1800"/>
</a>

<br/>

[![License: MIT](https://img.shields.io/badge/License-MIT-107C10?style=for-the-badge&labelColor=000000)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-107C10?style=for-the-badge&labelColor=000000&logo=cplusplus&logoColor=107C10)](https://en.cppreference.com/w/cpp/20)
[![Windows](https://img.shields.io/badge/Windows-x86%20%7C%20x64-107C10?style=for-the-badge&labelColor=000000&logo=windows&logoColor=107C10)](libs/)
[![TeamVanilla](https://img.shields.io/badge/Team-TeamVanilla-107C10?style=for-the-badge&labelColor=000000)](https://www.teamvanilla.org/)

<br/>

[![Stars](https://img.shields.io/github/stars/tsyvm/vangfx?style=for-the-badge&color=107C10&labelColor=000000)](../../stargazers)
[![Issues](https://img.shields.io/github/issues/tsyvm/vangfx?style=for-the-badge&color=107C10&labelColor=000000)](../../issues)
[![Last Commit](https://img.shields.io/github/last-commit/tsyvm/vangfx?style=for-the-badge&color=107C10&labelColor=000000)](../../commits)
[![Downloads](https://img.shields.io/github/downloads/tsyvm/vangfx/total?style=for-the-badge&color=107C10&labelColor=000000)](../../releases)

<br/>

[![DirectX 9](https://img.shields.io/badge/DirectX-9-107C10?style=flat-square&labelColor=000000)](#)
[![DirectX 11](https://img.shields.io/badge/DirectX-11-107C10?style=flat-square&labelColor=000000)](#)
[![DirectX 12](https://img.shields.io/badge/DirectX-12-107C10?style=flat-square&labelColor=000000)](#)
[![HLSL](https://img.shields.io/badge/HLSL-SM%203.0%E2%80%936.x-107C10?style=flat-square&labelColor=000000)](#)
[![DXBC](https://img.shields.io/badge/Bytecode-DXBC%20%7C%20DXIL-107C10?style=flat-square&labelColor=000000)](#)
[![No proxy DLL](https://img.shields.io/badge/No-proxy%20DLL-107C10?style=flat-square&labelColor=000000)](#)
[![Exception-free](https://img.shields.io/badge/Exception--free-std%3A%3Aexpected-107C10?style=flat-square&labelColor=000000)](#)

</div>

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

VanGFX is a production-grade C++20 DirectX pipeline interception and shader authoring library for Windows game mods and graphics tooling. Hook into D3D9, D3D11, or D3D12 with a single `ContextBuilder::build()` call — **no DLL proxy required**, no `DetourAttach`, no external hooking dependency. All patches are owned by RAII `ScopedPatch` objects and restored cleanly when the `Context` is destroyed.

Beyond raw observability it ships a high-level **Effect API** — replace the pixel shader on every draw in one line, inject a post-processing snippet without knowing anything about root signatures or descriptor heaps, drive live float parameters into your HLSL via a typed constant buffer, and capture the full GBuffer to disk in PNG/DDS/Raw. On D3D12 it also exposes PSO interception, descriptor heap tracking, root binding snapshots, resource barrier callbacks, and command list lifecycle events — all behind `#include <vangfx/vangfx.hpp>`.

VanGFX is distributed as **precompiled static libraries with public API headers**. Drop the headers and the matching `.lib` into your project and link — no build system integration or source compilation required.

<div align="center">

### 📑 Contents

[Features](#-features-at-a-glance) · [Requirements](#️-requirements) · [Installation](#-installation) · [Quick Start](#-quick-start)

[Context & Backends](#-context--backends) · [Event Callbacks](#-event-callbacks) · [Effect API](#-effect-api) · [Shader Compilation](#-shader-compilation)

[Uniforms](#-uniforms--live-parameters) · [GBuffer Capture](#️-gbuffer-capture) · [D3D12 Extensions](#-d3d12-extensions) · [Error Handling](#-error-handling)

[API Reference](#-api-reference) · [Source Layout](#-source-layout) · [Known Limitations](#-known-limitations)

</div>

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## ✨ Features at a Glance

**Backends & Context**
- **Three D3D backends** — D3D9/D3D9Ex, D3D11 (Feature Levels 9.1–12.1), D3D12 (SM 6.0–6.x)
- **`BackendKind::Auto`** — detects which DirectX runtime is loaded; no manual selection needed
- **Fluent `ContextBuilder`** — one call to hook, no raw pointers or manual vtable management required
- **No DLL proxy** — all hooks installed via direct vtable patching; transparent to the game's load order
- **RAII lifetime** — every `ScopedPatch` removed automatically when `Context` is destroyed

**Event Interception**
- **`on_draw`** — every `DrawPrimitive` / `Draw*` / `Dispatch*` call with full vertex, index, and instance counts
- **`on_render_target`** — every `OMSetRenderTargets`, with CPU descriptor handles on D3D12
- **`on_present`** — every `Present` / `Present1` / `EndScene`; set `intercept = true` to suppress
- **`on_resource`** — Create / Update / Destroy for textures, RTs, depth buffers, VBs, IBs, CBs, UAVs, SBs, BVHs
- **`on_state_change`** — blend, depth-stencil, rasterizer, samplers, topology, viewports, scissor rects
- **`on_query`** — Begin / End / GetData for all D3D9/D3D11/D3D12 query types
- **`on_device_lost`** / **`on_device_reset`** — device reset handling across all backends

**Effect API**
- **`replace_ps` / `replace_vs` / `replace_effect`** — compile and install a full shader replacement
- **`inject_ps_post`** — HLSL snippet runs *after* pixel output; modify `color` in-place
- **`inject_ps_pre`** — HLSL snippet runs *before* texture sampling; modify `vangfx_uv`
- **`ScopedShaderReplace` / `ScopedInjection`** — RAII auto-restore when guard goes out of scope
- **`ReplaceFilter`** — restrict to draws matching vertex count range, RT resolution, or custom predicate
- **`Uniforms`** — 8 × `float4` live-updated slots; `vangfx_time`, `vangfx_delta`, `vangfx_resolution` always available

**Shader Compilation**
- **`ShaderBuilder`** — fluent HLSL compiler; routes to `d3dcompiler_47.dll` (D3D9/D3D11) or `dxcompiler.dll` + `dxil.dll` (D3D12 SM 6.x) at runtime; no static link required
- **Pre-compiled blobs** — supply DXBC or DXIL bytecode directly via `.bytecode(ptr, size)`
- **`#define` forwarding**, include directories, optimization level, debug info, DXIL validation toggle, `-rootsig-define` for embedded root signatures

**GBuffer Capture**
- **`CaptureSession`** — RAII bounded capture; auto-stops after `max_frames`
- **Heuristic RT classifier** — assigns Albedo / Normal / Depth / DepthRaw / Specular / Emissive / Motion / AO / Velocity / Backbuffer from format + resolution + slot index
- **CPU readback** — D3D11 `CopyResource` path; D3D12 `CopyTextureRegion` + fence wait + barrier transitions
- **Buffer readback** — vertex, index, constant, structured, raw and UAV buffers, by byte range. D3D12 maps CPU-visible heaps in place and copies out of default heaps; D3D11 stages and maps; D3D9 locks in place
- **Draw → geometry (D3D12)** — `buffer_at()` resolves the GPU virtual address in a draw's vertex/index buffer view to the buffer and offset behind it, which is how you find out what a draw actually is
- **File output** — PNG (WIC), DDS (hand-rolled header), Raw byte dump; per-frame `on_frame` callback

**D3D12 Extensions**
- **`on_command_list`** — Reset / Close / Submit / Signal / Wait with `CommandListHandle` tracking
- **`on_pso_create`** — intercept `CreateGraphicsPipelineState` / `CreateComputePipelineState` / mesh shader PSOs; replace DXIL bytecode blobs before GPU compilation
- **`on_resource_barrier`** — Transition / Aliasing / UAV barrier batches; `intercept` to suppress
- **Descriptor heap registry** — CPU handle → `DescriptorHeapHandle` + index in O(N); root signature handle tracking
- **Root binding snapshots** — per-command-list CBV/SRV/UAV/table bindings captured at draw time and surfaced in `DrawEvent::root_bindings`
- **Upload heap ring buffer** — 64KB ring, 256-byte alignment, reset per frame; drives `SetGraphicsRootConstantBufferView` for injected constant buffers

**`std::expected` throughout** — no exceptions, no HRESULTs, no hidden failure paths.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🛠️ Requirements

| | |
|---|---|
| **Compiler** | MSVC 19.29+ with `/std:c++20` |
| **Windows SDK** | 10.0.19041.0+ for D3D9/D3D11 · 10.0.20348.0+ for D3D12 / DXR / mesh shaders |
| **D3D9 / D3D11** | `d3d9.lib` `d3d11.lib` `dxgi.lib` `d3dcompiler.lib` |
| **D3D12** | `d3d12.lib` `dxgi.lib` `dxguid.lib` `dxcompiler.lib` |
| **Platform** | Windows x86 / x64 only |

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 📦 Installation

VanGFX is distributed as **precompiled static libraries with public headers**. Copy `include/` into your project, link against the `.lib` for your target, and you're done. All libraries use a static CRT — no Redistributable required.

| Target | Configuration | Path |
|---|---|---|
| Windows x64 | Release | `libs/MSVC/win-x64/Release/vangfx.lib` |
| Windows x64 | Debug | `libs/MSVC/win-x64/Debug/vangfx.lib` |
| Windows x86 | Release | `libs/MSVC/win-x86/Release/vangfx.lib` |
| Windows x86 | Debug | `libs/MSVC/win-x86/Debug/vangfx.lib` |

### MSVC project setup (D3D9 / D3D11)

1. **Additional Include Directories** → `include\`
2. **Additional Library Directories** → `libs\MSVC\win-x64\Release\`
3. **Additional Dependencies** → `vangfx.lib` · `d3d9.lib` · `d3d11.lib` · `dxgi.lib` · `d3dcompiler.lib`
4. **Runtime Library** → `Multi-threaded (/MT)`

### MSVC project setup (D3D12 / SM 6.x)

Add to **Additional Dependencies** → `d3d12.lib` · `dxgi.lib` · `dxguid.lib` · `dxcompiler.lib`

### CMake (drop-in)

```cmake
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(VGX_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/MSVC/win-x64")
else()
    set(VGX_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/MSVC/win-x86")
endif()

add_library(VanGFX::vangfx STATIC IMPORTED)
set_target_properties(VanGFX::vangfx PROPERTIES
    IMPORTED_LOCATION_RELEASE "${VGX_LIB_DIR}/Release/vangfx.lib"
    IMPORTED_LOCATION_DEBUG   "${VGX_LIB_DIR}/Debug/vangfx.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(my_mod PRIVATE VanGFX::vangfx)
```

### Single include

```cpp
#include <vangfx/vangfx.hpp>   // everything — context, effects, capture, D3D12 extensions
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## ⚡ Quick Start

```cpp
#include <vangfx/vangfx.hpp>

void setup(ID3D12Device* device, ID3D12CommandQueue* queue,
           IDXGISwapChain* swapChain) {
    // Auto-detect the D3D backend, no proxy DLL. The device and queue come from
    // you: D3D11 and D3D12 keep no global to find them in.
    auto ctx = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::Auto)
        .log_level(vangfx::LogLevel::Info)
        .d3d12_device(device)
        .d3d12_command_queue(queue)
        .d3d12_swap_chain(swapChain)
        .build()
        .value();

    // Observe every draw call.
    ctx->on_draw([](vangfx::FrameContext& frame, vangfx::DrawEvent& ev) {
        if (ev.vertex_count > 10'000) {
            // large mesh — do something
        }
    });

    // Replace the pixel shader — one line. D3D9 and D3D11.
    vangfx::replace_ps(*ctx, R"(
        float4 main(float2 uv : TEXCOORD) : SV_Target {
            float4 c = tex.Sample(samp, uv);
            float  g = dot(c.rgb, float3(0.299, 0.587, 0.114));
            return float4(g, g, g, c.a);
        }
    )");

    // Or inject a post-processing snippet without touching the original shader.
    vangfx::inject_ps_post(*ctx, "color.rgb = min(color.rgb, 1.0);");

    // Hooks go out of scope → originals automatically restored.
}
```

> **On D3D12, `replace_ps` and `inject_ps_post` do nothing.** They install an
> effect by binding it per draw, which is a D3D9/D3D11 model; D3D12 has no
> shader binding call, only immutable pipeline states. They compile the shader
> and never install it, without reporting an error.
>
> The D3D12 route is `on_pso_create`, which hands you the pipeline state
> description before it is created and honours any shader bytecode you swap into
> it. See [D3D12 Extensions](#-d3d12-extensions).

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🎮 Context & Backends

`ContextBuilder` is the single entry point for all VanGFX functionality. Call `.build()` once at startup — it auto-detects the D3D backend and installs all vtable patches. The returned `Context` is the root object for every subsequent operation.

```cpp
auto ctx = vangfx::ContextBuilder{}
    .backend(vangfx::BackendKind::D3D11)  // or Auto, D3D9, D3D12
    .log_level(vangfx::LogLevel::Debug)
    .log_callback([](vangfx::LogLevel lvl, std::string_view msg) {
        printf("[vangfx] %s\n", std::string(msg).c_str());
    })
    .d3d11_device(device)                 // required — see the table below
    .d3d11_swap_chain(swapChain)          // required on D3D11
    .d3d12_min_shader_model(60)           // require SM 6.0+ — fail-fast if not met
    .auto_capture(true)                   // start GBuffer capture on first Present
    .build();

if (!ctx) {
    // ctx.error().message contains the full diagnostic
}
```

### What each backend needs

VanGFX never goes looking for a device. Direct3D 9 had a global to find one in; D3D11 and D3D12 do not, and neither lets a device be asked which queues or swap chains were created from it. Supply them from wherever you already have them — your own hook, a proxy DLL, or the host that loaded you.

| Backend | Required | Optional |
|---|---|---|
| D3D9  | `d3d9_device` | — |
| D3D11 | `d3d11_device`, `d3d11_swap_chain` | — |
| D3D12 | `d3d12_device`, `d3d12_command_queue` | `d3d12_swap_chain` — without it `on_present` never fires |

`build()` returns `ShaderError::NotInitialized` with a message naming whichever one is missing.

### `BackendKind::Auto`

VanGFX scans loaded modules at `build()` time. If `d3d12.dll` is found it targets D3D12; if only `d3d11.dll` is present it targets D3D11; otherwise D3D9. Specify a backend explicitly to override.

### Backend capability queries

```cpp
ctx->backend();                   // BackendKind::D3D9 / D3D11 / D3D12
ctx->d3d9_ver();                  // minor version (9 or 0)
ctx->d3d11_feature_level();       // e.g. 0xB100 = Feature Level 11.1
ctx->d3d12_shader_model();        // e.g. 66 = SM 6.6 (D3D12 only; 0 otherwise)
ctx->d3d12_raytracing_tier();     // 0 = none, 1 = Tier 1, 11 = Tier 1.1
ctx->d3d12_mesh_shader_tier();    // 0 = none, 1 = Tier 1
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🔁 Event Callbacks

All callbacks receive a `FrameContext&` and a typed event struct. Setting `ev.intercept = true` (where supported) suppresses the underlying D3D call.

```cpp
ctx->on_draw([](vangfx::FrameContext& frame, vangfx::DrawEvent& ev) {
    // ev.type, ev.vertex_count, ev.index_count, ev.instance_count
    // ev.dispatch_x/y/z (compute)
    // ev.pso, ev.command_list, ev.root_bindings (D3D12 only)
    // ev.intercept = true → suppress this draw
});

ctx->on_render_target([](vangfx::FrameContext& frame, vangfx::RenderTargetEvent& ev) {
    // ev.render_targets[], ev.depth_buffer, ev.rt_count
    // ev.d3d12_rtv_handles[], ev.d3d12_command_list (D3D12 only)
});

ctx->on_present([](vangfx::FrameContext& frame, vangfx::PresentEvent& ev) {
    // ev.sync_interval, ev.flags
    // ev.intercept = true → suppress Present (use with care)
});

ctx->on_resource([](vangfx::FrameContext& frame, vangfx::ResourceEvent& ev) {
    // ev.action: Create / Update / Destroy
    // ev.kind:   Texture / RenderTarget / DepthBuffer / VertexBuffer /
    //            IndexBuffer / ConstantBuffer / StructuredBuffer /
    //            UAV / RawBuffer / AccelStructure
});

ctx->on_state_change([](vangfx::FrameContext& frame, vangfx::StateChangeEvent& ev) {
    // ev.dirty bitmask — blend, depth, rasterizer, sampler, topology, viewport
});

ctx->on_device_lost([]() { /* release mod-side GPU resources */ });
ctx->on_device_reset([](vangfx::FrameContext& frame) { /* recreate */ });
```

### D3D12-only callbacks

```cpp
ctx->on_command_list([](vangfx::FrameContext& f, vangfx::d3d12::CommandListEvent& ev) {
    // ev.action: Reset / Close / Submit / Signal / Wait
    // ev.command_list, ev.command_queue (Submit / Signal / Wait)
});

ctx->on_pso_create([](vangfx::FrameContext& f, vangfx::d3d12::PSOCreateEvent& ev) {
    // ev.kind: Graphics / Compute / MeshShader
    // ev.vs, ev.ps, ev.gs, ev.hs, ev.ds, ev.cs — ShaderBytecodeView (non-owning)
    // Assign ev.ps.data / ev.ps.size to swap the blob before the driver sees it
});

ctx->on_resource_barrier([](vangfx::FrameContext& f,
                             vangfx::d3d12::ResourceBarrier* barriers,
                             uint32_t count) {
    // Inspect or suppress Transition / Aliasing / UAV records
});
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## ✏️ Effect API

```cpp
// Replace the pixel shader on every draw.
vangfx::EffectHandle h = vangfx::replace_ps(*ctx, R"(
    float4 main(float2 uv : TEXCOORD) : SV_Target {
        float4 c = tex.Sample(samp, uv);
        return float4(1.0 - c.rgb, c.a);   // invert
    }
)");

// Restrict to specific draws via ReplaceFilter.
vangfx::ReplaceFilter f;
f.min_verts = 3000;
f.rt_width  = 1920;
f.rt_height = 1080;
vangfx::replace_ps(*ctx, hlsl, f);

// RAII auto-restore.
auto guard = vangfx::ScopedShaderReplace::ps(*ctx, hlsl_source, filter);

// Inject without replacing — modifies color after game's pixel output.
vangfx::inject_ps_post(*ctx, "color.rgb = lerp(color.rgb, float3(1.0, 0.2, 0.2), 0.3);");
vangfx::inject_ps_pre(*ctx,
    "vangfx_uv.x += sin(vangfx_uv.y * 20.0 + vangfx_time * 3.0) * 0.005;");
```

**Variables available inside injected snippets:**

| Variable | Type | Available in |
|---|---|---|
| `color` | `float4` | `inject_ps_post` — modify in-place |
| `vangfx_uv` | `float2` | Both — screen UV [0,1]×[0,1] |
| `vangfx_time` | `float` | Both — seconds since context creation |
| `vangfx_delta` | `float` | Both — seconds since last frame |
| `vangfx_resolution` | `float2` | Both — back-buffer width × height |
| `vangfx_float4[0..7]` | `float4` | Both — set via `Uniforms::set()` |

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🔧 Shader Compilation

```cpp
auto shader = vangfx::ShaderBuilder{}
    .source(hlsl_text)
    .entry("PSMain")
    .profile("ps_5_0")
    .define("SHADOW_PASS", "1")
    .include_dir("shaders/")
    .optimization(3)
    .build(*ctx);

// D3D12 — SM 6.x via DXC
auto ps6 = vangfx::ShaderBuilder{}
    .source(hlsl_text)
    .profile("ps_6_5")
    .use_dxc(true)
    .validate_dxil(true)
    .root_signature_define("RS_MAIN")
    .build(*ctx);

// Pre-compiled bytecode blob
auto prebuilt = vangfx::ShaderBuilder{}
    .bytecode(blob_ptr, blob_size)
    .stage(vangfx::ShaderStage::Pixel)
    .build(*ctx);

auto effect  = ctx->link_effect(vs.value(), ps.value());
auto compute = ctx->link_compute_effect(cs.value());
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🎛️ Uniforms — Live Parameters

```cpp
vangfx::Uniforms u(*ctx);

u.set(0, 0.75f);                      // vangfx_float4[0].x
u.set(1, 1.0f, 0.5f, 0.2f);          // vangfx_float4[1].xyz
u.set(2, vangfx::Float4(r, g, b, a));

// HLSL side — always available in replacement / injection shaders:
// cbuffer vangfx_params : register(b0) {
//     float4 vangfx_float4[8];
//     float  vangfx_time;
//     float  vangfx_delta;
//     float2 vangfx_resolution;
// };
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🖼️ GBuffer Capture

```cpp
vangfx::CaptureConfig cfg;
cfg.max_frames    = 5;
cfg.cpu_readback  = true;
cfg.output_dir    = "captures/";
cfg.output_format = vangfx::CaptureFormat::PNG;
cfg.capture_layer(vangfx::GBufferLayerKind::Albedo)
   .capture_layer(vangfx::GBufferLayerKind::Normal)
   .capture_layer(vangfx::GBufferLayerKind::Depth);

cfg.on_frame = [](const vangfx::GBufferCapture& cap) {
    if (auto* albedo = cap.find(vangfx::GBufferLayerKind::Albedo)) {
        // albedo->pixels, albedo->row_pitch, albedo->width, albedo->height
    }
};

auto session = vangfx::CaptureSession::begin(*ctx, cfg).value();
```

| `GBufferLayerKind` | Typical format | Trigger |
|---|---|---|
| `Albedo` | R8G8B8A8 at BB resolution | Base colour / diffuse |
| `Normal` | R16G16 / R16G16B16A16_SNORM | World- or view-space normals |
| `Depth` | Linearised float | Scene depth 0=near 1=far |
| `DepthRaw` | D24S8 / D32_FLOAT | Raw hardware depth |
| `Specular` | R8G8B8A8 | Roughness / metallic pack |
| `Emissive` | R16G16B16A16_FLOAT | Glow / emission |
| `Motion` | R16G16_FLOAT | Screen-space motion vectors |
| `AmbientOcclusion` | R8 at reduced res | SSAO / baked AO |
| `Velocity` | R16G16_FLOAT | Velocity / TAA |
| `Backbuffer` | — | Final resolved backbuffer |

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## ⚙️ D3D12 Extensions

```cpp
// PSO interception — swap DXIL blobs before GPU compilation
ctx->on_pso_create([](vangfx::FrameContext&, vangfx::d3d12::PSOCreateEvent& ev) {
    if (ev.kind == vangfx::d3d12::PSOKind::Graphics && !ev.ps.empty()) {
        // assign ev.ps.data / ev.ps.size to redirect the blob
    }
});

// Resource barrier observation / suppression
ctx->on_resource_barrier([](vangfx::FrameContext& f,
                             vangfx::d3d12::ResourceBarrier* barriers,
                             uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        barriers[i].intercept = true;  // suppress (caller owns hazard)
});
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## ❌ Error Handling

Every VanGFX function returns `Result<T>` — a `std::expected<T, VanGFXError>`. No exceptions anywhere in the library.

```cpp
auto result = vangfx::ContextBuilder{}.build();
if (!result) {
    // result.error().code    — ShaderError enum value
    // result.error().message — human-readable description
}

// Monadic chaining (C++23 monadic expected)
vangfx::ShaderBuilder{}.source(hlsl).profile("ps_5_0").build(*ctx)
    .and_then([&](vangfx::Shader s) -> vangfx::Result<vangfx::Effect> {
        return ctx->link_effect(vs, s);
    });
```

| `ShaderError` | Meaning |
|---|---|
| `FileNotFound` | `source_file()` path does not exist |
| `CompileFailed` | HLSL compilation failed; `message` has full compiler diagnostic |
| `LinkFailed` | Stage combination is invalid |
| `Unsupported` | Feature not available on this backend |
| `DeviceLost` | D3D device was lost during the operation |
| `InvalidFormat` | Surface format not supported on this backend |
| `InvalidHandle` | Handle is null or already destroyed |
| `ResourceLost` | Underlying D3D resource was released during readback |
| `OutOfMemory` | GPU or CPU allocator exhausted |
| `CaptureNotActive` | `latest_capture()` called with no active session |
| `ReadbackFailed` | CPU readback failed (fence timeout or unreadable state) |
| `NotInitialized` | `ContextBuilder::build()` not called before this operation |

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 📁 Source Layout

```
VanGFX/
├── include/vangfx/
│   ├── vangfx.hpp                  ← single-include entry point
│   ├── core/
│   │   ├── context.hpp
│   │   ├── shader.hpp
│   │   └── error.hpp
│   ├── effect/
│   │   ├── replace.hpp
│   │   ├── inject.hpp
│   │   └── uniform.hpp
│   ├── capture/
│   │   ├── capture.hpp
│   │   └── gbuffer.hpp
│   ├── events/
│   │   ├── draw.hpp
│   │   ├── surface.hpp
│   │   ├── state.hpp
│   │   ├── resource.hpp
│   │   └── query.hpp
│   ├── resources/
│   │   ├── buffer.hpp
│   │   ├── texture.hpp
│   │   ├── surface.hpp
│   │   └── format.hpp
│   ├── pipeline/
│   │   ├── state.hpp
│   │   ├── topology.hpp
│   │   └── viewport.hpp
│   ├── d3d12/
│   │   ├── command.hpp
│   │   ├── heap.hpp
│   │   ├── pso.hpp
│   │   └── resource.hpp
│   └── compat/
│       ├── vtable.hpp
│       ├── com.hpp
│       ├── d3d9_slots.hpp
│       ├── d3d11_slots.hpp
│       ├── d3d12_slots.hpp
│       └── compat.hpp
└── libs/
    └── MSVC/
        ├── win-x64/
        │   ├── Release/vangfx.lib
        │   └── Debug/vangfx.lib
        └── win-x86/
            ├── Release/vangfx.lib
            └── Debug/vangfx.lib
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## ⚠️ Known Limitations

| Item | Status |
|---|---|
| **Effect API on D3D12 is not per-draw** | `replace_ps`, `replace_vs`, `replace_effect`, `inject_ps_post`, `inject_ps_pre` and the `Scoped*` guards work on D3D12, but they register a **substitution at PSO creation** rather than binding per draw — D3D12 has no call that binds a shader. So a `ReplaceFilter` selecting on vertex count or target size cannot be honoured, and a PSO built before the bind keeps its original shader. Bind during device creation to catch everything. |
| **`buffer_at` is D3D12-only** | Resolving a GPU virtual address to a buffer needs addresses to resolve, and D3D9/D3D11 bind buffers as interface pointers. Those backends return an empty `BufferRef` — a fact about the API, not a gap. Buffer *readback by handle* works on all three. |
| **D3D9 `D3DUSAGE_WRITEONLY` buffers cannot be read** | The flag lets the driver hold the memory where the CPU cannot usefully read it, so `readback` reports it rather than returning bytes that are not the contents. Most static geometry in a real game sets it. D3D9 has no constant/structured/raw/UAV buffer objects either, so only vertex and index buffers are ever registered. |
| **EXR output** | Encoder not yet implemented. `CaptureFormat::EXR` falls back to `.raw` with a `LogLevel::Warning`. tinyexr integration is deferred. |
| **D3D12 `ResourceBarrier` intercept** | Setting `barriers[i].intercept = true` suppresses the barrier. The library does not guard against correctness consequences — the caller owns the hazard. |
| **D3D11 swap chain** | Must be supplied via `d3d11_swap_chain()`. DXGI can walk from a device up to its factory but not back down to the swap chains created from it. |
| **Windows only** | D3D is a Windows API. Linux / macOS are not supported. |

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 📖 Documentation

Full API reference, architecture notes, and worked examples are in the **[VanGFX Functions Guide](VanGFX_Functions_Guide.md)**.

<div align="center">

<sub>Built and maintained by <a href="https://github.com/TsyVM">TsyVM</a> · <a href="https://www.teamvanilla.org/">TeamVanilla</a></sub>

<img width="100%" src="https://capsule-render.vercel.app/api?type=waving&color=0:006400,100:000000&height=80&section=footer"/>

</div>
