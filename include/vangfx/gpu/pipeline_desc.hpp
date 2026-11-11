#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace vangfx::gpu {

// One compiled or source HLSL stage.
// If bytecode is non-empty it is used as-is; otherwise hlsl is compiled at
// pipeline-creation time with the entry point named by entry.
struct ShaderSource {
    std::string           hlsl;            // HLSL source text
    std::string           entry  = "main"; // entry-point name
    std::vector<uint8_t>  bytecode;        // pre-compiled DXBC (empty = compile)
};

// Shader sources for a pipeline.  Set cs for compute; leave cs empty and fill
// vs+ps for a graphics pipeline.
struct ShaderPack {
    ShaderSource vs; // vertex shader (graphics)
    ShaderSource ps; // pixel shader  (graphics)
    ShaderSource cs; // compute shader (compute only)
};

// Description of a pipeline object created by Device::create_pipeline().
struct PipelineDesc {
    ShaderPack shaders;

    // Blend: src_alpha / (1-src_alpha) premultiplied blend on RT 0.
    bool alpha_blend = false;

    // Depth test.  Always false for fullscreen passes.
    bool depth_test  = false;
    bool depth_write = false;

    // Back-face culling.  Always false for fullscreen passes.
    bool cull_back   = false;
};

} // namespace vangfx::gpu
