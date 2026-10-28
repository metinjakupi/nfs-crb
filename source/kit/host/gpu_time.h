#pragma once
// gpu_time.h - Test83 (test report: after opening the graphics menu the overlay's GPU
// figure dropped to about 0.5 ms). The performance overlay's GPU figure was the presenter's own command
// buffer (GPUEndTime - GPUStartTime), which also counts any time that buffer spends waiting on the GPU
// behind the game's frame: 5-9.6 ms plateaus in the test session, then 0.45 ms (the composite alone)
// once the settings page had changed the presenter's phase. Neither was the game's rendering. The D3D9 Metal
// renderer now adds the GPU time of each of its finished command buffers and counts its presents here; the
// overlay shows that per game frame.
#include <atomic>
#include <cstdint>
inline std::atomic<uint64_t> g_host_game_gpu_us{0};     // GPU time of finished game command buffers, us
inline std::atomic<uint64_t> g_host_game_gpu_frames{0}; // game frames presented by the D3D9 renderer
// Test86 HDR output: the D3D9 renderer's frame had the core mod's pre-HUD scene marker (its alpha marks the
// scene, the HUD cleared it). Set by d3d9_metal at present, read when the presenter stages the frame.
inline std::atomic<bool> g_host_hdr_scene_mask{false};
