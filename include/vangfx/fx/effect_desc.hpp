#pragma once

/**
 * vangfx/fx/effect_desc.hpp — in-memory description of an effect loaded from
 * an effect folder on disk.
 *
 * File layout on disk:
 *
 *   effects\MyEffect\
 *       effect.ini           — describes the effect and its passes
 *       main.hlsl            — pixel shader for the "main" pass
 *       user.ini             — per-user parameter overrides (written at runtime)
 *
 * effect.ini format:
 *
 *   [Effect]
 *   Name=My Effect
 *   Description=Does something pretty
 *   Order=100
 *
 *   [Pass main]
 *   PS=main.hlsl
 *   VS=                       ; leave blank for the built-in fullscreen VS
 *   Inputs=backbuffer         ; comma-sep: backbuffer, depth, previous, <file.dds>
 *   Width=0                   ; 0 = same as backbuffer
 *   Height=0                  ; 0 = same as backbuffer
 */

#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>

namespace vangfx::fx {

struct PassDesc {
    std::string              name;
    std::string              ps_file;      // path relative to effect folder
    std::string              vs_file;      // empty = built-in fullscreen VS
    std::vector<std::string> inputs;       // "backbuffer", "depth", "previous", filename
    uint32_t                 width  = 0;   // 0 = backbuffer size
    uint32_t                 height = 0;
};

struct EffectDesc {
    std::string              name;
    std::string              description;
    int32_t                  order = 0;    // lower = runs first
    std::filesystem::path    folder;
    std::vector<PassDesc>    passes;

    // Parse effect.ini from an effect folder.  Returns false if the file
    // cannot be read or has no [Effect] section.
    bool load(const std::filesystem::path& effect_folder);

    // Write current parameter values to user.ini in the effect folder.
    bool save_user_params() const;

    // Per-effect parameters exposed to the overlay and to vangfx_float4[].
    // Index into vangfx_float4[0..7].x using the declared names.
    struct Param {
        std::string name;
        uint32_t    index = 0;    // vangfx_float4[index].x (0-7)
        float       value = 0.0f;
        float       min   = 0.0f;
        float       max   = 1.0f;
    };
    std::vector<Param> params;

    // The 8 × float4 values for the cbuffer (filled from params).
    float float4[8][4] = {};
};

} // namespace vangfx::fx
