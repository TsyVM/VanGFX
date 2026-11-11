// A minimal .asi (really a DLL) used by the ASI loader test.
// DllMain sets a flag that the test can read back via GetProcAddress.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static bool g_loaded = false;

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        g_loaded = true;
    return TRUE;
}

extern "C" __declspec(dllexport) bool vgx_asi_was_loaded() {
    return g_loaded;
}
