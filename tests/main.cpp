/**
 * tests/main.cpp — the runner.
 *
 *   vangfx_tests                 run every test, each in its own process
 *   vangfx_tests d3d12           run the tests whose name contains "d3d12"
 *   vangfx_tests --list          list them
 *   vangfx_tests --run NAME      run one test in this process (what the runner does)
 *
 * Exit code: the number of tests that failed.
 */

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdlib>
#include <cstring>

namespace vgx {

std::vector<Test>& tests() {
    static std::vector<Test> all;
    return all;
}

int add(const char* name, void (*fn)()) {
    tests().push_back({ name, fn });
    return 0;
}

void fail(const char* file, int line, const std::string& what) {
    const char* base = std::strrchr(file, '\\');
    std::printf("    FAILED at %s:%d\n      %s\n", base ? base + 1 : file, line, what.c_str());
    std::fflush(stdout);
    ExitProcess(1);
}

void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("    ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}

bool use_hardware() {
    char v[8]{};
    return GetEnvironmentVariableA("VGX_HW", v, sizeof(v)) && v[0] == '1';
}

} // namespace vgx

namespace {

LONG WINAPI crashed(EXCEPTION_POINTERS* e) {
    std::printf("    CRASHED: exception 0x%08lX at %p%s\n", e->ExceptionRecord->ExceptionCode,
                e->ExceptionRecord->ExceptionAddress,
                e->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW ? " (stack overflow)" : "");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

int run_one(const char* name) {
    SetUnhandledExceptionFilter(&crashed);
    AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS* e) -> LONG {
        // A stack overflow has no stack left for the unhandled-exception filter.
        if (e->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
            static const char msg[] = "    CRASHED: stack overflow\n";
            DWORD n = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), msg, sizeof(msg) - 1, &n, nullptr);
            TerminateProcess(GetCurrentProcess(), 4);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });
    for (const auto& t : vgx::tests()) {
        if (std::strcmp(t.name, name) == 0) {
            t.fn();
            std::fflush(stdout);
            return 0;
        }
    }
    std::printf("no test named %s\n", name);
    return 2;
}

// Runs one test in a child process. Returns its exit code (STILL_ACTIVE on timeout).
DWORD spawn(const char* name, DWORD timeout_ms, DWORD& elapsed) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string cmd = std::string("\"") + exe + "\" --run " + name;
    STARTUPINFOA si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    const DWORD start = GetTickCount();
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return 9;
    DWORD code = STILL_ACTIVE;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 5);
        WaitForSingleObject(pi.hProcess, 5000);
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    elapsed = GetTickCount() - start;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 3 && std::strcmp(argv[1], "--run") == 0) return run_one(argv[2]);
    if (argc >= 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& t : vgx::tests()) std::printf("%s\n", t.name);
        return 0;
    }
    const char* filter = argc >= 2 ? argv[1] : nullptr;

    std::printf("VanGFX tests on %s\n", vgx::use_hardware() ? "the hardware adapter (VGX_HW=1)" : "WARP");
    int ran = 0, failed = 0;
    std::vector<std::string> failures;
    for (const auto& t : vgx::tests()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++ran;
        std::printf("[ RUN  ] %s\n", t.name);
        DWORD ms = 0;
        const DWORD code = spawn(t.name, 120000, ms);
        if (code == 0) {
            std::printf("[ PASS ] %s (%lu ms)\n", t.name, ms);
        } else {
            ++failed;
            failures.emplace_back(t.name);
            std::printf("[ FAIL ] %s (%s %lu)\n", t.name, code == STILL_ACTIVE || code == 5 ? "timeout," : "exit", code);
        }
    }
    std::printf("\n%d of %d passed\n", ran - failed, ran);
    for (const auto& f : failures) std::printf("  failed: %s\n", f.c_str());
    return failed;
}
