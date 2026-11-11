# VanGFX 3.0 — Roadmap

Phases in build order. Each ends with: x86 and x64 Release builds clean at
`/W4`, the existing WARP suite still passing, and new tests for what the phase
added. "Verified here" means on this machine (Windows 11, VS 2022 / VS 18,
WARP); anything that needs a game or another OS says so.

---

## Phase 0 — Hygiene ✓ (2026-11-11)

| Item | Status |
|---|---|
| Snapshot to `Projects\VanGFX-pre-3.0-20261111` | **done** |
| `VANGFX_AUDIO` default → OFF; default configure succeeds | **done** |
| README: C++23, removed `include/vangfx/compat/` from layout, added `docs/next/` link | **done** |
| `.gitignore` | **done** |
| `git init` in `Projects\VanGFX` | **done** |

Baseline (2026-11-11): **55/55** tests. After Phase 0+1: **81/81** tests (x64 and x86).

## Phase 1 — Core config/paths + proxy foundation ✓ (2026-11-11)

| Item | Status |
|---|---|
| `vgx::Config` (ini + `VANGFX_*` env override), `vgx::Paths` | **done** — 10 unit tests |
| `vgx::ContentHash` v1 = MWOn12's FNV-1a + avalanche, bit-exact | **done** — 7 unit tests (hash, hex, file names) |
| `vgx::proxy::SystemModule`: System32/SysWOW64, chain setting, `call_once`, log | **done** — 8 unit tests |
| `tools/vgx_proxy.py`: export table → `.def`, x86/x64 MASM stubs, table, CMake project, spec.json | **done** — generator runs, 23 exports from d3d9.dll |
| Specs committed: d3d8 (5), d3d9 (23), dxgi (20), d3d11 (51), d3d12 (19) under `tools/specs/` | **done** |
| d3d9 Direct3DCreate9 → adapter count via SystemModule | **done** — proxy_tests |
| dxgi CreateDXGIFactory1 → D3D11 WARP device via SystemModule | **done** — proxy_tests |

**Remaining in Phase 1 (not yet done):**
- Resolver stub asm (.asm stubs + lazy resolver): generated but not compiled into the library (they go in a game proxy sub-project, not the lib). An end-to-end test that calls through a generated proxy's jmp stubs is a Phase 1 follow-up.
- Specs for opengl32, dinput8, version, winmm (can run vgx_proxy.py on them any time).
- Chain loading: code is written; not tested end-to-end without a real chain DLL.

Not in phase 1: COM wrapping, loader product.

## Phase 2 — COM wrapping + D3D8/D3D10 slot tables ✓ (2026-11-11)

| Item | Verified by |
|---|---|
| `proxy::ComWrap<I>` + wrap/unwrap registry, generated passthrough wrappers for `IDirect3D9`, `IDirect3DDevice9`, `IDXGIFactory*`, `IDXGISwapChain*` | refcount and `QueryInterface` identity tests; a wrapped D3D9 device draws (on hardware when present, else skipped) |
| `d3d8_slots.hpp`, `d3d10_slots.hpp` + `slot_checks` | static asserts |
| DXGI front end (`Present`, `Present1`, `ResizeBuffers`) shared by D3D10/11/12 | WARP tests |

After Phase 2: **92/92** tests (x64 and x86).

After Phase 3: **104/104** tests (x64 and x86).

After Phase 4: **131/131** tests (x64 and x86).

After Phase 5: **140/140** tests (x64, DXIL/SPIR-V soft-pass — dxcompiler.dll not on WARP PATH).

## Phase 3 — The shared D3D9 translator

| Item | Verified by |
|---|---|
| `translate/d3d9`: MWOn12 `core/` copied in (MWOn12/SFOn12 themselves untouched), SFOn12's fixes merged (FFP column-major matrices; empty stage = opaque white; the rest of the diff reviewed file by file) | shader translator tests: compile HLSL to `vs_3_0`/`ps_3_0` with FXC, translate, compile the result for `ps_5_0` and `ps_6_0`; FFP generator tests per permutation |
| `translate/targets/d3d9_to_d3d12`: MWOn12 `dx12/` copied in | builds; WARP draw tests through the translated device |
| An ASI loader in the kit (MWOn12's `AsiLoader` as reference) | unit test with a dummy `.asi` |

## Phase 4 — gpu layer (D3D9/11/12) + fx + loader (goal a) ✓ (2026-11-11)

| Item | Verified by |
|---|---|
| `gpu::Device`/`Pass` for D3D9, D3D11, D3D12 | 10 conformance tests: device create, texture create, pipeline create, fullscreen pass pixel, **state restored**, params cbuffer, begin_pass (D3D12) on WARP |
| `fx::Chain`: effect folders, `effect.ini`, parameters, `vangfx.hlsli` | 7 fx tests: depth service (5), effect_desc load/params/save, chain create/run/load/reload/overlay |
| `DepthService`: heuristic depth buffer discovery | 5 unit tests (most-draws wins, wrong-size ignored, begin_frame clears, null ignored) |
| `VanGFXParams` 256-byte CBV-aligned cbuffer (b0) | static_assert + params cbuffer test (time→red channel) |
| D3D11 state save/restore | d3d11_state_restored test |
| D3D12 ring-buffer SRV heap + per-pass barriers | d3d12_begin_pass crash-free test on WARP |

**Items deferred to Phase 5+:**
- Shader/texture replacement by hash (Phase 5 vgxc)
- Overlay slot + VanGUI 2.0 sample
- `vgx-loader` multi-name DLL support

## Phase 5 — vgxc and the cross-compilation toolchain ✓ (2026-11-11)

| Item | Verified by |
|---|---|
| DXC runtime loaded via `LoadLibraryW("dxcompiler.dll")` (Windows SDK 10.0.26100.0); `external/dxc/VENDORING.md` documents the find path | CMake reconfigure prints the SDK path; DXBC test passes; DXIL/SPIR-V soft-pass (dxcompiler.dll not on WARP runner PATH) |
| SPIRV-Cross source not yet vendored; GLSL/ESSL/MSL targets skip gracefully with `ok=false`; `external/spirv-cross/VENDORING.md` documents vendoring steps | `vgxc_cross_targets_skip` test |
| `vgxc_lib` STATIC (compile.cpp + pack.cpp) shared between CLI and test suite | builds without warnings at /W4 |
| `vgxc` CLI: `vgxc <effect-folder> [--targets csv] [--out dir] [--check] [-v]`; default targets `dxbc,dxil,spirv`; output to `<effect>/.pack/` | CLI builds; `vgxc_pack_effect` test |
| `compile(in, Target)`: HLSL → DXBC (d3dcompiler_47.dll), DXIL (dxcompiler.dll), SPIR-V (dxcompiler.dll -spirv) | `vgxc_compile_dxbc` passes; DXIL/SPIR-V soft-pass |
| `reflect_dxbc()`: D3DReflect → binding list (cbuffer/texture/sampler/uav) | `vgxc_reflect_dxbc` test |
| `pack_effect()`: reads `effect.ini`, iterates `[Pass …]` sections, compiles each HLSL to requested targets, writes outputs + `reflect.json` | `vgxc_pack_effect`, `vgxc_pack_all_bytecode`, `vgxc_reflect_json_content` tests |
| `reflect.json` schema: `{ "vgxc_version": 1, "effect": "…", "passes": [{…, "bindings": […]}] }` | `vgxc_reflect_json_content` test |
| `parse_targets("dxbc,dxil,spirv")` / `"all"` CSV parser | `vgxc_parse_targets` test |
| Slang re-evaluation appended to `02-WHY.md` | document |

**Items deferred to Phase 6+:**
- Vendor SPIRV-Cross and enable GLSL/ESSL/MSL targets (`-DVANGFX_SPIRV_CROSS=ON`)
- Ship `dxcompiler.dll` alongside `vgxc.exe` (Windows SDK is not redistributable as-is)
- `--check` portable-subset validation (requires SPIR-V validator)

## Phase 6 — Vulkan, OpenGL/GLES, D3D8 and D3D10 front ends ✓ (2026-11-11)

| Item | Status | Verified by |
|---|---|---|
| Vulkan implicit layer + `gpu` backend + manifest installer | **done** (stub — Unsupported until Vulkan SDK + hardware) | builds with `-DVANGFX_VULKAN=ON`; full draw tests require hardware Vulkan driver |
| OpenGL (WGL) front end via VanHooks + `gpu` backend | **done** | builds with `-DVANGFX_GL=ON`; gl_context_lifecycle, gl_device_* tests (hidden-window WARP-equivalent) |
| GLES/EGL front end + backend | **done** (stub — Unsupported until NDK target) | builds with `-DVANGFX_GLES=ON`; on-device run by the team |
| D3D8 front end; `d3d8_to_d3d9` target | **done** | builds with `-DVANGFX_D3D8=ON`; d3d8_shader_hash_*, d3d8_translate_* unit tests; draw tests require 32-bit hardware D3D9 |
| D3D10 front end | **done** | 146/146 WARP — `d3d10_draws_every_frame` ps_hash ✓ (slot table fix: missing SetPredication/VSGetShader/GetPredication shifted CreatePixelShader 80→82) |
| `d3d9_to_vulkan` target (same front end, DXC SPIR-V) | **done** (stub — returns `{}` until DXC SPIR-V available) | builds; full translator tests require hardware Vulkan |

All six items compile clean at `/W4` with `-DVANGFX_GL=ON -DVANGFX_D3D8=ON -DVANGFX_VULKAN=ON -DVANGFX_GLES=ON` (2026-11-11).

## Phase 7 — Metal and Android ✓ (2026-11-11)

| Item | Status | Verified by |
|---|---|---|
| Metal front end (swizzling, Metal 3 + Metal 4 present paths) + `gpu` backend | **done** (stub — Unsupported until macOS + Xcode 15+) | builds with `-DVANGFX_METAL=ON`; shipped as experimental; full tests require macOS hardware |
| Android: loader as an injected `.so` for GLES/Vulkan games where the team controls the APK (SHAR) | **done** (stub — `vangfx_android_attach/detach/context()` entry points) | builds on Windows; NDK `.so` requires `-DCMAKE_TOOLCHAIN_FILE=…/android.toolchain.cmake`; device run by the team |

Both items compile clean at `/W4` with `-DVANGFX_METAL=ON -DVANGFX_ANDROID=ON` (2026-11-11).

## Phase 8 — Release ✓ (2026-11-11)

| Item | Status |
|---|---|
| `GitHub/` SDK drop folder: `include/`, `lib/win-x64/`, `lib/win-x86/`, `loader/`, `docs/`, `samples/basic_hook/`, `NOTICES.md` | **done** — folder structure created; prebuilt `.lib` files are populated after each Release build |
| `VanGFX_Functions_Guide.md` updated for GL, D3D8, Vulkan, GLES, Metal, Android backends + `BackendKind` enum table | **done** (2026-11-11) |

## Ordering rationale

- **Proxy kit first** because it is self-contained, testable here end to end, and
  every later product (loader, game proxies) sits on it.
- **Translator before gpu/fx** because merging the MWOn12/SFOn12 drift gets
  harder every week both games are worked on.
- **gpu/fx on the three D3D backends before new APIs** because the effect model
  has to be right once before it is implemented six times.
- **Metal last** because nothing about it can be verified here.

## Resolved decisions

| Question | Decision |
|---|---|
| Version name | **VanGFX 3.0** |
| Scope | **VanGFX only** — other projects are read as reference, never modified |
| Git | **snapshot + `git init` locally** (Phase 0) |
| Phase 3 scope | Build the shared copy in VanGFX; each game adopts it when the team can test in-game |
| Loader overlay key | **Home** (F12 is SFOn12's; Insert is common in other tools) |
