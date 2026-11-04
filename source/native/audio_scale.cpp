// Carbon 1.4 float scaling: preserve scalar sample order and x87 rounding.
// Fall back for the small/unaligned cases and guest-stack aliasing.
#include "../kit/runtime/x86.h"
#include <cstdint>
#pragma STDC FP_CONTRACT OFF
extern "C" void fn_008ad12e(X86 *);
extern "C" void fn_008ad305(X86 *);
static bool overlap(uint32_t p, uint32_t bytes, uint32_t a, uint32_t b) {
  return uint64_t(p) + bytes > a && p < b;
}
extern "C" void nfsc_audio_scale(X86 *c) {
  const uint32_t sp = c->r[4], dst = rd32(sp + 4), src = rd32(sp + 8),
                 n = rd32(sp + 16);
  const uint64_t bytes = uint64_t(n) * 4;
  if (!n || n > 4096 || ((dst | src | n) & 15u) || rd8(0x00bc6e88u) ||
      uint64_t(dst) + bytes > UINT32_MAX ||
      uint64_t(src) + bytes > UINT32_MAX ||
      overlap(dst, uint32_t(bytes), sp - 12, sp + 20) ||
      overlap(src, uint32_t(bytes), sp - 12, sp + 20)) {
    fn_008ad12e(c);
    return;
  }
  const double gain = rdf32(sp + 12);
  wr32(sp - 4, c->r[5]);
  wr32(sp - 8, c->r[6]);
  wr32(sp - 12, c->r[7]);
  double last = 0;
  // No restrict or vector shortcut: overlapping buffers follow native order.
  for (uint32_t i = 0; i < n; ++i) {
    last = fx87(c, gain * double(rdf32(src + 4 * i)));
    wrf32(dst + 4 * i, fto_float(c, last));
  }
  const unsigned slot = (c->fpu_top - 1u) & 7u;
  c->st[slot] = last;
  c->st_bits[slot] = 0;
  c->st_exact[slot] = 0;
  ftag_put(c, slot, FTAG_EMPTY);
  c->r[0] = src + uint32_t(bytes) + 8u;
  c->r[1] = dst + uint32_t(bytes);
  c->r[2] = dst - src;
  c->eflags_cf = c->eflags_of = c->eflags_sf = c->eflags_af = 0;
  c->eflags_zf = c->eflags_pf = 1;
  c->eip = rd32(sp);
  c->r[4] = sp + 4;
  recomp_return(c);
}

// Carbon's scaled-add mixer. Keep each multiply and add separately rounded,
// and process overlapping buffers in the original scalar order.
extern "C" void nfsc_audio_mix(X86 *c) {
  const uint32_t sp = c->r[4], dst = rd32(sp + 4), src = rd32(sp + 8),
                 n = rd32(sp + 16);
  const uint64_t bytes = uint64_t(n) * 4;
  if (!n || n > 4096 || ((dst | src | n) & 15u) || rd8(0x00bc6e88u) ||
      uint64_t(dst) + bytes + 12u > UINT32_MAX ||
      uint64_t(src) + bytes + 8u > UINT32_MAX ||
      overlap(dst, uint32_t(bytes), sp - 24, sp + 20) ||
      overlap(src, uint32_t(bytes), sp - 24, sp + 20)) {
    fn_008ad305(c);
    return;
  }
  const double gain = rdf32(sp + 12);
  wr32(sp - 4, c->r[5]);
  wr32(sp - 8, c->r[1]);
  wr32(sp - 12, c->r[1]);
  wr32(sp - 16, c->r[3]);
  wr32(sp - 20, c->r[6]);
  wr32(sp - 24, c->r[7]);
  double last = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const double product = fx87(c, gain * double(rdf32(src + 4 * i)));
    last = fx87(c, product + double(rdf32(dst + 4 * i)));
    wrf32(dst + 4 * i, fto_float(c, last));
  }
  const unsigned slot = (c->fpu_top - 1u) & 7u;
  c->st[slot] = last;
  c->st_bits[slot] = 0;
  c->st_exact[slot] = 0;
  ftag_put(c, slot, FTAG_EMPTY);
  c->r[0] = dst + uint32_t(bytes) + 12u;
  c->r[1] = src + uint32_t(bytes) + 8u;
  c->r[2] = 0;
  c->eflags_cf = c->eflags_of = c->eflags_sf = c->eflags_af = 0;
  c->eflags_zf = c->eflags_pf = 1;
  c->eip = rd32(sp);
  c->r[4] = sp + 4;
  recomp_return(c);
}
