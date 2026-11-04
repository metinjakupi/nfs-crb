// MSVC's _ftol2 in Carbon 1.4 (0087eac0): ST0 to a 64-bit integer in EDX:EAX,
// truncating toward zero whatever the rounding mode, and ST0 popped. Every
// (int)float in the game calls it. The x87 steps go through the same helpers
// the translated routine uses (so rounding, IE and the register slots come out
// alike); what is left out is the flag arithmetic of each instruction whose
// flags the next one overwrites, and the stack round-trips of the registers.
#include "../kit/runtime/x86.h"
#include <cstdint>
#pragma STDC FP_CONTRACT OFF
extern "C" void fn_0087eac0(X86 *);
extern "C" void nfsc_ftol2(X86 *c) {
  const uint32_t sp = c->r[4];
  if (sp < 0x10000u + 0x40u || uint64_t(sp) + 8u > GUEST_SIZE) {
    fn_0087eac0(c);
    return;
  }
  // PUSH EBP / MOV EBP,ESP / SUB ESP,0x20 / AND ESP,-16.
  const uint32_t ebp = sp - 4u, s = (ebp - 0x20u) & 0xfffffff0u;
  wr32(ebp, c->r[5]);
  // FLD ST0 / FST float [ESP+0x18] / FISTP qword [ESP+0x10] / FILD qword [ESP+0x10].
  fpush_st(c, 0);
  wrf32(s + 0x18u, fto_float(c, ST(c, 0)));
  wr64(s + 0x10u, uint64_t(fist_i64(c)));
  fdrop(c);
  fpush_int(c, int64_t(rd64(s + 0x10u)));
  uint32_t edx = rd32(s + 0x18u), eax = rd32(s + 0x10u);
  if (eax == 0u) {
    edx = rd32(s + 0x14u);
    if ((edx & 0x7fffffffu) == 0u) {
      // Exactly zero, or the indefinite integer: both values stored and popped.
      wrf32(s + 0x18u, fto_float(c, ST(c, 0)));
      fdrop(c);
      wrf32(s + 0x18u, fto_float(c, ST(c, 0)));
      fdrop(c);
      c->r[0] = eax;
      c->r[2] = edx;
      c->eflags_cf = c->eflags_of = c->eflags_sf = 0; // TEST EDX,0x7fffffff
      c->eflags_zf = 1;
      c->eflags_pf = 1;
      goto out;
    }
  }
  {
    // FSUBP: the fraction left after rounding, stored as a float and popped.
    fset(c, 1, fx87(c, ST(c, 1) - ST(c, 0)));
    fdrop(c);
    wrf32(s, fto_float(c, ST(c, 0)));
    fdrop(c);
    uint32_t ecx = rd32(s), cf;
    const uint32_t a = rd32(s + 0x14u);
    uint32_t r;
    uint64_t rf;
    if (edx >> 31) {
      // Negative: XOR ECX,0x80000000 / ADD ECX,0x7fffffff / ADC EAX,0 / ADC EDX,0.
      ecx ^= 0x80000000u;
      rf = uint64_t(ecx) + 0x7fffffffu;
      ecx = uint32_t(rf);
      cf = uint32_t(rf >> 32) & 1u;
      rf = uint64_t(eax) + cf;
      eax = uint32_t(rf);
      cf = uint32_t(rf >> 32) & 1u;
      rf = uint64_t(a) + cf;
      r = uint32_t(rf);
      c->eflags_of = ((~a & (a ^ r)) >> 31) & 1u;
    } else {
      // ADD ECX,0x7fffffff / SBB EAX,0 / SBB EDX,0.
      rf = uint64_t(ecx) + 0x7fffffffu;
      ecx = uint32_t(rf);
      cf = uint32_t(rf >> 32) & 1u;
      rf = uint64_t(eax) - cf;
      eax = uint32_t(rf);
      cf = uint32_t(rf >> 32) & 1u;
      rf = uint64_t(a) - cf;
      r = uint32_t(rf);
      c->eflags_of = ((a & (a ^ r)) >> 31) & 1u;
    }
    c->eflags_cf = uint32_t(rf >> 32) & 1u;
    c->eflags_af = ((a ^ r) >> 4) & 1u;
    c->eflags_zf = r == 0;
    c->eflags_sf = r >> 31;
    c->eflags_pf = parity8(r);
    c->r[0] = eax;
    c->r[1] = ecx;
    c->r[2] = r;
  }
out:
  // LEAVE / RET.
  c->r[5] = rd32(ebp);
  c->eip = rd32(sp);
  c->r[4] = sp + 4u;
  recomp_return(c);
}
