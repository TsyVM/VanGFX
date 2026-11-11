/**
 * vgxc — VanGFX effect/shader pack compiler
 *
 * Usage:
 *   vgxc <effect-folder> [options]
 *
 * Options:
 *   --targets <csv>  comma-separated: dxbc,dxil,spirv,glsl,essl,msl (default: dxbc,dxil,spirv)
 *   --out <dir>      output directory (default: <effect>/.pack/)
 *   --check          validate portable subset only (no output files)
 *   -v               verbose output
 *   --help           show this help
 */

#include "pack.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static void print_help() {
    std::puts(
        "vgxc <effect-folder> [options]\n"
        "\n"
        "  --targets <csv>   dxbc,dxil,spirv,glsl,essl,msl or 'all'\n"
        "                    default: dxbc,dxil,spirv\n"
        "  --out <dir>       output dir (default: <effect>/.pack/)\n"
        "  --check           validate portable subset; no output files written\n"
        "  -v                verbose\n"
        "  --help            this message\n"
    );
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_help(); return 1; }

    std::string effect_arg;
    std::string out_arg;
    std::string targets_arg = "dxbc,dxil,spirv";
    bool check_only = false;
    bool verbose    = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_help(); return 0; }
        else if (arg == "--check")           check_only = true;
        else if (arg == "-v")                verbose    = true;
        else if (arg == "--targets" && i+1 < argc) targets_arg = argv[++i];
        else if (arg == "--out"     && i+1 < argc) out_arg     = argv[++i];
        else if (arg[0] != '-')                    effect_arg  = arg;
        else { std::fprintf(stderr, "unknown option: %s\n", arg.c_str()); return 1; }
    }

    if (effect_arg.empty()) { print_help(); return 1; }

    fs::path effect_folder = fs::path(effect_arg);
    if (!fs::is_directory(effect_folder)) {
        std::fprintf(stderr, "error: not a directory: %s\n", effect_arg.c_str());
        return 1;
    }

    fs::path out_dir = out_arg.empty()
        ? effect_folder / ".pack"
        : fs::path(out_arg);

    vgxc::PackOptions opts;
    opts.check_only = check_only;
    opts.verbose    = verbose;

    // Set all defaults off, then apply --targets
    opts.dxbc = opts.dxil = opts.spirv = false;
    opts.glsl = opts.essl = opts.msl   = false;

    std::string target_err;
    if (!vgxc::parse_targets(targets_arg, opts, target_err)) {
        std::fprintf(stderr, "error: %s\n", target_err.c_str());
        return 1;
    }

    auto result = vgxc::pack_effect(effect_folder, out_dir, opts);

    for (auto& e : result.errors) {
        bool is_warn = (e.rfind("[warn]", 0) == 0);
        std::fprintf(is_warn ? stdout : stderr, "%s\n", e.c_str());
    }

    if (result.ok) {
        if (check_only) {
            std::printf("check: %s OK\n", effect_folder.filename().string().c_str());
        } else if (verbose) {
            std::printf("pack: %s → %s\n",
                        effect_folder.filename().string().c_str(),
                        out_dir.string().c_str());
            for (auto& f : result.outputs)
                std::printf("  wrote: %s\n", f.c_str());
        } else {
            std::printf("pack: %s → %zu file(s) in %s\n",
                        effect_folder.filename().string().c_str(),
                        result.outputs.size(),
                        out_dir.string().c_str());
        }
        return 0;
    } else {
        std::fprintf(stderr, "pack: FAILED (%zu error(s))\n", result.errors.size());
        return 1;
    }
}
