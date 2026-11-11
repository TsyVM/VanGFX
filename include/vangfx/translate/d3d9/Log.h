// The renderer's diagnostic log -- MWOn12-render.log, beside the executable.
//
// Separate from the loader log, which holds only the backend banner. This is
// the detailed stream: warnings and errors are always written, and the
// VerboseLog setting adds informational and trace detail.
//
// The macros capture file, line and function at the call site, which matters
// because most messages here are emitted from deep inside a translation path
// where the message text alone would not identify the caller.
//
// Two conventions are worth keeping to. Emit a warning wherever the renderer
// silently does something other than what was asked -- an approximated format,
// a dropped draw, an unimplemented state -- because the visible symptom of
// each of those is something missing from the screen rather than an error.
// And log unimplemented paths once rather than per frame, or the message that
// matters is buried under thousands of repeats.

#pragma once

#ifndef VANGFX_D3D9_LOG_H
#define VANGFX_D3D9_LOG_H

#include <windows.h>
#include <cstdint>

namespace vangfx::d3d9 {
namespace log {

enum class Level : int {
    Trace = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Fatal = 4,
};

void Init() noexcept;

void Shutdown() noexcept;

void SetVerbose(bool on) noexcept;
bool Verbose() noexcept;

const wchar_t* FilePath() noexcept;

void Write(Level lvl, const char* file, int line, const char* func,
           const char* fmt, ...) noexcept;

void WriteHR(Level lvl, const char* file, int line, const char* func,
             HRESULT hr, const char* fmt, ...) noexcept;

const char* DecodeHR(HRESULT hr) noexcept;

}
}

#define DXLOG_TRACE(...) ::vangfx::d3d9::log::Write(::vangfx::d3d9::log::Level::Trace, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define DXLOG_INFO(...)  ::vangfx::d3d9::log::Write(::vangfx::d3d9::log::Level::Info,  __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define DXLOG_WARN(...)  ::vangfx::d3d9::log::Write(::vangfx::d3d9::log::Level::Warn,  __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define DXLOG_ERROR(...) ::vangfx::d3d9::log::Write(::vangfx::d3d9::log::Level::Error, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define DXLOG_HR(hr, ...)       ::vangfx::d3d9::log::WriteHR(::vangfx::d3d9::log::Level::Error, __FILE__, __LINE__, __FUNCTION__, (hr), __VA_ARGS__)
#define DXLOG_WARN_HR(hr, ...)  ::vangfx::d3d9::log::WriteHR(::vangfx::d3d9::log::Level::Warn,  __FILE__, __LINE__, __FUNCTION__, (hr), __VA_ARGS__)
#define DXLOG_FATAL_HR(hr, ...) ::vangfx::d3d9::log::WriteHR(::vangfx::d3d9::log::Level::Fatal, __FILE__, __LINE__, __FUNCTION__, (hr), __VA_ARGS__)

#define DX_FAILED(hr, ...)                                                       \
    ( FAILED(hr)                                                                 \
      ? (::vangfx::d3d9::log::WriteHR(::vangfx::d3d9::log::Level::Error, __FILE__,         \
                                 __LINE__, __FUNCTION__, (hr), __VA_ARGS__), true)\
      : false )

#endif
