# VanGFX 3.0 — Research Notes

What already exists, inside TeamVanilla and outside it, and what each one
teaches. Written 2026-11-11, before any VanGFX 3.0 code. The other documents in
this folder lean on the facts collected here.

| Document | Read it for |
|---|---|
| `00-RESEARCH.md` | this file: prior art, our own proxies, licences |
| `01-ARCHITECTURE.md` | the shape of VanGFX 3.0 |
| `02-WHY.md` | why that shape, and what was rejected |
| `03-WHEN-TO-USE-WHAT.md` | choosing a tool for a job |
| `04-HOW-TO.md` | doing the common jobs, step by step |
| `05-ROADMAP.md` | phases, order, what "done" means for each |

---

## 1. What VanGFX is today (2026 Release)

Measured from the source in `Projects\VanGFX`, not from the README.

- About 25,000 lines of C++23, MIT, one static library `vangfx.lib`.
- **Interception only.** VanGFX attaches to a device the caller already has
  (`ContextBuilder::d3d9_device()` and so on) and patches vtables. It is not a
  proxy and it does not translate.
- Backends: D3D9/9Ex, D3D11, D3D12. Each optional at configure time.
- Hooking: `src/internal/compat/hook_site.hpp`, one `hooks::Site` per method,
  installed once per vtable, with self-healing for D3D11's heap vtables.
  `slot_checks.c/.cpp` statically checks every slot index against the SDK
  headers. This is the strongest part of the codebase and is kept as it is.
- Events: draw, render target, present, resource, state, query, device
  lost/reset; D3D12 adds command lists, PSO creation, barriers, descriptor heaps.
- Effects: `replace_ps`/`replace_vs`/`inject_ps_post`/`inject_ps_pre`,
  `Uniforms` (8 × float4 + time/delta/resolution), filters by vertex count and
  target size. On D3D12 they substitute at PSO creation instead of per draw.
- Capture: G-buffer classifier, CPU readback, PNG/DDS/raw writers, its own
  deflate.
- Audio: XAudio2/DirectSound capture (outside the graphics scope; untouched).
- Tests: WARP-based, one executable, each test in its own process.

**Found while surveying (fix in Phase 0):**

- `tests/CMakeLists.txt` adds `audio_tests.cpp` when `VANGFX_AUDIO=ON` (the
  default), and the file does not exist. A clean configure with default options
  fails. `-DVANGFX_AUDIO=OFF` configures.
- The README badge and requirements table say C++20 / MSVC 19.29; the code needs
  C++23 (`std::expected`). `BUILD-INFO.md` is right, the README is not.
- The README's source layout lists `include/vangfx/compat/`; those headers moved
  to `src/internal/compat/` and are no longer public.
- `Projects\VanGFX` is not a git repository. Snapshots so far have been folder
  copies (`VanGFX-pre-rebuild-20260929`, `VanGFX-retired-20260929`).

## 2. TeamVanilla's own proxies

### 2.1 MWOn12 — Need for Speed: Most Wanted (D3D9 → D3D12)

`Official\Need for Speed - Most Wanted\MWOn12`, about 20,000 lines in
`src\DirectX-Handler`. A `d3d9.dll` the game loads in place of the system one.

Shape (from its `ARCHITECTURE.md`, confirmed in the tree):

| Part | Lines | API-dependent? | Reusable as-is? |
|---|---|---|---|
| `core/D3D9ShaderTranslator` — SM1–3 bytecode → HLSL | 1,657 | no | **yes** |
| `core/FFPShaderGen` — fixed-function → HLSL | 986 | no | **yes** |
| `core/FormatConverter` — D3DFORMAT ↔ DXGI_FORMAT | 580 | no | **yes** |
| `core/RenderStateTracker` — D3D9 shadow state | 222 | no | **yes** |
| `core/CapsTable` — the one D3DCAPS9 answer | 369 | no | yes, per-game overrides wanted |
| `core/ShaderMods` — dump/replace translated HLSL by bytecode hash | 362 | no | **yes** |
| `core/BackendSelect` — ini/env, adapter probe, fallback | 589 | no | the pattern, yes |
| `dx12/core/*` — DeviceContext12, PSO/shader caches, upload ring, allocator, blitter | ~5,000 | D3D12 | yes, as a D3D12 "target" |
| `dx12/d3d9proxy/*` — the `IDirect3D*9` COM objects | ~5,800 | D3D9 front, D3D12 back | yes, as the D3D9 "front end" |
| `directx/*` — passthrough wrappers for Backend=1 | ~470 | D3D9 | replaced by the proxy kit |

Rules it already learned and VanGFX 3.0 inherits verbatim:

- Never `LoadLibrary` or probe adapters under the loader lock; defer to the first
  API call behind `std::call_once`.
- `LoadLibrary("d3d9")` from inside `d3d9.dll` loads itself; always build an
  absolute System32 (or SysWOW64) path.
- A command list that failed `Close` must never be submitted.
- Per-frame resources are indexed by frame in flight, plugins included.
- In a 32-bit process, address space runs out before memory does: share upload
  pools rather than mapping a buffer per allocation.
- ASCII-only loader log.
- The "hard rule": no Microsoft or third-party mapping layer (D3D9On12,
  D3D11On12, WineD3D, DXVK) performs the translation. VanGFX 3.0 keeps this rule
  for the translators it ships.

Its SDK (`MWOn12SDK`) shows the user-facing model that works:

- **Shader kit**: `ShaderDump=1`, `ShaderMods=1`; the proxy writes every shader it
  translates as HLSL named by a hash of the game's own bytecode
  (`ps_<16 hex>.hlsl`, optional `_v<8 hex>` variant), and loads an edited copy
  from the parent folder. Bad edits log once and fall back to the original. No
  compiler, no C++. This is the single most important idea for goal (a).
- **Overlay** (`mwon12::Overlay`) and **graphics module** (`<mwon12/graphics.hpp>`,
  which attaches VanGFX to the D3D12 device MWOn12 renders with).
- The SDK's `docs/graphics.md` opens with "three things are called a graphics mod,
  and they want three different tools" — the right framing; reused in
  `03-WHEN-TO-USE-WHAT.md`.
- ASI loader and plugin ABI in C (`mwon12.h`), so plugins do not depend on the
  C++ ABI of the host.

### 2.2 SFOn12 — Soldier Front (D3D9 → D3D12, plus engine fixes)

`Official\Soldier Front\SFOn12\src\render` is a **fork of MWOn12's translator**.
Diffing the two (ignoring whitespace and line endings):

- 49 files exist in both; every one differs, most by a few lines (header names).
- Real divergence: `FFPShaderGen.cpp` (235 lines), `DeviceContext12.cpp` (125),
  `D9Device12_Present.cpp` (97), `Shaders12.cpp` (96), `BackendSelect.cpp` (95),
  `D9Device12.cpp` (88), `D9Device12_Draw.cpp` (53).
- SFOn12's README names two fixed-function bugs fixed there that MWOn12 still has:
  FFP matrices uploaded column-major (every untransformed draw transposed and
  invisible), and an empty texture stage reading transparent black instead of
  opaque white.

**This is the strongest argument for VanGFX 3.0's translator layer.** Two copies of
one translator already drift, and a bug fixed in one is still live in the
other. A third game would make three.

### 2.3 The Simpsons: Hit & Run — a source port, not a proxy

`Official\Simpsons-Hit-n-Run\Source\Engine\Render` is an engine renderer with its
own device interface (`Device.hpp`, 336 lines) and backends for D3D12
(1,771 lines), D3D11 (919) and OpenGL ES 3 (1,478; ANGLE on Windows, native on
Android). Lessons:

- A small, description-driven device (`BlendDesc` → cached `BlendState`, same
  description gives the same object) is enough for a whole game.
- **Shaders are HLSL once, compiled at build time** (`Tools/shaders_gen.py`): DXIL
  via DXC for D3D12, GLSL ES for GLES with a register-binding string. No runtime
  cross-compiler on the device. VanGFX 3.0's effect packs follow this.
- Conventions fixed to Direct3D's (depth 0..1, top-left origin, clockwise front
  faces, numbered slots); a backend that differs fixes it up itself.

### 2.4 Projects\DirectX\Proxies — third-party code already on disk

| Folder | What it is | Licence |
|---|---|---|
| `DirectX8-9` | crosire's **d3d8to9** | BSD-2-Clause |
| `DirectX9-12` | megai2's **d912pxy** (D3D9 → D3D12, made for Guild Wars 2) | MIT |
| `DirectX8-11`, `DirectX9-11`, `DirectX11-12` | empty | — |

The empty folders read as intended proxies that were never started; VanGFX 3.0's
proxy kit is meant to make each of them a small project.

### 2.5 VanGUI 2.0 and VanHooks

- VanGUI 2.0 (`Official\VanGUI-2.0\backends`) already has renderers for dx9, dx10,
  dx11, dx12, vulkan, metal, opengl2/3, webgpu, and platform layers for win32,
  android, osx, sdl. **VanGFX does not need a UI renderer of its own**: it needs
  to hand VanGUI the native device, queue and back buffer at present time, and
  restore the game's state afterwards.
- VanHooks (`Projects\VanHooks`) does inline, IAT, vtable and mid-function hooks
  on x86/x64/ARM64, Windows/Linux/macOS, `std::expected` API, Zydis-based.
  VanGFX's own `hooks::Site` covers vtables; inline hooks of exported functions
  (`wglSwapBuffers`, `eglSwapBuffers`, `vkQueuePresentKHR` when no layer is
  possible) are VanHooks' job. Its build fetches Zydis over the network; MWOn12
  removed it for that reason. VanGFX 3.0 consumes VanHooks' **prebuilt SDK**
  (headers + lib, as MWOn12 vendors it in `external/VanHooks`), never its source
  build.

## 3. Outside prior art

### 3.1 Injection / post-processing frameworks

**ReShade** (crosire, BSD-3-Clause). The closest thing to VanGFX 3.0's goal (a).
- One DLL, renamed per API (`dxgi.dll`, `d3d9.dll`, `d3d11.dll`, `opengl32.dll`, …)
  plus a Vulkan implicit layer.
- Add-on API: `reshade::api::device`, `command_list`, `command_queue`,
  deliberately modelled on D3D12/Vulkan; D3D9/10/11/GL are presented through the
  same interface, with "partial pipeline binding" allowed on the older APIs;
  `get_native()` reaches the real object. Recent versions added fences, ray
  tracing and mesh shader calls.
- Effects in its own language (ReShade FX), compiled at runtime per API.
- Takeaways: (1) a modern-shaped abstraction can represent older APIs; (2) the
  same hooking DLL serving every API under different names is what makes
  "drop in one file" work; (3) a bespoke effect language is a cost every
  contributor pays — VanGFX uses plain HLSL instead (see `02-WHY.md`).
- The licence would allow borrowing code with attribution; we borrow ideas, not
  code, so the event and object model stays ours.

**Special K** (Kaldaien). Texture dump/inject keyed by a checksum of the texture
data; a replacement must carry the dumped file's hash name. Its release notes
record a hash algorithm change breaking existing packs — **a hash, once shipped,
is a file format and must never change.**

**Dolphin** graphics mods / texture packs. File names encode size, mip flag and a
content hash; wildcard names match families of textures. Useful naming scheme
for VanGFX's texture packs.

### 3.2 API translation layers

| Project | Translates | Licence | Notes |
|---|---|---|---|
| **DXVK** | D3D8/9/10/11 → Vulkan | zlib | 3.0 (2026) moved to **dxbc-spirv**, an SSA compiler, and compiles shaders on worker threads |
| **vkd3d-proton** | D3D12 → Vulkan | **LGPL-2.1** | shares DXGI with DXVK; uses dxil-spirv |
| **dxbc-spirv** | DXBC SM ≤ 5.1 → SPIR-V via its own IR | MIT | built as a library to lower into custom targets; Meson |
| **dxil-spirv** | DXIL SM 6.x → SPIR-V | MIT | stable C API |
| **d3d8to9** | D3D8 → D3D9 | BSD-2 | small, complete, already on disk |
| **d912pxy** | D3D9 → D3D12 | MIT | GW2-specific, already on disk |
| **dxwrapper** | many old APIs → D3D9 etc. | custom permissive + bundled MIT/BSD/MPL-2.0 parts | includes d3d8to9 |
| **D3D9On12**, **D3D11On12** | D3D9/11 → D3D12, as DDI layers | MIT (source released) | built on **D3D12TranslationLayer** (MIT); excluded as a backend by the team's hard rule, usable as a *reference* for correctness questions |
| **WineD3D** | D3D → OpenGL/Vulkan | LGPL | reference only |
| **dgVoodoo2** | Glide/DDraw/D3D1–9 → D3D11/12 | closed, terms unclear | not usable |
| **Mesa Zink** / **Dozen** | GL → Vulkan / Vulkan → D3D12 | MIT | Zink runs over MoltenVK on macOS |
| **ANGLE** | GLES → D3D11/Vulkan/Metal/GL | BSD-3 | SHAR already uses it |
| **MoltenVK** | Vulkan → Metal | Apache-2.0 | |

### 3.3 Shader toolchains

| Tool | Does | Licence | Use in VanGFX 3.0 |
|---|---|---|---|
| `d3dcompiler_47` (FXC) | HLSL → DXBC SM 2–5.1 | Windows component | D3D9/10/11 targets (already used) |
| **DXC** | HLSL → DXIL SM 6.x, and → SPIR-V | NCSA (LLVM) + permissive bundled parts | D3D12 and Vulkan targets |
| **SPIRV-Cross** | SPIR-V → GLSL / ESSL / MSL / HLSL, reflection | Apache-2.0 | OpenGL, GLES and Metal targets; reflection everywhere |
| **glslang** | GLSL → SPIR-V | BSD-3 core, mixed | only if GLSL *input* is ever accepted |
| **Slang** | Slang/HLSL → SPIR-V, DXIL, Metal, GLSL, WGSL, CUDA | Apache-2.0 WITH LLVM-exception; Khronos-hosted, in the Vulkan SDK | strong candidate for phase 5+; Metal and WGSL still marked experimental |
| **Metal Shader Converter** | DXIL → Metal IR | Apple proprietary; redistribution unclear | optional, user-installed, never redistributed |
| dxbc-spirv / dxil-spirv | game bytecode → SPIR-V | MIT | translating *game* shaders for a Vulkan target |

Caveat noted by SPIRV-Cross itself: entry points and stage interfaces need
handling by hand when one language's features have no equal in another. Effects
therefore keep to a documented subset (`04-HOW-TO.md` §3).

### 3.4 Rendering abstraction libraries (for the mod-facing GPU layer)

| Library | Backends | Licence | Why not adopted |
|---|---|---|---|
| NVRHI | D3D11, D3D12, Vulkan | MIT | no GL/Metal; built to *own* a device, not to borrow a game's |
| Diligent | D3D11/12, Vulkan, Metal, WebGPU, GL/GLES | Apache-2.0 | large; also assumes it owns the device |
| bgfx | many | BSD-2 | owns the device and its own shader format |
| SDL_GPU | D3D12, Vulkan, Metal | zlib | owns the device; no D3D9/11 |

All of them create the device. A mod has to **borrow** the game's device, run
between two of the game's commands, and leave its state as it found it. None of
these is designed for that; ReShade's add-on API is, and it is the model.

### 3.5 Hooking per API

| API | How tools attach | Notes |
|---|---|---|
| D3D8/9/10/11, DXGI, D3D12 | proxy DLL by name, or vtable patch on an object | VanGFX does vtables today |
| OpenGL (Windows) | `opengl32.dll` proxy, or inline hook of `wglSwapBuffers` | Linux: `glXSwapBuffers`; EGL: `eglSwapBuffers` |
| Vulkan | **implicit layer** with a JSON manifest; must have a disable env var | OBS registers `VK_LAYER_OBS_HOOK` this way; a layer may intercept only `vkQueuePresentKHR` |
| Metal | Objective-C swizzle of `-[MTLCommandBuffer presentDrawable:]` / `CAMetalLayer nextDrawable`, injected with `DYLD_INSERT_LIBRARIES` | Metal 4 moves presentation to queue-level calls; both paths needed |

## 4. Licence policy that follows

- **Linkable or vendorable:** MIT, BSD-2/3, zlib, Apache-2.0, NCSA. Keep each
  licence beside the vendored code (as MWOn12 does for D3D12MA) and list it in the
  release's notices file.
- **Reference only, never copied:** LGPL (vkd3d-proton, WineD3D), GPL anything.
- **Not used:** dgVoodoo2 (closed), Metal Shader Converter as a redistributable
  (the user may point VanGFX at their own install).
- **Allowed by licence but excluded by rule:** D3D9On12 / D3D11On12 /
  D3D12TranslationLayer as a rendering backend (team hard rule). Reading them to
  settle a correctness question is fine and encouraged.
- **No FetchContent / network at build time.** Vendored, versioned copies.

## Sources

- ReShade docs: <https://crosire.github.io/reshade-docs/index.html>,
  `REFERENCE.md` <https://raw.githubusercontent.com/crosire/reshade/main/REFERENCE.md>,
  licence <https://raw.githubusercontent.com/crosire/reshade/main/LICENSE.md>
- ReShade 6.1.1 changelog: <https://www.softpedia.com/progChangelog/ReShade-me-Changelog-263063.html>
- DXVK 3.0: <https://phoronix.com/news/DXVK-3.0-Release>,
  <https://www.gamingonlinux.com/2026/06/dxvk-3-0-brings-lots-of-improvements-for-windows-games-on-linux-steamos/>;
  licence <https://raw.githubusercontent.com/doitsujin/dxvk/master/LICENSE>
- dxbc-spirv: <https://github.com/doitsujin/dxbc-spirv>; dxil-spirv: <https://github.com/HansKristian-Work/dxil-spirv>
- vkd3d-proton licence: <https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/LICENSE>
- d3d8to9 licence: <https://raw.githubusercontent.com/crosire/d3d8to9/main/LICENSE.md>; d912pxy: <https://raw.githubusercontent.com/megai2/d912pxy/master/LICENSE>
- dxwrapper licence: <https://raw.githubusercontent.com/elishacloud/dxwrapper/master/License.txt>
- dgVoodoo2 licensing discussion: <https://www.vogons.org/viewtopic.php?p=696075>
- D3D9On12: <https://github.com/microsoft/D3D9On12>; D3D12TranslationLayer: <https://github.com/microsoft/D3D12TranslationLayer>
- Mesa licence: <https://docs.mesa3d.org/license.html>; Zink: <https://docs.mesa3d.org/drivers/zink.html>; Dozen: <https://www.phoronix.com/news/Mesa-Dozen-VLK-D3D12>
- ANGLE licence: <https://raw.githubusercontent.com/google/angle/main/LICENSE>; MoltenVK licence: <https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/LICENSE>
- SPIRV-Cross: <https://manpages.debian.org/unstable/spirv-cross/spirv-cross.1.en.html>, licence <https://raw.githubusercontent.com/KhronosGroup/SPIRV-Cross/main/LICENSE>
- DXC licence: <https://raw.githubusercontent.com/microsoft/DirectXShaderCompiler/main/LICENSE.TXT>
- Slang licence: <https://raw.githubusercontent.com/shader-slang/slang/master/LICENSE>; Khronos launch: <https://x-cmd.com/blog/241128>
- glslang licence: <https://raw.githubusercontent.com/KhronosGroup/glslang/main/LICENSE.txt>
- Metal Shader Converter: <https://developer.apple.com/metal/shader-converter/>, redistribution question <https://developer.apple.com/forums/thread/829379>
- NVRHI: <https://forums.developer.nvidia.com/t/writing-portable-rendering-code-with-nvrhi/187195>; Diligent: <https://gitee.com/pentalimbed/DiligentEngine>; bgfx: <https://bkaradzic.github.io/bgfx/overview.html>
- Vulkan layers: <https://renderdoc.org/vulkan-layer-guide.html>, <https://vulkan.lunarg.com/doc/view/latest/mac/LoaderLayerInterface.html>, <https://vulkan.lunarg.com/doc/view/latest/mac/LoaderDebugging.html>
- Special K texture hashing: <https://github.com/Kaldaien/UnX/releases/tag/unx_091>, <https://steamcommunity.com/groups/SpecialK_Mods/discussions/0/2217311444324159112>
- Dolphin texture dumping: <https://forums.dolphin-emu.org/Thread-texture-dumping-details>, graphics mods <https://www.mintlify.com/dolphin-emu/dolphin/advanced/graphics-mods>
- OpenGL present hooking: <https://github.com/j0cb/RenderHook>, <https://werwolv.net/blog/dll_injection>
- Metal swizzling / injection: <https://book.hacktricks.xyz/macos-hardening/macos-security-and-privilege-escalation/macos-proces-abuse/macos-function-hooking>
