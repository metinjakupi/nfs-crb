#include "performance_overlay.h"

#include "../mods/mods_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>
// The round trip to the online server in ms, as the page measures it: a game's online script
// (Carbon's web/net.js) sets globalThis.recompOnlinePing to a number, or to null while it does not
// know one (connecting, a server that does not answer pings). -1: the page is not online (no such
// global), -2: online, not known. Asked on the browser's main thread, where the web presenter runs
// and globalThis is the page's window, at most four times a second (each update()).
EM_JS(double, recomp_web_online_ping, (), {
    const v = globalThis['recompOnlinePing'];
    if (v === undefined) return -1;
    return typeof v === 'number' && isFinite(v) && v >= 0 ? v : -2;
});
#endif

namespace {
// The online round trip for the PING line: ms, -2 online but not known, -1 no line.
double online_ping_ms() {
#ifdef __EMSCRIPTEN__
    return recomp_web_online_ping();
#else
    return -1;
#endif
}

struct Canvas {
    std::vector<uint8_t> &px;
    int w, h;
    void fill(int x, int y, int fw, int fh, int r, int g, int b, int a) {
        // Premultiplied, as the HUD pipeline blends One / OneMinusSrcAlpha.
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(w, x + fw), y1 = std::min(h, y + fh);
        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx) {
                uint8_t *p = &px[(size_t(yy) * w + xx) * 4];
                p[0] = uint8_t(r * a / 255);
                p[1] = uint8_t(g * a / 255);
                p[2] = uint8_t(b * a / 255);
                p[3] = uint8_t(a);
            }
    }
    // 6x8 glyphs at 2x: 12 pixels per column, 16 per row.
    void text(int x, int y, const char *str, int r, int g, int b) {
        for (; *str; ++str, x += 12) {
            const uint8_t *glyph = mods_font6x8_glyph(*str);
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 6; ++col)
                    if (glyph[row] & (0x20 >> col))
                        fill(x + col * 2, y + row * 2, 2, 2, r, g, b, 255);
        }
    }
};
} // namespace

PerformanceOverlay::~PerformanceOverlay() {
    if (device_ && texture_)
        device_->destroy(texture_);
}

// Rasterize a pacing snapshot into the overlay texture. The graph uses a fixed
// 50 ms scale and marks samples exceeding the selected frame budget. While the
// page is online (the web only) a PING line follows RPT and the box grows by it.
void PerformanceOverlay::update(gpu::Device *device, const FramePacingSnapshot &s, int mode,
                                int limit, const CaptureMarker &capture, uint64_t presenter_frame) {
    const double ping = online_ping_ms();
    const int ping_rows = ping == -1 ? 0 : 18;
    const int base_used = (mode == 2 ? height : 88) + ping_rows;
    const int used = base_used + (capture.enabled ? 80 : 0);
    pixels_.assign(size_t(width) * used * 4, 0);
    Canvas c{pixels_, width, used};
    c.fill(0, 0, width, used, 6, 9, 15, 224);
    char line[64];
    snprintf(line, sizeof line, "NEW %5.1f FPS  DSP %5.1f", s.new_fps, s.display_fps);
    c.text(10, 6, line, 115, 255, 196);
    snprintf(line, sizeof line, "FRAME %5.1f ms P95 %5.1f", s.median_ms, s.p95_ms);
    c.text(10, 24, line, 235, 242, 255);
    snprintf(line, sizeof line, "GPU %4.1f ms  AGE %4.1f ms", s.gpu_ms, s.age_ms);
    c.text(10, 42, line, 204, 217, 237);
    char cap[24];
    if (limit)
        snprintf(cap, sizeof cap, "CAP %d", limit);
    else
        snprintf(cap, sizeof cap, "ORIGINAL");
    snprintf(line, sizeof line, "RPT %3.0f%% DROP %llu %s", s.repeat_percent,
             (unsigned long long)s.drops, cap);
    line[26] = 0;
    c.text(10, 60, line, 204, 217, 237);
    if (ping_rows) {
        // Amber from 100 ms, red from 200 ms (the graph's late-frame red); "--" dimmed; a
        // server on this computer answers in under half a millisecond: "<1".
        if (ping >= 0) {
            const int ms = int(std::min(9999.0, std::floor(ping + 0.5)));
            if (ms)
                snprintf(line, sizeof line, "PING %4d ms", ms);
            else
                snprintf(line, sizeof line, "PING   <1 ms");
            if (ms >= 200)
                c.text(10, 78, line, 255, 102, 77);
            else if (ms >= 100)
                c.text(10, 78, line, 255, 210, 80);
            else
                c.text(10, 78, line, 204, 217, 237);
        } else {
            c.text(10, 78, "PING   --", 143, 166, 189);
        }
    }
    if (mode == 2) {
        // Fixed 0..50ms axis from the bottom edge. A red bar means a missed budget.
        const double budget = 1000.0 / (limit ? limit : 60);
        const int base = base_used - 13;
        c.fill(10, base, width - 20, 1, 51, 71, 92, 255);
        c.fill(10, base - int(std::min(50.0, budget) * .7), width - 20, 1, 102, 122, 140, 204);
        for (size_t i = 0; i < s.intervals_ms.size(); ++i) {
            const double ms = s.intervals_ms[i];
            const bool late = ms > budget * 1.2;
            const int bar = int(std::min(50.0, ms) * .7);
            c.fill(10 + int(i * 2.55), base - bar, 2, bar, late ? 255 : 77, late ? 102 : 217,
                   late ? 77 : 255, 255);
        }
    }
    if (capture.enabled) {
        snprintf(line, sizeof line, "CAP C%llu %s", (unsigned long long)capture.id,
                 capture_state::name(capture.state));
        c.text(10, base_used, line, 255, 210, 80);
        snprintf(line, sizeof line, "R%llu P%llu %s", (unsigned long long)capture.render_frame,
                 (unsigned long long)presenter_frame, capture.detailed ? "DETAIL" : "COUNTS");
        c.text(10, base_used + 18, line, 235, 242, 255);
        snprintf(line, sizeof line, "MONO %llu MS",
                 (unsigned long long)(capture.monotonic_ns / 1000000));
        c.text(10, base_used + 36, line, 235, 242, 255);
        c.text(10, base_used + 54, capture_state::reason(capture.reason), 255, 210, 80);
    }
    if (device_ != device || !texture_ || texture_h_ != used) {
        if (device_ && texture_)
            device_->destroy(texture_);
        device_ = device;
        texture_ = device->create_texture(
            {width, used, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
        texture_h_ = used;
    }
    if (texture_)
        device->upload(texture_, {0, 0, width, used}, pixels_.data(), width * 4);
}

void PerformanceOverlay::draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target,
                              int w, int h, const FramePacingSnapshot &s, double now, int mode,
                              int limit, const CaptureMarker &capture, uint64_t presenter_frame) {
    if (capture.enabled && !mode)
        mode = 1;
    if (!mode || !device || !target || !cb || w <= 0 || h <= 0)
        return;
    if ((capture.enabled && capture.render_frame != last_render_) || !texture_ ||
        mode != last_mode_ || now - refreshed_ >= .25 || device_ != device) {
        update(device, s, mode, limit, capture, presenter_frame);
        last_render_ = capture.render_frame;
        refreshed_ = now;
        last_mode_ = mode;
    }
    if (!texture_)
        return;
    gpu::RenderState state;
    state.color_format[0] = device->describe(target).format;
    state.color_count = 1;
    state.blend_enabled = true;
    state.src_rgb = state.src_alpha = gpu::Blend::One;
    state.dst_rgb = state.dst_alpha = gpu::Blend::OneMinusSrcAlpha;
    gpu::Pipeline pipeline = device->render_pipeline("hud", state);
    if (!pipeline)
        return;
    const double scale = std::clamp(double(h) / 900.0, 1.0, 3.0);
    const double ow = std::min(double(w) - 20, width * scale), oh = texture_h_ * (ow / width);
    float rect[] = {float(1 - 2 * (ow + 10) / w), float(1 - 20.0 / h), float(2 * ow / w),
                    float(-2 * oh / h)};
    gpu::RenderPass pass;
    pass.color_count = 1;
    pass.color[0].texture = target;
    pass.color[0].load = gpu::Load::Load;
    pass.color[0].store = gpu::Store::Store;
    device->begin_render_pass(cb, pass);
    device->set_pipeline(cb, pipeline);
    device->set_bytes(cb, gpu::Stage::Vertex, 0, rect, sizeof rect);
    device->set_texture(cb, gpu::Stage::Fragment, 0, texture_);
    gpu::SamplerState linear;
    linear.mag = linear.min = gpu::Filter::Linear;
    device->set_sampler(cb, gpu::Stage::Fragment, 0, linear);
    device->draw(cb, gpu::Primitive::TriangleStrip, 0, 4);
    device->end_render_pass(cb);
}
