# VanGFX 3.0 — How To

Step-by-step for the common jobs. Each section names the roadmap phase that
makes it work; until that phase lands, the section is the specification it is
built to.

---

## 1. Make a passthrough proxy for any DLL (phase 1)

A proxy that forwards everything and lets you add hooks.

```bat
:: 1. Generate the proxy sources from the real DLL's export table.
py tools\vgx_proxy.py C:\Windows\System32\d3d9.dll --out myproxy

:: myproxy\  exports.def  stubs_x64.asm  stubs_x86.asm  exports_table.cpp
::           spec.json    CMakeLists.txt  proxy_main.cpp
```

2. Build it like any VanGFX consumer:

```bat
cmake -S myproxy -B build-x64 -A x64 -DVANGFX_ROOT=<path to VanGFX>
cmake --build build-x64 --config Release
```

3. Copy `build-x64\Release\d3d9.dll` beside the game's executable.

What you get without writing anything: every export of the real DLL, by name
and ordinal (ordinal-only exports included), forwarded to
`System32\d3d9.dll` — or `SysWOW64\d3d9.dll` in a 32-bit process — loaded on the
first call, never in `DllMain`. `<game>\VanGFX\logs\loader.log` says which file
was loaded.

**Add your own code** in `proxy_main.cpp`:

```cpp
#include <vangfx/proxy/proxy.hpp>

// Runs once, on the first call into any export, outside the loader lock.
VGX_PROXY_ON_FIRST_CALL() {
    vangfx::proxy::log("hello from the proxy");
}
```

**Override one export** — mark it in `spec.json` and write it:

```json
{ "name": "Direct3DCreate9", "ordinal": 37, "mode": "implement" }
```

```cpp
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    auto real = vangfx::proxy::real<decltype(&Direct3DCreate9)>("Direct3DCreate9");
    IDirect3D9* d3d = real(sdk);
    // wrap it, attach VanGFX, log the SDK version, …
    return d3d;
}
```

Re-run the generator whenever you change `spec.json`; it never touches
`proxy_main.cpp`.

**Chain to another proxy** (another mod that is also `d3d9.dll`): rename the other
one, e.g. `d3d9.other.dll`, and in `<game>\VanGFX\vgx.ini`:

```ini
[Proxy]
Chain=d3d9.other.dll
```

The chain target is loaded instead of the system DLL and itself loads the
system one.

## 2. Install and use the drop-in loader (phase 4)

For players and effect authors; no C++.

1. Copy the loader DLL beside the game executable **under the name the game
   needs** (`03-WHEN-TO-USE-WHAT.md` §3): `d3d9.dll`, `dxgi.dll`, `d3d8.dll`,
   `opengl32.dll`. For Vulkan games, run `vgx-layer-install.bat` once instead.
2. Start the game once. `<game>\VanGFX\` appears with `vgx.ini`, `effects\`,
   `shaders\`, `textures\`, `logs\`.
3. Press **Home** for the overlay: effects on/off, order, parameters, dump
   switches, the log.

## 3. Write a post-process effect (phase 4)

An effect is a folder in `VanGFX\effects\`.

```
effects\Vignette\
  effect.ini
  vignette.hlsl
```

`effect.ini`:

```ini
[Effect]
Name=Vignette
Order=50

[Pass.Main]
PixelShader=vignette.hlsl:main
Input0=backbuffer
Output=backbuffer

[Param.Strength]
Slot=0            ; vangfx_float4[0].x
Default=0.35
Min=0
Max=1
```

`vignette.hlsl`:

```hlsl
#include <vangfx.hlsli>    // vangfx_params, VgxFullscreen, vgx_sample()

float4 main(VgxFullscreen i) : SV_Target
{
    float4 c   = vgx_sample(0, i.uv);              // Input0
    float  d   = distance(i.uv, 0.5) * 1.41421356;
    float  k   = 1.0 - vangfx_float4[0].x * d * d;
    return float4(c.rgb * k, c.a);
}
```

Save, restart (or press **Reload** in the overlay on Windows). Compile errors go
to `logs\vangfx.log` once, and the effect is skipped; the game keeps running.

**Portable HLSL subset.** To compile for every target, an effect keeps to:
Shader Model 5-style syntax; textures and samplers through `vgx_sample()` /
`vgx_load()` (the header declares them per target); no `#pragma` other than
`pack_matrix`; no geometry, hull or domain shaders; compute only where the
target has it (not D3D9). `vgxc --check` reports anything outside the subset.

**Ship it to players** with a pack, so their machine needs no compiler:

```bat
vgxc effects\Vignette --targets all
```

This writes `effects\Vignette\.pack\`; include the folder in the download.

**Depth.** `Input1=depth` binds the game's depth buffer when depth discovery has
found one; `vgx_has_depth()` in HLSL says whether it did. Always handle "no".

## 4. Replace one of the game's shaders (phase 4; MWOn12 has it today)

1. In `vgx.ini`:
   ```ini
   [Shaders]
   Dump=1
   Replace=1
   ```
2. Play through what you want to change. Each shader the game uses is written
   once to `VanGFX\shaders\Dumped\`, named by a hash of the game's own bytecode.
3. Copy the one you want **up a level** into `VanGFX\shaders\`. Dumping only ever
   writes into `Dumped\`, so it cannot overwrite your work.
4. Edit and restart. A failed compile logs once and the original is used.

Finding the right one: empty `Dumped\`, go straight to the scene, see what
appears; or make a candidate return `float4(1, 0, 1, 1)` and look for magenta.

What the file holds depends on the game's API:

| Game | Dumped as | You edit |
|---|---|---|
| D3D9 (translating proxy, e.g. MWOn12) | `.hlsl` translated from SM1–3 | the `.hlsl` |
| D3D9 / D3D10 / D3D11 (interception) | `.dxbc` and a disassembly `.asm`; `.hlsl` when FXC reflection allows a skeleton | write an `.hlsl` with the same inputs/outputs |
| D3D12 | `.dxil` + disassembly | `.hlsl` |
| Vulkan | `.spv` + SPIRV-Cross `.hlsl`/`.glsl` decompile | the decompiled file |
| OpenGL | `.glsl` as the game supplied it | the `.glsl` |

## 5. Replace a texture (phase 4)

1. `[Textures] Dump=1` and `Replace=1` in `vgx.ini`.
2. Textures are written to `VanGFX\textures\Dumped\` as
   `<w>x<h>_<fmt>_<hash>.dds` the first time the game uploads them.
3. Copy one up into `VanGFX\textures\`, edit it, keep the name. Any size works;
   keep the aspect ratio. Block-compressed formats are fine (BC1–BC7); the
   replacement need not use the original's format.
4. Restart.

## 6. Draw an overlay with VanGUI (phase 4)

From an `.asi` plugin loaded by the loader:

```cpp
#include <vangfx/vangfx.hpp>
#include <vangfx/fx/overlay.hpp>

VGX_PLUGIN_MAIN(vangfx::Context& ctx) {
    vangfx::fx::on_overlay(ctx, [](vangfx::fx::OverlayFrame& f) {
        // f.api(), f.native_device(), f.native_queue(), f.back_buffer()
        // Initialise the matching VanGUI 2.0 backend once from these, then:
        // VanGUI::NewFrame(); … ; render with the backend.
    });
}
```

The callback runs after the effect chain, at present, with the back buffer bound
as the only target and the game's state saved.

## 7. Add a new game to the D3D9 → D3D12 translator (phase 3)

1. `py tools\vgx_proxy.py C:\Windows\SysWOW64\d3d9.dll --out MyGameOn12 --translator d3d9_to_d3d12`
2. In the generated project, `game_caps.cpp` holds the `CapsTable` overrides
   (start with none; add only what the game demonstrably needs).
3. Game-specific fixes (frame pacing, clocks, window modes) go in the game
   project, never in `translate/`. A fix to *how D3D9 is translated* goes in
   `translate/` with a test, and every game gets it.
4. `MyGameOn12.ini` `[Renderer] Backend=DX9` must start the game through the
   system runtime: always keep the passthrough.

## 8. Add a new front end or gpu backend (contributors)

- A front end lives in `src/backend/<api>/`, raises the common events from
  `src/core/context_impl.hpp`, and gets a slot table plus a `slot_checks` entry
  if it patches vtables.
- A `gpu` backend lives in `src/gpu/<api>/`, implements `gpu::Device` and
  `gpu::Pass`, and must pass the shared `gpu` conformance tests
  (`tests/gpu_conformance.cpp`: clear, fullscreen pass, texture upload/readback,
  state restored afterwards).
- State restoration is tested, not assumed: the conformance test draws a game
  draw, runs a mod pass, draws again, and compares against the frame without the
  mod pass.
