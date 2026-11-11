/**
 * src/capture/file_queue.cpp — writing captured frames to disk off the render
 * thread.
 *
 * One worker thread, started when there is work and gone again after two idle
 * seconds, so a DLL that has flushed its captures can be unloaded without a
 * thread of its code still running. Its state is allocated once and never
 * freed, for the same reason in reverse: a worker still sleeping when the
 * process exits must not wake into a destroyed mutex.
 *
 * A frame is one job: its surfaces in order, each in the configured format (or
 * the nearest one that can hold it), then capture.json with what each write
 * did.
 */

#include "recorder.hpp"
#include "image.hpp"
#include "writers.hpp"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

namespace vangfx::capture {

namespace {

struct Job {
    std::shared_ptr<const FrameCapture> frame;
    CaptureFormat                       format = CaptureFormat::PNG;
    bool                                manifest = true;
};

struct Queue {
    std::mutex              mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Job>         jobs;
    bool                    running = false;   // a worker thread exists
    bool                    busy    = false;   // it is writing a job
};

Queue& queue() {
    static Queue* q = new Queue;   // never destroyed: see the top of the file
    return *q;
}

const char* role_name(SurfaceRole r) noexcept {
    switch (r) {
    case SurfaceRole::Depth:      return "Depth";
    case SurfaceRole::Backbuffer: return "Backbuffer";
    default:                      return "Color";
    }
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += c;
        }
    }
    return out;
}

void write_frame(const Job& job) {
    const FrameCapture& f = *job.frame;
    std::error_code ec;
    std::filesystem::create_directories(f.folder, ec);

    struct Result { std::string file, error, note; bool written = false; };
    std::vector<Result> results(f.surfaces.size());
    for (size_t i = 0; i < f.surfaces.size(); ++i) {
        const CapturedSurface& s = f.surfaces[i];
        Result& r = results[i];
        if (!s.available || s.file.empty()) continue;
        const ImageView img{ s.format, s.width, s.height, s.row_pitch, s.pixels.data() };
        std::filesystem::path path = s.file;
        WriteResult w;
        CaptureFormat fmt = job.format;
        // A format a PNG or EXR cannot hold (block-compressed, planar) goes out
        // as DDS; one DDS cannot name, as raw bytes.
        if ((fmt == CaptureFormat::PNG || fmt == CaptureFormat::EXR) && !layout_of(s.format).decodable()) {
            fmt = CaptureFormat::DDS;
            path.replace_extension(".dds");
            r.note = "not convertible to pixels; written as DDS";
        }
        switch (fmt) {
        case CaptureFormat::PNG: w = write_png(path, img); break;
        case CaptureFormat::EXR: w = write_exr(path, img); break;
        case CaptureFormat::DDS: w = write_dds(path, img); break;
        case CaptureFormat::Raw: w = write_raw(path, img); break;
        }
        if (!w.ok && fmt == CaptureFormat::DDS) {
            path.replace_extension(".raw");
            w = write_raw(path, img);
            r.note = "no DXGI format for a DDS header; written as raw bytes";
        }
        r.written = w.ok;
        r.file    = path.filename().string();
        r.error   = w.error;
        if (!w.note.empty()) r.note = r.note.empty() ? w.note : r.note + "; " + w.note;
    }
    if (!job.manifest) return;

    std::string j = "{\n";
    char buf[256];
    std::snprintf(buf, sizeof buf, "  \"frame\": %u,\n  \"width\": %u,\n  \"height\": %u,\n  \"passes\": %u,\n  \"draws\": %u,\n",
                  f.frame_index, f.width, f.height, f.passes, f.draws);
    j += buf;
    j += "  \"surfaces\": [\n";
    for (size_t i = 0; i < f.surfaces.size(); ++i) {
        const CapturedSurface& s = f.surfaces[i];
        const Result& r = results[i];
        std::snprintf(buf, sizeof buf,
                      "    { \"resource\": %llu, \"role\": \"%s\", \"layer\": \"%s\", \"format\": \"%s\", "
                      "\"width\": %u, \"height\": %u, \"pass\": %u, \"draws\": %u, \"available\": %s",
                      static_cast<unsigned long long>(s.resource_id), role_name(s.role),
                      GBufferCapture::layer_name(s.kind), format_name(s.format), s.width, s.height, s.pass, s.draws,
                      s.available ? "true" : "false");
        j += buf;
        if (r.written) j += ", \"file\": \"" + json_escape(r.file) + "\"";
        if (!r.error.empty()) j += ", \"error\": \"" + json_escape(r.error) + "\"";
        std::string note = s.note;
        if (!r.note.empty()) note = note.empty() ? r.note : note + "; " + r.note;
        if (!note.empty()) j += ", \"note\": \"" + json_escape(note) + "\"";
        j += i + 1 < f.surfaces.size() ? " },\n" : " }\n";
    }
    j += "  ]\n}\n";
    std::ofstream(f.folder / "capture.json", std::ios::binary | std::ios::trunc) << j;
}

void worker() {
    Queue& q = queue();
    std::unique_lock lk(q.mutex);
    for (;;) {
        if (q.jobs.empty()) {
            q.busy = false;
            q.idle.notify_all();
            if (!q.wake.wait_for(lk, std::chrono::seconds(2), [&] { return !q.jobs.empty(); })) {
                q.running = false;   // idle long enough: the thread goes
                q.idle.notify_all();
                return;
            }
        }
        Job job = std::move(q.jobs.front());
        q.jobs.pop_front();
        q.busy = true;
        lk.unlock();
        try { write_frame(job); } catch (...) {}
        lk.lock();
    }
}

} // namespace

void queue_frame_files(std::shared_ptr<const FrameCapture> frame, CaptureFormat format, bool manifest) {
    if (!frame || frame->folder.empty()) return;
    Queue& q = queue();
    std::lock_guard lk(q.mutex);
    q.jobs.push_back({ std::move(frame), format, manifest });
    q.busy = true;
    if (!q.running) {
        q.running = true;
        std::thread(worker).detach();
    } else {
        q.wake.notify_one();
    }
}

void flush_files() noexcept {
    Queue& q = queue();
    std::unique_lock lk(q.mutex);
    q.idle.wait(lk, [&] { return q.jobs.empty() && !q.busy; });
}

}  // namespace vangfx::capture
