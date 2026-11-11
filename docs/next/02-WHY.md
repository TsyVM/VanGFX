# VanGFX 3.0 — Why This Way

Every significant choice in `01-ARCHITECTURE.md`, what else was considered, and
why it lost. When a decision here turns out wrong, change this file in the same
change that reverses it.

---

## D1. Grow VanGFX instead of starting a new library

**Chosen:** VanGFX 3.0 is today's VanGFX plus new layers.
Today's interception code, `hooks::Site`, slot checks, capture and 55 passing
WARP tests are kept.

**Rejected:**
- *A new library beside VanGFX.* The interception half of goal (a) already
  exists and is tested; MWOn12's SDK already ships VanGFX to plugin authors.
  Two libraries would split the event model a mod is written against.
- *Rebuild from scratch.* The September rebuild (`VanGFX-pre-rebuild-20260929`)
  already paid for that once. The hook code that came out of it is the best part
  of the codebase.

## D2. Front end × target as the organising idea

**Chosen:** every job is "API received" × "API issued" (`01` §2).

**Why:** it makes interception and translation the same kind of thing, which
is what lets the effect layer be written once. It also tells a newcomer where
code goes: does it depend on what the game speaks, or on what we issue?

**Rejected:** *one module per proxy* (`d3d9to12/`, `d3d9tovk/`, …). That is
today's MWOn12/SFOn12 shape, and it is exactly how two copies of one D3D9
translator came to drift (`00` §2.2).

## D3. Copy the D3D9 translator into VanGFX as one shared core

**Chosen:** `translate/d3d9` is a copy of MWOn12's `core/` with SFOn12's fixes
merged in; `translate/targets/d3d9_to_d3d12` is a copy of MWOn12's `dx12/`.
**Scope: VanGFX only.** MWOn12, SFOn12 and every other project are read as
sources and never modified by this effort; whether and when a game switches to
VanGFX's copy is that game's own decision, later.

**Why:**
- SFOn12 fixed two fixed-function bugs (column-major FFP matrices; empty stage
  reading transparent black) that are still live in MWOn12. One copy means one
  fix.
- The next D3D9 game (Hit & Run's original PC release, the CrossFire and Drift
  City projects on disk) starts at "spec + caps overrides", not at "copy 20,000
  lines".

**Rejected:**
- *A git submodule shared by the games.* Nothing here is under git yet, and the
  games would still each patch "their" copy.
- *Leave them separate and port fixes by hand.* That is the current state, and
  the evidence says the fixes do not get ported.

**Cost accepted:** VanGFX's copy is verified by its own tests (translator output
compiles; WARP draws through the translated device), not in-game. A game that
later adopts it re-verifies in-game then.

## D4. Translate D3D9 shaders to HLSL, not to a target's bytecode

**Chosen (kept from MWOn12):** SM1–3 bytecode → HLSL text → a compiler per target.

**Why:** HLSL is the one form every target can reach: FXC/DXC for Direct3D, DXC's
SPIR-V backend for Vulkan, SPIRV-Cross onward to GLSL and MSL. One translator
feeds D3D12, Vulkan and Metal. And HLSL is readable, which is what the shader
kit puts in front of modders.

**Rejected:**
- *D3D9 bytecode → SPIR-V directly* (DXVK's route). Faster to compile, but it
  produces nothing a modder can edit, and it would be a second translator.
- *D3D9 bytecode → DXBC/DXIL directly.* Same objections, and DXIL needs signing.

**Cost accepted:** compile time. Mitigated by the disk cache MWOn12 already has
(`ShaderDiskCache`) and by compiling on worker threads (DXVK 3.0's lesson).

## D5. Plain HLSL for effects, not a custom language

**Chosen:** an effect is HLSL plus an `effect.ini`. Cross-compiled by DXC and
SPIRV-Cross.

**Why:**
- Every shader author already knows HLSL; every HLSL tool (editors, RenderDoc,
  PIX, Shader Playground) works on the files as they are.
- ReShade's FX language works, but it is a language every contributor learns and
  a compiler someone maintains.
- `.ini` because every TeamVanilla product already configures that way
  (`MWOn12.ini`, `SFOn12.ini`), and an `.ini` parser is already needed.

**Rejected:**
- *ReShade FX compatibility.* Large surface; would tie VanGFX to another
  project's language evolution. Can be added later as an importer if wanted.
- *Slang as the authoring language now.* The best long-term candidate (Khronos
  hosted, Apache-2.0 + LLVM exception, targets SPIR-V/DXIL/Metal/GLSL/WGSL), but
  its Metal and WGSL output are still marked experimental. Slang accepts most
  HLSL, so effects written in HLSL today can move to a Slang compiler later
  without being rewritten. Revisit at phase 5.
- *GLSL as the authoring language.* Fewer game modders know it, and the Direct3D
  targets would then need a GLSL → HLSL step.
- `#pragma vgx …` *annotations inside the HLSL.* Compilers warn on unknown
  pragmas, and the files stop being plain HLSL.

## D6. Precompiled effect packs (vgxc), runtime compile as a fallback

**Chosen:** a tool compiles an effect to every target ahead of time; the runtime
compiles only on Windows, only when no pack exists.

**Why:** Metal and Android cannot be assumed to have a compiler; nobody should
ship `dxcompiler.dll` to every player; and SHAR already proves the build-time
model on Android. Runtime compile on Windows keeps "edit, restart" working for
the modder writing the effect.

**Rejected:** *runtime cross-compilation everywhere* (bundle DXC + SPIRV-Cross
into the loader). Tens of MB per game folder, slow first frame, and DXC does not
build for every platform VanGFX targets.

## D7. A borrowed-device GPU layer of our own, not NVRHI/Diligent/bgfx/SDL_GPU

**Chosen:** `vangfx::gpu`, small, with backends that wrap a device the game
created.

**Why:** a mod runs *inside someone else's frame*. It must not create the device,
must save and restore state on the immediate-mode APIs, must submit on the game's
queue before its present on the explicit ones, and must survive the game
resizing or resetting under it. NVRHI, Diligent, bgfx and SDL_GPU all assume
they own the device (`00` §3.4). ReShade's add-on API is the one design built for
borrowing, and `gpu` follows its shape (D3D12/Vulkan-like objects, older APIs
presented through the same interface) without its code.

**Rejected:**
- *Use ReShade as the runtime and write VanGFX as add-ons.* Licence would permit
  it (BSD-3). But the product would be ReShade's, its hooks would own the
  process, and MWOn12-style translators — where *we* are the device — do not fit
  an add-on model at all.
- *Expose only native handles and let each mod write per-API code.* That is
  today's VanGFX for D3D12, and is why `replace_ps` does nothing there.

## D8. Generated assembly export stubs for proxies

**Chosen:** `tools/vgx_proxy.py` reads the real DLL's export table and generates
a `.def` and one `jmp [slot]` MASM stub per export, resolved lazily on first
call.

**Why:**
- Hand-written `.def` files miss exports. MWOn12's exports 12 of d3d9's 23
  symbols, including none of the six ordinal-only ones and not
  `Direct3DShaderValidatorCreate9`, which D3DX calls.
- A `jmp` stub forwards **any** signature, so undocumented and ordinal-only
  exports need no prototype.
- Lazy resolution keeps `LoadLibrary` out of `DllMain`.

**Rejected:**
- *Linker forwarders* (`Direct3DCreate9=d3d9.Direct3DCreate9` in the `.def`).
  A forwarder names a module, not a path, and the loader resolves `d3d9` to the
  already-loaded proxy. Forwarding to a renamed copy (`d3d9_orig.dll`) works but
  means shipping a copy of a system DLL, and it cannot honour a chain setting.
- *Typed C++ forwarders.* Need every signature, including undocumented ones; a
  wrong guess corrupts the stack on x86 `__stdcall`.
- *`LoadLibrary` in `DllMain` and patching the import table.* Loader lock.

**Cost accepted:** two `.asm` files (x86 and x64; ARM64 Windows is not a target
for these games). CMake's `ASM_MASM` support builds them; `ml.exe`/`ml64.exe`
ship with Visual Studio.

## D9. Vulkan through an implicit layer

**Chosen:** VanGFX registers as `VK_LAYER_TEAMVANILLA_vangfx` with a manifest and
a disable environment variable.

**Why:** the loader is designed for this (OBS, MangoHud and RenderDoc all do it),
the dispatch chain is explicit, and no code is patched. A layer that only needs
present can intercept only `vkQueuePresentKHR` and creation calls.

**Rejected:** *a `vulkan-1.dll` proxy.* The loader is not a fixed set of exports
games link against in a stable way, and replacing the loader breaks every other
layer. An in-process hook is kept only as a fallback for games that bundle their
own loader.

## D10. OpenGL and Metal through function hooks / swizzling

**Chosen:** GL: `wglSwapBuffers`/`eglSwapBuffers` inline hooks via VanHooks (or an
`opengl32` proxy when the loader is used). Metal: Objective-C swizzling of the
present calls, Metal 3 and Metal 4 paths.

**Why:** neither API has objects with vtables to patch; the end-of-frame
function is the one dependable place.

**Accepted risk:** Metal code is written and reviewed but **not compiled or run**
here; it is marked experimental in the release until someone with a Mac builds
and tests it.

## D11. Keep ContentHash v1 = MWOn12's FNV-1a + avalanche

**Chosen:** VanGFX's shader and texture hashes start with the exact function
MWOn12's `ShaderMods.cpp` uses.

**Why:** every existing `ps_<hash>.hlsl` written for MWOn12 keeps working
unchanged, and SFOn12's dumps line up with MWOn12's tools. Special K's history
shows what changing a shipped hash does to users' packs.

**Rejected:** *xxHash3* (faster). Not worth breaking packs; can be v2 later
with a different name prefix.

## D12. VanHooks as a prebuilt dependency, optional

**Chosen:** link the VanHooks SDK (headers + lib) when `VANGFX_VANHOOKS=ON`.

**Why:** it already does inline hooks on x86/x64/ARM64 across Windows, Linux and
macOS; writing a second one is waste. Its source build fetches Zydis over the
network, which MWOn12 removed for that reason, so VanGFX uses the prebuilt
drop as MWOn12 vendors it.

**Rejected:** *MinHook / Detours.* They work, but the team has its own.

## D13. Licence lines

**Chosen:** permissive only in anything shipped (MIT, BSD, zlib, Apache-2.0,
NCSA). LGPL projects (vkd3d-proton, WineD3D) are read, never copied.
dgVoodoo2 and Metal Shader Converter are not redistributed.

**Why:** VanGFX ships as a closed-source static library; LGPL code inside a
static library obliges relinkability, which the SDK drop cannot offer.

## D14. Phases end at something built and tested

**Chosen:** each phase in `05-ROADMAP.md` ends with a build on x86 and x64,
the full WARP suite still passing, and new tests for what was added.

**Why:** the September rebuild's lesson, and the reason the current suite exists.

---

## D12. Slang as the front compiler for vgxc (Phase 5 re-evaluation)

**Evaluated:** 2026-11-11, Phase 5.

**Question:** Should `vgxc` use [Shader Slang](https://shader-slang.com/) as its
front-end instead of accepting raw HLSL and driving DXC + d3dcompiler directly?

**Decision: Not now. HLSL → DXC/d3dcompiler stays.**

**Reasons to adopt Slang:**
- Single compiler for all targets (DXBC, DXIL, SPIR-V, GLSL, ESSL, MSL, WGSL,
  CUDA) with one coherent API; no SPIRV-Cross needed as a second step.
- Generics and interfaces give mod shaders a cleaner way to express conditionally
  compiled paths vs. HLSL preprocessor macros.
- Active development (NVIDIA + Google funding); growing game-engine adoption
  (Unreal, Godot, Falcor, Lumen).

**Reasons to stay with HLSL + DXC/d3dcompiler:**
1. **Source compatibility.** Every existing effect in the team's library is HLSL.
   A Slang front end breaks the common case without gain.
2. **DXC is already on the machine.** Windows SDK 10.0.26100.0 ships `dxcapi.h`
   and `dxcompiler.dll`. Slang would add a >10 MB vendored binary with its own
   update cadence.
3. **Slang's DXBC path is DXC + d3dcompiler under the hood.** For D3D9/10/11
   games (the primary targets) the net result is identical bytecode with an extra
   translation layer.
4. **API surface for mods.** `vgxc` is a build tool, not a runtime. Mod authors
   write one file (`effect.ini` + `.hlsl`); they do not call a compiler API
   directly. The choice of compiler is invisible to them.
5. **SPIRV-Cross is the right join point.** Once SPIRV-Cross is vendored
   (`-DVANGFX_SPIRV_CROSS=ON`), the pipeline is DXC → SPIR-V → SPIRV-Cross →
   GLSL/ESSL/MSL. That is the standard, well-tested route and matches how most
   real-time engines work today. Slang's SPIRV-Cross-free path is attractive but
   not mature enough for game targeting.

**Revisit when:** a mod author ships a shader that needs Slang generics, or when
Slang's DXBC/HLSL compatibility surface covers everything in the team's library.
