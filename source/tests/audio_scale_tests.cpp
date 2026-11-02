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
static constexpr uint32_t sp = 0x08001000u, data = 0x07000000u;
static unsigned checks, failures;
static void compare(X86 entry, uint32_t dst, uint32_t src, uint32_t count,
                    uint32_t gain) {
  uint8_t stack[256], wantstack[256], input[16384], output[16384];
  for (unsigned i = 0; i < 256; ++i)
    g_mem[sp - 128 + i] = uint8_t(i * 17);
  wr32(sp, GUEST_RETURN_SENTINEL);
  wr32(sp + 4, dst);
  wr32(sp + 8, src);
  wr32(sp + 12, gain);
  wr32(sp + 16, count);
  memcpy(stack, g_mem + sp - 128, sizeof stack);
  memcpy(input, g_mem + data, sizeof input);
  X86 original = entry;
  fn_008ad12e(&original);
  memcpy(wantstack, g_mem + sp - 128, sizeof stack);
  memcpy(output, g_mem + data, sizeof output);
  memcpy(g_mem + sp - 128, stack, sizeof stack);
  memcpy(g_mem + data, input, sizeof input);
  X86 candidate = entry;
  nfsc_audio_scale(&candidate);
  ++checks;
  if (memcmp(&candidate, &original, sizeof original) ||
      memcmp(output, g_mem + data, sizeof output) ||
      memcmp(wantstack, g_mem + sp - 128, sizeof stack)) {
    if (++failures <= 4) {
      printf("FAIL case %u n=%u dst=%x src=%x cw=%x\n", checks, count, dst, src,
             entry.fpu_cw);
      const auto *a = (const uint8_t *)&original,
                 *b = (const uint8_t *)&candidate;
      for (size_t i = 0; i < sizeof original; ++i)
        if (a[i] != b[i])
          printf("CPU byte %zu: %02x/%02x\n", i, a[i], b[i]);
    }
  }
}
int main() {
  harness_mem();
  wr8(0x00bc6e88u, 0);
  std::mt19937 rng(891214);
  X86 entry{};
  entry.r[4] = sp;
  entry.fpu_tag = 0xffff;
  entry.fpu_cw = 0x027f;
  const uint32_t counts[] = {0, 1, 3, 15, 16, 17, 31, 32, 64, 256, 1024};
  const uint16_t modes[] = {0x023f, 0x027f, 0x037f, 0x007f,
                            0x067f, 0x0a7f, 0x0e7f};
  const uint32_t special[] = {0,          0x80000000, 1,          0x007fffff,
                              0x00800000, 0x3f800000, 0x7f7fffff, 0x7f800000,
                              0xff800000, 0x7fc01234, 0x7fa00001};
  for (unsigned k = 0; k < 2400; ++k) {
    for (unsigned i = 0; i < 4096; ++i)
      wr32(data + i * 4, k < 77 ? special[(k + i) % 11] : rng());
    for (unsigned i = 0; i < 8; ++i) {
      entry.r[i] = rng();
      entry.st[i] = double(int(rng()));
      entry.st_bits[i] = rng();
      entry.st_exact[i] = uint8_t(i & 1);
    }
    entry.r[4] = sp;
    entry.fpu_cw = modes[k % 7];
    entry.fpu_top = k % 8;
    entry.fpu_tag = uint16_t(rng());
    entry.fpu_sw = uint16_t(rng());
    entry.eflags_cf = k & 1;
    entry.eflags_of = (k >> 1) & 1;
    entry.eflags_af = 1;
    uint32_t src = data + 4096, dst = data + 8192;
    if (k % 5 == 1)
      dst = src;
    else if (k % 5 == 2)
      dst = src + 16;
    else if (k % 5 == 3)
      dst = src - 16;
    if (k % 11 == 0) {
      src += 4;
      dst += 4;
    }
    compare(entry, dst, src, counts[k % 11], k < 77 ? special[k % 11] : rng());
  }
  // Independent sample values: 0, 0.5, -1 and 2 multiplied by 0.25.
  entry = {};
  entry.r[4] = sp;
  entry.fpu_cw = 0x027f;
  entry.fpu_tag = 0xffff;
  for (unsigned i = 0; i < 16; ++i)
    wrf32(data + i * 4, i % 4 == 0   ? 0.0f
                        : i % 4 == 1 ? 0.5f
                        : i % 4 == 2 ? -1.0f
                                     : 2.0f);
  compare(entry, data + 4096, data, 16, 0x3e800000);
  for (unsigned i = 0; i < 16; ++i) {
    ++checks;
    float expected = i % 4 == 0   ? 0.0f
                     : i % 4 == 1 ? 0.125f
                     : i % 4 == 2 ? -0.25f
                                  : 0.5f;
    if (rdf32(data + 4096 + i * 4) != expected)
      ++failures;
  }
  compare(entry, sp - 64, data, 16,
          0x3f000000); // scratch-stack alias retains the original path
  printf("Audio scale: %u comparisons, %u failures\n", checks, failures);
  if (failures)
    return 1;
  for (unsigned i = 0; i < 256; ++i)
    wrf32(data + 4 * i, float(int(i) - 128) / 256);
  wr32(sp, GUEST_RETURN_SENTINEL);
  wr32(sp + 4, data + 4096);
  wr32(sp + 8, data);
  wr32(sp + 12, 0x3f000000);
  wr32(sp + 16, 256);
  for (unsigned repeat = 0; repeat < 4; ++repeat)
    for (unsigned which = 0; which < 2; ++which) {
      const auto start = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < 50000; ++i) {
        X86 c = entry;
        if (which)
          nfsc_audio_scale(&c);
        else
          fn_008ad12e(&c);
      }
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
      printf("Benchmark %u %s %.3f ms\n", repeat,
             which ? "candidate" : "original", ms);
    }
}
