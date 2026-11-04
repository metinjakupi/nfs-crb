#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void nfsc_frustum_cull(X86 *c);
void nfsc_box_transform(X86 *c);
void nfsc_ftol2(X86 *c);
#ifdef __cplusplus
}
#endif
#define FN_0071b630 nfsc_frustum_cull
#define FN_0070f0b0 nfsc_box_transform
#define FN_0087eac0 nfsc_ftol2
