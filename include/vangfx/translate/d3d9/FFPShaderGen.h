// Generates HLSL for draws that use no shader.
//
// A D3D9 game may draw with no shader bound at all, leaving transform,
// lighting and texture blending to the driver's fixed-function pipeline.
// Neither D3D11 nor D3D12 has one, so the equivalent shaders are synthesised
// from the render state in force.
//
// FFPPermKey is the compact description of everything that changes the
// generated code -- how many lights and of what type, the texture stage
// operations and their arguments, the fog mode, what the vertex format
// supplies. Two draws with the same key can share a shader pair, so the key
// doubles as the cache key.
//
// It is laid out as bit fields packed into two 64-bit words followed by plain
// arrays, with no padding, so that hashing and comparing it are cheap. Adding
// a field means checking that assumption still holds.

#pragma once

#ifndef VANGFX_D3D9_FFP_SHADER_GEN_H
#define VANGFX_D3D9_FFP_SHADER_GEN_H

#include <d3d9.h>
#include <cstdint>
#include <string>

#include <vangfx/translate/d3d9/RenderStateTracker.h>

namespace vangfx::d3d9 {

#pragma pack(push, 16)

struct FFPLightData {
    int      Type;
    float    _pad0[3];
    float    Diffuse[4];
    float    Specular[4];
    float    Ambient[4];
    float    Position[4];
    float    Direction[4];
    float    Range;
    float    Falloff;
    float    Attenuation0;
    float    Attenuation1;
    float    Attenuation2;
    float    Theta;
    float    Phi;
    float    _pad1;
};
static_assert(sizeof(FFPLightData) == 128, "FFPLightData must be 128 bytes");

struct FFPConstantData {
    float    WorldMatrix[4][16];

    float    ViewMatrix[16];
    float    ProjectionMatrix[16];
    float    TextureMatrix[8][16];

    float    MaterialDiffuse[4];
    float    MaterialAmbient[4];
    float    MaterialSpecular[4];
    float    MaterialEmissive[4];
    float    MaterialPower;
    float    CameraWorldPos[3];

    float    GlobalAmbient[4];
    float    FogColor[4];
    float    FogStart;
    float    FogEnd;
    float    FogDensity;
    float    AlphaRef;

    float    TextureFactor[4];

    float    ViewportInfo[4];

    FFPLightData Lights[8];
    int      ActiveLightCount;
    int      _padLC[3];

    // Inverse transpose of each world matrix's upper 3x3: what D3D9 moves
    // normals by. A scaled world scales normals by the reciprocal, which with
    // NORMALIZENORMALS off is visible as brighter or darker lighting.
    float    WorldNormalMatrix[4][16];
};

#pragma pack(pop)

struct FFPStageDesc {
    uint8_t colorOp   = 1;
    uint8_t colorArg1 = 2;
    uint8_t colorArg2 = 1;
    uint8_t colorArg0 = 1;
    uint8_t alphaOp   = 1;
    uint8_t alphaArg1 = 2;
    uint8_t alphaArg2 = 1;
    uint8_t alphaArg0 = 1;

    bool operator==(const FFPStageDesc& o) const noexcept
    {
        return colorOp == o.colorOp && colorArg1 == o.colorArg1 &&
               colorArg2 == o.colorArg2 && colorArg0 == o.colorArg0 &&
               alphaOp == o.alphaOp && alphaArg1 == o.alphaArg1 &&
               alphaArg2 == o.alphaArg2 && alphaArg0 == o.alphaArg0;
    }
};

struct FFPPermKey {

    uint64_t lightingEnabled   : 1;
    uint64_t lightCount        : 4;
    uint64_t lightTypes        : 16;
    uint64_t specularEnabled   : 1;
    uint64_t colorVertex       : 1;
    uint64_t vertexBlendMode   : 2;
    uint64_t fogVertexMode     : 2;
    uint64_t texCoordCount     : 4;
    uint64_t texTransformFlags : 8;
    uint64_t hasNormal         : 1;
    uint64_t hasColor0         : 1;
    uint64_t hasColor1         : 1;
    uint64_t positionType      : 1;

    uint64_t mixedFullTexOutputs : 1;
    uint64_t rangeFog          : 1;

    // Where each lit colour comes from: 0 the material, 1 vertex COLOR0,
    // 2 vertex COLOR1. Already resolved against COLORVERTEX and against what
    // the vertex actually carries, as D3D9 does.
    uint64_t diffuseSource     : 2;
    uint64_t ambientSource     : 2;
    uint64_t specularSource    : 2;
    uint64_t emissiveSource    : 2;
    uint64_t normalizeNormals  : 1;
    uint64_t _vsReserved       : 10;

    uint64_t activeStageCount  : 4;
    uint64_t alphaTestEnabled  : 1;
    uint64_t alphaTestFunc     : 3;
    uint64_t fogPixelMode      : 2;
    uint64_t projTexMask       : 8;

    uint64_t mixedPsLink       : 1;
    uint64_t linkTexMask       : 8;
    uint64_t linkColor0        : 1;
    uint64_t linkColor1        : 1;
    uint64_t linkFog           : 1;

    uint64_t texStageKind      : 16;
    // Stages whose texture slot is empty. D3D9 reads an empty stage as opaque
    // white; a D3D12 null descriptor reads transparent black, which made every
    // untextured fixed-function fill (UI panels, fades, flat-shaded quads) vanish.
    uint64_t texMissingMask    : 8;
    uint64_t _psReserved       : 10;

    FFPStageDesc stages[8];

    uint8_t texGen[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

    // Components each input texture coordinate set carries (1-4, 0 = absent).
    // A texture transform sees a 2-D coordinate as (u, v, 1, 0), which is why
    // D3D9 games put a scroll offset in _31/_32 rather than _41/_42.
    uint8_t texInDim[8] = {};
    // D3DTSS_TEXTURETRANSFORMFLAGS per stage: count in bits 0-2, PROJECTED as 8.
    uint8_t texXform[8] = {};
    // Bit 0: the stage writes D3DTA_TEMP instead of CURRENT (D3DTSS_RESULTARG).
    uint8_t stageFlags[8] = {};

    bool operator==(const FFPPermKey& o) const noexcept {
        if (reinterpret_cast<const uint64_t*>(this)[0] != reinterpret_cast<const uint64_t*>(&o)[0] ||
            reinterpret_cast<const uint64_t*>(this)[1] != reinterpret_cast<const uint64_t*>(&o)[1])
            return false;
        for (int s = 0; s < 8; ++s)
            if (!(stages[s] == o.stages[s])) return false;
        for (int s = 0; s < 8; ++s)
            if (texGen[s] != o.texGen[s] || texInDim[s] != o.texInDim[s] ||
                texXform[s] != o.texXform[s] || stageFlags[s] != o.stageFlags[s])
                return false;
        return true;
    }
};
static_assert(sizeof(FFPPermKey) == 16 + 8 * sizeof(FFPStageDesc) + 32,
              "FFPPermKey layout changed unexpectedly");

struct FFPPermKeyHash {
    size_t operator()(const FFPPermKey& k) const noexcept {
        const uint64_t* w = reinterpret_cast<const uint64_t*>(&k);
        uint64_t h = w[0] ^ (w[1] * 0x9e3779b97f4a7c15ULL);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(k.stages);
        for (size_t i = 0; i < sizeof(k.stages); ++i) {
            h ^= bytes[i];
            h *= 0x100000001b3ULL;
        }
        for (size_t i = 0; i < 8; ++i) {
            h ^= k.texGen[i] | (uint64_t(k.texInDim[i]) << 8) |
                 (uint64_t(k.texXform[i]) << 16) | (uint64_t(k.stageFlags[i]) << 24);
            h *= 0x100000001b3ULL;
        }
        h ^= (h >> 30); h *= 0xbf58476d1ce4e5b9ULL;
        h ^= (h >> 27); h *= 0x94d049bb133111ebULL;
        return static_cast<size_t>(h ^ (h >> 31));
    }
};

struct FFPFixedState {

    D3DMATRIX    world[8]{};
    D3DMATRIX    view{};
    D3DMATRIX    proj{};
    D3DMATRIX    texMatrix[8]{};
    D3DMATERIAL9 material{};
    D3DLIGHT9    lights[8]{};
    bool         lightEnabled[8]{};

    void Reset() noexcept;
};

UINT ExpandFVF(DWORD fvf, D3DVERTEXELEMENT9* pOut) noexcept;

FFPPermKey BuildFFPKey(const D9RenderState& st,
                       const FFPFixedState& fx,
                       DWORD fvf,
                       const D3DVERTEXELEMENT9* pDecl) noexcept;

std::string GenerateFFPVS(const FFPPermKey& key) noexcept;

std::string GenerateFFPPS(const FFPPermKey& key) noexcept;

void FillFFPConstants(FFPConstantData& cb,
                      const D9RenderState& st,
                      const FFPFixedState& fx) noexcept;

}

#endif
