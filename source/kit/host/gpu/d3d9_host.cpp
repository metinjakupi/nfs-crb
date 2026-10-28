// d3d9_host.cpp - the Direct3D 9 device's host entry points (dx/host_d9.h),
// over whichever GPU backend the host's device belongs to (d3d9_backend.h).
//
// Draws, clears, blits and buffer writes are encoded on a thread of their own
// while the game carries on; everything that answers the game (a read, a query
// result) or hands a frame on (present) first lets that thread finish, then
// runs here. The draw's variable data is copied into the batch, because the
// device state it points at changes as soon as the call returns.
// RECOMP_D3D9_THREAD=0 encodes on the game thread instead.
//
// On the web the GPU belongs to the browser's main thread. Each hand-over asks
// that thread to drain the queue as soon as it is free (and its animation-frame
// loop drains it too, host_d9_pump); the game runs on a worker and waits there
// for anything it needs answered. A texture upload there is copied into the
// batch instead of waited for: a race load makes tens of thousands of them, one
// per mip level, and each wait was a round trip through the main thread.
#include "d3d9_backend.h"
#include "d3d9_common.h"

#include "../../dx/d3d9_shader.h"
#include "../../platform/os.h"
#include "../../platform/ab_phase.h"
#include "../d3d_render.h"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include "../present.h"
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

#ifdef __EMSCRIPTEN__
// One row per game frame the main thread hands to the presenter, in globalThis.__d3d9Frames:
// [when (ms), main-thread ms turning the game's commands into WebGPU calls since the last one,
//  game-thread ms spent waiting for the main thread meanwhile, ms a ready frame waited for a
//  presenter slot (GPU completions), draws, the in-run A/B phase (1: earlier behaviour)].
EM_JS(void, d3d9_stats_frame,
      (double t, double pump, double wait, double defer, int draws, int ab_old), {
    const f = globalThis.__d3d9Frames || (globalThis.__d3d9Frames = []);
    f.push([t, pump, wait, defer, draws, ab_old]);
    if (f.length > 200000) f.splice(0, 100000);
});
#endif

namespace {

D9Backend *g_test_backend = nullptr;

// The backend for the host's device: the first factory that takes it.
D9Backend *make_backend(gpu::Device *device) {
    if (!device)
        return nullptr;
#ifdef __APPLE__
    if (D9Backend *b = d9_metal_create(device))
        return b;
#endif
#ifdef __EMSCRIPTEN__
    if (D9Backend *b = d9_webgpu_create(device))
        return b;
#endif
#ifdef RECOMP_D9_VULKAN
    if (D9Backend *b = d9_vulkan_create(device))
        return b;
#endif
    return nullptr;
}

#ifdef __EMSCRIPTEN__
std::atomic<D9Backend *> g_web_backend{nullptr};
#endif

// RECOMP_D3D9_STATS: per-frame timing for the frame-rate harness.
bool d3d9_stats_on() {
    static const bool on = recomp_env("D3D9_STATS") != nullptr;
    return on;
}

D9Backend *backend() {
    if (g_test_backend)
        return g_test_backend;
#ifdef __EMSCRIPTEN__
    return g_web_backend.load(); // made on the main thread by host_d9_web_start
#endif
    static D9Backend *b = nullptr;
    static bool tried = false;
    if (tried)
        return b;
    D3DRenderer *shared = D3DRenderer::shared();
    if (!shared)
        return nullptr; // the host has not made its device yet; ask again later
    tried = true;
    if (recomp_env("D3D9_CPU"))
        return nullptr;
    b = make_backend(shared->device());
    if (b)
        fprintf(stderr, "d3d9: rendering on %s\n", b->name());
    else
        fprintf(stderr, "d3d9: no GPU renderer for this device; drawing on the CPU\n");
    return b;
}

// Bounded reads (host_d9_texture_read_async): one mailbox slot per ticket, filled by the backend
// when the GPU has run the copy (a Metal completion thread), read by the game without waiting.
// Only this small table is locked - never the render thread or the GPU.
struct Readback {
    uint64_t request = 0; // the newest request for the ticket
    bool done = false, failed = false;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bytes;
};
std::mutex &readback_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<uint32_t, Readback> &readbacks() {
    static std::unordered_map<uint32_t, Readback> r;
    return r;
}
std::atomic<uint64_t> g_readback_requests{0};
void deliver_readback(uint32_t ticket, uint64_t request, const uint8_t *bytes, uint32_t w,
                      uint32_t h) {
    std::lock_guard<std::mutex> lock(readback_mutex());
    auto it = readbacks().find(ticket);
    if (it == readbacks().end() || it->second.request != request)
        return; // superseded or forgotten
    Readback &r = it->second;
    r.done = true;
    r.failed = !bytes;
    r.w = w;
    r.h = h;
    if (bytes)
        r.bytes.assign(bytes, bytes + (size_t)w * h * 4u);
}

// Producer writes entries/forwarding; consumer writes receipt. Present's existing
// sync barrier drains all batches before taking a cross-thread snapshot.
static HostD9Accounting admission_totals{};

enum class Op : uint8_t {
    Draw,
    RejectedDraw,
    Clear,
    Stretch,
    Define,
    Drop,
    TextureUpload,
    BufferUpload,
    BufferDrop,
    QueryBegin,
    QueryEnd,
    QueryDrop,
    Probe,
    ReadAsync,
    Present,
    Call
};

struct Command {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    size_t at = 0;       // payload offset in the batch's bytes
    size_t state_at = 0; // a draw's sampler and render states, in the bytes
    HostD9Draw draw{};
    HostD9TextureDesc desc{};
    HostD9Target target{};
    HostD9Surface s0{}, s1{};
    int32_t r0[4] = {}, r1[4] = {};
    float z = 0;
    std::function<void()> *call = nullptr; // Op::Call: run on the render thread
};

// Test71 (RECOMP_D3D9_STATS only): what the game thread copies into the render batch per frame,
// how many batches it hands over, how many of those had to be made new and how often a batch's
// byte buffer had to grow (a reallocation and copy on the game thread).
static struct {
    uint64_t draws, bytes, const_bytes, state_puts, state_bytes, batches, fresh, grows;
} g_queue_stats;

// A batch's bytes. Each draw puts half a dozen small pieces (its declaration, constants,
// state tables...), thousands of draws a frame, and std::vector::insert cost more per call
// than the copy itself on the game thread; this is a capacity check and a memcpy.
class Bytes {
  public:
    Bytes() = default;
    Bytes(const Bytes &) = delete;
    Bytes &operator=(const Bytes &) = delete;
    ~Bytes() {
        free(p_);
    }
    const uint8_t *data() const {
        return p_;
    }
    size_t size() const {
        return n_;
    }
    size_t capacity() const {
        return cap_;
    }
    void clear() {
        n_ = 0;
    }
    void release() {
        free(p_);
        p_ = nullptr;
        n_ = cap_ = 0;
    }
    // Room for n more bytes at the end; returns where they go.
    uint8_t *append(size_t n) {
        if (cap_ - n_ < n) {
            const size_t want = std::max<size_t>({cap_ * 2, n_ + n, 64u << 10});
            uint8_t *q = (uint8_t *)realloc(p_, want);
            if (!q)
                abort();
            p_ = q;
            cap_ = want;
        }
        uint8_t *at = p_ + n_;
        n_ += n;
        return at;
    }

  private:
    uint8_t *p_ = nullptr;
    size_t n_ = 0, cap_ = 0;
};

struct Batch {
    std::vector<Command> commands;
    Bytes bytes;
    // The last draw's state tables in `bytes`, reused while the version holds.
    uint64_t state_version = 0;
    size_t state_at = 0;
    void clear() {
        commands.clear();
        bytes.clear();
        state_version = 0;
    }
    size_t put(const void *p, size_t n) {
        size_t at = bytes.size();
        if (n) {
            if (bytes.capacity() - at < n)
                ++g_queue_stats.grows;
            memcpy(bytes.append(n), p, n);
        }
        return at;
    }
};

class RenderThread {
  public:
    explicit RenderThread(D9Backend *r) : r_(r) {
        const char *env = recomp_env("D3D9_THREAD");
        threaded_ = !(env && !strcmp(env, "0"));
#ifdef __EMSCRIPTEN__
        threaded_ = true;
        pumped_ = true; // the browser's main loop drains the queue
#endif
        if (threaded_ && !pumped_)
            worker_ = std::thread([this] { run(); });
    }

    // ---- the game's side ----
    Batch &batch() {
        return *filling_;
    }
    // The browser's main thread drains the queue (the web).
    bool pumped() const {
        return pumped_;
    }
    // Bytes handed over that the pump has not run yet (pumped mode).
    size_t backlog() const {
        return backlog_.load(std::memory_order_relaxed);
    }
    // Pumped mode: a present joins the queue like a draw, so the game records
    // its next frame while the main thread encodes this one. It waits only
    // once it is kFramesAhead presents ahead of the main thread.
    void present_queued(uint32_t backbuffer, uint32_t width, uint32_t height) {
        Command c{Op::Present};
        c.a = backbuffer;
        c.b = width;
        c.c = height;
        filling_->commands.push_back(c);
        presents_queued_.fetch_add(1);
        hand_over();
        const uint64_t t0 = d3d9_stats_on() ? os_monotonic_ns() : 0;
        std::unique_lock<std::mutex> lock(m_);
        idle_cv_.wait(lock, [this] { return presents_queued_.load() < kFramesAhead; });
        if (t0)
            game_wait_ns_.fetch_add(os_monotonic_ns() - t0, std::memory_order_relaxed);
    }
    void submitted() {
        if (!threaded_) {
            execute(*filling_);
            filling_->clear();
            return;
        }
        // Hand work over often enough that the worker keeps pace with the game.
        if (filling_->commands.size() >= 48 || filling_->bytes.size() >= (1u << 20))
            hand_over();
    }
    // Everything queued has been encoded; the caller may use the backend.
    template <class F> auto sync(F &&f) -> decltype(f()) {
        if (!threaded_) {
            execute(*filling_);
            filling_->clear();
            return f();
        }
        if (pumped_)
            return sync_on_pump(std::forward<F>(f));
        hand_over();
        std::unique_lock<std::mutex> lock(m_);
        idle_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
        return f(); // the worker waits on m_ for new work, so it cannot run now
    }

    // The pumped queue: `f` runs on the pumping thread, after everything
    // queued before it, while the caller waits.
    template <class F> auto sync_on_pump(F &&f) -> decltype(f()) {
        using R = decltype(f());
        if constexpr (std::is_void<R>::value) {
            std::function<void()> job = [&] { f(); };
            run_on_pump(&job);
        } else {
            R result{};
            std::function<void()> job = [&] { result = f(); };
            run_on_pump(&job);
            return result;
        }
    }
    void run_on_pump(std::function<void()> *job) {
        Command c{Op::Call};
        c.call = job;
        filling_->commands.push_back(c);
        hand_over();
        const uint64_t t0 = d3d9_stats_on() ? os_monotonic_ns() : 0;
        std::unique_lock<std::mutex> lock(m_);
        idle_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
        if (t0)
            game_wait_ns_.fetch_add(os_monotonic_ns() - t0, std::memory_order_relaxed);
    }

#ifdef __EMSCRIPTEN__
    // The main thread: inside its frame or a drain, where locks may be held.
    void frame(bool inside) {
        main_depth_ += inside ? 1 : -1;
        if (!main_depth_ && pump_missed_) {
            pump_missed_ = false;
            pump();
        }
    }
    void asked_pump() {
        pump_asked_.store(false);
        if (main_depth_) {
            pump_missed_ = true; // drained when the frame ends
            return;
        }
        pump();
    }
#endif

    // Runs what is queued on the calling thread (pumped mode).
    void pump() {
#ifdef __EMSCRIPTEN__
        ++main_depth_;
        struct Leave {
            int &depth;
            ~Leave() {
                --depth;
            }
        } leave{main_depth_};
#endif
        std::unique_lock<std::mutex> lock(m_);
        while (!queue_.empty()) {
#ifdef __EMSCRIPTEN__
            // A present takes a presenter slot, and the GPU completions that
            // free one reach this thread through its event loop: leave the
            // frame queued until they have run, rather than wait for them here.
            if (ends_in_present(*queue_.front()) && !host_present_can_acquire()) {
                if (d3d9_stats_on() && !deferred_at_)
                    deferred_at_ = emscripten_get_now();
                pump_later(1);
                break;
            }
#endif
            std::unique_ptr<Batch> b = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            const size_t ran = b->bytes.size();
            const bool presented = ends_in_present(*b);
#ifdef __EMSCRIPTEN__
            if (d3d9_stats_on()) {
                if (deferred_at_ && presented) {
                    defer_ms_ += emscripten_get_now() - deferred_at_;
                    deferred_at_ = 0;
                }
                exec_started_ = emscripten_get_now();
            }
#endif
            execute(*b);
#ifdef __EMSCRIPTEN__
            if (d3d9_stats_on())
                pump_ms_ += emscripten_get_now() - exec_started_;
#endif
            b->clear();
            // A batch that carried a big texture keeps its storage as a spare
            // otherwise, and eight of those would hold on to a level's worth.
            if (b->bytes.capacity() > kSpareBytes)
                b->bytes.release();
            backlog_.fetch_sub(ran, std::memory_order_relaxed);
            lock.lock();
            busy_ = false;
            if (spare_.size() < 8)
                spare_.push_back(std::move(b));
            idle_cv_.notify_all(); // a present waiting for its frame budget
#ifdef __EMSCRIPTEN__
            // One frame per task: the browser shows it and delivers the GPU's
            // completions before the next one starts.
            if (presented && !queue_.empty()) {
                pump_later(0);
                break;
            }
#endif
        }
        idle_cv_.notify_all();
    }

  private:
    // A present is the last command of its batch (present_queued hands over).
    static bool ends_in_present(const Batch &b) {
        return !b.commands.empty() && b.commands.back().op == Op::Present;
    }
#ifdef __EMSCRIPTEN__
    // The main thread, leaving work queued: come back to it from the event
    // loop, unless a pump is already on its way. An immediate rather than a
    // zero timeout, which the browser stretches to 4 ms once timeouts nest.
    void pump_later(int ms) {
        if (pump_asked_.exchange(true))
            return;
        auto again = [](void *s) { ((RenderThread *)s)->asked_pump(); };
        if (ms)
            emscripten_set_timeout(again, ms, this);
        else
            emscripten_set_immediate(again, this);
    }
#endif

    void hand_over() {
        if (filling_->commands.empty())
            return;
        std::unique_ptr<Batch> next;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (pumped_)
                backlog_.fetch_add(filling_->bytes.size(), std::memory_order_relaxed);
            queue_.push_back(std::move(filling_));
            if (!spare_.empty()) {
                next = std::move(spare_.back());
                spare_.pop_back();
            }
        }
        work_cv_.notify_one();
        ++g_queue_stats.batches;
        if (!next)
            ++g_queue_stats.fresh;
        filling_ = next ? std::move(next) : std::make_unique<Batch>();
#ifdef __EMSCRIPTEN__
        if (pumped_ && !pump_asked_.exchange(true))
            // The proxied call only asks for a timeout. The main thread runs
            // proxied calls wherever it is blocked, including inside a browser
            // lock that drawing would take again (a queue's completion callback
            // waiting on the allocator, say); a timeout runs from the event loop
            // instead, where nothing of ours is part-way through.
            emscripten_proxy_async(
                emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                [](void *self) {
                    emscripten_async_call([](void *s) { ((RenderThread *)s)->asked_pump(); }, self,
                                          0);
                },
                this);
#endif
    }

    void run() {
        os_thread_prefer_performance();
#if defined(__APPLE__)
        pthread_setname_np("d3d9 render");
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), "d3d9 render");
#endif
        std::unique_lock<std::mutex> lock(m_);
        for (;;) {
            work_cv_.wait(lock, [this] { return !queue_.empty(); });
            std::unique_ptr<Batch> b = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            execute_pooled(*b);
            b->clear();
            lock.lock();
            busy_ = false;
            if (spare_.size() < 8)
                spare_.push_back(std::move(b));
            if (queue_.empty())
                idle_cv_.notify_all();
        }
    }
    void execute_pooled(Batch &b);

  public:
    void execute(Batch &b) {
        for (Command &cmd : b.commands) {
            const uint8_t *base = b.bytes.data();
            switch (cmd.op) {
            case Op::RejectedDraw:
            case Op::Draw: {
                HostD9Draw &d = cmd.draw;
                if (d.admission.serial) {
                    ++admission_totals.received;
                    admission_totals.received_rejected += cmd.op == Op::RejectedDraw;
                    admission_totals.received_serial_sum += d.admission.serial;
                    admission_totals.last_received = d.admission.serial;
                }
                size_t at = cmd.at;
                auto take = [&](size_t n) -> const uint8_t * {
                    const uint8_t *p = n ? base + at : nullptr;
                    at += n;
                    return p;
                };
                d.decl = take(d.decl_size);
                d.vconst = (const float *)take(d.vconst_count * 16u);
                d.pconst = (const float *)take(d.pconst_count * 16u);
                d.inline_vertices = take(d.inline_bytes);
                d.inline_indices = take(d.inline_index_bytes);
                d.sampler_state = (const uint32_t *)(base + cmd.state_at);
                d.render_state = (const uint32_t *)(base + cmd.state_at + 16 * 14 * 4);
                d.render_state_set = base + cmd.state_at + 16 * 14 * 4 + 256 * 4;
                d.label = cmd.a ? (const char *)take(cmd.a) : nullptr;
                if (cmd.op == Op::RejectedDraw)
                    r_->rejected_draw(d);
                else
                    r_->draw(d);
                ++frame_draws_;
                break;
            }
            case Op::Clear:
                r_->clear(cmd.target, cmd.r0, cmd.a,
                          cmd.a ? (const int32_t *)(base + cmd.at) : nullptr, cmd.b, cmd.c, cmd.z,
                          cmd.d);
                break;
            case Op::Stretch:
                r_->stretch(cmd.s0, cmd.r0, cmd.s1, cmd.r1, cmd.a);
                break;
            case Op::Define:
                r_->define(cmd.desc);
                break;
            case Op::Drop:
                r_->drop(cmd.a);
                break;
            case Op::TextureUpload:
                r_->upload(cmd.a, cmd.b, cmd.c, base + cmd.at, cmd.d);
                break;
            case Op::BufferUpload:
                r_->buffer_upload(cmd.a, cmd.b, cmd.c, base + cmd.at, cmd.d);
                break;
            case Op::BufferDrop:
                r_->buffer_drop(cmd.a);
                break;
            case Op::QueryBegin:
                r_->query_begin(cmd.a);
                break;
            case Op::QueryEnd:
                r_->query_end(cmd.a);
                break;
            case Op::QueryDrop:
                r_->query_drop(cmd.a);
                break;
            case Op::Probe:
                r_->probe_next(cmd.a ? (const char *)(base + cmd.at) : nullptr);
                break;
            case Op::ReadAsync:
                r_->read_async(cmd.a, cmd.b, cmd.c, cmd.d, (uint64_t)cmd.at, deliver_readback);
                break;
            case Op::Present:
                r_->present(cmd.a, cmd.b, cmd.c);
#ifdef __EMSCRIPTEN__
                if (pumped_ && d3d9_stats_on()) {
                    const double now = emscripten_get_now();
                    const uint64_t waited = game_wait_ns_.exchange(0, std::memory_order_relaxed);
                    d3d9_stats_frame(now, pump_ms_ + (now - exec_started_), waited * 1e-6,
                                     defer_ms_, (int)frame_draws_,
                                     recomp_ab::old_phase.load(std::memory_order_relaxed));
                    pump_ms_ = 0;
                    exec_started_ = now;
                    defer_ms_ = 0;
                }
#endif
                frame_draws_ = 0;
                presents_queued_.fetch_sub(1);
                break;
            case Op::Call:
                (*cmd.call)();
                break;
            }
        }
    }

  private:
    static constexpr size_t kSpareBytes = 4u << 20;
    static constexpr int kFramesAhead = 2;
    D9Backend *r_;
    bool threaded_ = false;
    bool pumped_ = false;
    std::atomic<size_t> backlog_{0};
    std::atomic<int> presents_queued_{0};
    std::atomic<bool> pump_asked_{false}; // a main-thread pump is on its way
    int main_depth_ = 0;                  // main thread only
    bool pump_missed_ = false;            // main thread only
    // RECOMP_D3D9_STATS (d3d9_stats_frame): game-thread waits for the main thread, and the
    // main thread's encoding time and presenter-slot waits since the last frame.
    std::atomic<uint64_t> game_wait_ns_{0};
    double pump_ms_ = 0, exec_started_ = 0, defer_ms_ = 0, deferred_at_ = 0; // main thread only
    uint32_t frame_draws_ = 0;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable work_cv_, idle_cv_;
    std::deque<std::unique_ptr<Batch>> queue_;
    std::vector<std::unique_ptr<Batch>> spare_;
    std::unique_ptr<Batch> filling_ = std::make_unique<Batch>();
    bool busy_ = false;
};

RenderThread *render_thread() {
    static RenderThread *t = nullptr;
    static D9Backend *for_backend = nullptr;
    D9Backend *r = backend();
    if (!r)
        return nullptr;
    if (r != for_backend) {
        // A test swapped the backend: the old thread (if any) is idle; leave it.
        for_backend = r;
        t = new RenderThread(r);
    }
    return t;
}

// The d9 caches decode a program once for everyone; decoding it here means the
// render thread only ever looks it up, so the bytes need not travel.
// Fills in any key the caller left out.
void predecode(HostD9Draw &d) {
    if (!d.vs_key)
        d.vs_key = d9sh::code_key(d.vs, d.vs ? d.vs_size : 0);
    if (!d.ps_key)
        d.ps_key = d9sh::code_key(d.ps, d.ps ? d.ps_size : 0);
    // The cache keeps a decoded program for good, and its lookup takes a lock the main
    // thread's draws take too: a key this thread had decoded for the previous draw needs none.
    thread_local uint64_t last_vs = 0, last_ps = 0;
    if (d.vs_key != last_vs) {
        d9sh::program_for_key(d.vs_key, d.vs, d.vs ? d.vs_size : 0);
        last_vs = d.vs_key;
    }
    if (d.ps_key != last_ps) {
        d9sh::program_for_key(d.ps_key, d.ps, d.ps ? d.ps_size : 0);
        last_ps = d.ps_key;
    }
}

void push(Op op, uint32_t a) {
    if (RenderThread *t = render_thread()) {
        Command c{op};
        c.a = a;
        t->batch().commands.push_back(c);
        t->submitted();
    }
}

// The game's side keeps each texture's shape, so a queued upload knows how
// many of the shim's bytes to copy before the call returns.
std::mutex &desc_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<uint32_t, HostD9TextureDesc> &descs() {
    static std::unordered_map<uint32_t, HostD9TextureDesc> d;
    return d;
}

// The bytes one upload reads: rows of `pitch`, one row per 4 pixel rows for the
// DXT formats (the shim's make_surface), the level's height as the backends
// count it. 0 for a texture this side never saw defined.
size_t upload_bytes(uint32_t id, uint32_t level, uint32_t pitch) {
    std::lock_guard<std::mutex> lock(desc_mutex());
    auto it = descs().find(id);
    if (it == descs().end() || level >= 32)
        return 0;
    const HostD9TextureDesc &d = it->second;
    uint32_t rows = std::max<uint32_t>((d.kind == HOST_D9_TEX_CUBE ? d.width : d.height) >> level, 1);
    if (d9gpu::is_dxt(d.format))
        rows = (rows + 3) / 4;
    return (size_t)rows * pitch;
}

// Past this many queued bytes an upload waits for the main thread after all,
// so a level load cannot pile up its whole texture set in the queue.
constexpr size_t kUploadBacklog = 64u << 20;

} // namespace

#ifdef __APPLE__
// Objective-C objects a batch creates are released as it finishes.
void d9_autorelease_run(void (*fn)(void *), void *arg);
void RenderThread::execute_pooled(Batch &b) {
    std::pair<RenderThread *, Batch *> job(this, &b);
    d9_autorelease_run(
        [](void *p) {
            auto *j = static_cast<std::pair<RenderThread *, Batch *> *>(p);
            j->first->execute(*j->second);
        },
        &job);
}
#else
void RenderThread::execute_pooled(Batch &b) {
    execute(b);
}
#endif

int host_d9_active(void) {
    return backend() != nullptr;
}
void host_d9_texture_define(const HostD9TextureDesc *desc) {
    RenderThread *t = render_thread();
    if (!t || !desc)
        return;
    Command c{Op::Define};
    c.desc = *desc;
    if (t->pumped()) {
        std::lock_guard<std::mutex> lock(desc_mutex());
        descs()[desc->id] = *desc;
    }
    t->batch().commands.push_back(c);
    t->submitted();
}
void host_d9_texture_drop(uint32_t id) {
    {
        std::lock_guard<std::mutex> lock(desc_mutex());
        descs().erase(id);
    }
    push(Op::Drop, id);
}
void host_d9_texture_upload(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                            uint32_t pitch) {
    RenderThread *t = render_thread();
    if (!t)
        return;
    // On the web, waiting means a round trip through the browser's main thread
    // for every mip level, so the bytes travel in the batch, as a buffer's do,
    // and the caller may reuse them at once. Elsewhere the render thread is a
    // thread of our own and the upload runs in step.
    const size_t n = t->pumped() && bytes ? upload_bytes(id, level, pitch) : 0;
    if (!n || t->backlog() > kUploadBacklog) {
        t->sync([&] { backend()->upload(id, face, level, bytes, pitch); });
        return;
    }
    Batch &b = t->batch();
    Command c{Op::TextureUpload};
    c.a = id;
    c.b = face;
    c.c = level;
    c.d = pitch;
    c.at = b.put(bytes, n);
    b.commands.push_back(c);
    t->submitted();
}
int host_d9_texture_read(uint32_t id, uint32_t face, uint32_t level, uint8_t *bytes,
                         uint32_t pitch) {
    RenderThread *t = render_thread();
    // The browser's backend cannot read a target back (it answers no), so the
    // round trip through the main thread would buy nothing.
    if (!t || t->pumped())
        return 0;
    return t->sync([&] { return backend()->read(id, face, level, bytes, pitch); }) ? 1 : 0;
}
void host_d9_texture_read_async(uint32_t id, uint32_t face, uint32_t level, uint32_t ticket) {
    RenderThread *t = render_thread();
    if (!t || !ticket)
        return;
    const uint64_t request = ++g_readback_requests;
    {
        std::lock_guard<std::mutex> lock(readback_mutex());
        Readback &r = readbacks()[ticket];
        r = Readback{};
        r.request = request;
    }
    Command c{Op::ReadAsync};
    c.a = id;
    c.b = face;
    c.c = level;
    c.d = ticket;
    c.at = (size_t)request;
    t->batch().commands.push_back(c);
    t->submitted();
}
int host_d9_texture_read_poll(uint32_t ticket, uint8_t *bytes, uint32_t pitch, uint32_t w,
                              uint32_t h) {
    std::lock_guard<std::mutex> lock(readback_mutex());
    auto it = readbacks().find(ticket);
    if (it == readbacks().end())
        return -1;
    const Readback &r = it->second;
    if (!r.done)
        return 0;
    if (r.failed || r.w != w || r.h != h || !bytes || pitch < w * 4u)
        return -1;
    for (uint32_t y = 0; y < h; ++y)
        memcpy(bytes + (size_t)y * pitch, r.bytes.data() + (size_t)y * w * 4u, (size_t)w * 4u);
    return 1;
}
void host_d9_texture_read_forget(uint32_t ticket) {
    std::lock_guard<std::mutex> lock(readback_mutex());
    readbacks().erase(ticket);
}
void host_d9_buffer_upload(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                           uint32_t size) {
    RenderThread *t = render_thread();
    if (!t || !bytes)
        return;
    Batch &b = t->batch();
    Command c{Op::BufferUpload};
    c.a = id;
    c.b = total;
    c.c = offset;
    c.d = size;
    c.at = b.put(bytes, size);
    b.commands.push_back(c);
    t->submitted();
}
void host_d9_buffer_drop(uint32_t id) {
    push(Op::BufferDrop, id);
}
void host_d9_admission_entry(uint64_t serial) {
    ++admission_totals.entries;
    admission_totals.entry_serial_sum += serial;
    admission_totals.last_entry = serial;
}
int host_d9_admission_enabled(void) {
    static const bool enabled = [] {
        const char *a = recomp_env("D3D9_ADMISSION");
        const char *t = recomp_env("D3D9_TRACE");
        return a && !strcmp(a, "1") && t && !strcmp(t, "1");
    }();
    return enabled;
}
static void queue_draw(const HostD9Draw *draw, bool rejected);
void host_d9_draw(const HostD9Draw *draw) {
    queue_draw(draw, false);
}
void host_d9_rejected_draw(const HostD9Draw *draw) {
    queue_draw(draw, true);
}
static void queue_draw(const HostD9Draw *draw, bool rejected) {
    RenderThread *t = render_thread();
    if (!t || !draw) {
        if (draw && draw->admission.serial)
            ++admission_totals.unforwarded;
        return;
    }
    Batch &b = t->batch();
    const size_t bytes_before = b.bytes.size();
    Command &c = b.commands.emplace_back();
    c.op = rejected ? Op::RejectedDraw : Op::Draw;
    c.draw = *draw;
    if (!rejected)
        predecode(c.draw);
    c.at = b.put(draw->decl, draw->decl ? draw->decl_size : 0);
    c.draw.decl_size = draw->decl ? draw->decl_size : 0;
    b.put(draw->vconst, draw->vconst ? draw->vconst_count * 16u : 0);
    c.draw.vconst_count = draw->vconst ? draw->vconst_count : 0;
    b.put(draw->pconst, draw->pconst ? draw->pconst_count * 16u : 0);
    c.draw.pconst_count = draw->pconst ? draw->pconst_count : 0;
    b.put(draw->inline_vertices, draw->inline_vertices ? draw->inline_bytes : 0);
    c.draw.inline_bytes = draw->inline_vertices ? draw->inline_bytes : 0;
    b.put(draw->inline_indices, draw->inline_indices ? draw->inline_index_bytes : 0);
    c.draw.inline_index_bytes = draw->inline_indices ? draw->inline_index_bytes : 0;
    // The consumer walks the per-draw payload through the label. Keep it
    // contiguous: state tables below may be inserted or reused by version.
    // The WebGPU renderer (the pumped queue) never reads a label, so the web skips the copy.
    if (draw->label && !t->pumped()) {
        size_t n = strlen(draw->label) + 1;
        b.put(draw->label, n);
        c.a = (uint32_t)n;
    }
    static const uint32_t zero_samplers[16 * 14] = {};
    static const uint32_t zero_states[256] = {};
    static const uint8_t zero_set[256] = {};
    if (!draw->state_version || draw->state_version != b.state_version) {
        b.state_at = b.put(draw->sampler_state ? draw->sampler_state : zero_samplers, 16 * 14 * 4);
        b.put(draw->render_state ? draw->render_state : zero_states, 256 * 4);
        b.put(draw->render_state_set ? draw->render_state_set : zero_set, 256);
        b.state_version = draw->state_version;
        ++g_queue_stats.state_puts;
        g_queue_stats.state_bytes += 16 * 14 * 4 + 256 * 4 + 256;
    }
    c.state_at = b.state_at;
    ++g_queue_stats.draws;
    g_queue_stats.bytes += b.bytes.size() - bytes_before;
    g_queue_stats.const_bytes += (draw->vconst ? draw->vconst_count * 16u : 0) + (draw->pconst ? draw->pconst_count * 16u : 0);
    // Programs are found by key on the render thread.
    c.draw.vs = nullptr;
    c.draw.ps = nullptr;
    if (draw->admission.serial) {
        ++admission_totals.forwarded;
        admission_totals.rejected += rejected;
        admission_totals.forwarded_serial_sum += draw->admission.serial;
    }
    t->submitted();
}
void host_d9_clear(const HostD9Target *target, const int32_t viewport[4], uint32_t count,
                   const int32_t *rects, uint32_t flags, uint32_t color, float z,
                   uint32_t stencil) {
    RenderThread *t = render_thread();
    if (!t || !target)
        return;
    Batch &b = t->batch();
    Command c{Op::Clear};
    c.target = *target;
    if (viewport)
        memcpy(c.r0, viewport, sizeof c.r0);
    c.a = rects ? count : 0;
    c.at = b.put(rects, rects ? count * 16u : 0);
    c.b = flags;
    c.c = color;
    c.z = z;
    c.d = stencil;
    b.commands.push_back(c);
    t->submitted();
}
void host_d9_stretch(HostD9Surface src, const int32_t src_rect[4], HostD9Surface dst,
                     const int32_t dst_rect[4], uint32_t filter) {
    RenderThread *t = render_thread();
    if (!t)
        return;
    Command c{Op::Stretch};
    c.s0 = src;
    c.s1 = dst;
    if (src_rect)
        memcpy(c.r0, src_rect, sizeof c.r0);
    if (dst_rect)
        memcpy(c.r1, dst_rect, sizeof c.r1);
    c.a = filter;
    t->batch().commands.push_back(c);
    t->submitted();
}
void host_d9_present(uint32_t backbuffer, uint32_t width, uint32_t height) {
    recomp_ab::tick(); // TEST ONLY phase of an in-run A/B; nothing unless RECOMP_AB_PERIOD is set
    {
        static const bool stats = recomp_env("D3D9_STATS") != nullptr;
        static uint64_t presents = 0;
        if (stats && ++presents % 600 == 0) {
            const double f = 600.0;
            fprintf(stderr,
                    "d3d9 host: per frame %.0f draws, %.0f KB copied into the batch (constants %.0f KB, state "
                    "tables %.0f KB in %.0f puts); %.1f batches, %.2f new, %.2f buffer growths\n",
                    g_queue_stats.draws / f, g_queue_stats.bytes / f / 1024.0, g_queue_stats.const_bytes / f / 1024.0,
                    g_queue_stats.state_bytes / f / 1024.0, g_queue_stats.state_puts / f, g_queue_stats.batches / f,
                    g_queue_stats.fresh / f, g_queue_stats.grows / f);
            g_queue_stats = {};
        }
    }
    RenderThread *t = render_thread();
    if (!t)
        return;
    // On the web nothing comes back from a present, so the game need not wait
    // for the main thread to encode the frame before it starts the next one.
    // Admission accounting snapshots the game's totals, so it stays in step.
    if (t->pumped() && !host_d9_admission_enabled()) {
        t->present_queued(backbuffer, width, height);
        return;
    }
    t->sync([&] {
        if (host_d9_admission_enabled()) {
            HostD9Accounting snapshot = admission_totals;
            snapshot.enabled = snapshot.drained = 1;
            backend()->admission_accounting(snapshot);
        }
        backend()->present(backbuffer, width, height);
    });
}
int host_d9_read_presented(uint8_t *rgb, uint32_t cap, uint32_t *w, uint32_t *h) {
    RenderThread *t = render_thread();
    return t && t->sync([&] { return backend()->read_presented(rgb, cap, w, h); }) ? 1 : 0;
}

void host_d9_query_begin(uint32_t id) {
    push(Op::QueryBegin, id);
}
void host_d9_query_end(uint32_t id) {
    push(Op::QueryEnd, id);
}
int host_d9_query_result(uint32_t id, uint32_t *count) {
    RenderThread *t = render_thread();
    return t && count ? t->sync([&] { return backend()->query_result(id, count); }) : -1;
}
void host_d9_query_drop(uint32_t id) {
    push(Op::QueryDrop, id);
}

bool host_d9_use_device_for_test(gpu::Device *device) {
    delete g_test_backend;
    g_test_backend = make_backend(device);
    return g_test_backend != nullptr;
}
void host_d9_probe_next_frame(const char *tag) {
    RenderThread *t = render_thread();
    if (!t)
        return;
    Batch &b = t->batch();
    Command c{Op::Probe};
    if (tag) {
        size_t n = strlen(tag) + 1;
        c.at = b.put(tag, n);
        c.a = (uint32_t)n;
    }
    b.commands.push_back(c);
    t->submitted();
}

#ifdef __EMSCRIPTEN__
bool host_d9_web_start(gpu::Device *device) {
    D9Backend *b = make_backend(device);
    g_web_backend.store(b);
    if (b)
        fprintf(stderr, "d3d9: rendering on %s\n", b->name());
    return b != nullptr;
}
void host_d9_pump(void) {
    if (!g_web_backend.load())
        return;
    if (RenderThread *t = render_thread())
        t->pump();
}
void host_d9_frame(bool inside) {
    if (!g_web_backend.load())
        return;
    if (RenderThread *t = render_thread())
        t->frame(inside);
}
#endif
