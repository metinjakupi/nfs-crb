#include "../d3d9_trace.h"
#include "../d3d9_capture.h"
#include <chrono>
#include <limits>
#include <string>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)

static void history() {
    d9trace::Ring ring(2, 1);
    size_t visits = 0;
    ring.visit([&](const auto &) { ++visits; });
    CHECK(visits == 0);
    HostD9Draw d{};
    for (uint64_t frame = 1; frame <= 100; ++frame) {
        d.start = static_cast<uint32_t>(frame);
        CHECK(ring.record(d) != nullptr);
        auto *e = ring.event("submit", {frame, frame, frame - 1, frame});
        CHECK(e && e->sequence == 1);
        ring.present({frame, frame, frame - 1, frame}, frame * 10);
        std::vector<uint64_t> seen;
        ring.visit([&](const auto &f) {
            seen.push_back(f.number);
            CHECK(f.complete && f.draws == 1 && f.events == 1);
            CHECK(f.draw[0].start == f.number && f.present_wall_us == f.number * 10);
        });
        CHECK(seen.size() == std::min<uint64_t>(frame, 10));
        CHECK(seen.back() == frame);
        CHECK(seen.front() == (frame > 10 ? frame - 9 : 1));
    }
    // Current frame counts toward the ten-frame bound; overflowing it cannot
    // overwrite earlier records or grow storage. Draw/event sequence stays ordered.
    const size_t bytes = ring.allocated_bytes();
    auto *a = ring.record(d);
    auto *b = ring.record(d);
    CHECK(a && b && a != b);
    a->base = -50;
    CHECK(ring.record(d) == nullptr);
    CHECK(ring.event("one", {}) != nullptr);
    CHECK(ring.event("two", {}) == nullptr);
    CHECK(ring.allocated_bytes() == bytes && a->base == -50);
    visits = 0;
    ring.visit([&](const auto &f) {
        ++visits;
        CHECK(f.number >= 92 && f.number <= 101);
        if (f.number == 101) {
            CHECK(!f.complete && f.draw_overflow == 1 && f.event_overflow == 1);
            CHECK(f.draw[0].sequence == 0 && f.draw[1].sequence == 1);
            CHECK(f.event[0].sequence == 3 && f.next_sequence == 5);
        }
    });
    CHECK(visits == 10);
}

static void immutable_values(const char *path) {
    d9trace::Ring ring(4, 4);
    HostD9Draw d{};
    uint32_t rs[256]{}, ss[224]{};
    float vc[4]{1, 2, 3, 4}, pc[4]{0, 0, 0, std::numeric_limits<float>::quiet_NaN()};
    char label[] = "effect \"car\"\n\\";
    d.render_state = rs;
    d.sampler_state = ss;
    d.vconst = vc;
    d.vconst_count = 1;
    d.pconst = pc;
    d.pconst_count = 1;
    d.label = label;
    d.base_vertex = -12;
    rs[52] = 1;
    rs[58] = 0xff;
    auto *a = ring.record(d);
    CHECK(a && a->nonfinite_constants == 1);
    const auto before = *a;
    vc[0] = 99;
    rs[52] = 0;
    ss[5] = 3;
    label[0] = 'X';
    d.vbool[0] = 1;
    auto *b = ring.record(d);
    CHECK(b && b->vertex_constants != a->vertex_constants);
    CHECK(b->state != a->state && b->samplers != a->samplers &&
          b->bool_constants != a->bool_constants);
    CHECK(memcmp(a, &before, sizeof before) == 0);
    CHECK(a->base == -12 && a->label[0] == 'e');
    a->result = d9trace::Result::MissingIndexBuffer;
    a->textures[0] = {7, 8, 9, 10, 11};
    a->texture_desc[0][2] = 21;
    ring.present({20, 20, 19, 7}, 123456789);
    // Optional artifact used by the independent Python JSON contract check.
    FILE *f = path ? fopen(path, "wb") : tmpfile();
    CHECK(f != nullptr);
    if (f) {
        CHECK(ring.write(f, "test\n\"tag\"", {21, 20, 20, 7}));
        CHECK(fclose(f) == 0);
    }
    CHECK(!ring.write(nullptr, "", {}));
    CHECK(d9trace::hash(nullptr, 0) == 0);
}

static void capture_lifecycle() {
    using namespace capture_state;
    d9trace::CaptureControl c;
    // Thousands of startup presents, beyond the failed owner's trigger time.
    for (uint64_t n = 1; n <= 20000; ++n) {
        CHECK(!c.frame(n * 16666667, false));
        CHECK(!c.next_detail && c.id == 0 && c.state == Disarmed);
    }
    uint64_t now = 400000000000ull;
    for (unsigned id = 1; id <= 3; ++id) {
        CHECK(c.request(now, 24000 + id));
        CHECK(c.state == Armed);
        CHECK(!c.request(now + 1, 24000 + id) && c.reason == 6);
        CHECK(c.request(now + 2000000000, 24120 + id));
        CHECK(c.id == id && c.state == Capturing);
        CHECK(!c.request(now + 2000000001, 24120 + id) && c.reason == 1);
        CHECK(!c.frame(now + 7999999999, true));
        CHECK(c.frame(now + 8000000000, true));
        CHECK(c.state == Saving);
        CHECK(!c.request(now + 8000000001, 24500 + id) && c.reason == 2);
        auto marker = c.marker(24500 + id, now + 8000000000, true);
        CHECK(marker.render_frame == 24500 + id && marker.id == id && marker.detailed);
        CHECK(marker.monotonic_ns == now + 8000000000);
        c.saved(true, id != 2);
        CHECK(c.state == (id == 2 ? Incomplete : Ready));
        now += 10000000000;
    }
    CHECK(!c.request(now, 30000) && c.reason == 3);
    c.saved(false, true);
    CHECK(c.state == Incomplete && c.reason == 5);
}
static void measured_capture_load(const char *path) {
    d9trace::Ring ring(6144, 1024, 16);
    HostD9Draw draw{};
    // Owner frame's common constant ranges were VS 15..20, PS 0..7.
    // Include their hashes and full state/sampler arrays in the timing.
    float vc[80]{}, pc[28]{};
    uint32_t rs[256]{}, samplers[224]{};
    draw.vconst = vc;
    draw.vconst_count = 20;
    draw.pconst = pc;
    draw.pconst_count = 7;
    draw.render_state = rs;
    draw.sampler_state = samplers;
    uint64_t ns = 1000000000;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned frame = 1; frame <= 40; ++frame) {
        ring.retain = frame % 2 == 0;
        for (unsigned n = 0; n < 5044; ++n)
            ring.record(draw);
        for (unsigned n = 0; n < 600; ++n)
            ring.event("load", {});
        CHECK(ring.current().draws_attempted == 5044);
        CHECK(ring.current().draws == (ring.retain ? 5044 : 0));
        CHECK(!ring.current().draw_overflow);
        ring.present({frame, frame - 1, frame - 2, frame}, 1000000 + frame, ns += 500000000);
    }
    unsigned visits = 0;
    ring.visit([&](const auto &f) {
        ++visits;
        CHECK(f.number >= 10 && f.number <= 40 && f.number % 2 == 0);
        CHECK(f.monotonic_ns == 1000000000 + f.number * 500000000);
        CHECK(f.present_wall_us == 1000000 + f.number && f.complete);
        CHECK(f.end.submitted == f.number - 1 && f.draws == 5044 && !f.draw_overflow);
    });
    CHECK(visits == 16);
    const double recording_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    printf("capture load recording: 20 full plus 20 counts-only frames, 5044 draws + 600 events "
           "each: %.3f ms\n",
           recording_ms);
    if (path) {
        FILE *good = fopen((std::string(path) + ".load.jsonl").c_str(), "wb");
        CHECK(good != nullptr);
        if (good) {
            CHECK(ring.write(good, "measured-demand", {}));
            fclose(good);
        }
    }
    ring.retain = true;
    for (unsigned n = 0; n < 6144; ++n)
        ring.record(draw);
    CHECK(ring.current().draws == 6144 && !ring.current().draw_overflow);
    for (unsigned n = 0; n < 10000; ++n)
        ring.record(draw);
    for (unsigned n = 0; n < 1025; ++n)
        ring.event("overflow", {});
    CHECK(ring.current().draws_attempted == 16144 && ring.current().draw_overflow == 10000);
    CHECK(ring.current().events_attempted == 1025 && ring.current().event_overflow == 1);
    FILE *f = path ? fopen((std::string(path) + ".overflow.jsonl").c_str(), "wb") : tmpfile();
    CHECK(f != nullptr);
    if (f) {
        CHECK(ring.write(f, "deliberate-overflow", {}));
        fclose(f);
    }
    ring.reset(50000);
    CHECK(ring.current_frame_number() == 50000);
    visits = 0;
    ring.visit([&](const auto &) { ++visits; });
    CHECK(visits == 0);
    printf("capture load: 5044 attempted draws/frame, capacity 6144, buffers %zu bytes each, "
           "elapsed %.3f ms (includes JSON stress save)\n",
           ring.allocated_bytes(),
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
               .count());
}

static void admission_history() {
    HostD9Accounting totals{};
    totals.enabled = 1;
    totals.entries = totals.forwarded = totals.entry_serial_sum = totals.forwarded_serial_sum = 1;
    CHECK(!strcmp(d9trace::accounting_status(totals), "pending"));
    totals.drained = 1;
    CHECK(!strcmp(d9trace::accounting_status(totals), "unexplained"));
    totals.received = totals.received_serial_sum = 1;
    totals.last_entry = totals.last_received = 1;
    CHECK(!strcmp(d9trace::accounting_status(totals), "complete"));
    ++totals.entries;
    ++totals.unforwarded;
    totals.entry_serial_sum += 2;
    totals.last_entry = 2;
    CHECK(!strcmp(d9trace::accounting_status(totals), "unforwarded"));

    d9trace::Ring ring(2, 1, 2);
    const size_t bytes = ring.allocated_bytes();
    HostD9Draw d{};
    d.admission.serial = 123;
    d.admission.api = 82;
    d.admission.caller = 0x1234;
    d.admission.resource_identity[0] = 0x5678;
    auto *accepted = ring.record(d);
    CHECK(accepted && accepted->admission.serial == 123);
    d.admission.serial++;
    d.admission.reason = 3;
    auto *rejected = ring.record(d);
    CHECK(rejected != nullptr);
    rejected->result = d9trace::Result::ShimRejected;
    d.admission.resource_identity[0] = 0;
    CHECK(accepted->admission.resource_identity[0] == 0x5678);
    CHECK(rejected->admission.resource_identity[0] == 0x5678);
    CHECK(ring.record(d) == nullptr);
    CHECK(ring.current().draw_overflow == 1 && ring.current().draws_attempted == 3);
    CHECK(ring.allocated_bytes() == bytes);
    FILE *f = tmpfile();
    CHECK(f != nullptr);
    if (f) {
        CHECK(ring.write(f, "admission", {}));
        rewind(f);
        std::string json;
        char b[4096];
        while (size_t n = fread(b, 1, sizeof b, f))
            json.append(b, n);
        CHECK(json.find("\"reason\":3") != std::string::npos);
        CHECK(json.find("shim_rejected") != std::string::npos);
        CHECK(json.find("\"caller\":4660") != std::string::npos);
        CHECK(json.find("\"records_complete\":false") != std::string::npos);
        fclose(f);
    }
}

int main(int argc, char **argv) {
    admission_history();
    capture_lifecycle();
    measured_capture_load(argc == 2 ? argv[1] : nullptr);
    history();
    immutable_values(argc == 2 ? argv[1] : nullptr);
    bool rejected = false;
    try {
        d9trace::Ring invalid(0, 1);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    CHECK(rejected);
    printf("d3d9 trace: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
