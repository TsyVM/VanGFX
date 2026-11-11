# VanGFX 3.0 — When to Use What

Three different things are called "a graphics mod", and two more are called
"a proxy". Each wants a different tool. Pick before starting; the wrong one
costs a weekend.

---

## 1. I want to change how a game looks

| You want to | Use | You need | C++? |
|---|---|---|---|
| Add a post-process look (colour grade, sharpen, bloom, CRT, fog) | **an effect folder** under `VanGFX\effects\` | a text editor, HLSL | no |
| Change one of the game's own shaders (lighting, water, a car's paint) | **shader replacement**: `ShaderDump=1`, copy the file up out of `Dumped\`, edit | a text editor | no |
| Swap textures (HD packs, UI, liveries) | **texture replacement**: `TextureDump=1`, copy the `.dds` up out of `Dumped\`, replace | an image editor that saves DDS | no |
| Draw a menu, HUD or debug view on top of the frame | **overlay** with VanGUI 2.0 | ~20 lines of C++ in an `.asi` | yes |
| React to draws as they happen: skip some, re-bind, inject per draw, capture the G-buffer | **`vangfx::Context`** events (`on_draw`, `on_pso_create`, …) | C++ | yes |

Rules of thumb:

- **Most ideas are row 1 or row 2.** If an effect folder does it, stop there: it
  runs on every API the game might use and anyone can install it.
- An effect sees the *finished* frame (plus depth, when depth discovery finds
  it). If the change has to happen *before* lighting, or only on some objects,
  it is shader replacement or events, not an effect.
- Shader replacement is tied to one game's bytecode hashes. Effects are not.

## 2. I want the game to run on a different API

| Situation | Use |
|---|---|
| A D3D9 game should render with D3D12 (stability, modern drivers, VanGFX D3D12 features) | the **d3d9 → d3d12 translator** (what MWOn12/SFOn12 use), in a game proxy |
| A D3D9 game on Vulkan | the **d3d9 → vulkan** target (roadmap phase 6), same front end |
| A D3D8 game | the **d3d8 → d3d9** target, then whatever D3D9 does |
| A D3D10/11 game on D3D12 | not planned: D3D11 drivers are mature; use interception, not translation. Revisit only with a concrete game that needs it |
| Anything on Linux/macOS | DXVK / vkd3d-proton / MoltenVK as the *user's* environment; VanGFX's interception still runs on top of them |

Rules of thumb:

- **Translate only when the target API buys something** the game cannot get
  otherwise (a dead driver path, a feature VanGFX's D3D12 events need, a
  platform). Translation is the most expensive thing VanGFX does to maintain.
- **Keep a passthrough option** in every translating proxy (`Backend=1` in
  MWOn12): the game must start on a machine the translator cannot serve.

## 3. I want a proxy DLL

| You want | Use |
|---|---|
| A DLL the game loads, which only hooks and forwards (e.g. to load mods, fix a window mode, read input) | **proxy kit, passthrough spec**: generate, write nothing but your hooks |
| A DLL that replaces one or two exports and forwards the rest | **proxy kit, spec with `implement` entries** |
| A full API replacement (translator) | **proxy kit + `ComWrap` + a translator target** |
| Two mods that both want to be `d3d9.dll` | **chain loading**: rename one (`d3d9.reshade.dll`), set `[Proxy] Chain=` in the other |
| Just load `.asi` files | **vgx-loader** under a generic name (`dinput8.dll`, `version.dll`, `winmm.dll`) |

Which DLL name to proxy:

| Game uses | Proxy | Notes |
|---|---|---|
| D3D8 | `d3d8.dll` | 32-bit only — `d3d8.dll` exists only in SysWOW64 |
| D3D9 | `d3d9.dll` | |
| D3D10/10.1 | `d3d10.dll` / `d3d10_1.dll` or `dxgi.dll` | `dxgi.dll` is earlier and catches the swap chain |
| D3D11 | `dxgi.dll` (present, swap chain) or `d3d11.dll` (device creation) | `dxgi.dll` is the usual choice |
| D3D12 | `dxgi.dll` | proxying `d3d12.dll` interferes with the Agility SDK redirect; avoid |
| OpenGL | `opengl32.dll` | or a function hook on `wglSwapBuffers` without a proxy |
| Vulkan | **no proxy**: implicit layer | |
| Unknown | `dinput8.dll`, `version.dll`, `winmm.dll` | widely imported; then attach by vtable / function hook |

## 4. I'm writing C++ against VanGFX: which layer?

| You need | Layer | Header |
|---|---|---|
| Events on any API | `Context` (front ends) | `<vangfx/vangfx.hpp>` |
| Draw your own pass on any API | `gpu::Device` / `gpu::Pass` | `<vangfx/gpu/gpu.hpp>` |
| Load and run effect folders from your own product | `fx::Chain` | `<vangfx/fx/fx.hpp>` |
| Make a proxy DLL | `proxy::` + `tools/vgx_proxy.py` | `<vangfx/proxy/proxy.hpp>` |
| Translate D3D9 | `translate::d3d9` + a target | `<vangfx/translate/d3d9.hpp>` |
| The real device for API-specific work | `gpu::Device::native()` or the event's native pointers | — |

Prefer the highest layer that does the job. `native()` is always there, but code
written against it runs on one API.

## 5. Which shader compiler

| Target | Compiler | When |
|---|---|---|
| D3D9 (SM 2–3), D3D10/11 (SM 4–5) | FXC (`d3dcompiler_47.dll`) | always for these; ships with Windows |
| D3D12 (SM 6.x) | DXC (`dxcompiler.dll` + `dxil.dll`) | packs: at build time; runtime only for modders iterating |
| D3D12 with SM 5.1 shaders | FXC is fine | D3D12 accepts DXBC |
| Vulkan | DXC `-spirv` | |
| OpenGL / GLES / Metal | SPIRV-Cross from the SPIR-V | build time (vgxc) only |

## 6. Which hash name

| File | Name | Hash of |
|---|---|---|
| Game shader (any API) | `<stage>_<16 hex>.<ext>`, variant `_v<8 hex>` | the game's own bytecode (ContentHash v1) |
| Fixed-function shader (D3D9) | `ffpvs_<16 hex>` / `ffpps_<16 hex>` | the FFP permutation key |
| Texture | `<w>x<h>_<fmt>_<16 hex>.dds` | mip 0's bytes as the game uploaded them |

Generic name first; a variant only when one case must differ from another.
