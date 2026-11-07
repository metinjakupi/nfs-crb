// crash_probe.cpp - what the guest was doing when a browser thread trapped.
//
// A WebAssembly trap ("memory access out of bounds", "unreachable", ...) is not
// a signal: no fault handler of boot.cpp runs and nothing is printed. The
// worker's JavaScript still runs afterwards (web/crash-capture.js, in the
// worker's 'error' event) and asks this function, on the thread that trapped,
// for the guest state that thread left behind. The instance is still usable
// after a trap; this reads thread-local state and guest memory, takes no lock,
// does not allocate and calls nothing that could wait.
//
// What the fields mean, exactly:
// - EIP is the last value translated code STORED in the register file: the
//   return address of the last RET, or the target of the last indirect call or
//   jump. Translated code does not keep EIP per instruction, so this is not the
//   faulting instruction; the wasm stack (fn_XXXXXXXX frames) names that function.
// - ESP/EBP and the other registers are as last written, at function level.
// - "returns" are dwords on the guest stack, from ESP up, that are known call
//   return addresses of the game's code: the guest's own call chain, newest first
//   (a stale stack slot can also match).
// - thread 1 is the game's main thread, and also any thread that never ran
//   guest code (presenter, audio): those report the main thread's register file.
// - mod callback names the core mod callback this thread was inside, if any.
#include "../runtime/x86.h"
#include "../runtime/mods_seam.h"
#include "../runtime/win32.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/heap.h>
#define CRASH_PROBE_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define CRASH_PROBE_EXPORT
#endif

namespace {
thread_local char t_probe[1536];
thread_local size_t t_used;

__attribute__((format(printf, 1, 2))) void put(const char *fmt, ...) {
    const size_t cap = sizeof t_probe;
    if (t_used >= cap - 1) return;
    va_list args;
    va_start(args, fmt);
    int w = vsnprintf(t_probe + t_used, cap - t_used, fmt, args);
    va_end(args);
    if (w > 0) t_used += (size_t)w < cap - t_used ? (size_t)w : cap - t_used - 1;
}

// Guest address a..a+3 readable without trapping again.
bool guest_readable(uint32_t a) {
    if (!g_mem || a < GUEST_NULL_LIMIT || a > GUEST_SIZE - 4u) return false;
#ifdef __EMSCRIPTEN__
    const uintptr_t at = (uintptr_t)g_mem + a;
    return at + 4u <= (uintptr_t)emscripten_get_heap_size();
#else
    return true;
#endif
}
} // namespace

extern "C" CRASH_PROBE_EXPORT const char *recomp_crash_probe(void) {
    t_used = 0;
    t_probe[0] = 0;
    X86 *c = guest_current_context();
    if (!c) {
        put("no guest register file");
        return t_probe;
    }
    put("guest thread %u: EIP=%08x ESP=%08x EBP=%08x EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESI=%08x EDI=%08x",
        (unsigned)guest_current_thread_id(), (unsigned)c->eip, (unsigned)c->r[R_ESP], (unsigned)c->r[R_EBP],
        (unsigned)c->r[R_EAX], (unsigned)c->r[R_ECX], (unsigned)c->r[R_EDX], (unsigned)c->r[R_EBX],
        (unsigned)c->r[R_ESI], (unsigned)c->r[R_EDI]);
    put("; returns:");
    unsigned found = 0;
    const uint32_t esp = c->r[R_ESP];
    for (uint32_t i = 0; i < 512 && found < 16; ++i) {
        const uint32_t a = esp + i * 4u;
        if (a < esp || !guest_readable(a)) break;
        uint32_t v;
        memcpy(&v, g_mem + a, 4);
        if (recomp_is_call_return(v) || recomp_module_is_call_return(v)) {
            put(" %08x", (unsigned)v);
            ++found;
        }
    }
    if (!found) put(" none found");
    const char *active = mods_active_callback_desc();
    if (active && *active) put("; mod callback: %s", active);
    return t_probe;
}
