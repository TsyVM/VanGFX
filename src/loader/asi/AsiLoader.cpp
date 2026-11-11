// ASI loading. See AsiLoader.h.
//
// The whole of it is: find the files, LoadLibrary each one, keep the handle.
// There is no ABI to check and no entry point to call, because an ASI has
// neither — its DllMain has already run by the time LoadLibrary returns.
//
// Two consequences worth being explicit about:
//   Nothing is ever unloaded. An ASI installs detours and leaves them there.
//   Nothing is validated. A DLL that is not really an ASI simply runs DllMain.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vangfx/loader/AsiLoader.h>

#include <mutex>
#include <vector>

namespace vangfx::loader::asi {
namespace {

std::vector<HMODULE> g_loaded;
std::once_flag       g_once;

} // namespace

void LoadAll(std::wstring_view dir)
{
    std::call_once(g_once, [dir_str = std::wstring(dir)]() {
        std::wstring pattern = dir_str;
        if (!pattern.empty() && pattern.back() != L'\\')
            pattern += L'\\';
        pattern += L"*.asi";

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;

        std::wstring base = pattern.substr(0, pattern.size() - 5); // strip *.asi
        do {
            std::wstring path = base + fd.cFileName;
            HMODULE mod = LoadLibraryW(path.c_str());
            if (mod) {
                g_loaded.push_back(mod);
                OutputDebugStringA("[VanGFX] ASI loaded: ");
                // narrow just for debug output
                char narrow[MAX_PATH]{};
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, narrow, MAX_PATH, nullptr, nullptr);
                OutputDebugStringA(narrow);
                OutputDebugStringA("\n");
            } else {
                OutputDebugStringA("[VanGFX] ASI load FAILED: ");
                char narrow[MAX_PATH]{};
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, narrow, MAX_PATH, nullptr, nullptr);
                OutputDebugStringA(narrow);
                OutputDebugStringA("\n");
            }
        } while (FindNextFileW(h, &fd));

        FindClose(h);
    });
}

unsigned LoadedCount() noexcept
{
    return static_cast<unsigned>(g_loaded.size());
}

} // namespace vangfx::loader::asi
