/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* DRONE: eight sustained voices in four pairs, touched into life from eight
 * keys (or latched with HOLD). Each pair has a pitch; SPREAD detunes the
 * second voice of the pair for beating. MOD cross-modulates oscillators:
 *   ROUTE 0  within each pair (A<->B)
 *   ROUTE 1  each pair modulated by the next pair (a ring, 4 -> 1)
 *   ROUTE 2  every voice by the summed drone (chaotic feedback)
 *   ROUTE 3  by the drone's own slow LFO
 * SHARP sets the touch response from slow swells to sharp plucks.
 * WAVE morphs sine -> triangle -> folded. Run it through the FX bus
 * (drive + modulated delay) for the full noise-drone character. */
#include "unit.h"
#include "dsp.h"

#define VOICES 8
#define SUB 16

struct dvoice { float phase, out, env, inc; uint8_t gate; };
static struct dvoice dv[VOICES];
static uint8_t raw[UNIT_PARAMS];
static float pair_pitch[4], mod_depth, spread_semis, attack_coef, release_coef, wave, lfo_inc, lfo_depth, level;
static unsigned route;
static float lfo_phase, sum_prev;

FM1_AUDIO_CODE void drone_param(unsigned p, unsigned v)
{
    if (p >= UNIT_PARAMS) return;
    raw[p] = (uint8_t)v;
    float x = d_unit(v);
    switch (p) {
    case D_PITCH1: case D_PITCH2: case D_PITCH3: case D_PITCH4:
        pair_pitch[p - D_PITCH1] = 24.0f + x * 60.0f; break;      /* C1 .. C6 */
    case D_MOD: mod_depth = x * x * 2.5f; break;
    case D_ROUTE: route = v * 4U / 128U; break;
    case D_SPREAD: spread_semis = x * x * 12.0f; break;
    case D_SHARP:
        attack_coef = d_coef(2.5f * d_exp2(-x * 9.0f));            /* 2.5 s .. 5 ms */
        release_coef = d_coef(4.0f * d_exp2(-x * 6.0f));           /* 4 s .. 60 ms */
        break;
    case D_WAVE: wave = x; break;
    case D_LFORATE: lfo_inc = 0.02f * d_exp2(x * 9.0f) / DSP_RATE; break;
    case D_LFODEPTH: lfo_depth = x; break;
    case D_LEVEL: level = x; break;
    default: break;
    }
}
FM1_AUDIO_CODE void drone_init(void)
{
    for (unsigned p = 0; p < UNIT_PARAMS; ++p) drone_param(p, unit_defaults[MOD_DRONE][p]);
    for (unsigned i = 0; i < VOICES; ++i) dv[i].phase = (float)i * 0.137f;
}
FM1_AUDIO_CODE void drone_note(unsigned voice, unsigned on)
{
    if (voice < VOICES) dv[voice].gate = on ? 1U : 0U;
}
FM1_AUDIO_CODE void drone_off(void)
{
    for (unsigned i = 0; i < VOICES; ++i) dv[i].gate = 0;
}
FM1_AUDIO_CODE unsigned drone_active(void)
{
    unsigned m = 0;
    for (unsigned i = 0; i < VOICES; ++i) if (dv[i].gate || dv[i].env > 0.001f) m |= 1U << i;
    return m;
}
DSP_INLINE float shape(float phase, float w)
{
    float s = d_sin(phase);
    if (w < 0.5f) return s + (d_tri(phase) - s) * (w * 2.0f);
    float t = d_tri(phase);
    return d_fold(t * (1.0f + (w - 0.5f) * 6.0f));
}
FM1_AUDIO_CODE void drone_render(float *mix, const struct unit_mod *mod)
{
    unsigned any = 0;
    for (unsigned i = 0; i < VOICES; ++i) if (dv[i].gate || dv[i].env > 0.0005f) any = 1;
    if (!any) { sum_prev = 0.0f; return; }
    float gain = level * 0.3f;
    for (unsigned base = 0; base < UNIT_BLOCK; base += SUB) {
        float lfo = d_sin(lfo_phase);
        lfo_phase += lfo_inc * (float)SUB; if (lfo_phase >= 1.0f) lfo_phase -= 1.0f;
        float vib = lfo * lfo_depth * 0.3f;
        for (unsigned i = 0; i < VOICES; ++i) {
            float p = pair_pitch[i >> 1] + ((i & 1U) ? spread_semis : 0.0f) + vib + mod->pitch;
            float inc = d_midi_hz(p) / DSP_RATE;
            dv[i].inc = inc > 0.45f ? 0.45f : inc;
        }
        float depth = mod_depth * (1.0f + mod->tone) * (route == 3 ? (0.5f + 0.5f * lfo) : 1.0f);
        for (unsigned j = 0; j < SUB; ++j) {
            float sum = 0.0f;
            for (unsigned i = 0; i < VOICES; ++i) {
                struct dvoice *v = &dv[i];
                v->env += (v->gate ? attack_coef : release_coef) * ((v->gate ? 1.0f : 0.0f) - v->env);
                if (v->env < 0.00001f && !v->gate) { v->env = 0.0f; v->out = 0.0f; continue; }
                float m;
                if (route == 0) m = dv[i ^ 1U].out;
                else if (route == 1) {
                    unsigned next = (((i >> 1) + 1U) & 3U) * 2U;
                    m = (dv[next].out + dv[next + 1U].out) * 0.5f;
                }
                else if (route == 2) m = sum_prev;
                else m = dv[i ^ 1U].out;
                float s = shape(v->phase + m * depth * 0.25f, wave);
                v->phase += v->inc; if (v->phase >= 1.0f) v->phase -= 1.0f;
                v->out = s * v->env;
                sum += v->out;
            }
            sum_prev = d_sat(sum * 0.35f);
            mix[base + j] += sum * gain;
        }
    }
}
