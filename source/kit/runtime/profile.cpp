#include "profile.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include "../platform/os.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define PROFILE_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define PROFILE_EXPORT __attribute__((used, visibility("default")))
#endif

extern "C" const int recomp_profile_enabled = [] {
    const char *value = recomp_env("PROFILE");
    return value && std::strcmp(value, "1") == 0;
}();
// Generated table supplies names; runtime-only tests need no generated table.
extern "C" __attribute__((weak)) const char *recomp_profile_name(uint32_t) {
    return nullptr;
}
namespace {
struct ThreadState {
    ProfileSlot slot;
    std::vector<uint32_t> stack;
    // The thread map (recomp_profile_threads_dump): every function this thread entered and how
    // often, the critical sections it entered (guest address) and the imports it called (index).
    // Written by the thread itself under `m`; the dump reads them under it too.
    std::mutex m;
    std::vector<uint32_t> calls;
    uint32_t first = PROFILE_IDLE;
    std::map<uint32_t, uint64_t> locks, imports;
    uint32_t tid = 0; // the guest's thread id (GetCurrentThreadId), from the first note
};
// Intentionally process-lifetime: a sampler snapshot can outlive thread exit.
thread_local ThreadState *local = nullptr;
std::mutex g_threads_m;
// kernel32.cpp's; 0 in runtime-only tests that do not link it.
extern "C" __attribute__((weak)) uint32_t recomp_guest_thread_id() {
    return 0;
}
std::vector<ThreadState *> &all_threads() {
    static auto *v = new std::vector<ThreadState *>;
    return *v;
}
ThreadState *thread_state() {
    if (!local) {
        local = new ThreadState;
        local->tid = recomp_guest_thread_id();
        sched_register_profile_slot(&local->slot);
        std::lock_guard<std::mutex> lock(g_threads_m);
        all_threads().push_back(local);
    }
    return local;
}
struct State {
    std::atomic<bool> stop{false};
    std::thread sampler;
    std::mutex mutex;
    std::map<uint32_t, uint64_t> counts;
    uint64_t idle = 0;
    double guest_ms = 0;
    uint64_t guest_frames = 0;
};
State &state() {
    static State *s = new State;
    return *s;
}
void report() {
    profile_stop();
    auto rows = profile_snapshot();
    uint64_t total = 0;
    for (auto row : rows)
        total += row.samples;
    fprintf(stderr,
            "profile: 1000 Hz baton-holder wall samples; includes shims/host waits in caller\n");
    fprintf(stderr, "profile: attributed=%llu idle=%llu\n", (unsigned long long)total,
            (unsigned long long)state().idle);
    for (size_t i = 0; i < rows.size() && i < 30; ++i) {
        auto row = rows[i];
        const char *name = recomp_profile_name(row.index);
        if (!name) {
            fprintf(stderr, "profile: unresolved index=%u samples=%llu\n", row.index,
                    (unsigned long long)row.samples);
            continue;
        }
        fprintf(stderr, "profile: %s %.2f%% %llu\n", name,
                total ? 100.0 * row.samples / total : 0.0, (unsigned long long)row.samples);
    }
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.guest_frames)
        fprintf(stderr, "profile: guest_ms_per_frame=%.3f\n", s.guest_ms / s.guest_frames);
    else
        fprintf(stderr, "profile: guest_ms_per_frame omitted: no phase stats supplied\n");
}
void start() {
#ifdef __EMSCRIPTEN__
    // No sampler thread on the web: a new pthread started from a guest thread can wait forever for a
    // worker (the pool is sized for the game). The thread map needs only the push counts.
    return;
#endif
    static const bool started = [] {
        std::atexit(report);
        state().sampler = std::thread([] {
            auto &s = state();
            auto next = std::chrono::steady_clock::now();
            while (!s.stop.load(std::memory_order_relaxed)) {
                next += std::chrono::milliseconds(1);
                std::this_thread::sleep_until(next);
                auto *slot = sched_current_holder_slot();
                uint32_t top = slot ? slot->top.load(std::memory_order_relaxed) : PROFILE_IDLE;
                std::lock_guard<std::mutex> lock(s.mutex);
                if (top == PROFILE_IDLE)
                    ++s.idle;
                else
                    ++s.counts[top];
                // A stalled sampler must not manufacture catch-up samples.
                auto now = std::chrono::steady_clock::now();
                if (now > next + std::chrono::milliseconds(1))
                    next = now;
            }
        });
        return true;
    }();
    (void)started;
}
} // namespace
void profile_note_guest_ms(double milliseconds) {
    if (!recomp_profile_enabled || !std::isfinite(milliseconds) || milliseconds < 0)
        return;
    start();
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.guest_ms += milliseconds;
    ++s.guest_frames;
}
extern "C" void recomp_profile_push(uint32_t index) {
    if (!recomp_profile_enabled)
        return;
    if (!local) {
        thread_state();
        start();
    }
    {
        std::lock_guard<std::mutex> lock(local->m);
        if (local->first == PROFILE_IDLE)
            local->first = index;
        if (index >= local->calls.size())
            local->calls.resize(index + 1024);
        ++local->calls[index];
    }
    local->stack.push_back(index);
    local->slot.top.store(index, std::memory_order_relaxed);
}
extern "C" uint32_t recomp_profile_depth() {
    return local ? static_cast<uint32_t>(local->stack.size()) : 0;
}
extern "C" void recomp_profile_truncate(uint32_t depth) {
    if (!local)
        return;
    if (depth < local->stack.size())
        local->stack.resize(depth);
    local->slot.top.store(local->stack.empty() ? PROFILE_IDLE : local->stack.back(),
                          std::memory_order_relaxed);
}
extern "C" void recomp_profile_pop() {
    uint32_t depth = recomp_profile_depth();
    if (depth)
        recomp_profile_truncate(depth - 1);
}
void profile_stop() {
    auto &s = state();
    s.stop.store(true, std::memory_order_relaxed);
    if (s.sampler.joinable())
        s.sampler.join();
}
std::vector<ProfileSample> profile_snapshot() {
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::vector<ProfileSample> rows;
    for (auto [index, samples] : s.counts)
        rows.push_back({index, samples});
    std::sort(rows.begin(), rows.end(), [](auto a, auto b) {
        return a.samples != b.samples ? a.samples > b.samples : a.index < b.index;
    });
    return rows;
}

// The thread map: per guest thread, the functions entered, critical sections entered and imports
// called since the start, as JSON in `path` (on the web the page reads it back from the file
// system). Profiling (RECOMP_PROFILE=1) must be on; otherwise the file holds an empty list.
extern "C" __attribute__((weak)) const char *imports_tramp_name(uint32_t) {
    return nullptr;
}
// RECOMP_THREAD_MAP=1 turns on just these two counters (cheap, no change to how calls are made); the
// per-function counts need RECOMP_PROFILE=1, which routes every call through recomp_call.
static bool thread_map_on() {
    static const bool on = recomp_profile_enabled || (recomp_env("THREAD_MAP") && !strcmp(recomp_env("THREAD_MAP"), "1"));
    return on;
}
void profile_note_lock(uint32_t critical_section) {
    if (!thread_map_on())
        return;
    ThreadState *t = thread_state();
    std::lock_guard<std::mutex> lock(t->m);
    ++t->locks[critical_section];
}
void profile_note_import(uint32_t index) {
    if (!thread_map_on())
        return;
    ThreadState *t = thread_state();
    std::lock_guard<std::mutex> lock(t->m);
    ++t->imports[index];
}
extern "C" PROFILE_EXPORT int recomp_profile_threads_dump(const char *path) {
    if (!path)
        path = "/tmp/recomp-threads.json"; // what the page reads back
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    std::lock_guard<std::mutex> list(g_threads_m);
    fprintf(f, "[");
    for (size_t i = 0; i < all_threads().size(); ++i) {
        ThreadState *t = all_threads()[i];
        std::lock_guard<std::mutex> lock(t->m);
        const char *first = t->first == PROFILE_IDLE ? nullptr : recomp_profile_name(t->first);
        fprintf(f, "%s\n{\"thread\":%zu,\"tid\":%u,\"first\":\"%s\",\"functions\":{", i ? "," : "", i, t->tid,
                first ? first : "");
        bool comma = false;
        for (size_t k = 0; k < t->calls.size(); ++k)
            if (t->calls[k]) {
                const char *name = recomp_profile_name(uint32_t(k));
                if (name)
                    fprintf(f, "%s\"%s\":%u", comma ? "," : "", name, t->calls[k]);
                else
                    fprintf(f, "%s\"#%zu\":%u", comma ? "," : "", k, t->calls[k]);
                comma = true;
            }
        fprintf(f, "},\"locks\":{");
        comma = false;
        for (auto &l : t->locks) {
            fprintf(f, "%s\"%08x\":%llu", comma ? "," : "", l.first, (unsigned long long)l.second);
            comma = true;
        }
        fprintf(f, "},\"imports\":{");
        comma = false;
        for (auto &m : t->imports) {
            const char *n = imports_tramp_name(m.first);
            fprintf(f, "%s\"%s\":%llu", comma ? "," : "", n ? n : "?", (unsigned long long)m.second);
            comma = true;
        }
        fprintf(f, "}}");
    }
    fprintf(f, "\n]\n");
    fclose(f);
    return 1;
}
