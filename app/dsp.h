/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Small float DSP helpers for the audio interrupt. Everything here inlines:
 * no libc, no 64-bit maths, no signed modulo, single-precision only. */
#ifndef UNIT_DSP_H
#define UNIT_DSP_H
#include <stdint.h>

#define DSP_INLINE static inline __attribute__((always_inline))
#define DSP_RATE 44100.0f
#define DSP_PI 3.14159265f

DSP_INLINE float d_abs(float x) { return x < 0.0f ? -x : x; }
DSP_INLINE float d_min(float a, float b) { return a < b ? a : b; }
DSP_INLINE float d_max(float a, float b) { return a > b ? a : b; }
DSP_INLINE float d_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
DSP_INLINE int d_floor(float x) { int i = (int)x; return (float)i > x ? i - 1 : i; }
DSP_INLINE float d_frac(float x) { return x - (float)d_floor(x); }
DSP_INLINE float d_exp2(float x)
{
    if (x < -30.0f) x = -30.0f;
    if (x > 30.0f) x = 30.0f;
    int i = d_floor(x);
    float f = x - (float)i;
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)(i + 127) << 23;
    return p * v.f;
}
/* tan(x) for 0 <= x <= ~1.3, Pade approximant. */
DSP_INLINE float d_tan(float x) { float x2 = x * x; return x * (15.0f - x2) / (15.0f - 6.0f * x2); }
/* sin(2*pi*phase) for any phase; 5th-order odd polynomial on a folded triangle. */
DSP_INLINE float d_sin(float phase)
{
    float t = d_frac(phase + 0.25f);            /* cos-aligned triangle */
    float x = 1.0f - 4.0f * d_abs(t - 0.5f);    /* -1..1, +1 at phase 0.25 */
    float x2 = x * x;
    return x * (1.5707963f + x2 * (-0.6459640f + x2 * 0.0796926f));
}
DSP_INLINE float d_tri(float phase) { return 1.0f - 4.0f * d_abs(d_frac(phase) - 0.5f); }
/* Triangle wavefolder: identity on [-1, 1], reflects beyond. */
DSP_INLINE float d_fold(float x)
{
    float t = (x + 1.0f) * 0.25f + 8.0f;
    t -= (float)d_floor(t);
    return 1.0f - 4.0f * d_abs(t - 0.5f);
}
/* Soft clip, rational tanh-like; exact at 0, saturates to +-1. */
DSP_INLINE float d_sat(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
DSP_INLINE float d_midi_hz(float pitch) { return 440.0f * d_exp2((pitch - 69.0f) * (1.0f / 12.0f)); }
/* One-pole coefficient for a time constant in seconds. */
DSP_INLINE float d_coef(float seconds) { return 1.0f - d_exp2(-1.442695f / (seconds * DSP_RATE)); }
/* 0..127 -> 1 ms .. 8 s, exponential. */
DSP_INLINE float d_cc_time(unsigned cc) { return 0.001f * d_exp2((float)cc * (13.0f / 127.0f)); }
DSP_INLINE float d_unit(unsigned cc) { return (float)cc * (1.0f / 127.0f); }
/* xorshift32 white noise in -1..1 */
DSP_INLINE float d_noise(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}
/* PolyBLEP residual */
DSP_INLINE float d_blep(float t, float dt, float inv)
{
    if (t < dt) { t *= inv; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) * inv; return t * t + t + t + 1.0f; }
    return 0.0f;
}
/* Zero-delay-feedback state-variable low-pass. */
struct d_svf { float ic1, ic2, a1, a2, a3; };
DSP_INLINE void d_svf_set(struct d_svf *f, float hz, float reso)
{
    if (hz < 20.0f) hz = 20.0f;
    if (hz > 16000.0f) hz = 16000.0f;
    float g = d_tan(DSP_PI * hz / DSP_RATE);
    float k = 2.0f - 1.94f * reso;
    f->a1 = 1.0f / (1.0f + g * (g + k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}
DSP_INLINE float d_svf_lp(struct d_svf *f, float x)
{
    float v3 = x - f->ic2;
    float v1 = f->a1 * f->ic1 + f->a2 * v3;
    float v2 = f->ic2 + f->a2 * f->ic1 + f->a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    return v2;
}
#endif
