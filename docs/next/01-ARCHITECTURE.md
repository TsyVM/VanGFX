# VanGFX 3.0 — Architecture

The shape of the next VanGFX: what the pieces are, what each one owns, and how
they connect. Rationale is in `02-WHY.md`; the order of building is in
`05-ROADMAP.md`. Facts quoted here are from `00-RESEARCH.md`.

---

## 1. Three goals, one library

| Goal | Who it is for | What VanGFX 3.0 gives them |
|---|---|---|
| **(a)** graphics mods for any game | modders, many of whom do not write C++ | a drop-in loader, HLSL effect folders, shader and texture replacement by hash, overlays through VanGUI |
| **(b)** D3D8–12 proxies | TeamVanilla engineers making the next MWOn12 | a proxy kit (generated exports, system-DLL loading, COM wrapping) and shared translator cores |
| **(c)** every graphics API | both of the above | one mod-facing GPU layer and one event model over D3D8/9/10/11/12, Vulkan, OpenGL/GLES and Metal |

Today's VanGFX is the interception half of (a), for D3D9/11/12. It stays, and
grows into the rest.

## 2. The two axes: front end and target

Every graphics job VanGFX does is a pair:

- the **front end** — the API the game speaks, which VanGFX receives;
- the **target** — the API VanGFX issues work to.

```
  game speaks ──►  FRONT END  ──►  (events, effects, replacement)  ──►  TARGET  ──► driver
                   d3d8 d3d9                                            d3d9 d3d11
                   d3d10 d3d11                                          d3d12 vulkan
                   d3d12 gl gles                                        gl gles metal
                   vulkan metal
```

- **Interception** is a front end whose target is the same API (D3D11 in,
  D3D11 out). This is everything VanGFX 2026 does.
- **A proxy that translates** is a front end with a different target (D3D9 in,
  D3D12 out). That is MWOn12 and SFOn12.
- The **effect layer** sits between them and only ever talks to the *target*.
  That is why an effect written once runs under MWOn12 (target D3D12, though
  the game is D3D9) and under plain interception of a D3D11 game.

## 3. Layers

```
┌───────────────────────────────────────────────────────────────────────────┐
│  PRODUCTS     vgx-loader (drop-in DLL / Vulkan layer)   game proxies      │
│               (MWOn12, SFOn12, …)                         vgxc (tool)     │
├───────────────────────────────────────────────────────────────────────────┤
│  fx           effect chain · shader replace · texture replace · uniforms  │
│               · depth discovery · overlay slot (VanGUI)                   │
├───────────────────────────────────────────────────────────────────────────┤
│  gpu          the BORROWED-device layer: textures, buffers, pipelines,    │
│               passes, state save/restore — one interface, every target    │
├──────────────────────────────┬────────────────────────────────────────────┤
│  front ends (intercept)      │  translate                                 │
│  d3d8 d3d9 d3d10 d3d11 d3d12 │  d3d9 core: state, caps, formats,          │
│  dxgi gl/wgl/egl vulkan-layer│    SM1–3 → HLSL, FFP → HLSL, shader mods   │
│  metal                       │  targets: d3d9→d3d12, d3d9→vulkan,         │
│                              │    d3d8→d3d9, dxbc→spirv (vendored)        │
├──────────────────────────────┴────────────────────────────────────────────┤
│  proxy        export stubs (generated) · system module loader · chain     │
│               loading · COM wrap/unwrap registry · loader-lock-safe init  │
├───────────────────────────────────────────────────────────────────────────┤
│  hook         hooks::Site (vtables, today's) · function hooks via VanHooks│
├───────────────────────────────────────────────────────────────────────────┤
│  core         Result/errors · log · config (ini + env) · content hash ·   │
│               paths · callback lists · event types — no graphics API      │
└───────────────────────────────────────────────────────────────────────────┘
```

Each layer depends only on layers below it. `core` builds on every platform
(Windows, Linux, Android, macOS); everything Windows-only says so in its target
name.

### 3.1 core

What exists in `src/core` and `src/internal` minus anything D3D, plus:

- **`vgx::Config`** — `[Section] Key=Value` ini, with an environment override
  for every key (`VANGFX_<SECTION>_<KEY>`), searched beside the DLL then beside
  the executable. MWOn12's `BackendSelect` pattern, generalised. A missing file
  is a valid config of defaults.
- **`vgx::ContentHash`** — **v1 is MWOn12's FNV-1a + avalanche, bit for bit**,
  so every existing MWOn12 shader-kit file keeps its name. The hash is a file
  format: versioned, never changed in place (Special K's lesson).
- **`vgx::Paths`** — the mod folder (`<game>\VanGFX\` by default; a product may
  rename it, as MWOn12 uses `MWOn12\`): `effects\`, `shaders\`, `shaders\Dumped\`,
  `textures\`, `textures\Dumped\`, `logs\`.
- **Log** — two logs, as MWOn12 learned: an ASCII loader log, and a UTF-8 (BOM)
  detail log.

### 3.2 hook

- `hooks::Site` and the slot tables in `src/internal/compat` stay as they are,
  including `slot_checks.c/.cpp`. New slot tables: `d3d8_slots`, `d3d10_slots`.
- **Function hooks** (inline: `wglSwapBuffers`, `eglSwapBuffers`, a
  `vkQueuePresentKHR` when no layer can be used) go through a thin
  `vgx::hook::function` interface implemented by the **prebuilt VanHooks SDK**
  (headers + lib, vendored like MWOn12's `external/VanHooks`). Configure option
  `VANGFX_VANHOOKS`; without it, function hooks return `Unsupported` and every
  vtable path still works.

### 3.3 proxy (Windows) — goal (b)'s foundation

A proxy DLL has four chores that are the same every time and wrong easily:

1. **Export every symbol the real DLL exports**, including ordinal-only ones.
   MWOn12's hand-written `.def` exports 12 of `d3d9.dll`'s 23 symbols: it is
   missing `Direct3DShaderValidatorCreate9` (which D3DX's shader assembler
   calls), `PSGPError`, `PSGPSampleTexture`, `DebugSetLevel`,
   `Direct3D9EnableMaximizedWindowedModeShim` and the six `[NONAME]` ordinals
   16–19, 22, 23. Any of those called is a failed load.
2. **Load the real DLL** by absolute System32/SysWOW64 path, never by name (it
   would load the proxy), and never under the loader lock.
3. **Chain** to another proxy instead when asked (`[Proxy] Chain=d3d9.reshade.dll`),
   so two mods that both want to be `d3d9.dll` can coexist.
4. **Wrap COM objects** the game gets back, keep wrapper identity stable through
   `QueryInterface`, and unwrap objects the game passes in.

The kit:

- **`tools/vgx_proxy.py`** reads a DLL's export table (via `dumpbin`, or a JSON
  spec checked into `proxy/specs/`) and generates, per DLL:
  - `exports.def` — every export by name and ordinal, `NONAME` preserved;
  - `stubs_x64.asm` / `stubs_x86.asm` — one MASM stub per export:
    `jmp [slot]`. The slot starts at a per-export *resolver* that saves the
    argument registers (x64: rcx, rdx, r8, r9, xmm0–3; x86 arguments are on the
    stack and untouched), calls `vgx_proxy_resolve(index)`, restores, and jumps
    to the result. **No signature is needed for any export**: an undocumented
    ordinal forwards as correctly as `Direct3DCreate9`.
  - `exports_table.cpp` — names/ordinals for the resolver.
  - Exports listed as `implement` in the spec are left out of the stubs and
    bound in the `.def` to the author's own C++ function instead.
- **`vgx::proxy::SystemModule`** — resolves `System32` vs `SysWOW64` (by process
  bitness, `GetSystemDirectoryW`/`GetSystemWow64DirectoryW`), honours the chain
  setting, loads once behind `std::call_once`, logs which file was loaded and why.
- **`vgx::proxy::ComWrap<I>`** (phase 2) — base for wrappers generated from the
  slot tables: refcount pairing, `QueryInterface` identity, a wrap/unwrap
  registry keyed by the real pointer, and passthrough of every method the author
  does not override.
- **Specs shipped:** `d3d8`, `d3d9`, `d3d10`, `d3d10_1`, `d3d11`, `dxgi`,
  `d3d12`, `opengl32`, `dinput8` (a common chain host), `version`/`winmm` (common
  generic hosts for an ASI loader).

### 3.4 front ends (interception)

Existing: D3D9/9Ex, D3D11, D3D12 under `src/backend/`. They are renamed
*front ends* in the docs but keep their folders until there is a reason to move
them. Added, in roadmap order:

| Front end | Attach point | Notes |
|---|---|---|
| DXGI | `IDXGISwapChain::Present/Present1/ResizeBuffers` | the API-neutral present hook D3D10/11/12 games share |
| D3D10 / 10.1 | vtables as D3D11 | D3D10 runs on the D3D11 runtime; most code is shared |
| D3D8 | vtables, or translate to D3D9 (§3.5) | 32-bit only (`d3d8.dll` exists only in SysWOW64) |
| OpenGL (WGL) | `wglSwapBuffers` / `SwapBuffers` function hook, or `opengl32` proxy | GL has no objects to patch; state save/restore by hand |
| GLES (EGL, Android) | `eglSwapBuffers` hook | built with NDK 29 here, run on device only |
| Vulkan | **implicit layer** (`VK_LAYER_TEAMVANILLA_vangfx`, JSON manifest, `DISABLE_VANGFX_LAYER` env var); in-process `vkGetDeviceProcAddr` hook as a fallback | the layer is the supported route |
| Metal | swizzle `-[MTLCommandBuffer presentDrawable:]` and Metal 4's queue-level present; `DYLD_INSERT_LIBRARIES` | written, not compiled here (no macOS machine) |

Every front end raises the **same** events (`on_present`, `on_draw`,
`on_render_target`, `on_resource`, …). API-only extras stay API-namespaced, as
`vangfx::d3d12::` is today.

### 3.5 translate — the shared translator cores

The D3D9 translator code, copied from MWOn12 `core/` with SFOn12's fixes merged
in, owned by VanGFX. MWOn12 and SFOn12 are unchanged; each adopts this copy
when the team can test in-game.

```
translate/
  d3d9/                         API-independent (MWOn12 core/ + SFOn12's fixes)
    state_tracker   caps_table   format_map   shader_translator (SM1–3 → HLSL)
    ffp_gen (fixed function → HLSL)   shader_mods (dump/replace, hash v1)
  targets/
    d3d9_to_d3d12/              MWOn12 dx12/core + dx12/d3d9proxy
    d3d9_to_vulkan/             same front end; HLSL → SPIR-V by DXC
    d3d8_to_d3d9/               D3D8 objects over D3D9 (own code; d3d8to9 as reference)
  bytecode/
    dxbc_to_spirv/              vendored dxbc-spirv (MIT), for D3D10/11 → Vulkan
```

The pivot that makes this cheap: **the D3D9 translator emits HLSL, not D3D12
bytecode.** HLSL compiles to DXIL (D3D12), to SPIR-V (Vulkan, by DXC's SPIR-V
backend), and from SPIR-V to MSL and GLSL (SPIRV-Cross). So one front end feeds
every target; only the resource/command half of a target is written per API.

A game proxy becomes: a spec, a `.def`, per-game `CapsTable` overrides, and the
game's own fixes (SFOn12's frame pacing and clock fixes stay in SFOn12).

### 3.6 gpu — the borrowed-device layer — goal (c)'s core

The mod-facing GPU interface. It does *not* create devices: it wraps the device,
queue and swap chain a front end found, records the mod's work between two of
the game's commands, and leaves the game's state as it found it.

```cpp
namespace vangfx::gpu {
class Device {                // one per game device, made by the front end
    Api api() const;          // D3D9 D3D10 D3D11 D3D12 Vulkan GL GLES Metal
    Result<Texture>  create_texture(const TextureDesc&, const InitData* = {});
    Result<Buffer>   create_buffer(const BufferDesc&, const void* init = {});
    Result<Pipeline> create_pipeline(const PipelineDesc&);   // takes a ShaderPack
    void*            native() const;   // the real ID3D12Device* / VkDevice / …
};
class Pass {                  // one mod pass inside a game frame
    // begin() saves the game's state (D3D9 state block, D3D11 context state,
    // GL state list; D3D12/Vulkan/Metal record into the mod's own command list)
    void set_pipeline(Pipeline&);
    void bind(uint32_t slot, Texture&);
    void bind(uint32_t slot, Buffer&);
    void draw_fullscreen();
    void draw(uint32_t vertices, uint32_t instances = 1);
    void dispatch(uint32_t x, uint32_t y, uint32_t z);
    // end() restores, or submits to the game's queue before its present
};
}
```

Conventions are SHAR's: Direct3D's clip space, top-left origin, numbered
slots; a backend that differs (GL's bottom-left, Vulkan's Y) fixes it up itself.

**Per-target state rule** (the part that breaks games when wrong):

| Target | How a pass leaves the game untouched |
|---|---|
| D3D9 | `IDirect3DStateBlock9` capture/apply around the pass |
| D3D10/11 | `ID3DDeviceContextState` swap (D3D11.1) or explicit save of everything the pass sets |
| D3D12 | own command list and allocator per frame in flight, submitted on the game's queue before `Present`; the game's lists are never touched (MWOn12's `MarkCommandStateDirty` is the in-process equivalent when the translator *is* the device) |
| Vulkan | own command pool per queue family; semaphore-chained before `vkQueuePresentKHR` |
| GL/GLES | save and restore the exact state list a pass touches (program, VAO, FBO, textures/samplers per unit, blend, depth, viewport, scissor) |
| Metal | own command buffer committed before the game's drawable is presented |

### 3.7 fx — the effect runtime — goal (a)'s core

Built only on `gpu` and the common events, so it is API-blind.

- **Effect chain.** An *effect* is a folder: `effect.ini` (passes, inputs,
  parameters, order) and `.hlsl` files. Inputs: `backbuffer`, `depth` (when
  depth discovery found it), `previous` (the last pass), any named intermediate,
  any texture file in the folder. Runs at present, in chain order.
- **Parameters.** Declared in `effect.ini`, edited live in the overlay, saved to
  `effects\<name>\user.ini`. Delivered in the existing `vangfx_params` cbuffer
  (`vangfx_float4[8]`, `vangfx_time`, `vangfx_delta`, `vangfx_resolution`), so
  today's `inject_ps_*` snippets remain valid.
- **Shader replacement by hash.** MWOn12's shader kit, for every front end:
  `ShaderDump=1` writes what the game uses into `shaders\Dumped\`; a file of the
  same name in `shaders\` replaces it. Names: `<stage>_<16 hex>[_v<8 hex>].<ext>`
  where the hash is ContentHash v1 of the game's own bytecode and `<ext>` says
  what the file holds — `.hlsl` (D3D9 translated, or written by hand), `.dxbc`,
  `.dxil`, `.spv`, `.glsl`. A replacement in HLSL is compiled for whatever the
  target needs. Bad edits log once and fall back.
- **Texture replacement by hash.** `TextureDump=1` writes `.dds` named
  `<w>x<h>_<fmt>_<16 hex>.dds` (hash of mip 0's bytes, Dolphin-style size
  prefix); a file of that name in `textures\` replaces it at creation (or first
  upload, for APIs that fill textures after creating them). Wildcards later.
- **Depth discovery.** The D3D9/11 heuristics behind today's G-buffer classifier,
  promoted to a service: "the depth buffer that saw the most draws at back-buffer
  size this frame", per API.
- **Overlay slot.** At present, after the chain, the overlay callback runs with
  the native device/queue/back buffer in hand so a **VanGUI 2.0** backend
  (dx9/dx10/dx11/dx12/vulkan/opengl3/metal all exist) draws the menu. VanGFX ships
  no UI renderer.

### 3.8 vgxc — the effect/shader pack compiler (tool, runs on the modder's PC)

```
vgxc effects\MyLook  --targets all
  → effects\MyLook\.pack\  main.ps.dxbc  main.ps.dxil  main.ps.spv
                           main.ps.glsl  main.ps.essl  main.ps.msl  reflect.json
```

FXC (`d3dcompiler_47`) for SM 2–5, DXC for DXIL and SPIR-V, SPIRV-Cross for
GLSL/ESSL/MSL and the reflection that tells every backend which slot is which.
A shipped effect carries its pack, so **the player's machine needs no
compiler** — required for Metal and Android, and faster everywhere. On Windows
the runtime can still compile an `.hlsl` without a pack (it does today) when
`d3dcompiler_47.dll` / `dxcompiler.dll` are present.

### 3.9 Products

- **vgx-loader** — the universal drop-in for goal (a). One DLL built from
  `proxy` + front ends + `fx`, shipped under several names (`d3d9.dll`,
  `d3d8.dll`, `dxgi.dll`, `d3d11.dll`, `opengl32.dll`, `dinput8.dll`) and as a
  Vulkan implicit layer. It works out which name it was loaded as, forwards that
  DLL's exports, attaches the matching front end, and runs the effect folder.
  It also loads `.asi` plugins (MWOn12's `AsiLoader` moves into the kit).
- **Game proxies** (MWOn12, SFOn12, the next one) — link the kit and a
  translator target, add game fixes. Their SDK/plugin ABIs (`<mwon12/mwon12.h>`)
  are theirs and unchanged.
- **The SDK drop** stays as today: headers + prebuilt static libs per arch and
  config, in a curated `GitHub` folder, sources private.

## 4. Build layout

```
CMakeLists.txt                     options per layer / front end / target
cmake/                             config.hpp.in, toolchains (android-arm64, …)
include/vangfx/
  core/ hook/ proxy/ gpu/ fx/      public API per layer
  events/ resources/ pipeline/     (existing)
  d3d12/ vulkan/ gl/ metal/        API-only extras
src/
  core/ capture/ effect/           (existing)
  proxy/                           SystemModule, resolver, chain, ComWrap
  backend/{d3d8,d3d9,d3d10,d3d11,d3d12,dxgi,gl,vulkan,metal}/
  gpu/{d3d9,d3d11,d3d12,vulkan,gl,metal}/
  fx/                              chain, replace registry, texture packs
  translate/{d3d9,targets/*}/
  internal/compat/                 slot tables + slot_checks (existing)
proxy/specs/*.json                 export specs (generated, committed)
tools/vgx_proxy.py  tools/vgxc/    generators
external/                          vendored, each with its LICENSE + VENDORING.md
loader/                            vgx-loader product
samples/                           a passthrough proxy per API, an effect, a texture pack
tests/                             WARP tests (existing) + proxy + translate + fx
docs/next/                         these documents
```

**Options** (each defaults to what builds on the host): `VANGFX_D3D8`,
`VANGFX_D3D9`, `VANGFX_D3D10`, `VANGFX_D3D11`, `VANGFX_D3D12`, `VANGFX_DXGI`,
`VANGFX_VULKAN`, `VANGFX_GL`, `VANGFX_GLES`, `VANGFX_METAL`, `VANGFX_PROXY`,
`VANGFX_TRANSLATE_D3D9`, `VANGFX_FX`, `VANGFX_VANHOOKS`, `VANGFX_AUDIO`,
`VANGFX_TESTS`. A consumer tests them through the generated `vangfx/config.hpp`
exactly as today.

**Toolchains verified here:** MSVC (VS 2022 and VS 18) x86/x64; Android NDK 29
arm64-v8a/x86_64 (compile and unit tests that need no GPU). Not verifiable
here: Metal (no macOS machine), Linux.

## 5. Invariants

These hold across every layer and are tested where they can be:

1. **Nothing runs under the loader lock** but recording paths. No `LoadLibrary`,
   no adapter probe, no thread creation in `DllMain`.
2. **A proxy never loads itself.** System DLLs by absolute path only.
3. **A hash is a file format.** ContentHash v1 never changes; a new algorithm is
   v2 with a new file-name prefix.
4. **A bad mod never stops the game.** Failed compile, missing file, wrong size
   → log once, use the original.
5. **The game's state is the game's.** Every pass restores or isolates (§3.6).
6. **No exceptions across the API**; `Result<T>` (`std::expected`) everywhere, as
   today.
7. **No network at build time**; every dependency vendored with its licence.
8. **Our code translates.** Shipped translators never route through D3D9On12,
   D3D11On12, WineD3D, DXVK or vkd3d (the MWOn12 hard rule).
9. **The word the team does not write is not written.** Docs and code say
   "VanGUI" or "immediate-mode library".
