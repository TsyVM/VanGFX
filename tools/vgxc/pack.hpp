#pragma once

/**
 * tools/vgxc/pack.hpp — effect folder → .pack/ output directory
 *
 * pack_effect() reads effect.ini from the given folder, compiles every pass
 * shader to the requested targets, writes the outputs to <out_dir>/, and
 * generates reflect.json alongside them.
 */

#include <filesystem>
#include <string>
#include <vector>

namespace vgxc {

struct PackOptions {
    bool dxbc      = true;
    bool dxil      = true;
    bool spirv     = true;
    bool glsl      = false;  // requires VANGFX_SPIRV_CROSS
    bool essl      = false;
    bool msl       = false;
    bool check_only = false;  // validate without writing files
    bool verbose   = false;
};

struct PackResult {
    bool                    ok = true;
    std::vector<std::string> errors;
    std::vector<std::string> outputs;  // files written (relative to out_dir)
};

PackResult pack_effect(const std::filesystem::path& effect_folder,
                        const std::filesystem::path& out_dir,
                        const PackOptions& opts);

// Parse --targets=<csv> into a PackOptions, returning false on unknown target.
bool parse_targets(const std::string& csv, PackOptions& opts, std::string& error);

}  // namespace vgxc
