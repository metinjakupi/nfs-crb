// Compare full CPU state, guest stack and sample bytes with the original
// translated routine. The original is the oracle; no output uses the candidate.
#include "../kit/runtime/seh.h"
#include "../kit/runtime/x86.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
extern "C" uint8_t *harness_mem(void);
extern "C" void fn_008ad12e(X86 *);
extern "C" void nfsc_audio_scale(X86 *);
extern "C" void fn_008ad305(X86 *);
extern "C" void nfsc_audio_mix(X86 *);
extern "C" {
uint32_t recomp_frame_watch = 0;
void recomp_frame_changed(X86 *, uint32_t, uint32_t, uint32_t) { std::abort(); }
void discovery_note(const char *, uint32_t, uint32_t) { std::abort(); }
void discovery_write() { std::abort(); }
int interp_call(X86 *, uint32_t) { std::abort(); }
int recomp_run_thunk(X86 *, uint32_t) { std::abort(); }
void recomp_module_register(const RecompModule *) {}
jmp_buf *recomp_seh_frame_enter(X86 *) { std::abort(); }
void recomp_seh_frame_leave(X86 *) { std::abort(); }
void recomp_seh_land(X86 *) { std::abort(); }
uint32_t recomp_seh_pending_target() { return 0; }
void recomp_seh_intercept(X86 *, uint32_t) { std::abort(); }
}

extern "C" void fn_007e83e0(X86 *);
extern "C" void nfsc_point_triangle(X86 *);
static constexpr uint32_t sp = 0x08001000u, data = 0x07000000u;
static unsigned checks, failures;
static void check(X86 entry, uint32_t point, uint32_t tri) {
  wr32(sp, GUEST_RETURN_SENTINEL);
  wr32(sp + 4, point);
  wr32(sp + 8, tri);
  uint8_t stack[256], region[512], wantstack[256], wantregion[512];
  memcpy(stack, g_mem + sp - 128, sizeof stack);
  memcpy(region, g_mem + data, sizeof region);
  X86 want = entry;
  fn_007e83e0(&want);
  memcpy(wantstack, g_mem + sp - 128, sizeof stack);
  memcpy(wantregion, g_mem + data, sizeof region);
  memcpy(g_mem + sp - 128, stack, sizeof stack);
  memcpy(g_mem + data, region, sizeof region);
  X86 got = entry;
  nfsc_point_triangle(&got);
  checks++;
  if (memcmp(&want, &got, sizeof got) ||
      memcmp(wantstack, g_mem + sp - 128, sizeof stack) ||
      memcmp(wantregion, g_mem + data, sizeof region)) {
    if (++failures <= 3) {
      printf("FAIL %u cw=%x top=%u point=%x tri=%x\n", checks, entry.fpu_cw,
             entry.fpu_top, point, tri);
      for (unsigned i = 0; i < sizeof got; i++)
        if (((uint8_t *)&want)[i] != ((uint8_t *)&got)[i])
          printf("CPUbyte%u %02x/%02x\n", i, ((uint8_t *)&want)[i],
                 ((uint8_t *)&got)[i]);
    }
  }
}
int main() {
  harness_mem();
  std::mt19937 rng(783);
  X86 c{};
  const uint16_t modes[] = {0x027f, 0x037f, 0x007f, 0x067f, 0x0a7f, 0x0e7f};
  const uint32_t specials[] = {0,          0x80000000, 1,          0x007fffff,
                               0x00800000, 0x3f800000, 0x7f7fffff, 0x7f800000,
                               0xff800000, 0x7fc01234, 0x7fa00001};
  for (unsigned k = 0; k < 18000; k++) {
    for (unsigned i = 0; i < 128; i++)
      wr32(data + 4 * i, k < 400 ? specials[(k + i) % 11] : rng());
    for (unsigned i = 0; i < 8; i++) {
      c.r[i] = rng();
      c.st[i] = double(int(rng()));
      c.st_bits[i] = rng();
      c.st_exact[i] = i & 1;
    }
    c.r[4] = sp;
    c.fpu_top = k % 8;
    c.fpu_tag = uint16_t(rng());
    c.fpu_cw = modes[k % 6];
    c.fpu_sw = uint16_t(rng());
    c.eflags_af = rng() & 1;
    c.eflags_zf = rng() & 1;
    c.eflags_sf = rng() & 1;
    c.eflags_cf = rng() & 1;
    c.eflags_of = rng() & 1;
    c.eflags_pf = rng() & 1;
    if (k % 97 == 0)
      wr32(0x9c248c, 0x7fc01234u);
    else
      wrf32(0x9c248c, k % 31 == 0 ? 0.25f : 0.0f);
    check(c, k % 3 == 0 ? data : data + 256, data);
  }
  // Independent boundary fixtures: both windings, every vertex, edges,
  // exterior/interior, and a collinear/zero-area triangle. Parity remains full
  // state.
  for (unsigned winding = 0; winding < 2; winding++)
    for (unsigned degenerate = 0; degenerate < 2; degenerate++)
      for (unsigned mode = 0; mode < 6; mode++) {
        const float x[] = {0, 4, degenerate ? 8.0f : 0.0f},
                    z[] = {0, 0, degenerate ? 0.0f : 4.0f};
        for (unsigned i = 0; i < 3; i++) {
          unsigned v = winding ? 2 - i : i;
          wrf32(data + i * 16, x[v]);
          wrf32(data + i * 16 + 8, z[v]);
        }
        const float points[][2] = {
            {0, 0}, {4, 0}, {0, 4},         {2, 0},        {0, 2},
            {2, 2}, {1, 1}, {-0.00001f, 0}, {4.00001f, 0}, {100, 100}};
        for (auto &p : points) {
          wrf32(data + 256, p[0]);
          wrf32(data + 264, p[1]);
          c.r[4] = sp;
          c.fpu_cw = modes[mode];
          wrf32(0x9c248c, 0);
          check(c, data + 256, data);
        }
      }
  // Stack/point/triangle overlap remains read-only, including aliases to args.
  for (unsigned i = 0; i < 8; i++) {
    c.r[4] = sp;
    c.fpu_top = i;
    c.fpu_cw = modes[i % 6];
    wrf32(0x9c248c, 0);
    check(c, sp - 16, data);
    check(c, data + 256, sp - 16);
    check(c, sp + 4, sp + 4);
    wrf32(0x8000u, 1);
    wrf32(0x8008u, 1);
    check(c, 0x8000u, data);
  }
  printf("geometry: %u checks, %u failures\n", checks, failures);
  if (failures)
    return 1;
  for (unsigned i = 0; i < 3; i++) {
    wrf32(data + i * 16, i == 1 ? 4 : 0);
    wrf32(data + i * 16 + 8, i == 2 ? 4 : 0);
  }
  wrf32(data + 256, 1);
  wrf32(data + 264, 1);
  wr32(sp, GUEST_RETURN_SENTINEL);
  wr32(sp + 4, data + 256);
  wr32(sp + 8, data);
  wrf32(0x9c248c, 0);
  for (unsigned round = 0; round < 4; round++)
    for (unsigned order = 0; order < 2; order++) {
      bool candidate = (round + order) & 1;
      auto start = std::chrono::steady_clock::now();
      uint32_t sum = 0;
      for (unsigned i = 0; i < 100000; i++) {
        X86 t = c;
        t.r[4] = sp;
        t.fpu_cw = 0x027f;
        (candidate ? nfsc_point_triangle : fn_007e83e0)(&t);
        sum += t.r[0];
      }
      double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - start)
                      .count();
      printf("synthetic round=%u path=%s ms=%.3f checksum=%u\n", round,
             candidate ? "candidate" : "control", ms, sum);
    }
}
