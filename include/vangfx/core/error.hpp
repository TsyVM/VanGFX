#pragma once

/**
 * vangfx/core/error.hpp — VanGFX error types and Result<T> alias.
 *
 * Every VanGFX API call returns Result<T> = std::expected<T, VanGFXError>.
 * No HRESULTs, no exceptions — all errors are values.
 */

#include <expected>
#include <string>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  ShaderError
// ─────────────────────────────────────────────────────────────────────────────

enum class ShaderError {
    FileNotFound,
    CompileFailed,    // message holds full compiler diagnostic output
    LinkFailed,
    Unsupported,      // stage or feature not available on this backend
    DeviceLost,
    InvalidFormat,    // format not supported on this backend
    InvalidHandle,    // handle is null or expired
    ResourceLost,     // underlying D3D resource was released by the game
    OutOfMemory,
    CaptureNotActive,
    ReadbackFailed,
    NotInitialized,   // Context::build() was not called
};

// ─────────────────────────────────────────────────────────────────────────────
//  VanGFXError  — error value carried by Result<T>
// ─────────────────────────────────────────────────────────────────────────────

struct VanGFXError {
    ShaderError code    = ShaderError::NotInitialized;
    std::string message;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Result<T>  — std::expected alias used throughout the VanGFX API
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
using Result = std::expected<T, VanGFXError>;

// ─────────────────────────────────────────────────────────────────────────────
//  Helper factories
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline VanGFXError make_error(ShaderError code,
                                             std::string msg = {}) noexcept {
    return { code, std::move(msg) };
}

template<typename T>
[[nodiscard]] inline std::unexpected<VanGFXError> gfx_err(ShaderError code,
                                                            std::string msg = {}) {
    return std::unexpected(VanGFXError{ code, std::move(msg) });
}

} // namespace vangfx
