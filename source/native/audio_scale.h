#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void nfsc_audio_scale(X86 *c);
void nfsc_audio_mix(X86 *c);
#ifdef __cplusplus
}
#endif
#define FN_008ad12e nfsc_audio_scale
#define FN_008ad305 nfsc_audio_mix

// The configured native header also publishes the geometry entry override.
#include "geometry.h"
// And the view-frustum box test, its box transform and _ftol2 (cull.cpp, ftol.cpp).
#include "cull.h"
