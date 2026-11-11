#pragma once

/**
 * vangfx/audio.hpp — VanGFX audio subsystem single-include.
 *
 * Pulls in XAudio2 / DirectSound interception, framework playback,
 * DSP effect injection, and audio capture.
 *
 * Usage:
 *   #include <vangfx/audio.hpp>
 *
 * Additional linker dependencies (add alongside vangfx.lib):
 *   xaudio2.lib   (XAudio2 backend — Windows 8+, in-box)
 *   dsound.lib    (DirectSound backend — legacy games)
 *
 * Built and maintained by TeamVanilla — https://www.teamvanilla.org/
 */

#include "audio/voice.hpp"
#include "audio/events.hpp"
#include "audio/playback.hpp"
#include "audio/capture.hpp"
#include "audio/effect.hpp"
#include "audio/context.hpp"
