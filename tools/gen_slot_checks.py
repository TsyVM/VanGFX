"""
Generates src/internal/compat/slot_checks.c: one static assert per vtable slot
constant in src/internal/compat/*_slots.hpp, checked against the Windows SDK's own
C vtable layout (the <Interface>Vtbl structs the SDK headers declare when
CINTERFACE is defined).

The slot tables are hand-written numbers. A wrong one does not fail to compile:
it patches a different method, and the hook then runs with the wrong arguments.
With the checks built into the library, a table that disagrees with the SDK
stops the build instead.

    py tools/gen_slot_checks.py            # rewrite slot_checks.c and slot_checks.cpp
    py tools/gen_slot_checks.py --report   # list constants and what they map to

Run it after editing any *_slots.hpp.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPAT = os.path.join(ROOT, "src", "internal", "compat")
OUT_C = os.path.join(COMPAT, "slot_checks.c")
OUT_CPP = os.path.join(COMPAT, "slot_checks.cpp")

# (header, C++ namespace path) -> (SDK vtable struct used for method lookups,
#                                  SDK vtable struct whose size is kSlotCount)
# The lookup struct is the newest interface in the family, so every method a
# table names -- base or extension -- is a member of it at its real index.
MAPPING = {
    ("d3d12_slots.hpp", "vangfx::d3d12_slots::common"): ("ID3D12ObjectVtbl", "ID3D12ObjectVtbl"),
    ("d3d12_slots.hpp", "vangfx::d3d12_slots::swap_chain3"): ("IDXGISwapChain4Vtbl", "IDXGISwapChain3Vtbl"),
    ("d3d12_slots.hpp", "vangfx::d3d12_slots::command_queue"): ("ID3D12CommandQueueVtbl", "ID3D12CommandQueueVtbl"),
    ("d3d12_slots.hpp", "vangfx::d3d12_slots::graphics_command_list"): ("ID3D12GraphicsCommandList7Vtbl", "ID3D12GraphicsCommandListVtbl"),
    ("d3d12_slots.hpp", "vangfx::d3d12_slots::device"): ("ID3D12Device14Vtbl", "ID3D12DeviceVtbl"),
    ("d3d11_slots.hpp", "vangfx::d3d11_slots::swap_chain"): ("IDXGISwapChainVtbl", "IDXGISwapChainVtbl"),
    ("d3d11_slots.hpp", "vangfx::d3d11_slots::swap_chain1"): ("IDXGISwapChain1Vtbl", "IDXGISwapChain1Vtbl"),
    ("d3d11_slots.hpp", "vangfx::d3d11_slots::device_context"): ("ID3D11DeviceContext4Vtbl", "ID3D11DeviceContextVtbl"),
    ("d3d11_slots.hpp", "vangfx::d3d11_slots::device"): ("ID3D11Device5Vtbl", "ID3D11DeviceVtbl"),
    ("d3d11_slots.hpp", "vangfx::d3d11_slots::multithread"): ("ID3D11MultithreadVtbl", "ID3D11MultithreadVtbl"),
    ("dxgi_slots.hpp", "vangfx::dxgi_slots::factory"): ("IDXGIFactory7Vtbl", "IDXGIFactoryVtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots"): ("IDirect3DDevice9ExVtbl", "IDirect3DDevice9Vtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots::device_ex"): ("IDirect3DDevice9ExVtbl", "IDirect3DDevice9ExVtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots::direct3d"): ("IDirect3D9ExVtbl", "IDirect3D9Vtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots::direct3d_ex"): ("IDirect3D9ExVtbl", "IDirect3D9ExVtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots::swap_chain"): ("IDirect3DSwapChain9ExVtbl", "IDirect3DSwapChain9Vtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots::query"): ("IDirect3DQuery9Vtbl", "IDirect3DQuery9Vtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots::state_block"): ("IDirect3DStateBlock9Vtbl", "IDirect3DStateBlock9Vtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots::vertex_buffer"): ("IDirect3DVertexBuffer9Vtbl", "IDirect3DVertexBuffer9Vtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots::index_buffer"): ("IDirect3DIndexBuffer9Vtbl", "IDirect3DIndexBuffer9Vtbl"),
    ("d3d9_slots.hpp", "vangfx::d3d9_slots::texture"): ("IDirect3DTexture9Vtbl", "IDirect3DTexture9Vtbl"),
    ("xaudio2_slots.hpp", "vangfx::xaudio2_slots::IXAudio2"): ("IXAudio2Vtbl", "IXAudio2Vtbl"),
    ("xaudio2_slots.hpp", "vangfx::xaudio2_slots::IXAudio2Voice"): ("IXAudio2VoiceVtbl", "IXAudio2VoiceVtbl"),
    ("xaudio2_slots.hpp", "vangfx::xaudio2_slots::IXAudio2SourceVoice"): ("IXAudio2SourceVoiceVtbl", "IXAudio2SourceVoiceVtbl"),
    ("xaudio2_slots.hpp", "vangfx::xaudio2_slots::IXAudio2MasteringVoice"): ("IXAudio2MasteringVoiceVtbl", "IXAudio2MasteringVoiceVtbl"),
    ("dsound_slots.hpp", "vangfx::dsound_slots::IDirectSound8"): ("IDirectSound8Vtbl", "IDirectSound8Vtbl"),
    ("dsound_slots.hpp", "vangfx::dsound_slots::IDirectSoundBuffer8"): ("IDirectSoundBuffer8Vtbl", "IDirectSoundBuffer8Vtbl"),
}

# Constants whose name is not the method's name.
RENAMES = {}

CONST = re.compile(r"(?:inline\s+)?constexpr\s+uint32_t\s+(\w+)\s*=\s*(\d+)\s*;")
NS_OPEN = re.compile(r"^\s*namespace\s+([\w:]+)\s*\{")
NS_ALIAS = re.compile(r"^\s*namespace\s+\w+\s*=")


def parse(path):
    """Yield (namespace path, name, value) for every slot constant in a header."""
    stack = []
    depth = 0
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.split("//")[0]
            if NS_ALIAS.match(line):
                continue
            m = NS_OPEN.match(line)
            if m:
                stack.append((m.group(1), depth))
                depth += 1
                continue
            for m in CONST.finditer(line):
                yield "::".join(n for n, _ in stack), m.group(1), int(m.group(2))
            depth += line.count("{") - line.count("}")
            while stack and depth <= stack[-1][1]:
                stack.pop()


def main():
    report = "--report" in sys.argv
    c_lines, cpp_lines, missing = [], [], []
    count = 0
    for (header, ns), (lookup, base) in MAPPING.items():
        consts = [(n, v) for (p, n, v) in parse(os.path.join(COMPAT, header)) if p == ns]
        if not consts:
            missing.append(f"{ns}: no constants found in {header}")
            continue
        c_lines.append(f"/* {ns}  ->  {lookup} */")
        cpp_lines.append(f"// {ns}")
        for name, value in consts:
            label = f"{ns}::{name} = {value}"
            if name == "kSlotCount":
                c_lines.append(f'_Static_assert(sizeof({base}) / sizeof(void*) == {value}, "{label}");')
            else:
                member = RENAMES.get((ns, name), name)
                c_lines.append(f'_Static_assert(VANGFX_SLOT({lookup}, {member}) == {value}, "{label}");')
            cpp_lines.append(f'static_assert({ns}::{name} == {value}, "{label}: run tools/gen_slot_checks.py");')
            count += 1
            if report:
                print(f"{label}  ->  {lookup}.{RENAMES.get((ns, name), name)}")
        c_lines.append("")
        cpp_lines.append("")
    if missing:
        print("\n".join(missing), file=sys.stderr)
        return 1

    header = """/* Generated by tools/gen_slot_checks.py -- do not edit by hand.
 *
 * Every vtable slot VanGFX patches, checked against the Windows SDK's own C layout
 * of the interface (the <Interface>Vtbl structs the SDK declares for C). A slot
 * table entry that names the wrong method stops the build here instead of
 * patching the wrong function at run time. Compiled as C because that is the
 * mode every one of these SDK headers supports.
 */
"""
    c_text = header + """
#define CINTERFACE
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define D3D11_NO_HELPERS
#include <windows.h>
#include <stddef.h>
#include <d3d9.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <xaudio2.h>
#include <dsound.h>

#define VANGFX_SLOT(vtbl, member) (offsetof(vtbl, member) / sizeof(void*))

""" + "\n".join(c_lines) + f"/* {count} slots checked. */\n"

    cpp_text = """// Generated by tools/gen_slot_checks.py -- do not edit by hand.
//
// The slot tables still hold the numbers slot_checks.c verified against the SDK.
// If one of these fails, a *_slots.hpp was edited: rerun the generator, and
// slot_checks.c will then check the new number against the SDK.

#include "d3d9_slots.hpp"
#include "d3d11_slots.hpp"
#include "d3d12_slots.hpp"
#include "dxgi_slots.hpp"
#include "xaudio2_slots.hpp"
#include "dsound_slots.hpp"

""" + "\n".join(cpp_lines) + f"// {count} slots.\n"

    for path, text in ((OUT_C, c_text), (OUT_CPP, cpp_text)):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    print(f"wrote slot_checks.c and slot_checks.cpp: {count} checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
