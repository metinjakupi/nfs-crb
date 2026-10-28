#pragma once
#include "frame_pacing.h"
#include "capture_marker.h"
#include "gpu/gpu.h"

#include <cstdint>
#include <vector>

// Presenter-thread only. Small immutable texture refreshed four times a
// second; drawn after the game copy, never into cached/guest game pixels.
// Text is the mods 6x8 bitmap font at 2x, so the overlay needs no text API.
class PerformanceOverlay {
  public:
    ~PerformanceOverlay();
    // Blend the overlay onto `target` (drawable pixels `w` x `h`) inside `cb`.
    // `mode` 0 draws nothing, 1 the counters, 2 the counters and the graph.
    void draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
              const FramePacingSnapshot &s, double now, int mode, int limit,
              const CaptureMarker &capture = {}, uint64_t presenter_frame = 0);

  private:
    static constexpr int width = 330, height = 136;
    void update(gpu::Device *device, const FramePacingSnapshot &s, int mode, int limit,
                const CaptureMarker &capture, uint64_t presenter_frame);
    gpu::Device *device_ = nullptr;
    gpu::Texture texture_;
    int texture_h_ = 0;
    double refreshed_ = -1;
    int last_mode_ = 0;
    uint64_t last_render_ = 0;
    std::vector<uint8_t> pixels_;
};
