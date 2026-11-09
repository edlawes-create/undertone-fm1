/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Undertone unit core: event queues, global LFO, shared FX bus, output. */
#include "unit.h"
#include "dsp.h"

#define QUEUE 128U   /* power of two */
enum { EV_NOTE = 1, EV_PARAM, EV_OFF };

/* Defaults: 0..127 per parameter. Unused slots stay 0. */
const uint8_t unit_defaults[MOD_COUNT][UNIT_PARAMS] FM1_AUDIO_DATA = {
    /* VOICE: wave fold cutoff reso | coast envamt drive glide | div1 div2 div3 close |
     *        lvl0 lvl1 lvl2 lvl3 | mode scale root detune | A D S R */
    { 0, 0, 80, 30,   0, 80, 0, 0,   23, 38, 0, 127,   100, 89, 76, 0,   127, 0, 0, 64,   10, 70, 90, 70 },
    /* PERC: tune ratio fm fold | decay tone sweep noise | spread bounce velo level */
    { 70, 16, 40, 10,   40, 60, 50, 10,   40, 0, 80, 100 },
    /* DRONE: pitch1..4 | mod route spread sharp | wave lforate lfodepth level */
    { 44, 59, 70, 85,   30, 0, 15, 30,   20, 30, 20, 100 },
    /* FX: drive time feedback mix | modrate moddepth tone send | lforate lfodepth lfotarget lfoshape */
    { 0, 70, 40, 25,   30, 10, 100, 100,   40, 0, 0, 0 },
};

struct queue { volatile uint32_t data[QUEUE]; volatile unsigned write, read; };
static struct queue queues[2];   /* [SRC_PANEL], [SRC_MIDI] */

DSP_INLINE void barrier(void)
{
#ifndef FM1_HOST_TEST
    __asm__ volatile("csync" ::: "memory");
#endif
}
static int push(unsigned source, uint32_t event)
{
    if (source > SRC_MIDI) return -1;
    struct queue *q = &queues[source];
    unsigned w = q->write;
    if (w - q->read >= QUEUE) return -2;
    q->data[w & (QUEUE - 1U)] = event;
    barrier();
    q->write = w + 1U;
    return 0;
}
int unit_note(unsigned source, unsigned module, unsigned note, unsigned velocity)
{
    if (module > MOD_DRONE || note > 127U || velocity > 127U) return -1;
    return push(source, EV_NOTE | (module << 4) | (note << 8) | (velocity << 16));
}
int unit_param(unsigned source, unsigned module, unsigned param, unsigned value)
{
    if (module >= MOD_COUNT || param >= UNIT_PARAMS || value > 127U) return -1;
    return push(source, EV_PARAM | (module << 4) | (param << 8) | (value << 16));
}
int unit_all_off(unsigned source) { return push(source, EV_OFF); }

volatile unsigned unit_midi_module;
int unit_midi(const uint8_t *m, size_t n)
{
    if (!m || !n || m[0] < 0x80U || m[0] >= 0xf0U) return 0;
    unsigned status = m[0] & 0xf0U, module = unit_midi_module;
    if (module > MOD_DRONE) module = MOD_VOICE;
    if (n >= 3 && status == 0x90U) (void)unit_note(SRC_MIDI, module, m[1] & 127U, m[2] & 127U);
    else if (n >= 3 && status == 0x80U) (void)unit_note(SRC_MIDI, module, m[1] & 127U, 0);
    else if (n >= 3 && status == 0xb0U && (m[1] == 120U || m[1] == 123U)) (void)unit_all_off(SRC_MIDI);
    return 1;   /* every channel message belongs to the unit */
}
static volatile unsigned drone_mask;
unsigned unit_drone_active(void) { return drone_mask; }
void unit_init(void) {}

/* ---------- audio side ---------- */
static uint8_t fx[UNIT_PARAMS];
static uint8_t started;
static float mix[UNIT_BLOCK];
#define DELAY 32768U              /* 0.74 s at 44.1 kHz, power of two */
static int16_t delay_line[DELAY];
static unsigned delay_write;
static float fb_state, tone_coef, drive_gain, drive_norm, mix_wet, mix_dry, feedback, send;
static float time_samples, mod_depth, mod_phase, mod_inc;
static float lfo_phase, lfo_inc, lfo_depth, lfo_hold;
static unsigned lfo_target, lfo_shape;
static uint32_t lfo_noise = 0x9e3779b9U;

static FM1_AUDIO_CODE void fx_param(unsigned p, unsigned v)
{
    fx[p] = (uint8_t)v;
    float x = d_unit(v);
    switch (p) {
    case F_DRIVE: drive_gain = 1.0f + x * x * 12.0f; drive_norm = 1.0f / d_sat(drive_gain * 0.5f) * 0.5f; break;
    case F_TIME: time_samples = 0.010f * d_exp2(x * 6.2f) * DSP_RATE; break;   /* 10 ms .. 735 ms */
    case F_FEEDBACK: feedback = x * 0.95f; break;
    case F_MIX: mix_wet = x; mix_dry = 1.0f - 0.5f * x; break;
    case F_MODRATE: mod_inc = 0.05f * d_exp2(x * 7.0f) / DSP_RATE; break;      /* 0.05 .. 6.4 Hz */
    case F_MODDEPTH: mod_depth = x * x * 0.012f * DSP_RATE; break;             /* up to 12 ms */
    case F_TONE: tone_coef = d_clamp(d_coef(1.0f / (6.2832f * (300.0f * d_exp2(x * 6.0f)))), 0.01f, 1.0f); break;
    case F_SEND: send = x; break;
    case F_LFORATE: lfo_inc = 0.03f * d_exp2(x * 9.5f) / DSP_RATE; break;      /* 0.03 .. 22 Hz */
    case F_LFODEPTH: lfo_depth = x * x; break;
    case F_LFOTARGET: lfo_target = v * 3U / 128U; break;                       /* pitch, tone, fx time */
    case F_LFOSHAPE: lfo_shape = v * 4U / 128U; break;                         /* sine, tri, square, s&h */
    default: break;
    }
}
static FM1_AUDIO_CODE void start(void)
{
    voice_init(); perc_init(); drone_init();
    for (unsigned p = 0; p < UNIT_PARAMS; ++p) fx_param(p, unit_defaults[MOD_FX][p]);
    fb_state = 0.0f;
    started = 1;
}
static FM1_AUDIO_CODE void dispatch(uint32_t e)
{
    unsigned type = e & 15U, module = (e >> 4) & 15U, a = (e >> 8) & 127U, b = (e >> 16) & 127U;
    if (type == EV_OFF) { voice_off(); perc_off(); drone_off(); return; }
    if (type == EV_NOTE) {
        if (module == MOD_VOICE) voice_note(a, b);
        else if (module == MOD_PERC) perc_note(a, b);
        else if (module == MOD_DRONE) {
            /* Eight white keys F3..F4 are the eight drone voices. */
            static const uint8_t keys[8] FM1_AUDIO_DATA = {53, 55, 57, 59, 60, 62, 64, 65};
            for (unsigned i = 0; i < 8; ++i) if (keys[i] == a) drone_note(i, b != 0);
        }
        return;
    }
    if (type == EV_PARAM) {
        if (module == MOD_VOICE) voice_param(a, b);
        else if (module == MOD_PERC) perc_param(a, b);
        else if (module == MOD_DRONE) drone_param(a, b);
        else fx_param(a, b);
    }
}
static FM1_AUDIO_CODE void drain(struct queue *q)
{
    unsigned r = q->read;
    while (r != q->write) {
        barrier();
        dispatch(q->data[r & (QUEUE - 1U)]);
        ++r;
    }
    q->read = r;
}
static FM1_AUDIO_CODE float lfo_value(void)
{
    float v, p = lfo_phase;
    if (lfo_shape == 0) v = d_sin(p);
    else if (lfo_shape == 1) v = d_tri(p);
    else if (lfo_shape == 2) v = p < 0.5f ? 1.0f : -1.0f;
    else v = lfo_hold;
    lfo_phase = p + lfo_inc * (float)UNIT_BLOCK;
    if (lfo_phase >= 1.0f) { lfo_phase -= 1.0f; lfo_hold = d_noise(&lfo_noise); }
    return v * lfo_depth;
}
FM1_AUDIO_CODE void unit_render_add(int32_t *output)
{
    if (!output) return;
    if (!started) start();
    drain(&queues[SRC_PANEL]);
    drain(&queues[SRC_MIDI]);

    float lfo = lfo_value();
    struct unit_mod mod;
    mod.pitch = lfo_target == 0 ? lfo * 12.0f : 0.0f;
    mod.tone = lfo_target == 1 ? lfo : 0.0f;
    float lfo_time = lfo_target == 2 ? lfo * 0.5f : 0.0f;

    voice_render(mix, &mod);
    perc_render(mix, &mod);
    drone_render(mix, &mod);
    drone_mask = drone_active();

    float base_time = time_samples * (1.0f + lfo_time);
    for (unsigned j = 0; j < UNIT_BLOCK; ++j) {
        float dry = mix[j];
        mix[j] = 0.0f;
        if (drive_gain > 1.01f) dry = d_sat(dry * drive_gain) * drive_norm * 2.0f;
        /* modulated delay, linear interpolation */
        float m = base_time + mod_depth * (1.0f + d_sin(mod_phase));
        mod_phase += mod_inc; if (mod_phase >= 1.0f) mod_phase -= 1.0f;
        if (m < 2.0f) m = 2.0f;
        if (m > (float)(DELAY - 4U)) m = (float)(DELAY - 4U);
        int whole = (int)m;
        float frac = m - (float)whole;
        unsigned i0 = (delay_write - (unsigned)whole) & (DELAY - 1U);
        unsigned i1 = (i0 - 1U) & (DELAY - 1U);
        float a = (float)delay_line[i0], b = (float)delay_line[i1];
        float wet = (a + (b - a) * frac) * (1.0f / 32767.0f);
        fb_state += tone_coef * (wet - fb_state);
        float in = d_sat(dry * send + fb_state * feedback);
        delay_line[delay_write] = (int16_t)(in * 32767.0f);
        delay_write = (delay_write + 1U) & (DELAY - 1U);
        float y = dry * mix_dry + fb_state * mix_wet;
        /* ~3x gain into a soft limiter: single notes sit near -10 dBFS,
         * dense chords and drones saturate gently instead of clipping. */
        float f = d_sat(y * 2.0f) * (0.7f * 8388607.0f);
        int32_t add = (int32_t)f;
        for (unsigned c = 0; c < 2; ++c) {
            int32_t s = output[2 * j + c] + add;
            output[2 * j + c] = s > 8388607 ? 8388607 : s < -8388608 ? -8388608 : s;
        }
    }
}
