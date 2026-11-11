#pragma once

/**
 * src/gpu/gl/device_gl.hpp — OpenGL gpu::Device / gpu::Pass implementation.
 *
 * Implements the gpu::Device interface using core OpenGL 3.3.  Extension
 * functions are loaded lazily via wglGetProcAddress.
 *
 * PipelineDesc::shaders.vs.hlsl / .ps.hlsl are treated as GLSL 330 core
 * source (not HLSL) since no HLSL→GLSL translator is available without
 * SPIRV-Cross (not yet vendored).  SPIRV-Cross support will be wired in when
 * external/spirv-cross is populated (Phase 5+).
 *
 * Factory:  vangfx::gpu::make_gl_device(HGLRC)
 */

#include <vangfx/gpu/device.hpp>
#include <vangfx/gpu/pass.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>

// GL 1.5+ sized integer types absent from the Windows SDK's GL 1.1 gl.h.
#ifndef GL_VERSION_1_5
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#endif

#include <memory>
#include <vector>

namespace vangfx::gpu {

// ── GL extension function types ───────────────────────────────────────────────
// (All loaded lazily at first device creation; NULL if unavailable.)

struct GLProcs {
    // Buffer objects (GL 1.5)
    void (WINAPI* GenBuffers)(GLsizei, GLuint*)           = nullptr;
    void (WINAPI* BindBuffer)(GLenum, GLuint)              = nullptr;
    void (WINAPI* BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void (WINAPI* BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
    void (WINAPI* DeleteBuffers)(GLsizei, const GLuint*)   = nullptr;
    void (WINAPI* BindBufferBase)(GLenum, GLuint, GLuint)  = nullptr;

    // VAO (GL 3.0)
    void (WINAPI* GenVertexArrays)(GLsizei, GLuint*)       = nullptr;
    void (WINAPI* BindVertexArray)(GLuint)                 = nullptr;
    void (WINAPI* DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;

    // FBO (GL 3.0)
    void (WINAPI* GenFramebuffers)(GLsizei, GLuint*)       = nullptr;
    void (WINAPI* BindFramebuffer)(GLenum, GLuint)         = nullptr;
    void (WINAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    void (WINAPI* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;

    // Shaders (GL 2.0)
    GLuint (WINAPI* CreateShader)(GLenum)                  = nullptr;
    void   (WINAPI* ShaderSource)(GLuint, GLsizei, const char**, const GLint*) = nullptr;
    void   (WINAPI* CompileShader)(GLuint)                 = nullptr;
    void   (WINAPI* DeleteShader)(GLuint)                  = nullptr;
    void   (WINAPI* GetShaderiv)(GLuint, GLenum, GLint*)   = nullptr;
    void   (WINAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*) = nullptr;

    // Programs (GL 2.0)
    GLuint (WINAPI* CreateProgram)()                       = nullptr;
    void   (WINAPI* AttachShader)(GLuint, GLuint)          = nullptr;
    void   (WINAPI* LinkProgram)(GLuint)                   = nullptr;
    void   (WINAPI* UseProgram)(GLuint)                    = nullptr;
    void   (WINAPI* DeleteProgram)(GLuint)                 = nullptr;
    void   (WINAPI* GetProgramiv)(GLuint, GLenum, GLint*)  = nullptr;
    void   (WINAPI* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*) = nullptr;
    void   (WINAPI* Uniform1i)(GLint, GLint)               = nullptr;
    GLint  (WINAPI* GetUniformLocation)(GLuint, const char*) = nullptr;
    GLuint (WINAPI* GetUniformBlockIndex)(GLuint, const char*) = nullptr;
    void   (WINAPI* UniformBlockBinding)(GLuint, GLuint, GLuint) = nullptr;

    // Textures (GL 1.0 — but needed here for the texture unit functions)
    void (WINAPI* ActiveTexture)(GLenum)                   = nullptr; // GL 1.3

    // DrawArrays / UseProgram are GL 1.1 / 2.0 (opengl32.dll exports them).
    void (WINAPI* DrawArrays)(GLenum, GLint, GLsizei)      = nullptr; // GL 1.1
    void (WINAPI* UseProgram2)(GLuint)                     = nullptr; // alias

    bool loaded = false;

    static GLProcs& get() noexcept;
    void load() noexcept;
};

// ── GL resource data ──────────────────────────────────────────────────────────

struct GLTextureData final : TextureData {
    GLuint  tex_id    = 0;
    bool    owns      = true;
    void* native() const noexcept override { return reinterpret_cast<void*>((uintptr_t)tex_id); }
};

struct GLBufferData final : BufferData {
    GLuint  buf_id    = 0;
    GLsizei size_bytes = 0;
    void* native() const noexcept override { return reinterpret_cast<void*>((uintptr_t)buf_id); }
};

struct GLPipelineData final : PipelineData {
    GLuint  program   = 0;
    bool    alpha_blend = false;
    bool    depth_test  = false;
    void* native() const noexcept { return reinterpret_cast<void*>((uintptr_t)program); }
};

// ── GL Pass ───────────────────────────────────────────────────────────────────

class GLPass final : public Pass {
public:
    GLPass(GLProcs& p, GLuint vao_fullscreen, GLuint params_ubo);
    ~GLPass() override;

    void set_render_target(Texture& rt)                      override;
    void set_viewport(uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h)                override;
    void set_pipeline(Pipeline& p)                           override;
    void bind(uint32_t slot, Texture& t)                     override;
    void update_params(const VanGFXParams& params)           override;
    void draw_fullscreen()                                    override;
    void draw(uint32_t vertices, uint32_t instances)         override;
    void dispatch(uint32_t x, uint32_t y, uint32_t z)        override;
    void end()                                                override;

private:
    void save_state()    noexcept;
    void restore_state() noexcept;

    GLProcs&   procs_;
    GLuint     vao_fullscreen_;
    GLuint     params_ubo_;
    bool       ended_    = false;

    // Saved state
    GLint  saved_program_        = 0;
    GLint  saved_vao_            = 0;
    GLint  saved_fbo_            = 0;
    GLint  saved_viewport_[4]    = {};
    GLint  saved_active_tex_     = 0;
    GLboolean saved_depth_test_  = GL_FALSE;
    GLboolean saved_blend_       = GL_FALSE;
    GLint  saved_tex_[8]         = {};

    // Current custom FBO (0 = default framebuffer)
    GLuint custom_fbo_ = 0;
};

// ── GL Device ─────────────────────────────────────────────────────────────────

class GLDevice final : public Device {
public:
    explicit GLDevice(HGLRC hglrc);
    ~GLDevice() override;

    Api api() const override { return Api::GL; }

    Result<Texture>  create_texture(const vangfx::TextureDesc& desc,
                                     const InitData* init) override;
    Result<Buffer>   create_buffer(uint32_t size, const void* init) override;
    Result<Pipeline> create_pipeline(const PipelineDesc& desc)      override;
    void update_buffer(Buffer& buf, const void* data, uint32_t size) override;
    std::unique_ptr<Pass> begin_pass() override;
    void* native() const override { return hglrc_; }
    Texture wrap_texture(void* native_tex, void* /*srv*/, void* /*rtv*/,
                         uint32_t w, uint32_t h,
                         vangfx::SurfaceFormat fmt) override;

private:
    HGLRC  hglrc_         = nullptr;
    GLuint vao_fullscreen_ = 0;
    GLuint params_ubo_    = 0;
    GLProcs* procs_       = nullptr;

    void ensure_init();
    bool init_done_ = false;
};

// Factory function
std::unique_ptr<Device> make_gl_device(HGLRC hglrc);

} // namespace vangfx::gpu
