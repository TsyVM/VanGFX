/**
 * src/capture/recorder.cpp — see recorder.hpp.
 */

#include "recorder.hpp"
#include "image.hpp"
#include "../core/context_impl.hpp"

#include <format_map.hpp>

#include <algorithm>
#include <cstdio>

namespace vangfx::capture {

Recorder::Recorder(ContextImpl& ctx, CaptureConfig cfg, bool whole_frame)
    : ctx_(ctx), cfg_(std::move(cfg)), host_(ctx.snapshot_host()), started_(whole_frame) {}

Recorder::~Recorder() {
    // Copies still waiting (a capture stopped mid-frame) go back to the host,
    // which frees each once the GPU is done with it.
    for (auto& p : pending_)
        if (p.snap && host_) host_->retire(std::move(p.snap));
}

void Recorder::observe(const TargetInfo& t, uint8_t slot, uint8_t set_count) {
    if (!t.id) return;
    auto it = observed_index_.find(t.id);
    if (it != observed_index_.end()) {
        Observed& o = observed_[it->second];
        if (o.info.format == SurfaceFormat::Unknown && t.format != SurfaceFormat::Unknown) o.info = t;
        return;
    }
    observed_index_.emplace(t.id, observed_.size());
    observed_.push_back({ t, 0, 0, slot, set_count });
}

bool Recorder::within_budget(const TargetInfo& t, std::string& why) {
    const uint64_t pitch = fmtmap::row_pitch(t.format, t.width);
    const uint64_t size  = (pitch ? pitch : uint64_t(t.width) * 16) * fmtmap::row_count(t.format, t.height);
    if (bytes_ + size > cfg_.max_bytes) {
        why = "left out: the frame's copies would pass CaptureConfig::max_bytes";
        return false;
    }
    bytes_ += size;
    return true;
}

void Recorder::end_pass(const void* scope, Binding& b) {
    ++passes_;
    const uint32_t pass = passes_;
    auto each = [&](const TargetInfo& t) {
        if (!t.id) return;
        if (auto it = observed_index_.find(t.id); it != observed_index_.end()) observed_[it->second].last_pass = pass;
        if (cfg_.scope != CaptureScope::Passes || !cfg_.cpu_readback) return;
        Pending p;
        p.info  = t;
        p.pass  = pass;
        p.draws = b.draws;
        p.scope = scope;
        if (!host_) p.why = "this backend cannot copy surfaces";
        else if (!host_->can_snapshot_in(scope, p.why)) {}
        else if (within_budget(t, p.why))
            p.snap = host_->snapshot(scope, t, p.why);
        pending_.push_back(std::move(p));
    };
    for (uint32_t i = 0; i < b.set.count; ++i) each(b.set.colour[i]);
    each(b.set.depth);
}

void Recorder::bind(const void* scope, const TargetSet& set) {
    std::lock_guard lk(mutex_);
    Binding& b = scopes_[scope];
    if (b.open && b.draws) end_pass(scope, b);
    b.set   = set;
    b.draws = 0;
    b.open  = true;
    const auto n = static_cast<uint8_t>(set.count);
    for (uint32_t i = 0; i < set.count; ++i) observe(set.colour[i], static_cast<uint8_t>(i), n);
    observe(set.depth, 8, n);
}

void Recorder::draw(const void* scope) {
    std::lock_guard lk(mutex_);
    auto it = scopes_.find(scope);
    ++draws_;
    if (it == scopes_.end() || !it->second.open) return;
    Binding& b = it->second;
    ++b.draws;
    // A target's slot is the one it is first drawn through: D3D9 binds one
    // slot per call, so the binding it was first seen in may be half made.
    const auto n = static_cast<uint8_t>(b.set.count);
    auto count = [&](const TargetInfo& t, uint8_t slot) {
        if (!t.id) return;
        auto o = observed_index_.find(t.id);
        if (o == observed_index_.end()) return;
        Observed& ob = observed_[o->second];
        if (!ob.draws) { ob.slot = slot; ob.set_count = n; }
        ++ob.draws;
    };
    for (uint32_t i = 0; i < b.set.count; ++i) count(b.set.colour[i], static_cast<uint8_t>(i));
    count(b.set.depth, 8);
}

bool Recorder::knows(const void* scope) {
    std::lock_guard lk(mutex_);
    auto it = scopes_.find(scope);
    return it != scopes_.end() && it->second.open;
}

void Recorder::pass_break(const void* scope) {
    std::lock_guard lk(mutex_);
    auto it = scopes_.find(scope);
    if (it == scopes_.end() || !it->second.open || !it->second.draws) return;
    end_pass(scope, it->second);
    it->second.draws = 0;
}

void Recorder::scope_end(const void* scope) {
    std::lock_guard lk(mutex_);
    auto it = scopes_.find(scope);
    if (it == scopes_.end()) return;
    if (it->second.open && it->second.draws) end_pass(scope, it->second);
    scopes_.erase(it);
}

void Recorder::submitted(const void* scope) {
    std::lock_guard lk(mutex_);
    for (auto& p : pending_)
        if (p.scope == scope) p.submitted = true;
}

void Recorder::reset_frame() {
    for (auto& [scope, b] : scopes_) b.draws = 0;   // bindings outlive the frame on D3D9 and D3D11
    observed_.clear();
    observed_index_.clear();
    pending_.clear();
    passes_ = 0;
    draws_  = 0;
    bytes_  = 0;
    // Targets still bound are what the next frame draws into first.
    for (auto& [scope, b] : scopes_) {
        if (!b.open) continue;
        const auto n = static_cast<uint8_t>(b.set.count);
        for (uint32_t i = 0; i < b.set.count; ++i) observe(b.set.colour[i], static_cast<uint8_t>(i), n);
        observe(b.set.depth, 8, n);
    }
}

bool Recorder::present(FrameContext& fc) {
    std::vector<Pending>  frame_copies;
    std::vector<Pending>  pass_copies;
    std::vector<Observed> observed;
    Pending               bb;
    uint64_t              bb_id = 0;
    uint32_t              passes = 0, draws = 0;
    {
        std::lock_guard lk(mutex_);
        // A capture begun part-way through a frame takes the next whole one.
        if (!started_) {
            started_ = true;
            for (auto& p : pending_)
                if (p.snap && host_) host_->retire(std::move(p.snap));
            reset_frame();
            return false;
        }
        // Passes still open where a copy can be recorded now -- the D3D9 device,
        // a D3D11 immediate context -- end here. An open D3D12 list is still
        // being recorded: its pass ends when it closes, in some later frame.
        for (auto& [scope, b] : scopes_) {
            if (!b.open || !b.draws) continue;
            if (host_ && host_->records_later(scope)) continue;
            end_pass(scope, b);
            b.draws = 0;
        }

        // The end of the frame: every target drawn into, and the back buffer --
        // listed once, as the back buffer, when the frame drew into it directly.
        if (host_) bb.snap = host_->snapshot_backbuffer(fc, bb.info, bb.why, cfg_.cpu_readback);
        if (!cfg_.cpu_readback) bb.why = "cpu_readback is off";
        bb.info.role = SurfaceRole::Backbuffer;
        bb_id = bb.info.id;
        std::vector<const Observed*> drawn;
        for (const Observed& o : observed_) {
            if (!o.draws) continue;
            if (bb_id && o.info.id == bb_id) { bb.draws = o.draws; continue; }
            drawn.push_back(&o);
        }

        std::vector<CapturedSurface> probe;
        std::vector<uint8_t> slots, counts;
        for (const Observed* o : drawn) {
            CapturedSurface s;
            s.resource_id = o->info.id; s.role = o->info.role; s.format = o->info.format;
            s.width = o->info.width;    s.height = o->info.height;
            probe.push_back(s);
            slots.push_back(o->slot);
            counts.push_back(o->set_count);
        }
        if (cfg_.scope == CaptureScope::GBuffer) classify(probe, slots, counts, bb.info.width, bb.info.height, cfg_.layer_mask);

        size_t k = 0;
        for (const Observed* po : drawn) {
            const Observed& o = *po;
            const CapturedSurface& s = probe[k++];
            if (cfg_.scope == CaptureScope::GBuffer && s.kind == GBufferLayerKind::Unknown) continue;
            Pending p;
            p.info  = o.info;
            p.draws = o.draws;
            if (!cfg_.cpu_readback)        p.why = "cpu_readback is off";
            else if (!host_)               p.why = "this backend cannot copy surfaces";
            else if (within_budget(o.info, p.why)) p.snap = host_->snapshot(nullptr, o.info, p.why);
            frame_copies.push_back(std::move(p));
            observed.push_back(o);
        }
        pass_copies.swap(pending_);
        passes = passes_;
        draws  = draws_;
        reset_frame();
    }

    if (cfg_.cpu_readback && host_) host_->flush();

    auto frame   = std::make_shared<FrameCapture>();
    auto storage = std::make_shared<std::vector<std::vector<uint8_t>>>();
    storage->reserve(frame_copies.size() + pass_copies.size() + 1);
    frame->frame_index = fc.frame_index();
    frame->width       = bb.info.width;
    frame->height      = bb.info.height;
    frame->passes      = passes;
    frame->draws       = draws;

    auto take = [&](Pending& p, uint32_t pass) {
        CapturedSurface s;
        s.resource_id = p.info.id;
        s.role        = bb_id && p.info.id == bb_id ? SurfaceRole::Backbuffer : p.info.role;
        s.format      = p.info.format;
        s.width       = p.info.width;
        s.height      = p.info.height;
        s.pass        = pass;
        s.draws       = p.draws;
        s.note        = p.why;
        if (p.snap) {
            if (p.snap->deferred() && !p.submitted) {
                s.note = "the command list it was copied in was not executed before Present";
            } else if (auto px = p.snap->read()) {
                s.format    = px->format;
                s.width     = px->width;
                s.height    = px->height;
                s.row_pitch = px->row_pitch;
                storage->push_back(std::move(px->bytes));
                s.pixels    = std::span<const uint8_t>(storage->back());
                s.available = true;
            } else {
                s.note = px.error().message;
            }
            if (host_) host_->retire(std::move(p.snap));
        }
        frame->surfaces.push_back(std::move(s));
    };

    std::vector<uint8_t> slots, counts;
    for (size_t i = 0; i < frame_copies.size(); ++i) {
        take(frame_copies[i], 0);
        slots.push_back(observed[i].slot);
        counts.push_back(observed[i].set_count);
    }
    if (bb.snap || bb.info.id || !bb.why.empty()) {
        take(bb, 0);
        if (!frame->width) { frame->width = frame->surfaces.back().width; frame->height = frame->surfaces.back().height; }
        slots.push_back(0);
        counts.push_back(1);
    }
    classify(frame->surfaces, slots, counts, frame->width, frame->height, cfg_.layer_mask);
    const size_t frame_end = frame->surfaces.size();
    for (Pending& p : pass_copies) take(p, p.pass);
    // A pass copy is of a resource the end-of-frame list has classified.
    for (size_t i = frame_end; i < frame->surfaces.size(); ++i) {
        for (size_t j = 0; j < frame_end; ++j)
            if (frame->surfaces[j].resource_id == frame->surfaces[i].resource_id) { frame->surfaces[i].kind = frame->surfaces[j].kind; break; }
    }

    // The G-buffer view: the first surface of each kind the mask wants.
    GBufferCapture& g = frame->gbuffer;
    g.frame_index = frame->frame_index;
    g.width       = frame->width;
    g.height      = frame->height;
    for (size_t j = 0; j < frame_end && g.layer_count < GBufferCapture::kMaxLayers; ++j) {
        const CapturedSurface& s = frame->surfaces[j];
        if (s.kind == GBufferLayerKind::Unknown || !cfg_.wants_layer(s.kind) || g.has(s.kind)) continue;
        GBufferLayer& l = g.layers[g.layer_count++];
        l.kind      = s.kind;
        l.available = s.available;
        l.texture   = { s.resource_id };
        l.format    = s.format;
        l.width     = s.width;
        l.height    = s.height;
        l.pixels    = s.pixels;
        l.row_pitch = s.row_pitch;
    }
    frame->storage = storage;
    g.storage      = storage;

    // File names, before anyone sees the frame.
    if (!cfg_.output_dir.empty()) {
        char folder[32];
        std::snprintf(folder, sizeof folder, "frame_%05u", frame->frame_index);
        frame->folder = cfg_.output_dir / folder;
        const char* ext = cfg_.output_format == CaptureFormat::PNG ? ".png"
                        : cfg_.output_format == CaptureFormat::EXR ? ".exr"
                        : cfg_.output_format == CaptureFormat::DDS ? ".dds" : ".raw";
        for (CapturedSurface& s : frame->surfaces) {
            if (!s.available) continue;
            std::string name;
            char buf[96];
            if (s.pass) { std::snprintf(buf, sizeof buf, "p%03u_", s.pass); name = buf; }
            if (s.role == SurfaceRole::Backbuffer) {
                name += "backbuffer";
            } else {
                std::snprintf(buf, sizeof buf, "%s%llu_%s", s.role == SurfaceRole::Depth ? "ds" : "rt",
                              static_cast<unsigned long long>(s.resource_id), s.role == SurfaceRole::Depth ? "Depth" : "Color");
                name += buf;
                if (s.kind != GBufferLayerKind::Unknown && s.kind != GBufferLayerKind::DepthRaw) {
                    name += '_';
                    name += GBufferCapture::layer_name(s.kind);
                }
            }
            s.file = (frame->folder / (name + ext)).string();
        }
    }

    std::shared_ptr<const FrameCapture> done = frame;
    ctx_.set_last_capture(done);
    if (!cfg_.output_dir.empty()) queue_frame_files(done, cfg_.output_format, cfg_.write_manifest);
    try { if (cfg_.on_capture) cfg_.on_capture(*done); } catch (...) {}
    try { if (cfg_.on_frame)   cfg_.on_frame(done->gbuffer); } catch (...) {}

    ++frames_done_;
    return cfg_.max_frames && frames_done_ >= cfg_.max_frames;
}

// ── G-buffer classification ──────────────────────────────────────────────────

void classify(std::vector<CapturedSurface>& surfaces, const std::vector<uint8_t>& slots,
              const std::vector<uint8_t>& set_counts, uint32_t bbw, uint32_t bbh, uint32_t layer_mask) {
    uint32_t used = 0;
    auto take = [&](CapturedSurface& s, GBufferLayerKind k) -> bool {
        const uint32_t bit = 1u << static_cast<uint32_t>(k);
        if ((used & bit) || !(layer_mask & bit)) return false;
        used |= bit;
        s.kind = k;
        return true;
    };
    for (size_t i = 0; i < surfaces.size(); ++i) {
        CapturedSurface& s = surfaces[i];
        s.kind = GBufferLayerKind::Unknown;
        const uint8_t slot  = i < slots.size() ? slots[i] : 0;
        const uint8_t count = i < set_counts.size() ? set_counts[i] : 1;
        if (s.role == SurfaceRole::Backbuffer) { take(s, GBufferLayerKind::Backbuffer); continue; }
        const bool full    = (!bbw || s.width == bbw) && (!bbh || s.height == bbh);
        const bool reduced = bbw && bbh && (s.width < bbw || s.height < bbh);
        if (s.role == SurfaceRole::Depth) { if (full) take(s, GBufferLayerKind::DepthRaw); continue; }
        const Layout l = layout_of(s.format);
        const bool ldr   = l.kind == ChannelKind::Unorm && l.bits == 8 && l.channels >= 3;
        const bool fp    = l.kind == ChannelKind::Float;
        const bool ten   = l.kind == ChannelKind::Unorm && l.bits == 10;
        const bool snorm = l.kind == ChannelKind::Snorm;
        if (count >= 2 && full) {
            // Multiple render targets at screen size: the G-buffer pass.
            if (slot == 0 && ldr && take(s, GBufferLayerKind::Albedo)) continue;
            if ((ten || snorm || (fp && l.channels == 2 && slot == 1) || (fp && l.channels == 4 && slot == 1)) &&
                take(s, GBufferLayerKind::Normal)) continue;
            if (fp && l.channels == 2 && slot >= 2 && take(s, GBufferLayerKind::Motion)) continue;
            if (fp && l.channels >= 3 && slot >= 2 && take(s, GBufferLayerKind::Emissive)) continue;
            if (ldr && slot >= 1 && take(s, GBufferLayerKind::Specular)) continue;
            continue;
        }
        // Single targets.
        if (l.channels == 1 && (fp || l.bits == 16) && reduced && take(s, GBufferLayerKind::Depth)) continue;
        if (l.channels <= 2 && l.bits == 8 && l.kind == ChannelKind::Unorm && take(s, GBufferLayerKind::AmbientOcclusion)) continue;
        if (fp && l.channels == 2 && take(s, GBufferLayerKind::Motion)) continue;
    }
}

const char* format_name(SurfaceFormat f) noexcept {
    using S = SurfaceFormat;
    switch (f) {
    case S::RGBA8: return "RGBA8"; case S::BGRA8: return "BGRA8"; case S::RGBA16F: return "RGBA16F";
    case S::RGBA32F: return "RGBA32F"; case S::R11G11B10F: return "R11G11B10F"; case S::R10G10B10A2: return "R10G10B10A2";
    case S::R8: return "R8"; case S::RG8: return "RG8"; case S::R16F: return "R16F"; case S::R32F: return "R32F";
    case S::RG16F: return "RG16F"; case S::RG32F: return "RG32F"; case S::A8R8G8B8: return "A8R8G8B8";
    case S::X8R8G8B8: return "X8R8G8B8"; case S::A16B16G16R16F: return "A16B16G16R16F";
    case S::A32B32G32R32F: return "A32B32G32R32F"; case S::R5G6B5: return "R5G6B5";
    case S::BC1: return "BC1"; case S::BC2: return "BC2"; case S::BC3: return "BC3"; case S::BC4: return "BC4";
    case S::BC5: return "BC5"; case S::BC6H: return "BC6H"; case S::BC6H_SF16: return "BC6H_SF16"; case S::BC7: return "BC7";
    case S::D16: return "D16"; case S::D24S8: return "D24S8"; case S::D24X8: return "D24X8"; case S::D32F: return "D32F";
    case S::D32FS8: return "D32FS8"; case S::INTZ: return "INTZ"; case S::RAWZ: return "RAWZ"; case S::DF16: return "DF16";
    case S::DF24: return "DF24"; case S::R8_TYPELESS: return "R8_TYPELESS"; case S::R16_TYPELESS: return "R16_TYPELESS";
    case S::R32_TYPELESS: return "R32_TYPELESS"; case S::R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case S::R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS"; case S::R32G32B32A32_TYPELESS: return "R32G32B32A32_TYPELESS";
    case S::R32G32_TYPELESS: return "R32G32_TYPELESS"; case S::R32G32B32_TYPELESS: return "R32G32B32_TYPELESS";
    case S::R24G8_TYPELESS: return "R24G8_TYPELESS"; case S::R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    case S::BC1_TYPELESS: return "BC1_TYPELESS"; case S::BC2_TYPELESS: return "BC2_TYPELESS"; case S::BC3_TYPELESS: return "BC3_TYPELESS";
    case S::BC4_TYPELESS: return "BC4_TYPELESS"; case S::BC5_TYPELESS: return "BC5_TYPELESS"; case S::BC6H_TYPELESS: return "BC6H_TYPELESS";
    case S::BC7_TYPELESS: return "BC7_TYPELESS"; case S::R8_UINT: return "R8_UINT"; case S::R8_SINT: return "R8_SINT";
    case S::R16_UINT: return "R16_UINT"; case S::R16_SINT: return "R16_SINT"; case S::R32_UINT: return "R32_UINT";
    case S::R32_SINT: return "R32_SINT"; case S::R8G8_UINT: return "R8G8_UINT"; case S::R8G8_SINT: return "R8G8_SINT";
    case S::R16G16_UINT: return "R16G16_UINT"; case S::R16G16_SINT: return "R16G16_SINT"; case S::R32G32_UINT: return "R32G32_UINT";
    case S::R32G32_SINT: return "R32G32_SINT"; case S::R8G8B8A8_UINT: return "R8G8B8A8_UINT"; case S::R8G8B8A8_SINT: return "R8G8B8A8_SINT";
    case S::R16G16B16A16_UINT: return "R16G16B16A16_UINT"; case S::R16G16B16A16_SINT: return "R16G16B16A16_SINT";
    case S::R32G32B32A32_UINT: return "R32G32B32A32_UINT"; case S::R32G32B32A32_SINT: return "R32G32B32A32_SINT";
    case S::B5G6R5: return "B5G6R5"; case S::B5G5R5A1: return "B5G5R5A1"; case S::B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
    case S::R10G10B10A2_UINT: return "R10G10B10A2_UINT"; case S::NV12: return "NV12"; case S::P010: return "P010";
    case S::YUY2: return "YUY2"; case S::RGBA8_SRGB: return "RGBA8_SRGB"; case S::BGRA8_SRGB: return "BGRA8_SRGB";
    case S::BC7_SRGB: return "BC7_SRGB"; case S::RGBA16_UNORM: return "RGBA16_UNORM"; case S::RGBA16_SNORM: return "RGBA16_SNORM";
    case S::RG16_UNORM: return "RG16_UNORM"; case S::RG16_SNORM: return "RG16_SNORM"; case S::R16_UNORM: return "R16_UNORM";
    case S::RGB32F: return "RGB32F";
    default: return "Unknown";
    }
}

}  // namespace vangfx::capture
