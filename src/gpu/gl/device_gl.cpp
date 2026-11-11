/**
 * src/gpu/gl/device_gl.cpp — GLDevice + GLPass implementation.
 *
 * Uses core OpenGL 3.3.  Extension function pointers are loaded via
 * wglGetProcAddress on first GLDevice construction.
 *
 * PipelineDesc::shaders.vs.hlsl / .ps.hlsl are treated as GLSL 330 core
 * source text (not HLSL).  bytecode is ignored for GL.
 */

#include "device_gl.hpp"
#include <vangfx/core/error.hpp>

#include <cstring>
#include <string>
#include <vector>

// GL enum constants not guaranteed in the Windows SDK's gl.h (GL 1.1 only).
#ifndef GL_ARRAY_BUFFER
#  define GL_ARRAY_BUFFER               0x8892
#  define GL_UNIFORM_BUFFER             0x8A11
#  define GL_DYNAMIC_DRAW               0x88E8
#  define GL_STATIC_DRAW                0x88B4
#  define GL_FRAGMENT_SHADER            0x8B30
#  define GL_VERTEX_SHADER              0x8B31
#  define GL_COMPILE_STATUS             0x8B81
#  define GL_LINK_STATUS                0x8B82
#  define GL_INFO_LOG_LENGTH            0x8B84
#  define GL_FRAMEBUFFER                0x8D40
#  define GL_COLOR_ATTACHMENT0          0x8CE0
#  define GL_TEXTURE_2D                 0x0DE1
#  define GL_RGBA                       0x1908
#  define GL_RGBA8                      0x8058
#  define GL_RGBA16F                    0x881A
#  define GL_RGBA32F                    0x8814
#  define GL_R8                         0x8229
#  define GL_R16F                       0x822D
#  define GL_R32F                       0x822E
#  define GL_RG8                        0x822B
#  define GL_RG16F                      0x822F
#  define GL_RG32F                      0x8230
#  define GL_RGB16F                     0x881B
#  define GL_RGB32F                     0x8815
#  define GL_DEPTH_COMPONENT24          0x81A6
#  define GL_DEPTH_COMPONENT32F         0x8CAC
#  define GL_DEPTH24_STENCIL8           0x88F0
#  define GL_TEXTURE_MIN_FILTER         0x2801
#  define GL_TEXTURE_MAG_FILTER         0x2800
#  define GL_LINEAR                     0x2601
#  define GL_CLAMP_TO_EDGE              0x812F
#  define GL_TEXTURE_WRAP_S             0x2802
#  define GL_TEXTURE_WRAP_T             0x2803
#  define GL_UNSIGNED_BYTE              0x1401
#  define GL_FLOAT                      0x1406
#  define GL_HALF_FLOAT                 0x140B
#  define GL_TRIANGLES                  0x0004
#  define GL_BLEND                      0x0BE2
#  define GL_DEPTH_TEST                 0x0B71
#  define GL_SRC_ALPHA                  0x0302
#  define GL_ONE_MINUS_SRC_ALPHA        0x0303
#  define GL_ONE                        1
#  define GL_ZERO                       0
#  define GL_VERTEX_ARRAY_BINDING       0x85B5
#  define GL_DRAW_FRAMEBUFFER_BINDING   0x8CA6
#  define GL_CURRENT_PROGRAM            0x8B8D
#  define GL_ACTIVE_TEXTURE             0x84E0
#  define GL_TEXTURE0                   0x84C0
#  define GL_TEXTURE_BINDING_2D         0x8069
#endif

namespace vangfx::gpu {

// ── GL proc loader ────────────────────────────────────────────────────────────

static void* load_proc(const char* name) noexcept {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (!p) {
        // Fallback for GL 1.1 functions that live in opengl32.dll directly.
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) p = reinterpret_cast<void*>(GetProcAddress(gl, name));
    }
    return p;
}

#define LOAD(fn, type, name) \
    fn = reinterpret_cast<type>(load_proc(name))

GLProcs& GLProcs::get() noexcept {
    static GLProcs s;
    if (!s.loaded) s.load();
    return s;
}

void GLProcs::load() noexcept {
    // Buffer objects
    LOAD(GenBuffers,    decltype(GenBuffers),    "glGenBuffers");
    LOAD(BindBuffer,    decltype(BindBuffer),    "glBindBuffer");
    LOAD(BufferData,    decltype(BufferData),    "glBufferData");
    LOAD(BufferSubData, decltype(BufferSubData), "glBufferSubData");
    LOAD(DeleteBuffers, decltype(DeleteBuffers), "glDeleteBuffers");
    LOAD(BindBufferBase,decltype(BindBufferBase),"glBindBufferBase");

    // VAO
    LOAD(GenVertexArrays,    decltype(GenVertexArrays),    "glGenVertexArrays");
    LOAD(BindVertexArray,    decltype(BindVertexArray),    "glBindVertexArray");
    LOAD(DeleteVertexArrays, decltype(DeleteVertexArrays), "glDeleteVertexArrays");

    // FBO
    LOAD(GenFramebuffers,       decltype(GenFramebuffers),       "glGenFramebuffers");
    LOAD(BindFramebuffer,       decltype(BindFramebuffer),       "glBindFramebuffer");
    LOAD(FramebufferTexture2D,  decltype(FramebufferTexture2D),  "glFramebufferTexture2D");
    LOAD(DeleteFramebuffers,    decltype(DeleteFramebuffers),    "glDeleteFramebuffers");

    // Shaders
    LOAD(CreateShader,    decltype(CreateShader),    "glCreateShader");
    LOAD(ShaderSource,    decltype(ShaderSource),    "glShaderSource");
    LOAD(CompileShader,   decltype(CompileShader),   "glCompileShader");
    LOAD(DeleteShader,    decltype(DeleteShader),    "glDeleteShader");
    LOAD(GetShaderiv,     decltype(GetShaderiv),     "glGetShaderiv");
    LOAD(GetShaderInfoLog,decltype(GetShaderInfoLog),"glGetShaderInfoLog");

    // Programs
    LOAD(CreateProgram,     decltype(CreateProgram),     "glCreateProgram");
    LOAD(AttachShader,      decltype(AttachShader),      "glAttachShader");
    LOAD(LinkProgram,       decltype(LinkProgram),       "glLinkProgram");
    LOAD(UseProgram,        decltype(UseProgram),        "glUseProgram");
    LOAD(DeleteProgram,     decltype(DeleteProgram),     "glDeleteProgram");
    LOAD(GetProgramiv,      decltype(GetProgramiv),      "glGetProgramiv");
    LOAD(GetProgramInfoLog, decltype(GetProgramInfoLog), "glGetProgramInfoLog");
    LOAD(Uniform1i,         decltype(Uniform1i),         "glUniform1i");
    LOAD(GetUniformLocation,    decltype(GetUniformLocation),    "glGetUniformLocation");
    LOAD(GetUniformBlockIndex,  decltype(GetUniformBlockIndex),  "glGetUniformBlockIndex");
    LOAD(UniformBlockBinding,   decltype(UniformBlockBinding),   "glUniformBlockBinding");

    // Misc
    LOAD(ActiveTexture, decltype(ActiveTexture), "glActiveTexture");
    LOAD(DrawArrays,    decltype(DrawArrays),    "glDrawArrays");

    loaded = true;
}

#undef LOAD

// ── Format helpers ────────────────────────────────────────────────────────────

struct GLFormat { GLenum internal_fmt, base_fmt, type; };

static GLFormat to_gl_format(vangfx::SurfaceFormat f) noexcept {
    switch (f) {
    case vangfx::SurfaceFormat::RGBA8:    return { GL_RGBA8,  GL_RGBA, GL_UNSIGNED_BYTE };
    case vangfx::SurfaceFormat::BGRA8:    return { GL_RGBA8,  GL_RGBA, GL_UNSIGNED_BYTE }; // swizzle not set
    case vangfx::SurfaceFormat::RGBA16F:  return { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT };
    case vangfx::SurfaceFormat::RGBA32F:  return { GL_RGBA32F, GL_RGBA, GL_FLOAT };
    case vangfx::SurfaceFormat::R8:       return { GL_R8,     0x1903 /*GL_RED*/, GL_UNSIGNED_BYTE };
    case vangfx::SurfaceFormat::R16F:     return { GL_R16F,   0x1903, GL_HALF_FLOAT };
    case vangfx::SurfaceFormat::R32F:     return { GL_R32F,   0x1903, GL_FLOAT };
    case vangfx::SurfaceFormat::RG8:      return { GL_RG8,    0x8227 /*GL_RG*/, GL_UNSIGNED_BYTE };
    case vangfx::SurfaceFormat::RG16F:    return { GL_RG16F,  0x8227, GL_HALF_FLOAT };
    case vangfx::SurfaceFormat::RG32F:    return { GL_RG32F,  0x8227, GL_FLOAT };
    case vangfx::SurfaceFormat::D32F:     return { GL_DEPTH_COMPONENT32F, 0x1902 /*GL_DEPTH_COMPONENT*/, GL_FLOAT };
    case vangfx::SurfaceFormat::D24S8:    return { GL_DEPTH24_STENCIL8, 0x84F9 /*GL_DEPTH_STENCIL*/, 0x84FA /*GL_UNSIGNED_INT_24_8*/ };
    default:                              return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
    }
}

// ── GLPass ────────────────────────────────────────────────────────────────────

GLPass::GLPass(GLProcs& p, GLuint vao_fullscreen, GLuint params_ubo)
    : procs_(p), vao_fullscreen_(vao_fullscreen), params_ubo_(params_ubo)
{
    save_state();
}

GLPass::~GLPass() {
    if (!ended_) end();
}

void GLPass::save_state() noexcept {
    glGetIntegerv(GL_CURRENT_PROGRAM,         &saved_program_);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,    &saved_vao_);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&saved_fbo_);
    glGetIntegerv(GL_VIEWPORT,                 saved_viewport_);
    glGetIntegerv(GL_ACTIVE_TEXTURE,          &saved_active_tex_);
    saved_depth_test_ = glIsEnabled(GL_DEPTH_TEST);
    saved_blend_      = glIsEnabled(GL_BLEND);
    // Save bound textures for slots 0-7.
    for (int i = 0; i < 8; ++i) {
        if (procs_.ActiveTexture) procs_.ActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_tex_[i]);
    }
    if (procs_.ActiveTexture) procs_.ActiveTexture(saved_active_tex_);
}

void GLPass::restore_state() noexcept {
    if (procs_.UseProgram)      procs_.UseProgram(saved_program_);
    if (procs_.BindVertexArray) procs_.BindVertexArray(saved_vao_);
    if (procs_.BindFramebuffer) procs_.BindFramebuffer(GL_FRAMEBUFFER, saved_fbo_);
    glViewport(saved_viewport_[0], saved_viewport_[1],
               saved_viewport_[2], saved_viewport_[3]);
    if (saved_depth_test_) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (saved_blend_)      glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    // Restore textures.
    for (int i = 0; i < 8; ++i) {
        if (procs_.ActiveTexture) procs_.ActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, saved_tex_[i]);
    }
    if (procs_.ActiveTexture) procs_.ActiveTexture(saved_active_tex_);
    // Destroy the custom FBO we may have created.
    if (custom_fbo_ && procs_.DeleteFramebuffers) {
        procs_.DeleteFramebuffers(1, &custom_fbo_);
        custom_fbo_ = 0;
    }
}

void GLPass::set_render_target(Texture& rt) {
    auto* d = static_cast<GLTextureData*>(rt.data());
    if (!d) return;
    // Create a temporary FBO to render into this texture.
    if (!custom_fbo_ && procs_.GenFramebuffers) {
        procs_.GenFramebuffers(1, &custom_fbo_);
    }
    if (custom_fbo_ && procs_.BindFramebuffer && procs_.FramebufferTexture2D) {
        procs_.BindFramebuffer(GL_FRAMEBUFFER, custom_fbo_);
        procs_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, d->tex_id, 0);
    }
}

void GLPass::set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
               static_cast<GLsizei>(w), static_cast<GLsizei>(h));
}

void GLPass::set_pipeline(Pipeline& p) {
    auto* d = static_cast<GLPipelineData*>(p.data());
    if (!d) return;
    if (procs_.UseProgram) procs_.UseProgram(d->program);
    // Blend state.
    if (d->alpha_blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    // Depth state.
    if (d->depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    // Bind params UBO to binding point 0 (matches b0 convention).
    if (procs_.BindBufferBase && params_ubo_)
        procs_.BindBufferBase(GL_UNIFORM_BUFFER, 0, params_ubo_);
}

void GLPass::bind(uint32_t slot, Texture& t) {
    auto* d = static_cast<GLTextureData*>(t.data());
    if (!d) return;
    if (slot >= 8) return;
    if (procs_.ActiveTexture) procs_.ActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, d->tex_id);
}

void GLPass::update_params(const VanGFXParams& params) {
    if (!params_ubo_) return;
    if (!procs_.BindBuffer || !procs_.BufferSubData) return;
    procs_.BindBuffer(GL_UNIFORM_BUFFER, params_ubo_);
    procs_.BufferSubData(GL_UNIFORM_BUFFER, 0,
                         static_cast<GLsizeiptr>(sizeof(VanGFXParams)), &params);
    procs_.BindBuffer(GL_UNIFORM_BUFFER, 0);
}

void GLPass::draw_fullscreen() {
    if (!procs_.BindVertexArray || !procs_.DrawArrays) return;
    procs_.BindVertexArray(vao_fullscreen_);
    procs_.DrawArrays(GL_TRIANGLES, 0, 3);
}

void GLPass::draw(uint32_t vertices, uint32_t /*instances*/) {
    if (!procs_.DrawArrays) return;
    procs_.DrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices));
}

void GLPass::dispatch(uint32_t, uint32_t, uint32_t) {
    // Compute shaders not implemented for GL yet.
}

void GLPass::end() {
    if (ended_) return;
    ended_ = true;
    restore_state();
}

// ── GLDevice ──────────────────────────────────────────────────────────────────

GLDevice::GLDevice(HGLRC hglrc) : hglrc_(hglrc) {}

GLDevice::~GLDevice() {
    if (init_done_ && procs_) {
        if (vao_fullscreen_ && procs_->DeleteVertexArrays)
            procs_->DeleteVertexArrays(1, &vao_fullscreen_);
        if (params_ubo_ && procs_->DeleteBuffers)
            procs_->DeleteBuffers(1, &params_ubo_);
    }
}

void GLDevice::ensure_init() {
    if (init_done_) return;
    init_done_ = true;
    procs_ = &GLProcs::get();
    if (!procs_->loaded) return;

    // Empty VAO for fullscreen triangle draw.
    if (procs_->GenVertexArrays) {
        procs_->GenVertexArrays(1, &vao_fullscreen_);
    }
    // Params UBO — 256 bytes, matching VanGFXParams layout.
    if (procs_->GenBuffers && procs_->BindBuffer && procs_->BufferData) {
        procs_->GenBuffers(1, &params_ubo_);
        procs_->BindBuffer(GL_UNIFORM_BUFFER, params_ubo_);
        procs_->BufferData(GL_UNIFORM_BUFFER,
                           static_cast<GLsizeiptr>(sizeof(VanGFXParams)),
                           nullptr, GL_DYNAMIC_DRAW);
        procs_->BindBuffer(GL_UNIFORM_BUFFER, 0);
    }
}

Result<Texture> GLDevice::create_texture(const vangfx::TextureDesc& desc,
                                          const InitData* init) {
    ensure_init();
    if (!procs_ || !procs_->loaded)
        return gfx_err<Texture>(ShaderError::Unsupported, "GL procs not loaded");

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return gfx_err<Texture>(ShaderError::OutOfMemory, "glGenTextures failed");

    glBindTexture(GL_TEXTURE_2D, tex);
    auto fmt = to_gl_format(desc.format);
    const void* data = init ? init->data : nullptr;
    glTexImage2D(GL_TEXTURE_2D, 0,
                 static_cast<GLint>(fmt.internal_fmt),
                 static_cast<GLsizei>(desc.width),
                 static_cast<GLsizei>(desc.height),
                 0, fmt.base_fmt, fmt.type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    auto d = std::make_unique<GLTextureData>();
    d->tex_id = tex;
    d->owns   = true;
    return Texture{ std::move(d), desc.width, desc.height, desc.format };
}

Result<Buffer> GLDevice::create_buffer(uint32_t size, const void* init) {
    ensure_init();
    if (!procs_ || !procs_->GenBuffers)
        return gfx_err<Buffer>(ShaderError::Unsupported, "GL procs not loaded");

    GLuint buf = 0;
    procs_->GenBuffers(1, &buf);
    if (!buf) return gfx_err<Buffer>(ShaderError::OutOfMemory, "glGenBuffers failed");
    procs_->BindBuffer(GL_UNIFORM_BUFFER, buf);
    procs_->BufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(size),
                       init, GL_DYNAMIC_DRAW);
    procs_->BindBuffer(GL_UNIFORM_BUFFER, 0);

    auto d = std::make_unique<GLBufferData>();
    d->buf_id     = buf;
    d->size_bytes = static_cast<GLsizei>(size);
    return Buffer{ std::move(d), size };
}

static GLuint compile_gl_shader(GLProcs& p, GLenum stage, const char* src) {
    if (!p.CreateShader) return 0;
    GLuint sh = p.CreateShader(stage);
    p.ShaderSource(sh, 1, &src, nullptr);
    p.CompileShader(sh);
    GLint ok = 0;
    p.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        // Shader failed — caller checks program link status.
        p.DeleteShader(sh);
        return 0;
    }
    return sh;
}

Result<Pipeline> GLDevice::create_pipeline(const PipelineDesc& desc) {
    ensure_init();
    if (!procs_ || !procs_->CreateProgram)
        return gfx_err<Pipeline>(ShaderError::Unsupported, "GL procs not loaded");

    const char* vs_src = desc.shaders.vs.hlsl.c_str();
    const char* ps_src = desc.shaders.ps.hlsl.c_str();

    GLuint vs = compile_gl_shader(*procs_, GL_VERTEX_SHADER,   vs_src);
    GLuint ps = compile_gl_shader(*procs_, GL_FRAGMENT_SHADER, ps_src);
    if (!vs || !ps) {
        if (vs) procs_->DeleteShader(vs);
        if (ps) procs_->DeleteShader(ps);
        return gfx_err<Pipeline>(ShaderError::CompileFailed, "GLSL shader compile failed");
    }

    GLuint prog = procs_->CreateProgram();
    procs_->AttachShader(prog, vs);
    procs_->AttachShader(prog, ps);
    procs_->LinkProgram(prog);
    procs_->DeleteShader(vs);
    procs_->DeleteShader(ps);

    GLint ok = 0;
    procs_->GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        procs_->GetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        procs_->DeleteProgram(prog);
        return gfx_err<Pipeline>(ShaderError::LinkFailed, "GLSL program link failed");
    }

    // Bind uniform block "VanGFXParams" to binding point 0 if present.
    if (procs_->GetUniformBlockIndex && procs_->UniformBlockBinding) {
        GLuint idx = procs_->GetUniformBlockIndex(prog, "VanGFXParams");
        if (idx != 0xFFFFFFFFu) procs_->UniformBlockBinding(prog, idx, 0);
    }

    // Bind sampler uniforms tex0..tex7 to texture units 0..7.
    if (procs_->UseProgram && procs_->GetUniformLocation && procs_->Uniform1i) {
        procs_->UseProgram(prog);
        char name[8];
        for (int i = 0; i < 8; ++i) {
            name[0]='t'; name[1]='e'; name[2]='x';
            name[3]= static_cast<char>('0'+i); name[4]='\0';
            GLint loc = procs_->GetUniformLocation(prog, name);
            if (loc >= 0) procs_->Uniform1i(loc, i);
        }
        procs_->UseProgram(0);
    }

    auto d = std::make_unique<GLPipelineData>();
    d->program     = prog;
    d->alpha_blend = desc.alpha_blend;
    d->depth_test  = desc.depth_test;
    return Pipeline{ std::move(d) };
}

void GLDevice::update_buffer(Buffer& buf, const void* data, uint32_t size) {
    if (!procs_ || !procs_->BindBuffer || !procs_->BufferSubData) return;
    auto* d = static_cast<GLBufferData*>(buf.data());
    if (!d) return;
    procs_->BindBuffer(GL_UNIFORM_BUFFER, d->buf_id);
    procs_->BufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(size), data);
    procs_->BindBuffer(GL_UNIFORM_BUFFER, 0);
}

std::unique_ptr<Pass> GLDevice::begin_pass() {
    ensure_init();
    return std::make_unique<GLPass>(*procs_, vao_fullscreen_, params_ubo_);
}

Texture GLDevice::wrap_texture(void* native_tex, void* /*srv*/, void* /*rtv*/,
                                uint32_t w, uint32_t h,
                                vangfx::SurfaceFormat fmt) {
    auto d = std::make_unique<GLTextureData>();
    d->tex_id = static_cast<GLuint>(reinterpret_cast<uintptr_t>(native_tex));
    d->owns   = false;
    return Texture{ std::move(d), w, h, fmt };
}

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<Device> make_gl_device(HGLRC hglrc) {
    return std::make_unique<GLDevice>(hglrc);
}

} // namespace vangfx::gpu
