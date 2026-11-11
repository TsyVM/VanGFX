#pragma once

/**
 * vangfx/gpu/resources.hpp — gpu-layer Texture, Buffer, and Pipeline handles.
 *
 * Each type is an RAII owner of a backend-allocated resource.  It is non-
 * copyable but movable, and is valid() after a successful Device::create_*()
 * call.  native() returns the underlying API pointer for backends that need
 * direct access.
 */

#include <vangfx/resources/format.hpp>  // SurfaceFormat
#include <memory>
#include <cstdint>

namespace vangfx::gpu {

// ── Texture ──────────────────────────────────────────────────────────────────

struct TextureData {
    virtual ~TextureData() = default;
    virtual void* native() const noexcept = 0;
    virtual void* srv()    const noexcept { return nullptr; } // SRV (D3D11/D3D12)
    virtual void* rtv()    const noexcept { return nullptr; } // RTV (D3D11)
    virtual uint64_t gpu_va() const noexcept { return 0; }   // GPU VA (D3D12)
};

class Texture {
    std::unique_ptr<TextureData> data_;
    uint32_t      width_  = 0;
    uint32_t      height_ = 0;
    vangfx::SurfaceFormat format_ = vangfx::SurfaceFormat::RGBA8;
public:
    Texture() = default;
    Texture(std::unique_ptr<TextureData> d, uint32_t w, uint32_t h, vangfx::SurfaceFormat f)
        : data_(std::move(d)), width_(w), height_(h), format_(f) {}
    Texture(Texture&&) noexcept = default;
    Texture& operator=(Texture&&) noexcept = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] bool  valid()  const noexcept { return data_ != nullptr; }
    [[nodiscard]] uint32_t width()  const noexcept { return width_;  }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] vangfx::SurfaceFormat format() const noexcept { return format_; }
    [[nodiscard]] void*   native() const noexcept { return data_ ? data_->native() : nullptr; }
    [[nodiscard]] void*   srv()    const noexcept { return data_ ? data_->srv()    : nullptr; }
    [[nodiscard]] void*   rtv()    const noexcept { return data_ ? data_->rtv()    : nullptr; }
    [[nodiscard]] uint64_t gpu_va() const noexcept { return data_ ? data_->gpu_va() : 0; }
    [[nodiscard]] TextureData* data() const noexcept { return data_.get(); }
};

// ── Buffer ───────────────────────────────────────────────────────────────────

struct BufferData {
    virtual ~BufferData() = default;
    virtual void* native() const noexcept = 0;
    virtual uint64_t gpu_va() const noexcept { return 0; }
};

class Buffer {
    std::unique_ptr<BufferData> data_;
    uint32_t size_ = 0;
public:
    Buffer() = default;
    Buffer(std::unique_ptr<BufferData> d, uint32_t size)
        : data_(std::move(d)), size_(size) {}
    Buffer(Buffer&&) noexcept = default;
    Buffer& operator=(Buffer&&) noexcept = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] bool     valid()  const noexcept { return data_ != nullptr; }
    [[nodiscard]] uint32_t size()   const noexcept { return size_;  }
    [[nodiscard]] void*    native() const noexcept { return data_ ? data_->native() : nullptr; }
    [[nodiscard]] uint64_t gpu_va() const noexcept { return data_ ? data_->gpu_va() : 0; }
    [[nodiscard]] BufferData* data() const noexcept { return data_.get(); }
};

// ── Pipeline ─────────────────────────────────────────────────────────────────

struct PipelineData {
    virtual ~PipelineData() = default;
};

class Pipeline {
    std::unique_ptr<PipelineData> data_;
public:
    Pipeline() = default;
    explicit Pipeline(std::unique_ptr<PipelineData> d) : data_(std::move(d)) {}
    Pipeline(Pipeline&&) noexcept = default;
    Pipeline& operator=(Pipeline&&) noexcept = default;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] PipelineData* data() const noexcept { return data_.get(); }
};

} // namespace vangfx::gpu
