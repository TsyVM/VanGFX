#pragma once
// ASI loading.
//
// An `.asi` is a DLL with a different extension and no required exports: it
// does its work from DllMain. The convention predates this project by twenty
// years; most mods people already have are written to it.
//
// LoadAll is called after the first device is created (never from DllMain),
// because LoadLibrary under the loader lock can deadlock.

#include <string>
#include <string_view>

namespace vangfx::loader::asi {

// Loads every .asi found in `dir`, once per process (std::call_once guards
// repeated calls). Pass an absolute path; a trailing backslash is optional.
void LoadAll(std::wstring_view dir);

// How many modules are loaded. 0 until LoadAll is called.
unsigned LoadedCount() noexcept;

} // namespace vangfx::loader::asi
