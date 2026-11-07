/* paint_fast.h - the skin compositor's hot pixel loops in native code (included by nfsc.c after paint.h).
 *
 * A friend's car with a vinyl gets its skin from the game's software compositor (0xb76860, steps of
 * 007dba00; logs/paint-slots-20261105). A CPU profile of the game thread while 48 cars were painted
 * (logs/paint-speed-20261105/runs/base-3) put ~80% of the compositor's time in five canvas routines,
 * translated x86 with an x87 float-to-int call (_ftol2 0087eac0, dispatched as a hook) per colour channel:
 *
 *   007b4b50  canvas vtable +0x20  blend a colour through a 2x2-supersampled coverage mask
 *   007b4ef0  canvas vtable +0x18  blend a colour through a float alpha image
 *   007b5100  canvas vtable +0x28  blend a colour through a float image ramp, inside a full mask
 *   007b6a40  thiscall float image  recursive Gaussian blur (Young / van Vliet), rows then columns
 *   007b68d0  thiscall float image  clear to zero
 *
 * Each is replaced here (REPLACE hooks) by C that performs the same x87 operations in the same order
 * on doubles, as the runtime's own x87 does (kit/runtime/x86.h: registers are doubles, fx87 rounds to
 * single only under precision control 00, NaN results become the x87 indefinite and raise IE, FCOM
 * sets C3 C2 C0, FST float rounds by RC), and leaves the guest registers, flags and FPU status as the
 * original's RET does. Only the stack scratch below ESP is not written. When a call's state is outside
 * what was checked (rounding mode not nearest, precision single, a rectangle outside its canvas, guest
 * memory not where expected) the original runs instead.
 *
 * 007b4ef0 is not a listed function start, so the runtime will not hook it; the canvas vtable's slot
 * +0x18 (0x009f17f8, .rdata) is pointed at 007b4d80 (slot +0x1c, hookable), and that hook tells the two
 * apart by the call instruction before its return address (FF 5x 18 = call [reg+0x18]). The slot is
 * restored at shutdown. Nothing else in the image refers to 007b4d80 or 007b4ef0 (searched 2026-11-05).
 *
 * PAINT_FAST_VERIFY (test build): every call runs the native code first (into memory that is then put
 * back), then the original, and compares the bytes it may write and every register, flag and FPU field;
 * the original's result is kept. Mismatches and a running count are logged ("paint fast verify").
 * PAINT_FAST_STATS (diagnostic): times every call of the compositor's canvas functions.
 * RECOMP_PAINT_FAST=0 (test): no native code (the game's own loops), for before/after runs. */

#ifndef PAINT_FAST_STATS
#define PAINT_FAST_STATS 0
#endif
#ifndef PAINT_FAST_VERIFY
#define PAINT_FAST_VERIFY 0
#endif

#define PF_BLEND_MASK 0x007b4b50u
#define PF_BLEND_ALPHA 0x007b4ef0u
#define PF_FILL_MASK 0x007b4d80u
#define PF_BLEND_RAMP 0x007b5100u
#define PF_BLUR 0x007b6a40u
#define PF_CLEAR 0x007b68d0u
#define PF_VT_ALPHA 0x009f17f8u /* canvas vtable 0x009f17e0 + 0x18 = 007b4ef0 */

typedef struct PfFpu {
    uint16_t sw;
} PfFpu;

static inline double pf_indef(void) {
    uint64_t bits = 0xfff8000000000000ull;
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
/* fx87 under PC 53/64-bit (the only precision the natives run under; see pf_cw_ok) */
static inline double pf_r(PfFpu *f, double r) {
    if (__builtin_expect(r != r, 0)) {
        f->sw |= 0x0001u;
        return pf_indef();
    }
    return r;
}
/* fdivz */
static inline double pf_div(PfFpu *f, double a, double b) {
    if (b == 0.0 && a == a && !isinf(a) && a != 0.0)
        f->sw |= 0x0004u;
    return pf_r(f, a / b);
}
/* FCOM (any NaN raises IE) */
static inline void pf_fcom(PfFpu *f, double a, double b) {
    uint16_t sw = (uint16_t)(f->sw & (uint16_t)~0x4700u);
    if (isnan(a) || isnan(b))
        sw |= 0x4500u | 0x0001u;
    else if (a < b)
        sw |= 0x0100u;
    else if (a == b)
        sw |= 0x4000u;
    f->sw = sw;
}
/* FST/FSTP float under RC nearest */
static inline float pf_f(double v) { return (float)v; }
/* _ftol2 0087eac0 as source/native/ftol.cpp performs it (RC nearest): the value truncated toward zero;
 * NaN or out of range: IE and 0 in EAX. Returns EAX. */
static inline uint32_t pf_ftol(PfFpu *f, double v) {
    if (__builtin_expect(fabs(v) < 4503599627370496.0, 1))
        return (uint32_t)(uint64_t)(int64_t)v;
    if (v != v || !(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) {
        f->sw |= 0x0001u;
        return 0;
    }
    return (uint32_t)(uint64_t)(int64_t)v; /* an integer already */
}
static inline uint32_t pf_parity8(uint32_t v) {
    v &= 0xffu;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (~v) & 1u;
}
static void pf_flags_add(pop_cpu_v1 *c, uint32_t a, uint32_t b) {
    const uint64_t rf = (uint64_t)a + b;
    const uint32_t r = (uint32_t)rf;
    c->cf = (uint32_t)(rf >> 32) & 1u;
    c->of = ((~(a ^ b) & (a ^ r)) >> 31) & 1u;
    c->af = ((a ^ b ^ r) >> 4) & 1u;
    c->zf = r == 0;
    c->sf = r >> 31;
    c->pf = pf_parity8(r);
}
static void pf_flags_sub(pop_cpu_v1 *c, uint32_t a, uint32_t b) {
    const uint64_t rf = (uint64_t)a - b;
    const uint32_t r = (uint32_t)rf;
    c->cf = (uint32_t)(rf >> 32) & 1u;
    c->of = (((a ^ b) & (a ^ r)) >> 31) & 1u;
    c->af = ((a ^ b ^ r) >> 4) & 1u;
    c->zf = r == 0;
    c->sf = r >> 31;
    c->pf = pf_parity8(r);
}
static inline int pf_cw_ok(const pop_cpu_v1 *c) {
    return ((c->fpu_cw >> 8) & 3u) != 0u && ((c->fpu_cw >> 10) & 3u) == 0u;
}
static inline uint32_t pf_arg(const pop_cpu_v1 *c, unsigned k) { return get_u32(c->esp + 4u + 4u * k); }
static void *pf_mem(uint32_t addr, uint32_t len) {
    void *p = NULL;
    if (!len || g_api->guest_ptr(g_api, addr, len, &p) != POP_OK)
        return NULL;
    return p;
}
static inline int32_t pf_i32(const void *p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline float pf_ldf(const uint8_t *p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}
static inline void pf_stf(uint8_t *p, float v) { memcpy(p, &v, 4); }

/* What the native code of one call writes, for the verifier: a byte range of guest memory. */
typedef struct PfSpan {
    uint32_t addr, len;
} PfSpan;

/* ---- canvas (ARGB): +4 pixels, +8 width, +0xc height, +0x10 pitch (pixels); mask (1 bit per
 * sample, 2x2 samples a pixel): +0 words, +0xc words a row; float image: +0 data, +4 width, +8 height,
 * +0xc pitch (floats). Rectangle: x0, y0, x1, y1 (end exclusive). */
typedef struct PfCanvas {
    uint32_t pix;
    int32_t w, h, pitch;
    uint8_t *p;
} PfCanvas;
typedef struct PfFloats {
    uint32_t data;
    int32_t w, h, pitch;
    const uint8_t *p;
} PfFloats;
typedef struct PfMask {
    uint32_t words;
    int32_t pitch;
    const uint8_t *p;
} PfMask;
typedef struct PfRect {
    int32_t x0, y0, x1, y1;
} PfRect;

static int pf_rect(uint32_t a, PfRect *r) {
    const int32_t *p = (const int32_t *)pf_mem(a, 16);
    if (!p)
        return 0;
    r->x0 = pf_i32(p);
    r->y0 = pf_i32(p + 1);
    r->x1 = pf_i32(p + 2);
    r->y1 = pf_i32(p + 3);
    return 1;
}
/* The canvas, with its rows y0..y1-1 (all the pixels the blends may touch) mapped; 0 when the
 * rectangle is not inside the canvas (then the original runs). */
static int pf_canvas(uint32_t self, const PfRect *r, PfCanvas *cv, PfSpan *span) {
    const uint8_t *s = (const uint8_t *)pf_mem(self, 0x14);
    if (!s)
        return 0;
    cv->pix = (uint32_t)pf_i32(s + 4);
    cv->w = pf_i32(s + 8);
    cv->h = pf_i32(s + 0xc);
    cv->pitch = pf_i32(s + 0x10);
    if (cv->w <= 0 || cv->h <= 0 || cv->pitch < cv->w || cv->w > 8192 || cv->h > 8192 || cv->pitch > 8192)
        return 0;
    if (r->y0 >= r->y1 || r->x0 >= r->x1) { /* nothing is written */
        cv->p = NULL;
        span->addr = span->len = 0;
        return 1;
    }
    if (r->x0 < 0 || r->y0 < 0 || r->x1 > cv->w || r->y1 > cv->h)
        return 0;
    span->addr = cv->pix + 4u * (uint32_t)cv->pitch * (uint32_t)r->y0;
    span->len = 4u * (uint32_t)cv->pitch * (uint32_t)(r->y1 - r->y0);
    uint8_t *rows = (uint8_t *)pf_mem(span->addr, span->len);
    if (!rows)
        return 0;
    cv->p = rows - 4u * (uint32_t)cv->pitch * (uint32_t)r->y0; /* indexed by the canvas's own y */
    return 1;
}
static int pf_floats(uint32_t self, PfFloats *f) {
    const uint8_t *s = (const uint8_t *)pf_mem(self, 0x10);
    if (!s)
        return 0;
    f->data = (uint32_t)pf_i32(s);
    f->w = pf_i32(s + 4);
    f->h = pf_i32(s + 8);
    f->pitch = pf_i32(s + 0xc);
    if (f->w < 0 || f->h < 0 || f->pitch < f->w || f->w > 8192 || f->h > 8192 || f->pitch > 8192)
        return 0;
    f->p = NULL;
    if (f->w && f->h) {
        f->p = (const uint8_t *)pf_mem(f->data, 4u * (uint32_t)f->pitch * (uint32_t)f->h);
        if (!f->p)
            return 0;
    }
    return 1;
}
/* The float image's value at (ax, ay) as the blends read it: 0.0 outside (the checks are signed). */
static inline double pf_sample(const PfFloats *f, int32_t ax, int32_t ay) {
    if (ax < 0 || ay < 0 || ax >= f->w || ay >= f->h)
        return 0.0;
    return (double)pf_ldf(f->p + 4u * ((uint32_t)f->pitch * (uint32_t)ay + (uint32_t)ax));
}
static int pf_mask(uint32_t self, const PfRect *r, PfMask *m) {
    const uint8_t *s = (const uint8_t *)pf_mem(self, 0x10);
    if (!s)
        return 0;
    m->words = (uint32_t)pf_i32(s);
    m->pitch = pf_i32(s + 0xc);
    if (r->y0 >= r->y1 || r->x0 >= r->x1) {
        m->p = NULL;
        return 1;
    }
    if (m->pitch <= 0 || m->pitch > 4096)
        return 0;
    const uint32_t last = (uint32_t)(2 * (r->y1 - 1) + 1) * (uint32_t)m->pitch + ((uint32_t)(2 * (r->x1 - 1)) >> 5);
    m->p = (const uint8_t *)pf_mem(m->words, 4u * (last + 1u));
    return m->p != NULL;
}
/* The 2x2 samples of pixel (x, y): bits 2x, 2x+1 of rows 2y, 2y+1 (SAR by 2x & 31; bits 0/1 only). */
static inline void pf_bits(const PfMask *m, int32_t x, int32_t y, uint32_t *w0, uint32_t *w1) {
    const uint32_t row = (uint32_t)(2 * y) * (uint32_t)m->pitch, word = (uint32_t)(2 * x) >> 5, sh = (uint32_t)(2 * x) & 31u;
    *w0 = (uint32_t)pf_i32(m->p + 4u * (row + word)) >> sh;
    *w1 = (uint32_t)pf_i32(m->p + 4u * (row + (uint32_t)m->pitch + word)) >> sh;
}

/* One pixel's colour blend, the four channels in the game's order (R +2, G +1, B +0, A +3):
 * new = ftol(d * inv + cf) with cf = float(colour * k) for R, G, B and ak = k * A kept in a register. */
typedef struct PfBlend {
    float inv, rc, gc, bc;
    double ak;
} PfBlend;
static inline void pf_px(PfFpu *f, uint8_t *px, const PfBlend *b) {
    const double inv = (double)b->inv;
    px[2] = (uint8_t)pf_ftol(f, pf_r(f, pf_r(f, (double)px[2] * inv) + (double)b->rc));
    px[1] = (uint8_t)pf_ftol(f, pf_r(f, pf_r(f, (double)px[1] * inv) + (double)b->gc));
    px[0] = (uint8_t)pf_ftol(f, pf_r(f, pf_r(f, (double)px[0] * inv) + (double)b->bc));
    px[3] = (uint8_t)pf_ftol(f, pf_r(f, pf_r(f, (double)px[3] * inv) + b->ak));
}

/* ---- 007b4b50: thiscall canvas (mask, colour, rect, mirror), RET 0x10 ----
 * For each pixel of rect (rows, then x ascending): k = 64 * (samples set) * 0.00390625; inv = float(1 - k);
 * blend; with mirror also the pixel at width - 1 - x of the same row, right after. k = 0 leaves the
 * pixel as it is (d * 1 + 0), so those pixels are skipped. */
static int pf_blend_mask(pop_cpu_v1 *c, PfSpan *span) {
    if (!pf_cw_ok(c))
        return 0;
    const uint32_t self = c->ecx, mask = pf_arg(c, 0), colour = pf_arg(c, 1), rect = pf_arg(c, 2);
    const uint8_t mirror = (uint8_t)pf_arg(c, 3);
    PfRect r;
    PfCanvas cv;
    PfMask m;
    if (!pf_rect(rect, &r) || !pf_canvas(self, &r, &cv, span) || !pf_mask(mask, &r, &m))
        return 0;
    PfFpu f = {c->fpu_sw};
    const double R = (double)((colour >> 16) & 0xffu), G = (double)((colour >> 8) & 0xffu), B = (double)(colour & 0xffu),
                 A = (double)(colour >> 24);
    PfBlend bl[5];
    for (int n = 1; n <= 4; ++n) { /* 64 * n * 0.00390625, as FILD * FMUL float */
        const double k = pf_r(&f, (double)(64 * n) * (double)0.00390625f);
        bl[n].inv = pf_f(pf_r(&f, 1.0 - k));
        bl[n].rc = pf_f(pf_r(&f, R * k));
        bl[n].gc = pf_f(pf_r(&f, G * k));
        bl[n].bc = pf_f(pf_r(&f, B * k));
        bl[n].ak = pf_r(&f, k * A);
    }
    for (int32_t y = r.y0; y < r.y1 && r.x0 < r.x1; ++y) {
        uint8_t *row = cv.p + 4u * (uint32_t)cv.pitch * (uint32_t)y;
        for (int32_t x = r.x0; x < r.x1; ++x) {
            uint32_t w0, w1;
            pf_bits(&m, x, y, &w0, &w1);
            const unsigned n = (w0 & 1u) + ((w0 >> 1) & 1u) + (w1 & 1u) + ((w1 >> 1) & 1u);
            if (!n)
                continue;
            pf_px(&f, row + 4u * (uint32_t)x, &bl[n]);
            if (mirror)
                pf_px(&f, row + 4u * (uint32_t)(cv.w - 1 - x), &bl[n]);
        }
    }
    /* what the RET leaves: EAX rect, ECX the last row (y1, or y0 when empty), EDX y1 */
    c->eax = rect;
    c->ecx = (uint32_t)(r.y0 < r.y1 ? r.y1 : r.y0);
    c->edx = (uint32_t)r.y1;
    pf_flags_add(c, c->esp - 0x1cu, 0x1cu);
    c->fpu_sw = f.sw;
    return 1;
}

/* ---- 007b4ef0: thiscall canvas (floats, colour, dx, dy, rect, mirror), RET 0x18 ----
 * As 007b4b50 with k = the float image at (x + dx, y + dy) (0.0 outside it), used unrounded. */
static int pf_blend_alpha(pop_cpu_v1 *c, PfSpan *span) {
    if (!pf_cw_ok(c))
        return 0;
    const uint32_t self = c->ecx, img = pf_arg(c, 0), colour = pf_arg(c, 1), rect = pf_arg(c, 4);
    const int32_t dx = (int32_t)pf_arg(c, 2), dy = (int32_t)pf_arg(c, 3);
    const uint8_t mirror = (uint8_t)pf_arg(c, 5);
    PfRect r;
    PfCanvas cv;
    PfFloats fl;
    if (!pf_rect(rect, &r) || !pf_canvas(self, &r, &cv, span) || !pf_floats(img, &fl))
        return 0;
    PfFpu f = {c->fpu_sw};
    const double R = (double)((colour >> 16) & 0xffu), G = (double)((colour >> 8) & 0xffu), B = (double)(colour & 0xffu),
                 A = (double)(colour >> 24);
    for (int32_t y = r.y0; y < r.y1 && r.x0 < r.x1; ++y) {
        uint8_t *row = cv.p + 4u * (uint32_t)cv.pitch * (uint32_t)y;
        const int32_t ay = (int32_t)((uint32_t)y + (uint32_t)dy);
        for (int32_t x = r.x0; x < r.x1; ++x) {
            const int32_t ax = (int32_t)((uint32_t)x + (uint32_t)dx);
            const double k = pf_sample(&fl, ax, ay);
            if (k == 0.0)
                continue; /* d * 1 + 0: unchanged */
            PfBlend b;
            b.inv = pf_f(pf_r(&f, 1.0 - k));
            b.rc = pf_f(pf_r(&f, R * k));
            b.gc = pf_f(pf_r(&f, G * k));
            b.bc = pf_f(pf_r(&f, B * k));
            b.ak = pf_r(&f, k * A);
            pf_px(&f, row + 4u * (uint32_t)x, &b);
            if (mirror)
                pf_px(&f, row + 4u * (uint32_t)(cv.w - 1 - x), &b);
        }
    }
    c->eax = rect;
    if (r.y0 < r.y1) {
        c->ecx = (uint32_t)r.y1;
        if (r.x0 < r.x1)
            c->edx = rect;
    }
    pf_flags_add(c, c->esp - 0x18u, 0x18u);
    c->fpu_sw = f.sw;
    return 1;
}

/* ---- 007b5100: thiscall canvas (floats, dx, dy, lo, hi, invert, colour, mask, rect, mirror), RET 0x28 ----
 * d = lo - hi; s = |d| < 1e-4 ? 1e12 : 1 / (float(d) * float(d)). For each pixel whose 4 samples are
 * all set: u = image(x + dx, y + dy) - lo; w = float((|u| * u + df * df) * s), clamped to [0, 1]
 * (w <= 0 or NaN: 0); invert: 1 - w; k = w * float(A * (1/255)); then the blend with A as a float. */
static int pf_blend_ramp(pop_cpu_v1 *c, PfSpan *span) {
    if (!pf_cw_ok(c))
        return 0;
    const uint32_t self = c->ecx, img = pf_arg(c, 0), colour = pf_arg(c, 6), mask = pf_arg(c, 7), rect = pf_arg(c, 8);
    const int32_t dx = (int32_t)pf_arg(c, 1), dy = (int32_t)pf_arg(c, 2);
    const uint32_t lo_bits = pf_arg(c, 3), hi_bits = pf_arg(c, 4);
    const uint8_t invert = (uint8_t)pf_arg(c, 5), mirror = (uint8_t)pf_arg(c, 9);
    float lo, hi;
    memcpy(&lo, &lo_bits, 4);
    memcpy(&hi, &hi_bits, 4);
    PfRect r;
    PfCanvas cv;
    PfFloats fl;
    PfMask m;
    if (!pf_rect(rect, &r) || !pf_canvas(self, &r, &cv, span) || !pf_floats(img, &fl) || !pf_mask(mask, &r, &m))
        return 0;
    PfFpu f = {c->fpu_sw};
    const double A = (double)(colour >> 24);
    const float Af = pf_f(A);
    const float aK = pf_f(pf_r(&f, A * (double)0.003921568859368563f));
    const double d = pf_r(&f, (double)lo - (double)hi);
    const float df = pf_f(d);
    pf_fcom(&f, fabs(d), (double)9.999999747378752e-05f);
    double s;
    {
        const uint32_t ah = f.sw >> 8;
        if (pf_parity8(ah & 5u) == 0u) /* JP not taken: |d| < eps */
            s = (double)999999995904.0f;
        else
            s = pf_div(&f, 1.0, pf_r(&f, (double)df * (double)df));
    }
    const double R = (double)((colour >> 16) & 0xffu), G = (double)((colour >> 8) & 0xffu), B = (double)(colour & 0xffu);
    for (int32_t y = r.y0; y < r.y1 && r.x0 < r.x1; ++y) {
        uint8_t *row = cv.p + 4u * (uint32_t)cv.pitch * (uint32_t)y;
        const int32_t ay = (int32_t)((uint32_t)dy + (uint32_t)y);
        for (int32_t x = r.x0; x < r.x1; ++x) {
            uint32_t w0, w1;
            pf_bits(&m, x, y, &w0, &w1);
            if (!(w0 & w1 & (w0 >> 1) & (w1 >> 1) & 1u))
                continue;
            const int32_t ax = (int32_t)((uint32_t)x + (uint32_t)dx);
            const double u = pf_r(&f, pf_sample(&fl, ax, ay) - (double)lo);
            const double uu = pf_r(&f, fabs(u) * u);
            const double dd = pf_r(&f, (double)df * (double)df);
            const float wf = pf_f(pf_r(&f, pf_r(&f, uu + dd) * s));
            double w;
            pf_fcom(&f, (double)wf, 0.0);
            if (f.sw & 0x4100u) /* C0 or C3: w < 0, w == 0, NaN */
                w = 0.0;
            else {
                pf_fcom(&f, 1.0, (double)wf);
                w = pf_parity8((f.sw >> 8) & 5u) ? (double)wf : 1.0; /* JP taken: 1 >= w */
            }
            if (invert)
                w = pf_r(&f, 1.0 - (double)pf_f(w));
            const double k = pf_r(&f, w * (double)aK);
            PfBlend b;
            b.inv = pf_f(pf_r(&f, 1.0 - k));
            b.rc = pf_f(pf_r(&f, R * k));
            b.gc = pf_f(pf_r(&f, G * k));
            b.bc = pf_f(pf_r(&f, B * k));
            b.ak = pf_r(&f, k * (double)Af);
            pf_px(&f, row + 4u * (uint32_t)x, &b);
            if (mirror)
                pf_px(&f, row + 4u * (uint32_t)(cv.w - 1 - x), &b);
        }
    }
    c->eax = rect;
    c->ecx = (uint32_t)(r.y0 < r.y1 ? r.y1 : r.y0);
    if (r.y0 < r.y1)
        c->edx = (uint32_t)r.y1;
    pf_flags_add(c, c->esp - 0x28u, 0x28u);
    c->fpu_sw = f.sw;
    return 1;
}

/* ---- 007b6a40: thiscall floats (sigma, rect), RET 8: recursive Gaussian blur ----
 * Coefficients as 007b6a40..6b3f computes them; the area is the rect clamped to 4 from every edge.
 * Rows: forward x ascending, then backward x descending (y = B x + binv ((b3 p3 + b2 p2) + b1 p1),
 * the additions grouped as the game's 4-unrolled loops group them: element 0 of a block and the
 * remainder (b3 p3 + b2 p2) + b1 p1, elements 1-3 (b3 p3 + b1 p1) + b2 p2). Then columns, each forward
 * then backward, (b3 p3 + b2 p2) + b1 p1. */
typedef struct PfIir {
    float b1, b2, b3, binv, B;
} PfIir;
static inline float pf_iir(PfFpu *f, const PfIir *k, double x, float p3, float p2, float p1, int grouping) {
    double s;
    if (!grouping)
        s = pf_r(f, pf_r(f, pf_r(f, (double)p3 * k->b3) + pf_r(f, (double)p2 * k->b2)) + pf_r(f, (double)p1 * k->b1));
    else
        s = pf_r(f, pf_r(f, pf_r(f, (double)p3 * k->b3) + pf_r(f, (double)p1 * k->b1)) + pf_r(f, (double)p2 * k->b2));
    s = pf_r(f, s * (double)k->binv);
    return pf_f(pf_r(f, s + pf_r(f, (double)k->B * x)));
}
/* One run of the filter over n values at p, step `step` floats (negative: backward), with the three
 * values before the run (p3 oldest). blocks: 1 for the rows' 4-unrolled loop, 0 for the columns. */
static void pf_iir_run(PfFpu *f, const PfIir *k, uint8_t *p, int32_t n, int32_t step, float p3, float p2, float p1, int blocks) {
    int32_t i = 0;
    if (blocks && n >= 4) {
        for (;;) {
            for (int e = 0; e < 4; ++e) {
                uint8_t *q = p + 4 * (intptr_t)step * (i + e);
                const float y = pf_iir(f, k, (double)pf_ldf(q), p3, p2, p1, e != 0);
                pf_stf(q, y);
                p3 = pf_f((double)p2);
                p2 = p1;
                p1 = y;
            }
            i += 4;
            if (!(i + 3 < n))
                break;
        }
    }
    for (; i < n; ++i) {
        uint8_t *q = p + 4 * (intptr_t)step * i;
        const float y = pf_iir(f, k, (double)pf_ldf(q), p3, p2, p1, 0);
        pf_stf(q, y);
        p3 = pf_f((double)p2);
        p2 = p1;
        p1 = y;
    }
}
static int pf_blur(pop_cpu_v1 *c, PfSpan *span) {
    if (!pf_cw_ok(c))
        return 0;
    const uint32_t self = c->ecx, rect = pf_arg(c, 1), sig_bits = pf_arg(c, 0);
    float sigma;
    memcpy(&sigma, &sig_bits, 4);
    PfRect r;
    PfFloats fl;
    if (!pf_rect(rect, &r) || !pf_floats(self, &fl))
        return 0;
    PfFpu f = {c->fpu_sw};
    /* q */
    pf_fcom(&f, (double)sigma, 2.5);
    double q;
    if (f.sw & 0x0100u) { /* sigma < 2.5 (or NaN) */
        q = pf_r(&f, (double)sigma * (double)0.2689099907875061f);
        q = pf_r(&f, 1.0 - q);
        q = pf_r(&f, sqrt(q));
        q = pf_r(&f, q * (double)4.145540237426758f);
        q = pf_r(&f, (double)3.971560001373291f - q);
    } else {
        q = pf_r(&f, (double)sigma * (double)0.9871100187301636f);
        q = pf_r(&f, q - (double)0.9632999897003174f);
    }
    const double q2 = pf_r(&f, q * q);
    const float q2f = pf_f(q2);
    const double q3 = pf_r(&f, q2 * q);
    const float a = pf_f(pf_r(&f, q * (double)2.4441299438476562f));
    PfIir k;
    k.b1 = pf_f(pf_r(&f, pf_r(&f, pf_r(&f, (double)1.266610026359558f * q3) + pf_r(&f, (double)q2f * (double)2.8561899662017822f)) + (double)a));
    const float z = pf_f(pf_r(&f, q * 0.0));
    k.b2 = pf_f(pf_r(&f, pf_r(&f, pf_r(&f, (double)q2f * (double)-1.4280999898910522f) - pf_r(&f, q3 * (double)1.266610026359558f)) + (double)z));
    const double q3k = pf_r(&f, q3 * (double)0.42220500111579895f);
    k.b3 = pf_f(pf_r(&f, pf_r(&f, pf_r(&f, (double)q2f * 0.0) + q3k) + (double)z));
    {
        double t = pf_r(&f, (double)q2f * (double)1.4280999898910522f);
        t = pf_r(&f, t + q3k);
        t = pf_r(&f, t + (double)a);
        t = pf_r(&f, t + (double)1.5782500505447388f);
        k.binv = pf_f(pf_div(&f, 1.0, t));
    }
    {
        double t = pf_r(&f, (double)k.b3 + (double)k.b2);
        t = pf_r(&f, t + (double)k.b1);
        t = pf_r(&f, t * (double)k.binv);
        k.B = pf_f(pf_r(&f, 1.0 - t));
    }
    const int32_t xs = r.x0 > 4 ? r.x0 : 4, xe = r.x1 < fl.w - 4 ? r.x1 : fl.w - 4;
    const int32_t ys = r.y0 > 4 ? r.y0 : 4, ye = r.y1 < fl.h - 4 ? r.y1 : fl.h - 4;
    /* the bytes the passes may write (and read: 3 past each end) */
    span->addr = fl.data;
    span->len = 4u * (uint32_t)fl.pitch * (uint32_t)fl.h;
    uint8_t *d = (uint8_t *)fl.p;
    if ((ys < ye && xs < xe) && (!d || xs < 3 || ys < 3 || xe + 3 > fl.w || ye + 3 > fl.h))
        return 0; /* (cannot happen with the clamps above; the original runs) */
    const uint32_t P = (uint32_t)fl.pitch;
#define PF_AT(x, y) (d + 4u * (P * (uint32_t)(y) + (uint32_t)(x)))
    if (ys < ye && xs < xe) {
        for (int32_t y = ys; y < ye; ++y) {
            pf_iir_run(&f, &k, PF_AT(xs, y), xe - xs, 1, pf_f((double)pf_ldf(PF_AT(xs - 3, y))), pf_ldf(PF_AT(xs - 2, y)),
                       pf_ldf(PF_AT(xs - 1, y)), 1);
            pf_iir_run(&f, &k, PF_AT(xe - 1, y), xe - xs, -1, pf_f((double)pf_ldf(PF_AT(xe + 2, y))), pf_ldf(PF_AT(xe + 1, y)),
                       pf_ldf(PF_AT(xe, y)), 1);
        }
    }
    if (xs < xe) {
        for (int32_t x = xs; x < xe; ++x) {
            if (ys < ye) {
                pf_iir_run(&f, &k, PF_AT(x, ys), ye - ys, (int32_t)P, pf_ldf(PF_AT(x, ys - 3)), pf_ldf(PF_AT(x, ys - 2)),
                           pf_ldf(PF_AT(x, ys - 1)), 0);
                pf_iir_run(&f, &k, PF_AT(x, ye - 1), ye - ys, -(int32_t)P, pf_ldf(PF_AT(x, ye + 2)), pf_ldf(PF_AT(x, ye + 1)),
                           pf_ldf(PF_AT(x, ye)), 0);
            }
        }
    }
#undef PF_AT
    /* what the RET leaves */
    if (xs < xe) {
        c->eax = (uint32_t)xe;
        c->edx = (uint32_t)xe;
        pf_flags_sub(c, (uint32_t)xe, (uint32_t)xe);
    } else {
        c->eax = (uint32_t)xs;
        c->edx = (uint32_t)(ys < ye ? ye : ys);
        pf_flags_sub(c, (uint32_t)xs, (uint32_t)xe);
    }
    c->fpu_sw = f.sw;
    return 1;
}

/* ---- 007b68d0: thiscall floats: REP STOSD / STOSB of w * h * 4 zero bytes; EAX 0, ECX 0, EDX the byte
 * count, EDI restored ---- */
static int pf_clear(pop_cpu_v1 *c, PfSpan *span) {
    const uint8_t *s = (const uint8_t *)pf_mem(c->ecx, 12);
    if (!s || c->df)
        return 0;
    const uint32_t data = (uint32_t)pf_i32(s), n = (uint32_t)pf_i32(s + 8) * (uint32_t)pf_i32(s + 4) * 4u;
    span->addr = data;
    span->len = n;
    if (n) {
        void *p = pf_mem(data, n);
        if (!p || n > 64u * 1024u * 1024u)
            return 0;
        memset(p, 0, n);
    }
    c->eax = 0;
    c->ecx = 0;
    c->edx = n;
    /* the flags of AND ECX,3 (AF as it was) */
    c->cf = c->of = c->sf = 0;
    c->zf = (n & 3u) == 0;
    c->pf = pf_parity8(n & 3u);
    return 1;
}

/* ---- the hooks ---- */
typedef int (*PfNative)(pop_cpu_v1 *c, PfSpan *span);
enum { PF_K_MASK, PF_K_ALPHA, PF_K_RAMP, PF_K_BLUR, PF_K_CLEAR, PF_KINDS };
static const struct {
    uint32_t addr, args;
    PfNative fn;
    const char *name;
} kPf[PF_KINDS] = {
    {PF_BLEND_MASK, 0x10, pf_blend_mask, "blend-mask 007b4b50"},
    {PF_BLEND_ALPHA, 0x18, pf_blend_alpha, "blend-alpha 007b4ef0"},
    {PF_BLEND_RAMP, 0x28, pf_blend_ramp, "blend-ramp 007b5100"},
    {PF_BLUR, 0x08, pf_blur, "blur 007b6a40"},
    {PF_CLEAR, 0x00, pf_clear, "clear 007b68d0"},
};
static uint32_t g_pf_hook[PF_KINDS + 1];
static uint32_t g_pf_native[PF_KINDS], g_pf_fallback[PF_KINDS];
static uint32_t g_pf_vt_old; /* the vtable slot's value before the redirect */
static int g_pf_on = -1;

#if PAINT_FAST_VERIFY
#define PF_VERIFY_NOTE " (VERIFY build: the game's own results are kept, the native ones compared)"
static uint32_t g_pf_checked[PF_KINDS], g_pf_bad[PF_KINDS];
static uint8_t *g_pf_save, *g_pf_mine;
static uint32_t g_pf_save_cap;
static void pf_verify_say(void) {
    char line[400];
    int at = snprintf(line, sizeof line, "core.nfsc: paint fast verify:");
    for (int i = 0; i < PF_KINDS; ++i)
        at += snprintf(line + at, sizeof line - at, " %s %u checked %u differ %u fallback;", kPf[i].name, g_pf_checked[i], g_pf_bad[i],
                       g_pf_fallback[i]);
    log_line(line);
}
/* The guest bytes a call of `kind` may write, from its arguments (as the native code finds them). */
static void pf_verify_region(const pop_cpu_v1 *cpu, int kind, uint32_t *a, uint32_t *len) {
    *a = *len = 0;
    if (kind == PF_K_BLUR || kind == PF_K_CLEAR) {
        const uint8_t *s = (const uint8_t *)pf_mem(cpu->ecx, 0x10);
        if (s) {
            *a = (uint32_t)pf_i32(s);
            *len = kind == PF_K_CLEAR ? (uint32_t)pf_i32(s + 4) * (uint32_t)pf_i32(s + 8) * 4u
                                      : 4u * (uint32_t)pf_i32(s + 0xc) * (uint32_t)pf_i32(s + 8);
        }
        return;
    }
    PfRect r;
    const uint32_t ra = kind == PF_K_MASK ? pf_arg(cpu, 2) : kind == PF_K_ALPHA ? pf_arg(cpu, 4) : pf_arg(cpu, 8);
    const uint8_t *s = (const uint8_t *)pf_mem(cpu->ecx, 0x14);
    if (s && pf_rect(ra, &r) && r.y1 > r.y0 && r.y0 >= 0) {
        *a = (uint32_t)pf_i32(s + 4) + 4u * (uint32_t)pf_i32(s + 0x10) * (uint32_t)r.y0;
        *len = 4u * (uint32_t)pf_i32(s + 0x10) * (uint32_t)(r.y1 - r.y0);
    }
}
#else
#define PF_VERIFY_NOTE ""
#endif

static void pf_run(const PopModApi *api, pop_cpu_v1 *cpu, int kind) {
    const uint32_t addr = kPf[kind].addr;
#if PAINT_FAST_VERIFY
    /* the native code first, on a copy of the CPU, then its bytes put back; then the original */
    uint32_t a, len;
    pf_verify_region(cpu, kind, &a, &len);
    uint8_t *mem = len && len <= 64u * 1024u * 1024u ? (uint8_t *)pf_mem(a, len) : NULL;
    if (len && mem && g_pf_save_cap < len) {
        free(g_pf_save);
        free(g_pf_mine);
        g_pf_save = (uint8_t *)malloc(len);
        g_pf_mine = (uint8_t *)malloc(len);
        g_pf_save_cap = g_pf_save && g_pf_mine ? len : 0;
    }
    pop_cpu_v1 mine = *cpu;
    PfSpan span = {0, 0};
    int ok = 0;
    if (!len || (mem && g_pf_save_cap >= len)) {
        if (len)
            memcpy(g_pf_save, mem, len);
        ok = kPf[kind].fn(&mine, &span);
        if (len) {
            memcpy(g_pf_mine, mem, len);
            memcpy(mem, g_pf_save, len);
        }
        mine.eip = get_u32(cpu->esp);
        mine.esp = cpu->esp + 4u + kPf[kind].args;
        if (ok && span.len && (span.addr < a || span.addr + span.len > a + len)) {
            char line[160];
            snprintf(line, sizeof line, "core.nfsc: paint fast verify %s: span %08x+%x outside the saved %08x+%x", kPf[kind].name,
                     span.addr, span.len, a, len);
            log_line(line);
        }
    }
    api->call_original(api, addr, cpu);
    if (!ok) {
        ++g_pf_fallback[kind];
        return;
    }
    ++g_pf_checked[kind];
    int bad = 0;
    char what[200] = "";
    if (len && memcmp(g_pf_mine, mem, len)) {
        uint32_t i = 0, n = 0;
        while (i < len && g_pf_mine[i] == mem[i])
            ++i;
        for (uint32_t j = i; j < len; ++j)
            n += g_pf_mine[j] != mem[j];
        snprintf(what, sizeof what, "memory at %08x (+%x of %x): mine %02x game %02x, %u bytes differ", a + i, i, len, g_pf_mine[i], mem[i], n);
        bad = 1;
    }
#define PF_CMP(fld)                                                                                                       \
    if (!bad && mine.fld != cpu->fld) {                                                                                  \
        snprintf(what, sizeof what, #fld " mine %x game %x", (unsigned)mine.fld, (unsigned)cpu->fld);                     \
        bad = 1;                                                                                                          \
    }
    PF_CMP(eax) PF_CMP(ecx) PF_CMP(edx) PF_CMP(ebx) PF_CMP(esp) PF_CMP(ebp) PF_CMP(esi) PF_CMP(edi) PF_CMP(eip)
    PF_CMP(cf) PF_CMP(zf) PF_CMP(sf) PF_CMP(of) PF_CMP(pf) PF_CMP(af) PF_CMP(df) PF_CMP(fpu_top) PF_CMP(fpu_cw)
    PF_CMP(fpu_sw) PF_CMP(fpu_tag)
#undef PF_CMP
    if (bad && ++g_pf_bad[kind] <= 12u) {
        char line[320];
        snprintf(line, sizeof line, "core.nfsc: paint fast verify %s DIFFERS (call %u): %s", kPf[kind].name, g_pf_checked[kind], what);
        log_line(line);
    }
    static uint32_t said;
    if (++said % 64u == 0)
        pf_verify_say();
#else
    PfSpan span;
    if (kPf[kind].fn(cpu, &span)) {
        ++g_pf_native[kind];
        api->hook_return(api, cpu, cpu->eax, kPf[kind].args);
        return;
    }
    ++g_pf_fallback[kind];
    api->call_original(api, addr, cpu);
#endif
}

static void pf_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    pf_run(api, cpu, (int)(uintptr_t)user);
}
/* 007b4d80 (canvas slot +0x1c, fill through a mask) and, through the redirected slot +0x18, 007b4ef0 */
static void pf_hook_slot(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t ra = get_u32(cpu->esp);
    uint8_t b0 = 0, b1 = 0, b2 = 0;
    api->guest_read_u8(api, ra - 3u, &b0);
    api->guest_read_u8(api, ra - 2u, &b1);
    api->guest_read_u8(api, ra - 1u, &b2);
    if (b0 == 0xffu && (b1 & 0xf8u) == 0x50u && b1 != 0x54u && b2 == 0x18u) { /* CALL [reg+0x18] */
        pf_run(api, cpu, PF_K_ALPHA);
        return;
    }
    api->call_original(api, PF_FILL_MASK, cpu);
}

#if PAINT_FAST_STATS
static const uint32_t kPfStatAddr[] = {0x007b71a0u, 0x007c6d80u, 0x007d7580u, 0x007c7590u, 0x007b6960u, 0x007dba00u};
#define PF_NSTAT (sizeof kPfStatAddr / sizeof kPfStatAddr[0])
static uint64_t g_pf_us[PF_NSTAT], g_pf_n[PF_NSTAT];
static uint32_t g_pf_hooks[PF_NSTAT], g_pf_outputs;
static void pf_stat(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    const unsigned i = (unsigned)(uintptr_t)user;
    const uint64_t t0 = paint_us();
    api->call_original(api, cpu->target, cpu);
    g_pf_us[i] += paint_us() - t0;
    ++g_pf_n[i];
    if (kPfStatAddr[i] == 0x007d7580u && ++g_pf_outputs % 8u == 0) {
        char line[1000];
        int at = snprintf(line, sizeof line, "core.nfsc: paint fast stats after %u outputs:", g_pf_outputs);
        for (unsigned k = 0; k < PF_NSTAT && at < (int)sizeof line - 80; ++k)
            at += snprintf(line + at, sizeof line - at, " %08x n%llu %lluus;", kPfStatAddr[k], (unsigned long long)g_pf_n[k],
                           (unsigned long long)g_pf_us[k]);
        log_line(line);
    }
}
#endif

static void paint_fast_install(void) {
#if PAINT_FAST_STATS
    for (unsigned i = 0; i < PF_NSTAT; ++i)
        if (g_api->hook_install(g_api, kPfStatAddr[i], pf_stat, POP_HOOK_REPLACE, (void *)(uintptr_t)i, &g_pf_hooks[i]) != POP_OK)
            g_pf_hooks[i] = 0;
#endif
    const char *e = getenv("RECOMP_PAINT_FAST");
    g_pf_on = !(e && *e == '0');
    if (!g_pf_on) {
        log_line("core.nfsc: paint fast: TEST off (RECOMP_PAINT_FAST=0): the game's own compositor loops");
        return;
    }
    int ok = 1;
    for (int i = 0; i < PF_KINDS; ++i) {
        if (i == PF_K_ALPHA)
            continue;
        if (g_api->hook_install(g_api, kPf[i].addr, pf_hook, POP_HOOK_REPLACE, (void *)(uintptr_t)i, &g_pf_hook[i]) != POP_OK) {
            g_pf_hook[i] = 0;
            ok = 0;
            char line[96];
            snprintf(line, sizeof line, "core.nfsc: paint fast: no hook on %s", kPf[i].name);
            log_line(line);
        }
    }
    /* the alpha blend through the vtable slot, only when the slot still holds 007b4ef0 */
    if (get_u32(PF_VT_ALPHA) == PF_BLEND_ALPHA && get_u32(PF_VT_ALPHA + 4u) == PF_FILL_MASK &&
        g_api->hook_install(g_api, PF_FILL_MASK, pf_hook_slot, POP_HOOK_REPLACE, NULL, &g_pf_hook[PF_KINDS]) == POP_OK) {
        g_pf_vt_old = PF_BLEND_ALPHA;
        mp_put(PF_VT_ALPHA, PF_FILL_MASK);
    } else {
        g_pf_hook[PF_KINDS] = 0;
        ok = 0;
        log_line("core.nfsc: paint fast: the alpha blend 007b4ef0 stays the game's (vtable slot or hook not as expected)");
    }
    log_line(ok ? "core.nfsc: paint fast: compositor loops 007b4b50 007b4ef0 007b5100 007b6a40 007b68d0 native" PF_VERIFY_NOTE
                : "core.nfsc: paint fast: some compositor loops native (see above)" PF_VERIFY_NOTE);
}

static void paint_fast_shutdown(void) {
#if PAINT_FAST_STATS
    for (unsigned i = 0; i < PF_NSTAT; ++i)
        if (g_pf_hooks[i])
            g_api->hook_remove(g_api, g_pf_hooks[i]);
#endif
    if (g_pf_vt_old) {
        mp_put(PF_VT_ALPHA, g_pf_vt_old);
        g_pf_vt_old = 0;
    }
    for (int i = 0; i <= PF_KINDS; ++i) {
        if (g_pf_hook[i])
            g_api->hook_remove(g_api, g_pf_hook[i]);
        g_pf_hook[i] = 0;
    }
}
