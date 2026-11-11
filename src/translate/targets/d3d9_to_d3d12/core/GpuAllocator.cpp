// GpuAllocator stub — falls back to committed D3D12 resources.
//
// The full implementation depends on D3D12 Memory Allocator (D3D12MA), an
// external library not bundled with VanGFX. This stub skips suballocation:
// every caller that needs a resource falls back to its own committed-resource
// path, which the header documents as always valid ("An invalid Alloc means
// the resource is committed and owns its own memory, which is still a
// legitimate outcome when the allocator is off or has refused.").

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include <vangfx/translate/targets/d3d9_to_d3d12/core/GpuAllocator.h>
#include <vangfx/translate/d3d9/Log.h>

namespace vangfx::d3d9to12 {

GpuAllocator::~GpuAllocator() { Shutdown(); }

bool GpuAllocator::Init(ID3D12Device*, IDXGIAdapter*) noexcept
{
    return true;
}

void GpuAllocator::Shutdown() noexcept {}

HRESULT GpuAllocator::CreateResource(
    const D3D12_RESOURCE_DESC&,
    D3D12_HEAP_TYPE,
    D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*,
    const wchar_t*,
    ID3D12Resource** outRes,
    Alloc*           outAlloc) noexcept
{
    if (outRes)   *outRes   = nullptr;
    if (outAlloc) outAlloc->a = nullptr;
    return E_NOTIMPL;  // caller falls back to its own committed resource path
}

void GpuAllocator::Retire(const Alloc&, std::uint64_t) noexcept {}
void GpuAllocator::Collect(std::uint64_t) noexcept {}
void GpuAllocator::SetFrameIndex(std::uint32_t) noexcept {}

void GpuAllocator::Stats(std::uint64_t* outAllocatedBytes,
                          std::uint64_t* outUsedBytes,
                          std::uint32_t* outAllocationCount,
                          std::uint32_t* outBlockCount) const noexcept
{
    if (outAllocatedBytes)   *outAllocatedBytes   = 0;
    if (outUsedBytes)        *outUsedBytes        = 0;
    if (outAllocationCount)  *outAllocationCount  = 0;
    if (outBlockCount)       *outBlockCount       = 0;
}

void GpuAllocator::LogBudget(const char*) const noexcept {}

} // namespace vangfx::d3d9to12
