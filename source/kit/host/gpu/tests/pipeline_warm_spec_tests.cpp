// Test70: text form and bounds of a recorded pipeline (metal/pipeline_warm_spec.h). No device, no game.
#include "../metal/pipeline_warm_spec.h"

#include <cstdio>

static int failures = 0, checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                       \
        }                                                                                          \
    } while (0)

static PipeSpec sample() {
    PipeSpec p;
    p.vkey = std::string(64, 'a');
    p.fkey = std::string(63, '0') + "f";
    p.samples = 4;
    p.depth = 260;
    p.color[0] = {80, 15, 1, 4, 5, 0, 1, 5, 0};
    p.color[1] = {115, 8, 0, 0, 0, 0, 0, 0, 0};
    p.attr[0] = {30, 0, 0};
    p.attr[3] = {9, 12, 0};
    p.attr[7] = {31, 0, 8};
    p.layout[0] = {24, 1, 1};
    p.layout[8] = {16, 0, 0};
    return p;
}

int main() {
    {   // round trip of the canonical text
        PipeSpec p = sample(), q;
        CHECK(p.valid());
        const std::string t = p.text();
        CHECK(t == "p1 " + std::string(64, 'a') + " " + std::string(63, '0') + "f s4 d260 c0=80,15,1,4,5,0,1,5,0 "
                   "c1=115,8,0,0,0,0,0,0,0 a0=30,0,0 a3=9,12,0 a7=31,0,8 l0=24,1,1 l8=16,0,0");
        CHECK(PipeSpec::parse(t, &q));
        CHECK(q.text() == t);
        CHECK(q.samples == 4 && q.depth == 260 && q.color[0].dst_a == 5 && q.attr[7].buffer == 8 && q.layout[8].stride == 16);
    }
    {   // any field that differs gives a different identity (a variant never matches another's line)
        const std::string base = sample().text();
        PipeSpec p = sample();
        p.samples = 1;
        CHECK(p.text() != base);
        p = sample();
        p.color[0].mask = 7;   // a bloom mask pass vs the colour-only write
        CHECK(p.text() != base);
        p = sample();
        p.color[0].blend = 0;
        p.color[0] = {80, 15, 0, 0, 0, 0, 0, 0, 0};
        CHECK(p.text() != base);
        p = sample();
        p.depth = 0;
        CHECK(p.text() != base);
        p = sample();
        p.layout[0].stride = 28;
        CHECK(p.text() != base);
        p = sample();
        p.fkey[0] = 'b';
        CHECK(p.text() != base);
    }
    {   // refused: anything not the exact canonical text of a valid spec
        const std::string t = sample().text();
        PipeSpec q;
        CHECK(!PipeSpec::parse(t + " ", &q));
        CHECK(!PipeSpec::parse(" " + t, &q));
        CHECK(!PipeSpec::parse("p2" + t.substr(2), &q));
        CHECK(!PipeSpec::parse(t + " a0=30,0,0", &q));        // repeated / out of order
        CHECK(!PipeSpec::parse(t + " c4=80,15,0,0,0,0,0,0,0", &q));   // no fifth attachment
        CHECK(!PipeSpec::parse(t + " l9=16,0,0", &q));
        CHECK(!PipeSpec::parse(t + " x0=1", &q));
        std::string u = t;
        u.replace(u.find(" s4 "), 4, " s3 ");
        CHECK(!PipeSpec::parse(u, &q));                       // sample count not 1/2/4/8
        u = t;
        u.replace(u.find("c0=80,15"), 8, "c0=80,16");
        CHECK(!PipeSpec::parse(u, &q));                       // write mask beyond RGBA
        u = t;
        u.replace(u.find("l0=24"), 5, "l0=26");
        CHECK(!PipeSpec::parse(u, &q));                       // stride not a multiple of 4
        u = t;
        u.replace(u.find("a3=9,12,0"), 9, "a3=9,12,2");
        CHECK(!PipeSpec::parse(u, &q));                       // attribute in a buffer without a layout
        u = t;
        u.replace(3, 1, "A");
        CHECK(!PipeSpec::parse(u, &q));                       // key not lowercase hex
        u = t;
        u.replace(u.find("c1=115,8,0"), 10, "c1=115,8,0,1");
        CHECK(!PipeSpec::parse(u, &q));
        u = t;
        u.replace(u.find("c1=115,8,0,0"), 12, "c1=115,8,0,3");
        CHECK(!PipeSpec::parse(u, &q));                       // blend factors without blending
        CHECK(!PipeSpec::parse("", &q));
        CHECK(!PipeSpec::parse("p1 ", &q));
        CHECK(!PipeSpec::parse(std::string(3000, 'p'), &q));
        u = t;
        u.replace(u.find(" d260"), 5, " d0260");
        CHECK(!PipeSpec::parse(u, &q));                       // not canonical
    }
    {   // no attachment at all is not a pipeline the renderer creates
        PipeSpec p = sample();
        p.depth = 0;
        p.color[0] = {};
        p.color[1] = {};
        CHECK(!p.valid());
        p.depth = 260;
        CHECK(p.valid());   // depth-only (shadow map) pass
    }
    std::printf("pipeline_warm_spec_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
