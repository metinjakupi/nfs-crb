#pragma once
#include "../platform/os.h"
#include <cstring>
#include <atomic>

inline std::atomic<bool> g_performance_reports_stopped{false};

// Enabled by the player only when the bounded automatic reporter is installed.
// Keep native builds and the explicit web A/B control free of collection work.
inline bool host_performance_reports_enabled() {
#ifdef __EMSCRIPTEN__
    static const bool enabled = [] {
        const char *value = recomp_env("PERF_REPORTS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled && !g_performance_reports_stopped.load(std::memory_order_relaxed);
#else
    return false;
#endif
}

#ifdef __EMSCRIPTEN__
extern "C" void host_performance_render(double at, double encode, double wait, double defer);
extern "C" void host_performance_gpu(double at, double ms, int passes);
#endif
