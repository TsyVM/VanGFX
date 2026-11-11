"""
vgx_proxy.py — VanGFX proxy generator

Reads a DLL's export table with dumpbin and produces:

    exports.def        — EXPORTS section with every named and ordinal export
    stubs_x64.asm      — MASM x64: jmp [slot] stubs + lazy resolver
    stubs_x86.asm      — MASM x86: jmp [slot] stubs + lazy resolver
    exports_table.cpp  — slot table initialised to the resolver
    CMakeLists.txt     — ready-to-build proxy project
    proxy_main.cpp     — VGX_PROXY_ON_FIRST_CALL() placeholder (not overwritten)
    spec.json          — per-export mode: "forward" or "implement"

Usage:
    py tools\\vgx_proxy.py <dll_path> --out <dir> [--name <name>]
                           [--translator d3d9_to_d3d12]

Options:
    --out          Output directory (created if absent).
    --name         Override the proxy DLL name (default: stem of <dll_path>).
    --translator   Add a translator target dependency to the CMakeLists.
    --dumpbin      Path to dumpbin.exe  (auto-discovered from VS installs).
    --spec         Path to an existing spec.json to merge (preserves "implement" entries).
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# ── dumpbin discovery ──────────────────────────────────────────────────────────

DUMPBIN_SEARCH = [
    # VS 2022 Community / Professional / Enterprise, x64 host
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC",
    r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC",
    r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC",
    # VS 2019
    r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC",
]

def find_dumpbin() -> str:
    for base in DUMPBIN_SEARCH:
        base = Path(base)
        if not base.is_dir():
            continue
        # Pick the newest version
        versions = sorted(base.iterdir(), reverse=True)
        for ver in versions:
            candidate = ver / "bin" / "Hostx64" / "x64" / "dumpbin.exe"
            if candidate.is_file():
                return str(candidate)
    # Last resort: on PATH
    return "dumpbin.exe"

# ── export table parsing ───────────────────────────────────────────────────────

class Export:
    __slots__ = ("ordinal", "hint", "name", "noname")
    def __init__(self, ordinal: int, hint: int | None, name: str | None):
        self.ordinal = ordinal
        self.hint    = hint
        self.name    = name         # None for NONAME / ordinal-only exports
        self.noname  = name is None

def parse_exports(dll_path: str, dumpbin: str) -> list[Export]:
    """Run dumpbin -nologo -exports and parse the output."""
    result = subprocess.run(
        [dumpbin, "-nologo", "-exports", dll_path],
        capture_output=True, text=True, check=True
    )
    exports = []
    # The table starts after the "ordinal hint RVA  name" header line
    in_table = False
    for line in result.stdout.splitlines():
        if re.search(r'\bordinal\b.*\bhint\b', line, re.I):
            in_table = True
            continue
        if not in_table:
            continue
        # Named export:   "     27    0 0006A820 D3DPERF_BeginEvent"
        m = re.match(r'^\s+(\d+)\s+([0-9A-Fa-f]+)\s+[0-9A-Fa-f]+\s+(\S+)', line)
        if m:
            name = m.group(3)
            if name == "[NONAME]":
                exports.append(Export(int(m.group(1)), None, None))
            else:
                exports.append(Export(int(m.group(1)), int(m.group(2), 16), name))
            continue
        # NONAME (no hint):  "     16      000B5080 [NONAME]"
        m = re.match(r'^\s+(\d+)\s+[0-9A-Fa-f]+\s+\[NONAME\]', line)
        if m:
            exports.append(Export(int(m.group(1)), None, None))
            continue
        # Bare ordinal:    "      5"
        m = re.match(r'^\s+(\d+)\s*$', line)
        if m:
            exports.append(Export(int(m.group(1)), None, None))
            continue
        # End of table on a blank line after content
        if in_table and exports and not line.strip():
            break
    return exports

# ── slot name helpers ──────────────────────────────────────────────────────────

def slot_name(exp: Export) -> str:
    if exp.name:
        return f"vgx_slot_{exp.name}"
    return f"vgx_slot_ord{exp.ordinal}"

def stub_label(exp: Export) -> str:
    if exp.name:
        return exp.name
    return f"vgx_ord{exp.ordinal}_stub"

# ── file generators ────────────────────────────────────────────────────────────

def write_def(exports: list[Export], dll_stem: str, out: Path):
    lines = [f"LIBRARY {dll_stem}", "EXPORTS"]
    for exp in exports:
        if exp.name:
            lines.append(f"    {exp.name} @{exp.ordinal}")
        else:
            lines.append(f"    vgx_ord{exp.ordinal}_stub @{exp.ordinal} NONAME")
    (out / "exports.def").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_stubs_x64(exports: list[Export], dll_stem: str, out: Path):
    lines = [
        "; stubs_x64.asm — generated by vgx_proxy.py",
        "; Do not edit.  Re-run the generator to update.",
        ".CODE",
        "",
        "; Lazy resolver — called on the first use of any export.",
        "; Saves the full set of argument registers before calling",
        "; vgx_resolve_slot(), then jumps through the now-filled slot.",
        "EXTERN vgx_resolve_slot:PROC",
        "",
        "vgx_lazy_resolver PROC",
        "    ; rcx = slot address (passed by the per-export trampoline below)",
        "    ; Save argument registers across the resolver call.",
        "    push    rsp",
        "    push    rbp",
        "    mov     rbp, rsp",
        "    sub     rsp, 88",             # 4 GP + 4 XMM + 8 shadow + align
        "    and     rsp, -16",
        "    mov     [rsp+32], rdx",
        "    mov     [rsp+40], r8",
        "    mov     [rsp+48], r9",
        "    movdqu  [rsp+56], xmm1",
        "    movdqu  [rsp+72], xmm2",
        "    movdqu  [rsp+88], xmm3",
        "    ; rcx already holds the slot pointer; rdx holds the original rcx value",
        "    ; that the per-export trampoline stashed in rdx before jumping here.",
        "    call    vgx_resolve_slot",
        "    ; Restore",
        "    movdqu  xmm3, [rsp+88]",
        "    movdqu  xmm2, [rsp+72]",
        "    movdqu  xmm1, [rsp+56]",
        "    mov     r9,  [rsp+48]",
        "    mov     r8,  [rsp+40]",
        "    mov     rdx, [rsp+32]",
        "    lea     rsp, [rbp]",
        "    pop     rbp",
        "    pop     rsp",           # discard pushed rsp (rbp restored above)
        "    ; rax = resolved function pointer; jump to it",
        "    jmp     rax",
        "vgx_lazy_resolver ENDP",
        "",
    ]

    for exp in exports:
        sn = slot_name(exp)
        label = stub_label(exp)
        lines += [
            f"EXTERN {sn}:QWORD",
            f"{label} PROC",
            f"    mov  rcx, OFFSET {sn}",
            f"    jmp  QWORD PTR [{sn}]",
            f"{label} ENDP",
            "",
        ]

    lines.append("END")
    (out / "stubs_x64.asm").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_stubs_x86(exports: list[Export], dll_stem: str, out: Path):
    lines = [
        "; stubs_x86.asm — generated by vgx_proxy.py",
        "; Do not edit.  Re-run the generator to update.",
        ".MODEL FLAT, STDCALL",
        ".CODE",
        "",
        "EXTERN _vgx_resolve_slot@4:PROC",
        "",
    ]
    for exp in exports:
        sn = slot_name(exp)
        label = stub_label(exp)
        lines += [
            f"EXTERN _{sn}:DWORD",
            f"PUBLIC _{label}",
            f"_{label} PROC",
            f"    jmp  DWORD PTR [_{sn}]",
            f"_{label} ENDP",
            "",
        ]
    lines.append("END")
    (out / "stubs_x86.asm").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_exports_table(exports: list[Export], dll_stem: str, out: Path):
    lines = [
        "// exports_table.cpp — generated by vgx_proxy.py",
        "// Do not edit.  Re-run the generator to update.",
        "#include <vangfx/proxy/proxy.hpp>",
        "",
        "// Forward-declared resolver (defined in stubs_*.asm).",
        "extern \"C\" void vgx_lazy_resolver();",
        "",
        "namespace {",
        "",
        "// One slot per export.  Initialised to the lazy resolver; filled on",
        "// first call by vgx_resolve_slot() (called from the asm resolver).",
    ]
    for exp in exports:
        sn = slot_name(exp)
        lines.append(f'extern "C" void* {sn} = reinterpret_cast<void*>(&vgx_lazy_resolver);')
    lines += [
        "",
        "} // namespace",
        "",
        "// Called by the asm lazy resolver with a pointer to the slot.",
        '// Returns the resolved function pointer and fills the slot so',
        '// subsequent calls are a single jmp without going through here.',
        "extern \"C\" void* vgx_resolve_slot(void** slot) {",
        f'    return vangfx::proxy::resolve_slot(slot, "{dll_stem}.dll");',
        "}",
    ]
    (out / "exports_table.cpp").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_spec(exports: list[Export], existing_spec: dict | None, out: Path):
    spec = []
    existing = {e["name"]: e for e in (existing_spec or []) if e.get("name")}
    for exp in exports:
        name = exp.name or f"[ordinal {exp.ordinal}]"
        entry = existing.get(name, {})
        spec.append({
            "name":    name,
            "ordinal": exp.ordinal,
            "noname":  exp.noname,
            "mode":    entry.get("mode", "forward"),
        })
    (out / "spec.json").write_text(json.dumps(spec, indent=2), encoding="utf-8")


def write_cmake(dll_stem: str, translator: str | None, out: Path):
    proxy_target = f"{dll_stem}_proxy"
    lines = [
        f"# CMakeLists.txt — {dll_stem} passthrough proxy",
        "# Generated by vgx_proxy.py.  Edit the target name and options as needed.",
        "cmake_minimum_required(VERSION 3.20)",
        f'project({proxy_target} LANGUAGES C CXX ASM_MASM)',
        "",
        "# ── VanGFX ───────────────────────────────────────────────────────────────",
        'if(NOT DEFINED VANGFX_ROOT)',
        '    message(FATAL_ERROR "Set VANGFX_ROOT to the VanGFX source tree.")',
        "endif()",
        'add_subdirectory(${VANGFX_ROOT} ${CMAKE_BINARY_DIR}/vangfx_build EXCLUDE_FROM_ALL)',
        "",
        "# ── Proxy DLL ────────────────────────────────────────────────────────────",
        f"add_library({proxy_target} SHARED",
        "    proxy_main.cpp",
        "    exports_table.cpp",
        "    $<IF:$<EQUAL:$<TARGET_PROPERTY:$<TARGET_NAME>,COMPILE_OPTIONS:-m32>,1>,",
        "        stubs_x86.asm,",
        "        stubs_x64.asm>",
        ")",
        "",
        "# Pick the right stub file for the target architecture.",
        f"if(CMAKE_SIZEOF_VOID_P EQUAL 4)",
        f"    target_sources({proxy_target} PRIVATE stubs_x86.asm)",
        f"else()",
        f"    target_sources({proxy_target} PRIVATE stubs_x64.asm)",
        f"endif()",
        "",
        "# Remove the auto-generated lib prefix on Windows.",
        f'set_target_properties({proxy_target} PROPERTIES',
        f'    OUTPUT_NAME "{dll_stem}"',
        '    PREFIX ""',
        ")",
        "",
        f"target_link_libraries({proxy_target} PRIVATE vangfx::vangfx)",
        f"target_link_options({proxy_target} PRIVATE /DEF:${{CMAKE_CURRENT_SOURCE_DIR}}/exports.def)",
        "",
        "set_property(TARGET {proxy_target} PROPERTY MSVC_RUNTIME_LIBRARY",
        '    "MultiThreaded$<$<CONFIG:Debug>:Debug>")',
    ]
    if translator:
        lines += [
            "",
            f"# Translator target",
            f"target_link_libraries({proxy_target} PRIVATE vangfx::translate_{translator})",
        ]
    (out / "CMakeLists.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_proxy_main(dll_stem: str, out: Path):
    path = out / "proxy_main.cpp"
    if path.exists():
        return  # never overwrite the user's implementation
    path.write_text(f"""\
// proxy_main.cpp — {dll_stem} proxy
// This file is NOT regenerated; edit it freely.

#include <vangfx/proxy/proxy.hpp>

// Called once, on the first call into any export, outside the loader lock.
VGX_PROXY_ON_FIRST_CALL() {{
    vangfx::proxy::log("{dll_stem}: proxy loaded");
}}
""", encoding="utf-8")


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dll_path",           help="Path to the DLL to proxy")
    ap.add_argument("--out",    required=True, help="Output directory")
    ap.add_argument("--name",             help="Override DLL stem name")
    ap.add_argument("--translator",       help="Translator target to link")
    ap.add_argument("--dumpbin",          help="Path to dumpbin.exe")
    ap.add_argument("--spec",             help="Existing spec.json to merge")
    args = ap.parse_args()

    dll_path = args.dll_path
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    dll_stem = args.name or Path(dll_path).stem.lower()
    dumpbin  = args.dumpbin or find_dumpbin()

    print(f"[vgx_proxy] dumpbin: {dumpbin}")
    print(f"[vgx_proxy] dll:     {dll_path}")
    print(f"[vgx_proxy] out:     {out}")
    print(f"[vgx_proxy] stem:    {dll_stem}")

    exports = parse_exports(dll_path, dumpbin)
    if not exports:
        print("ERROR: no exports found — check the DLL path and dumpbin output", file=sys.stderr)
        sys.exit(1)
    print(f"[vgx_proxy] exports: {len(exports)}")

    existing_spec = None
    spec_src = args.spec or str(out / "spec.json")
    if os.path.exists(spec_src):
        with open(spec_src, encoding="utf-8") as f:
            existing_spec = json.load(f)

    write_def(exports, dll_stem, out)
    write_stubs_x64(exports, dll_stem, out)
    write_stubs_x86(exports, dll_stem, out)
    write_exports_table(exports, dll_stem, out)
    write_spec(exports, existing_spec, out)
    write_cmake(dll_stem, args.translator, out)
    write_proxy_main(dll_stem, out)

    print(f"[vgx_proxy] done -- {len(exports)} exports -> {out}")


if __name__ == "__main__":
    main()
