// Test66: ordering rules of the shader warm-up queue (metal/shader_warm_queue.h). No device, no game.
#include "../metal/shader_warm_queue.h"

#include <atomic>
#include <cstdio>
#include <thread>

static int failures = 0, checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                       \
        }                                                                                          \
    } while (0)

int main() {
    using namespace std::chrono;
    {   // finished entry: handed over once
        ShaderWarmQueue<int> q;
        CHECK(q.add_known("a"));
        CHECK(!q.add_known("a"));
        CHECK(q.begin("a"));
        q.finish("a", 7, true);
        int lib = 0;
        CHECK(q.take("a", &lib) && lib == 7);
        CHECK(!q.take("a", &lib));   // second use: not handed out twice (functions are cached above)
        CHECK(q.hits() == 1 && q.coalesced() == 0);
    }
    {   // queued entry needed first: claimed, worker skips it
        ShaderWarmQueue<int> q;
        q.add_known("b");
        int lib = 0;
        CHECK(!q.take("b", &lib));
        CHECK(q.claimed() == 1);
        CHECK(!q.begin("b"));
        CHECK(!q.take("b", &lib));
        CHECK(q.claimed() == 1);     // counted once
    }
    {   // unknown key: plain miss, nothing claimed
        ShaderWarmQueue<int> q;
        int lib = 0;
        CHECK(!q.take("z", &lib));
        CHECK(q.claimed() == 0);
    }
    {   // in-flight entry: the draw path waits for the worker instead of compiling a second time
        ShaderWarmQueue<int> q;
        q.add_known("c");
        CHECK(q.begin("c"));
        std::atomic<bool> got{false};
        int lib = 0;
        const auto t0 = steady_clock::now();
        std::thread draw([&] { got = q.take("c", &lib); });
        std::this_thread::sleep_for(milliseconds(60));
        q.finish("c", 9, true);
        draw.join();
        const auto waited = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        CHECK(got && lib == 9);
        CHECK(q.coalesced() == 1 && q.claimed() == 0);
        CHECK(waited >= 50 && waited < 1500);
    }
    {   // in-flight compile fails: the waiter is released and compiles itself
        ShaderWarmQueue<int> q;
        q.add_known("d");
        CHECK(q.begin("d"));
        bool got = true;
        int lib = 0;
        std::thread draw([&] { got = q.take("d", &lib); });
        std::this_thread::sleep_for(milliseconds(20));
        q.finish("d", 0, false);
        draw.join();
        CHECK(!got);
    }
    {   // stuck worker: the wait is bounded
        ShaderWarmQueue<int> q;
        q.add_known("e");
        CHECK(q.begin("e"));
        int lib = 0;
        const auto t0 = steady_clock::now();
        CHECK(!q.take("e", &lib, milliseconds(80)));
        const auto waited = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        CHECK(waited >= 70 && waited < 1000);
        q.finish("e", 1, true);      // late result is still usable by a later draw
        CHECK(q.take("e", &lib) && lib == 1);
    }
    {   // a key the worker is NOT on does not wait
        ShaderWarmQueue<int> q;
        q.add_known("f");
        q.add_known("g");
        CHECK(q.begin("f"));
        int lib = 0;
        const auto t0 = steady_clock::now();
        CHECK(!q.take("g", &lib));
        CHECK(duration_cast<milliseconds>(steady_clock::now() - t0).count() < 20);
        CHECK(q.coalesced() == 0 && q.claimed() == 1);
    }
    std::printf("shader_warm_queue_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
