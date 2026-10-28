// Test70: the recorded identity of one render pipeline for the shader warm-up (d3d9_metal.mm, class
// ShaderWarm), kept free of Metal so its text form and bounds can be tested without a device
// (tests/pipeline_warm_spec_tests.cpp).
//
// Test66/69 left one residual first-use hold: entering the main menu on a cold system Metal cache
// (first launch of a new bundle, earliest skips) held one frame 103 ms creating 12 pipelines, with
// every library already compiled by the warm-up. The warm-up covered libraries only; a pipeline's
// device compile happens when the pipeline state is created. The draw path now records each
// pipeline it creates as one line of text naming both recorded libraries and every descriptor field
// the renderer sets (sample count, attachment formats, write masks, blend state, depth format and
// the vertex layout), and the background worker creates the recorded pipelines after the libraries.
// The draw path takes a finished pipeline whose line is identical, so a different variant (another
// sample count, write mask or blend state) never matches and is created as before.
//
// One line: "p1 <vertex key> <fragment key> s<samples> d<depth format>" then, in index order,
// " c<i>=<format>,<write mask>,<blend>,<src rgb>,<dst rgb>,<rgb op>,<src a>,<dst a>,<a op>" for each
// colour attachment, " a<reg>=<format>,<offset>,<buffer>" for each vertex attribute and
// " l<buffer>=<stride>,<step function>,<step rate>" for each vertex buffer layout. Blend fields are
// 0 when blending is off. Values are the Metal enumerations' numbers; the renderer additionally
// checks pixel formats against the ones it creates.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

struct PipeSpec {
    struct Color {
        uint32_t format = 0, mask = 0, blend = 0, src_rgb = 0, dst_rgb = 0, rgb_op = 0, src_a = 0, dst_a = 0,
                 a_op = 0;
    };
    struct Attr {
        uint32_t format = 0, offset = 0, buffer = 0;
    };
    struct Layout {
        uint32_t stride = 0, step = 0, rate = 0;
    };
    std::string vkey, fkey;   // 64 lowercase hex digits each (the libraries' warm-up keys)
    uint32_t samples = 1, depth = 0;
    Color color[4];      // format 0 = attachment unused
    Attr attr[16];       // format 0 = attribute unused
    Layout layout[9];    // stride 0 = layout unused (buffer 8 is the renderer's constant zero buffer)

    static bool hex64(const std::string &s) {
        if (s.size() != 64)
            return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        return true;
    }
    // Bounds every value must meet (the renderer only ever records values inside them).
    bool valid() const {
        if (!hex64(vkey) || !hex64(fkey))
            return false;
        if (samples != 1 && samples != 2 && samples != 4 && samples != 8)
            return false;
        if (depth > 1000)
            return false;
        bool any_color = false;
        for (const Color &c : color) {
            if (!c.format) {
                if (c.mask || c.blend || c.src_rgb || c.dst_rgb || c.rgb_op || c.src_a || c.dst_a || c.a_op)
                    return false;
                continue;
            }
            any_color = true;
            if (c.format > 1000 || c.mask > 15 || c.blend > 1)
                return false;
            if (c.src_rgb > 18 || c.dst_rgb > 18 || c.src_a > 18 || c.dst_a > 18 || c.rgb_op > 4 || c.a_op > 4)
                return false;
            if (!c.blend && (c.src_rgb || c.dst_rgb || c.rgb_op || c.src_a || c.dst_a || c.a_op))
                return false;
        }
        if (!any_color && !depth)
            return false;
        for (const Attr &a : attr) {
            if (!a.format) {
                if (a.offset || a.buffer)
                    return false;
                continue;
            }
            if (a.format > 64 || a.offset >= 4096 || a.buffer > 8 || !layout[a.buffer].stride)
                return false;
        }
        for (const Layout &l : layout) {
            if (!l.stride) {
                if (l.step || l.rate)
                    return false;
                continue;
            }
            if (l.stride > 4096 || l.stride % 4 || l.step > 4 || l.rate > 1)
                return false;
        }
        return true;
    }
    std::string text() const {
        std::string s = "p1 " + vkey + " " + fkey;
        char b[160];
        snprintf(b, sizeof b, " s%u d%u", samples, depth);
        s += b;
        for (int i = 0; i < 4; ++i)
            if (const Color &c = color[i]; c.format) {
                snprintf(b, sizeof b, " c%d=%u,%u,%u,%u,%u,%u,%u,%u,%u", i, c.format, c.mask, c.blend, c.src_rgb,
                         c.dst_rgb, c.rgb_op, c.src_a, c.dst_a, c.a_op);
                s += b;
            }
        for (int i = 0; i < 16; ++i)
            if (const Attr &a = attr[i]; a.format) {
                snprintf(b, sizeof b, " a%d=%u,%u,%u", i, a.format, a.offset, a.buffer);
                s += b;
            }
        for (int i = 0; i < 9; ++i)
            if (const Layout &l = layout[i]; l.stride) {
                snprintf(b, sizeof b, " l%d=%u,%u,%u", i, l.stride, l.step, l.rate);
                s += b;
            }
        return s;
    }
    // Parses one line (without its newline). Accepts only the exact canonical text of a valid spec:
    // anything reordered, repeated, padded or out of bounds is refused.
    static bool parse(const std::string &line, PipeSpec *out) {
        if (line.size() > 2048 || line.compare(0, 3, "p1 ") != 0)
            return false;
        PipeSpec p;
        size_t pos = 3;
        auto token = [&](std::string *t) {
            if (pos >= line.size())
                return false;
            size_t e = line.find(' ', pos);
            if (e == std::string::npos)
                e = line.size();
            *t = line.substr(pos, e - pos);
            pos = e + (e < line.size() ? 1 : 0);
            return !t->empty();
        };
        auto nums = [](const std::string &t, size_t from, uint32_t *v, int n) {
            const char *c = t.c_str() + from;
            for (int i = 0; i < n; ++i) {
                if (*c < '0' || *c > '9')
                    return false;
                char *end = nullptr;
                unsigned long x = strtoul(c, &end, 10);
                if (x > 1000000ul || end == c)
                    return false;
                v[i] = (uint32_t)x;
                c = end;
                if (i + 1 < n) {
                    if (*c != ',')
                        return false;
                    ++c;
                }
            }
            return *c == 0;
        };
        std::string t;
        if (!token(&p.vkey) || !token(&p.fkey))
            return false;
        uint32_t v[9];
        if (!token(&t) || t[0] != 's' || !nums(t, 1, v, 1))
            return false;
        p.samples = v[0];
        if (!token(&t) || t[0] != 'd' || !nums(t, 1, v, 1))
            return false;
        p.depth = v[0];
        while (token(&t)) {
            size_t eq = t.find('=');
            if (eq == std::string::npos || eq < 2 || eq > 3)
                return false;
            uint32_t idx[1];
            if (!nums(t.substr(0, eq), 1, idx, 1))
                return false;
            const std::string rest = t.substr(eq + 1);
            if (t[0] == 'c' && idx[0] < 4) {
                if (!nums(rest, 0, v, 9))
                    return false;
                p.color[idx[0]] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]};
            } else if (t[0] == 'a' && idx[0] < 16) {
                if (!nums(rest, 0, v, 3))
                    return false;
                p.attr[idx[0]] = {v[0], v[1], v[2]};
            } else if (t[0] == 'l' && idx[0] < 9) {
                if (!nums(rest, 0, v, 3))
                    return false;
                p.layout[idx[0]] = {v[0], v[1], v[2]};
            } else {
                return false;
            }
        }
        if (!p.valid() || p.text() != line)
            return false;
        *out = p;
        return true;
    }
};
