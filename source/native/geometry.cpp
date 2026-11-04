// Carbon 1.4 X/Z point-in-triangle test. Keep the translated x87 operation
// order, precision control, scratch slots, condition flags and return state.
#include "../kit/runtime/x86.h"
#include <cstdint>
#pragma STDC FP_CONTRACT OFF
extern "C" void fn_007e83e0(X86 *);
namespace {
// Some generated backends carry an entry selector in their CPU envelope.
// Preserve the original wrapper's zero entry without platform conditionals.
template <class Cpu> static void entry_zero(Cpu *c) {
  if constexpr (requires { c->recomp_entry; })
    c->recomp_entry = 0;
}

struct Edge {
  double value, second, last;
};
static Edge edge(X86 *c, double a, double b, double d, double e, double f,
                 double g, double h, double i) {
  const double first = fx87(c, fx87(c, a - b) * fx87(c, d - e));
  const double left = fx87(c, f - g), right = fx87(c, h - i);
  const double second = fx87(c, left * right);
  return {fx87(c, first - second), second, right};
}
static void compare(X86 *c, const Edge &v, uint8_t mask) {
  const double values[3] = {v.value, v.second, v.last};
  for (unsigned i = 0; i < 3; i++) {
    const unsigned slot = (c->fpu_top - 1u - i) & 7u;
    c->st[slot] = values[i];
    c->st_bits[slot] = 0;
    c->st_exact[slot] = 0;
    ftag_put(c, slot, FTAG_EMPTY);
  }
  fcom(c, v.value, double(rdf32(0x009c248cu)));
  c->r[0] = (c->r[0] & 0xffff0000u) | fstsw(c);
  const uint8_t flags = uint8_t(c->r[0] >> 8) & mask;
  c->eflags_cf = c->eflags_of = c->eflags_sf = 0;
  c->eflags_zf = flags == 0;
  c->eflags_pf = parity8(flags);
}
static void finish(X86 *c, bool inside) {
  c->r[0] = (c->r[0] & 0xffffff00u) | (inside ? 1u : 0u);
  if (!inside) {
    c->eflags_cf = c->eflags_of = c->eflags_sf = 0;
    c->eflags_zf = c->eflags_pf = 1;
  }
  c->eip = rd32(c->r[4]);
  c->r[4] += 4u;
  recomp_return(c);
}
} // namespace
extern "C" void nfsc_point_triangle(X86 *c) {
  const uint32_t sp = c->r[4];
  if (sp < 0x10000u || uint64_t(sp) + 12u > GUEST_SIZE) {
    fn_007e83e0(c);
    return;
  }
  const uint32_t point = rd32(sp + 4u), tri = rd32(sp + 8u);
  if (point < 0x10000u || tri < 0x10000u ||
      uint64_t(point) + 12u > GUEST_SIZE || uint64_t(tri) + 44u > GUEST_SIZE) {
    fn_007e83e0(c);
    return;
  }
  const double px = rdf32(point), pz = rdf32(point + 8u), ax = rdf32(tri),
               az = rdf32(tri + 8u), bx = rdf32(tri + 16u),
               bz = rdf32(tri + 24u), cx = rdf32(tri + 32u),
               cz = rdf32(tri + 40u);
  // Exceptional operands use the complete original instruction path.
  if (!isfinite(px) || !isfinite(pz) || !isfinite(ax) || !isfinite(az) ||
      !isfinite(bx) || !isfinite(bz) || !isfinite(cx) || !isfinite(cz)) {
    fn_007e83e0(c);
    return;
  }
  entry_zero(c);
  c->r[1] = tri;
  c->r[2] = point;
  compare(c, edge(c, pz, bz, ax, bx, px, bx, az, bz), 0x41u);
  if (!c->eflags_pf) {
    compare(c, edge(c, bx, cx, pz, cz, bz, cz, px, cx), 0x41u);
    if (c->eflags_pf) {
      finish(c, false);
      return;
    }
    compare(c, edge(c, pz, az, cx, ax, cz, az, px, ax), 0x41u);
    finish(c, !c->eflags_pf);
    return;
  }
  compare(c, edge(c, pz, cz, bx, cx, bz, cz, px, cx), 0x01u);
  if (!c->eflags_zf) {
    finish(c, false);
    return;
  }
  compare(c, edge(c, cx, ax, pz, az, cz, az, px, ax), 0x01u);
  finish(c, c->eflags_zf);
}
