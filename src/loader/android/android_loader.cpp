/**
 * src/loader/android/android_loader.cpp — Android injected .so loader stub.
 *
 * Phase 7: VanGFX injected as a shared library into GLES/Vulkan Android games
 * where the team controls the APK (SHAR method).
 *
 * On Android the entry point is JNI_OnLoad (or a constructor attribute __init),
 * which fires when the .so is loaded by the game's ClassLoader or by a payload
 * injected via /proc/pid/mem.  It auto-builds a Context with GLES or Vulkan
 * depending on which API the game's process has already loaded.
 *
 * This file compiles on Windows so the VANGFX_ANDROID=ON configure path is
 * always available, but the resulting object is a stub — the real NDK .so is
 * built separately with the Android CMake toolchain:
 *
 *   cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
 *         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
 *         -DVANGFX_GLES=ON -DVANGFX_VULKAN=ON -DVANGFX_ANDROID=ON ..
 *
 * Runtime flow (Android, SHAR injection):
 *   1. Payload maps vangfx.so into the game process.
 *   2. __attribute__((constructor)) vangfx_android_init() runs.
 *   3. If libvulkan.so is already mapped → BackendKind::Vulkan.
 *      Else if libGLES*.so / libEGL.so → BackendKind::GLES.
 *   4. ContextBuilder::build() hooks the detected API.
 *   5. VanGFX is live; mods loaded via the ASI loader receive callbacks.
 */

#ifdef __ANDROID__
#  include <jni.h>
#  include <android/log.h>
#  define ANDROID_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "VanGFX", fmt, ##__VA_ARGS__)
#else
#  define ANDROID_LOG(fmt, ...) (void)0
#endif

#include <vangfx/vangfx.hpp>

namespace vangfx::loader::android {

static std::unique_ptr<vangfx::Context> g_ctx;

// Called by the injection payload or JNI_OnLoad.
// Safe to call more than once (second call is a no-op).
void vangfx_android_attach() {
    if (g_ctx) return;

    auto result = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::Auto)
        .log_level(vangfx::LogLevel::Info)
        .build();

    if (!result) {
        ANDROID_LOG("VanGFX attach failed: %s", result.error().message.c_str());
        return;
    }

    g_ctx = std::move(*result);
    ANDROID_LOG("VanGFX attached (backend %d)", (int)g_ctx->backend());
}

void vangfx_android_detach() {
    g_ctx.reset();
}

vangfx::Context* vangfx_android_context() noexcept {
    return g_ctx.get();
}

} // namespace vangfx::loader::android

// ── JNI entry point (Android only) ───────────────────────────────────────────

#ifdef __ANDROID__
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    vangfx::loader::android::vangfx_android_attach();
    return JNI_VERSION_1_6;
}
#endif

// ── Constructor attribute (non-JNI injection path) ───────────────────────────

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
static void vangfx_android_ctor() {
    vangfx::loader::android::vangfx_android_attach();
}
#endif
