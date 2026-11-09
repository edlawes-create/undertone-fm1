/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* PERC: West Coast percussion. Each strike pings a complex oscillator
 * (sine modulator FM-ing a sine carrier, then a wavefolder) through a
 * vactrol-style low-pass gate. The gate's filter and level fall together,
 * so the sound darkens as it decays. Optional pitch sweep, noise burst and
 * BOUNCE retriggers (strikes that speed up and fade, like a dropped ball).
 * Every key is a pitch; SPREAD sets how far apart neighbouring keys are. */
#include "unit.h"
#include "dsp.h"

#define VOICES 6
#define SUB 16

struct hit {
    float car, modp, env, penv, gain, base, vel;
    struct d_svf f;
    float bounce_gap, bounce_left, bounce_level;
    uint32_t age;
    uint8_t note, active, bounces;
};
static struct hit hits[VOICES];
static uint8_t raw[UNIT_PARAMS];
static float tune, ratio, fm, fold, decay_coef, tone, sweep, noise_amt, spread, velo, level;
static unsigned bounce_count;
static uint32_t noise_state = 0x1234567U, age_counter;

FM1_AUDIO_CODE void perc_param(unsigned p, unsigned v)
{
    if (p >= UNIT_PARAMS) return;
    raw[p] = (uint8_t)v;
    float x = d_unit(v);
    switch (p) {
    case P_TUNE: tune = (x - 0.5f) * 48.0f; break;                    /* +-24 semitones */
    case P_RATIO: {
        /* musical ratios first, then inharmonic territory */
        static const float ratios[9] FM1_AUDIO_DATA = {0.5f, 1.0f, 1.5f, 2.0f, 2.76f, 3.0f, 3.53f, 4.2f, 5.4f};
        float pos = x * 8.0f; int i = d_floor(pos); if (i > 7) i = 7;
        float f = pos - (float)i;
        ratio = ratios[i] + (ratios[i + 1] - ratios[i]) * f;
        break;
    }
    case P_FM: fm = x * x * 6.0f; break;
    case P_FOLD: fold = x; break;
    case P_DECAY: decay_coef = d_coef(0.015f + x * x * 2.0f); break;  /* 15 ms .. 2 s */
    case P_TONE: tone = x; break;
    case P_SWEEP: sweep = x * x * 36.0f; break;                        /* semitones at strike */
    case P_NOISE: noise_amt = x; break;
    case P_SPREAD: spread = x * 2.0f; break;                           /* 1.0 = chromatic */
    case P_BOUNCE: bounce_count = v * 9U / 128U; break;                /* 0..8 retriggers */
    case P_VELO: velo = x; break;
    case P_LEVEL: level = x; break;
    default: break;
    }
}
FM1_AUDIO_CODE void perc_init(void)
{
    for (unsigned p = 0; p < UNIT_PARAMS; ++p) perc_param(p, unit_defaults[MOD_PERC][p]);
}
FM1_AUDIO_CODE void perc_off(void)
{
    for (unsigned i = 0; i < VOICES; ++i) hits[i].bounces = 0;
}
static FM1_AUDIO_CODE void strike(struct hit *h, float gain)
{
    h->env = 1.0f;
    h->penv = 1.0f;
    h->gain = gain;
    h->active = 1;
    h->f.ic1 = h->f.ic2 = 0.0f;
}
FM1_AUDIO_CODE void perc_note(unsigned note, unsigned velocity)
{
    if (!velocity) return;   /* percussion ignores note-off */
    struct hit *h = 0;
    for (unsigned i = 0; i < VOICES && !h; ++i) if (hits[i].active && hits[i].note == note) h = &hits[i];
    for (unsigned i = 0; i < VOICES && !h; ++i) if (!hits[i].active) h = &hits[i];
    if (!h) { h = &hits[0]; for (unsigned i = 1; i < VOICES; ++i) if (hits[i].age < h->age) h = &hits[i]; }
    h->note = (uint8_t)note;
    h->age = ++age_counter;
    h->vel = 1.0f - velo + velo * d_unit(velocity);
    h->car = 0.0f; h->modp = 0.0f;
    h->bounces = (uint8_t)bounce_count;
    h->bounce_gap = 0.16f * DSP_RATE;
    h->bounce_left = h->bounce_gap;
    h->bounce_level = 1.0f;
    strike(h, 1.0f);
}
FM1_AUDIO_CODE void perc_render(float *mix, const struct unit_mod *mod)
{
    float fold_amt = d_clamp(fold + mod->tone * 0.5f, 0.0f, 1.0f);
    float fold_gain = 1.0f + fold_amt * 5.0f;
    float out_gain = level * 0.9f;
    for (unsigned vi = 0; vi < VOICES; ++vi) {
        struct hit *h = &hits[vi];
        if (!h->active) continue;
        for (unsigned base = 0; base < UNIT_BLOCK; base += SUB) {
            /* bounce scheduling */
            if (h->bounces) {
                h->bounce_left -= (float)SUB;
                if (h->bounce_left <= 0.0f) {
                    h->bounce_gap *= 0.72f;
                    h->bounce_left = h->bounce_gap;
                    h->bounce_level *= 0.7f;
                    --h->bounces;
                    strike(h, h->bounce_level);
                }
            }
            float pitch = 48.0f + tune + ((float)h->note - 60.0f) * spread + sweep * h->penv * h->penv + mod->pitch;
            float inc = d_midi_hz(pitch) / DSP_RATE;
            if (inc > 0.45f) inc = 0.45f;
            float minc = inc * ratio;
            if (minc > 0.45f) minc = 0.45f;
            /* low-pass gate: cutoff rides the vactrol envelope */
            float open = h->env * (0.35f + tone * 0.65f);
            d_svf_set(&h->f, 40.0f * d_exp2(open * 9.0f + tone * 1.5f + mod->tone * 2.0f), 0.15f);
            float index = fm * h->env;
            for (unsigned j = 0; j < SUB; ++j) {
                float m = d_sin(h->modp) * index;
                float s = d_sin(h->car + m);
                h->car += inc; if (h->car >= 1.0f) h->car -= 1.0f;
                h->modp += minc; if (h->modp >= 1.0f) h->modp -= 1.0f;
                if (fold_amt > 0.002f) s = d_fold(s * fold_gain);
                float e3 = h->env * h->env * h->env;
                s += d_noise(&noise_state) * noise_amt * e3 * 1.5f;
                float y = d_svf_lp(&h->f, s);
                mix[base + j] += y * h->env * h->gain * h->vel * out_gain;
                h->env -= decay_coef * h->env;
                h->penv -= h->penv * 0.0015f;
            }
            if (h->env < 0.0002f && !h->bounces) { h->active = 0; break; }
        }
    }
}
