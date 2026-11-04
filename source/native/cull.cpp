// Carbon 1.4 box-against-view-frustum test (0071b630), the hottest translated
// routine in a race frame. thiscall: ECX is a view whose first dword points at
// six planes (x, y, z, d floats) from +0x140; on the stack the box's two
// corners and an optional matrix, which the cdecl helper 0070f0b0 applies to
// stack copies of the corners. Returns 0 outside, 1 crossing, 2 inside; RET 0xc.
//
// Same x87 operation order and precision control (fx87 after every operation,
// fto_float on every store), same guest stack writes, registers, flags, FPU
// status, tags and register slots as the translated routine; only the
// emulated x87 stack traffic in between is gone.
#include "../kit/runtime/x86.h"
#include <cmath>
#include <cstdint>
#pragma STDC FP_CONTRACT OFF
extern "C" void fn_0071b630(X86 *);
namespace {
constexpr uint32_t kHalf = 0x009c2888u, kZero = 0x009c248cu, kReturn = 0x0071b681u;
bool readable(uint32_t a, uint32_t n) {
  return a >= 0x10000u && uint64_t(a) + n <= GUEST_SIZE;
}
// An FCOMP against 0.0 followed by FNSTSW AX / TEST AH,0x5: true when the
// parity flag comes out clear, i.e. exactly one of C0 and C2 is set (below).
bool below_zero(X86 *c, double v) {
  fcom(c, v, double(rdf32(kZero)));
  return !parity8((fstsw(c) >> 8) & 0x5u);
}
void leave_slot(X86 *c, unsigned depth, double v) {
  const unsigned slot = (c->fpu_top - depth) & 7u;
  c->st[slot] = v;
  c->st_bits[slot] = 0;
  c->st_exact[slot] = 0;
  ftag_put(c, slot, FTAG_EMPTY);
}
} // namespace

extern "C" void nfsc_frustum_cull(X86 *c) {
  const uint32_t sp = c->r[4], view = c->r[1];
  // The frame (aligned down, 0x40 of locals, the helper's three arguments and
  // return address) lies within 0x80 bytes below ESP.
  if (!readable(sp - 0x80u, 0x90u) || !readable(view, 4u)) {
    fn_0071b630(c);
    return;
  }
  const uint32_t lo = rd32(sp + 4u), hi = rd32(sp + 8u), matrix = rd32(sp + 12u);
  if (!readable(lo, 12u) || !readable(hi, 12u)) {
    fn_0071b630(c);
    return;
  }
  // PUSH EBP / MOV EBP,ESP / AND ESP,-16 / SUB ESP,0x3c / PUSH ESI.
  const uint32_t ebp = sp - 4u, s = ((ebp & 0xfffffff0u) - 0x3cu) - 4u;
  wr32(ebp, c->r[5]);
  wr32(s, c->r[6]);
  c->r[5] = ebp;
  c->r[6] = view;
  wr32(s + 0x38u, rd32(lo + 8u));
  wr32(s + 0x30u, rd32(lo));
  wr32(s + 0x34u, rd32(lo + 4u));
  c->r[1] = rd32(hi);
  c->r[2] = rd32(hi + 4u);
  c->r[0] = rd32(hi + 8u);
  wr32(s + 0x18u, c->r[0]);
  c->r[0] = matrix;
  c->eflags_cf = c->eflags_of = 0; // TEST EAX,EAX
  c->eflags_zf = matrix == 0;
  c->eflags_sf = matrix >> 31;
  c->eflags_pf = parity8(matrix);
  wr32(s + 0x10u, c->r[1]);
  wr32(s + 0x14u, c->r[2]);
  if (matrix) {
    c->r[1] = s + 0x10u;
    wr32(s - 4u, c->r[1]);
    c->r[2] = s + 0x30u;
    wr32(s - 8u, c->r[2]);
    wr32(s - 12u, matrix);
    wr32(s - 16u, kReturn);
    c->r[4] = s - 16u;
    recomp_call(c, 0x0070f0b0u);
    c->r[4] += 0xcu; // ADD ESP,0xc (its flags are dead)
  } else {
    c->r[4] = s;
  }
  // The planes are read through the same guest accessors the original uses,
  // so an unusual pointer here behaves as it does there.
  const uint32_t planes = rd32(c->r[6]);
  c->r[0] = planes;
  c->r[6] = 0;
  c->r[2] = 1;
  c->r[1] = planes + 0x144u;
  const double half = rdf32(kHalf);
  float centre[3], extent[3];
  for (unsigned k = 0; k < 3; k++) {
    const double sum = fx87(c, double(rdf32(s + 0x10u + 4u * k)) + double(rdf32(s + 0x30u + 4u * k)));
    centre[k] = fto_float(c, fx87(c, sum * half));
    wrf32(s + 0x20u + 4u * k, centre[k]);
  }
  for (unsigned k = 0; k < 3; k++) {
    extent[k] = fto_float(c, fx87(c, double(rdf32(s + 0x10u + 4u * k)) - double(centre[k])));
    wrf32(s + 0x30u + 4u * k, extent[k]);
  }
  uint32_t af = c->eflags_af;
  for (;;) {
    const uint32_t p = c->r[1];
    const double ny = rdf32(p), nz = rdf32(p + 4u), nx = rdf32(p - 4u), d = rdf32(p + 8u);
    const double across = fx87(c, fx87(c, std::fabs(ny) * double(extent[1])) +
                                      fx87(c, std::fabs(nz) * double(extent[2])));
    wrf32(s + 0xcu, fto_float(c, std::fabs(nx)));
    const double radius = fx87(c, across + fx87(c, std::fabs(nx) * double(extent[0])));
    const double yz = fx87(c, fx87(c, double(centre[1]) * ny) + fx87(c, double(centre[2]) * nz));
    const double dist = fx87(c, fx87(c, yz + fx87(c, double(centre[0]) * nx)) + d);
    const double far_side = fx87(c, dist + radius);
    if (below_zero(c, far_side)) {
      leave_slot(c, 3, far_side);
      leave_slot(c, 2, dist);
      leave_slot(c, 1, radius);
      c->r[0] = 0; // XOR EAX,EAX
      c->eflags_cf = c->eflags_of = c->eflags_sf = 0;
      c->eflags_zf = c->eflags_pf = 1;
      c->eflags_af = af;
      break;
    }
    const double near_side = fx87(c, dist - radius);
    if (below_zero(c, near_side))
      c->r[6] = 1;
    leave_slot(c, 3, far_side);
    leave_slot(c, 2, near_side);
    leave_slot(c, 1, radius);
    const uint32_t i = ++c->r[2];
    c->r[1] += 0x10u;
    af = ((i ^ 6u ^ (i - 6u)) >> 4) & 1u; // CMP EDX,6
    if (i > 6u) {
      c->r[0] = c->r[6] ? 1u : 2u; // XOR / TEST ESI,ESI / SETZ AL / INC EAX
      c->eflags_cf = 0;
      c->eflags_of = c->eflags_zf = c->eflags_sf = c->eflags_af = 0;
      c->eflags_pf = parity8(c->r[0]);
      break;
    }
  }
  // POP ESI / MOV ESP,EBP / POP EBP / RET 0xc.
  c->r[6] = rd32(s);
  c->r[5] = rd32(ebp);
  c->eip = rd32(sp);
  c->r[4] = sp + 16u;
  recomp_return(c);
}

// Carbon 1.4 box-by-matrix (0070f0b0), cdecl (matrix, lo, hi): rewrites the
// box corners in place as the box around the transformed box (Arvo's method:
// start from the translation row, then for every matrix element add the
// smaller of element * lo[i] and element * hi[i] to the new lo, the larger to
// the new hi). The translated routine keeps its six sums on the x87 stack, up
// to eight deep; here push position k of the original (ST(depth-1-k)) is the
// local p[k], and the register file is written back as the original leaves it.
extern "C" void fn_0070f0b0(X86 *);
extern "C" void nfsc_box_transform(X86 *c) {
  const uint32_t sp = c->r[4];
  if (!readable(sp - 0x20u, 0x30u)) {
    fn_0070f0b0(c);
    return;
  }
  const uint32_t m = rd32(sp + 4u), lo = rd32(sp + 8u), hi = rd32(sp + 12u);
  if (!readable(m, 0x40u) || !readable(lo, 12u) || !readable(hi, 12u)) {
    fn_0070f0b0(c);
    return;
  }
  // SUB ESP,0xc / PUSH EBX / PUSH EBP / PUSH ESI / the translation's x and y
  // bits copied through two locals / PUSH EDI.
  const uint32_t ty_bits = rd32(m + 0x34u), tx_bits = rd32(m + 0x30u);
  wr32(sp - 0x10u, c->r[3]);
  wr32(sp - 0x14u, c->r[5]);
  wr32(sp - 0x18u, c->r[6]);
  wr32(sp - 0x8u, ty_bits);
  wr32(sp - 0xcu, tx_bits);
  wr32(sp - 0x1cu, c->r[7]);
  double p[8];
  p[0] = rdf32(m + 0x30u);
  p[1] = rdf32(m + 0x34u);
  p[2] = rdf32(m + 0x38u);
  p[3] = rdf32(m + 0x38u);
  p[4] = rdf32(sp - 0x8u);
  p[5] = rdf32(sp - 0xcu);
  const uint32_t tmp = sp + 4u; // [ESP+0x20] inside the loop: the matrix argument's slot
  // Element times lo and hi, the hi product rounded through the scratch slot,
  // and whether FCOM / FNSTSW / TEST AH,5 / JP falls through (lo product below).
  auto pair = [&](uint32_t i, uint32_t element, double &b_stored) {
    const double e = rdf32(element);
    p[6] = fx87(c, double(rdf32(lo + 4u * i)) * e);
    p[7] = fx87(c, double(rdf32(hi + 4u * i)) * e);
    const float b = fto_float(c, p[7]);
    wrf32(tmp, b);
    b_stored = rdf32(tmp);
    fcom(c, p[6], b_stored);
    // FNSTSW AX, with seven values on the stack (the high half of EAX is the
    // matrix pointer's); later additions may still set IE in the status word.
    c->r[0] = (m & 0xffff0000u) |
              uint16_t((c->fpu_sw & uint16_t(~0x3800u)) | uint16_t(((c->fpu_top - 7u) & 7u) << 11));
    return bool(parity8((c->r[0] >> 8) & 0x5u));
  };
  for (uint32_t i = 0; i < 3; i++) {
    const uint32_t row = m + 0x10u * i;
    double b;
    // x: new lo sum p[0], new hi sum p[5].
    if (!pair(i, row, b)) {
      p[0] = fx87(c, p[0] + p[6]);
      p[5] = fx87(c, p[5] + b);
    } else {
      p[7] = b;
      p[0] = fx87(c, p[0] + p[7]);
      p[5] = fx87(c, p[5] + p[6]);
    }
    // y: new lo sum p[1], new hi sum p[4].
    if (!pair(i, row + 4u, b)) {
      p[1] = fx87(c, p[1] + p[6]);
      p[6] = b;
    } else {
      p[7] = b;
      p[1] = fx87(c, p[1] + p[7]);
    }
    p[4] = fx87(c, p[4] + p[6]);
    // z: new lo sum p[2], new hi sum p[3].
    if (!pair(i, row + 8u, b)) {
      p[2] = fx87(c, p[2] + p[6]);
      p[6] = b;
    } else {
      p[7] = b;
      p[2] = fx87(c, p[2] + p[7]);
    }
    p[3] = fx87(c, p[3] + p[6]);
  }
  // FXCH ST5 / FSTP [EBX] / FXCH ST3 / FSTP [EBX+4] / FXCH / FSTP [EBX+8] /
  // FXCH ST2 / FSTP [EBP] / FSTP [EBP+4] / FSTP [EBP+8], slots as they end up.
  const double min_x = p[0], max_x = p[5], min_y = p[1], max_y = p[4], min_z = p[2], max_z = p[3];
  wrf32(lo, fto_float(c, min_x));
  wrf32(lo + 4u, fto_float(c, min_y));
  wrf32(lo + 8u, fto_float(c, min_z));
  wrf32(hi, fto_float(c, max_x));
  wrf32(hi + 4u, fto_float(c, max_y));
  wrf32(hi + 8u, fto_float(c, max_z));
  const double ends[8] = {max_z, max_y, max_x, min_z, min_y, min_x, p[6], p[7]};
  for (unsigned k = 0; k < 8; k++)
    leave_slot(c, k + 1u, ends[k]);
  c->r[1] = lo + 12u;
  c->r[2] = m + 0x38u;
  c->r[3] = rd32(sp - 0x10u);
  c->r[5] = rd32(sp - 0x14u);
  c->r[6] = rd32(sp - 0x18u);
  c->r[7] = rd32(sp - 0x1cu);
  // ADD ESP,0xc's flags are the ones left.
  const uint32_t a = sp - 0xcu, r = sp;
  c->eflags_cf = uint32_t((uint64_t(a) + 0xcu) >> 32) & 1u;
  c->eflags_of = ((~(a ^ 0xcu) & (a ^ r)) >> 31) & 1u;
  c->eflags_af = ((a ^ 0xcu ^ r) >> 4) & 1u;
  c->eflags_zf = r == 0;
  c->eflags_sf = r >> 31;
  c->eflags_pf = parity8(r);
  c->eip = rd32(sp);
  c->r[4] = sp + 4u;
  recomp_return(c);
}
