/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Undertone v0.1 — subharmonic voice engine for FM1 Open. See undertone.h. */
#include "undertone.h"

#define UT_INLINE static inline __attribute__((always_inline))
#define UT_RATE 44100.0f
#define UT_VOICES 4
#define UT_OSCS 4           /* main + three subharmonics */
#define UT_SUBBLOCK 16      /* filter coefficients update every 16 frames */
#define UT_QUEUE 64

enum { MODE_JI, MODE_ET, MODE_SCALE };
enum { ST_OFF, ST_ATTACK, ST_DECAY, ST_RELEASE };

/* CC map */
enum {
    CC_MOD = 1, CC_VOLUME = 7, CC_SUSTAIN = 64,
    CC_DIV1 = 20, CC_DIV2 = 21, CC_DIV3 = 22,
    CC_LVL1 = 23, CC_LVL2 = 24, CC_LVL3 = 25, CC_LVL0 = 26,
    CC_MODE = 27, CC_SCALE = 28, CC_ROOT = 29, CC_CLOSE = 30,
    CC_WAVE = 70, CC_RESO = 71, CC_RELEASE = 72, CC_ATTACK = 73, CC_CUTOFF = 74,
    CC_DECAY = 75, CC_SUSTAIN_LVL = 76, CC_ENVAMT = 77, CC_FOLD = 78,
    CC_ALL_SOUND_OFF = 120, CC_ALL_NOTES_OFF = 123
};

/* 12 * log2(d), semitones below the played note for subharmonic f/d. */
static const float sub_semis[17] FM1_AUDIO_DATA = {
    0.0f, 0.0f, 12.0f, 19.0195500f, 24.0f, 27.8631371f, 31.0195500f, 33.6882590f,
    36.0f, 38.0391000f, 39.8631371f, 41.5131794f, 43.0195500f, 44.4052766f,
    45.6882590f, 46.8826871f, 48.0f
};

#define B(x) (1U << (x))
/* Scale masks, pitch classes relative to the root. */
static const uint16_t scales[12] FM1_AUDIO_DATA = {
    B(0)|B(2)|B(4)|B(5)|B(7)|B(9)|B(11),       /* 0 major */
    B(0)|B(2)|B(3)|B(5)|B(7)|B(8)|B(10),       /* 1 natural minor */
    B(0)|B(2)|B(3)|B(5)|B(7)|B(9)|B(10),       /* 2 dorian */
    B(0)|B(1)|B(3)|B(5)|B(7)|B(8)|B(10),       /* 3 phrygian */
    B(0)|B(2)|B(4)|B(6)|B(7)|B(9)|B(11),       /* 4 lydian */
    B(0)|B(2)|B(4)|B(5)|B(7)|B(9)|B(10),       /* 5 mixolydian */
    B(0)|B(2)|B(3)|B(5)|B(7)|B(8)|B(11),       /* 6 harmonic minor */
    B(0)|B(2)|B(3)|B(5)|B(7)|B(9)|B(11),       /* 7 melodic minor */
    B(0)|B(2)|B(4)|B(7)|B(9),                  /* 8 major pentatonic */
    B(0)|B(3)|B(5)|B(7)|B(10),                 /* 9 minor pentatonic */
    B(0)|B(2)|B(4)|B(6)|B(8)|B(10),            /* 10 whole tone */
    B(0)|B(1)|B(3)|B(4)|B(6)|B(7)|B(9)|B(10)   /* 11 octatonic (half-whole) */
};
#undef B

struct osc { float phase, inc, inv; };
struct voice {
    struct osc o[UT_OSCS];
    float env, vel, ic1, ic2, a1, a2, a3;
    uint32_t age;
    uint8_t note, gate, held, stage;
};
struct params {
    uint8_t div[3], mode, scale, root, close, sustain;
    float lvl[4], wave, fold, mod, cutoff, reso, envamt, sus, volume;
    float acoef, dcoef, rcoef;
};

/* Routing task -> audio ISR, single producer / single consumer. */
static volatile uint32_t queue[UT_QUEUE];
static volatile unsigned q_write, q_read;
static struct voice voices[UT_VOICES];
static struct params P;
static uint32_t age_counter;
static float mix[UT_SUBBLOCK];   /* zeroed after each use; no memset in the ISR */
static uint8_t initialized;

UT_INLINE void barrier(void)
{
#ifndef FM1_HOST_TEST
    __asm__ volatile("csync" ::: "memory");
#endif
}

/* ---------- math without libc ---------- */
UT_INLINE float ut_abs(float x) { return x < 0.0f ? -x : x; }
UT_INLINE int ut_floor(float x) { int i = (int)x; return (float)i > x ? i - 1 : i; }
UT_INLINE float ut_exp2(float x)
{
    if (x < -30.0f) x = -30.0f;
    if (x > 30.0f) x = 30.0f;
    int i = ut_floor(x);
    float f = x - (float)i;
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)(i + 127) << 23;
    return p * v.f;
}
/* tan(x) for 0 <= x <= ~1.25, Pade approximant. */
UT_INLINE float ut_tan(float x) { float x2 = x * x; return x * (15.0f - x2) / (15.0f - 6.0f * x2); }
UT_INLINE float midi_hz(float pitch) { return 440.0f * ut_exp2((pitch - 69.0f) * (1.0f / 12.0f)); }
/* One-pole coefficient for a time constant in seconds. */
UT_INLINE float time_coef(float seconds) { return 1.0f - ut_exp2(-1.442695f / (seconds * UT_RATE)); }
UT_INLINE float cc_time(unsigned cc) { return 0.001f * ut_exp2((float)cc * (13.0f / 127.0f)); } /* 1 ms .. 8 s */

/* ---------- harmony ---------- */
static FM1_AUDIO_CODE float snap_to_scale(float target)
{
    unsigned mask = scales[P.scale];
    int base = ut_floor(target), best = base;
    float best_distance = 1.0e9f;
    for (int c = base - 6; c <= base + 7; ++c) {
        int pc = c - (int)P.root;
        while (pc < 0) pc += 12;
        while (pc >= 12) pc -= 12;
        if (!(mask & (1U << pc))) continue;
        float d = ut_abs((float)c - target);
        if (d < best_distance) { best_distance = d; best = c; }
    }
    return (float)best;
}
static FM1_AUDIO_CODE float sub_pitch(unsigned note, unsigned divisor)
{
    float pure = (float)note - sub_semis[divisor], p;
    if (P.mode == MODE_JI) p = pure;
    else if (P.mode == MODE_ET) p = (float)ut_floor(pure + 0.5f);
    else p = snap_to_scale(pure);
    if (P.close) while (p < (float)note - 12.0f) p += 12.0f;
    return p;
}
static FM1_AUDIO_CODE void set_osc(struct osc *o, float pitch)
{
    float hz = midi_hz(pitch);
    if (hz < 1.0f) hz = 1.0f;
    if (hz > 18000.0f) hz = 18000.0f;
    o->inc = hz / UT_RATE;
    o->inv = 1.0f / o->inc;
}
static FM1_AUDIO_CODE void tune_voice(struct voice *v)
{
    set_osc(&v->o[0], (float)v->note);
    for (unsigned i = 0; i < 3; ++i)
        set_osc(&v->o[i + 1], P.div[i] ? sub_pitch(v->note, P.div[i]) : (float)v->note);
}
static FM1_AUDIO_CODE void retune_all(void)
{
    for (unsigned i = 0; i < UT_VOICES; ++i) if (voices[i].stage != ST_OFF) tune_voice(&voices[i]);
}

/* ---------- parameters ---------- */
static FM1_AUDIO_CODE void defaults(void)
{
    P.div[0] = 3; P.div[1] = 5; P.div[2] = 2;
    P.lvl[0] = 0.8f; P.lvl[1] = 0.7f; P.lvl[2] = 0.6f; P.lvl[3] = 0.0f;
    P.mode = MODE_SCALE; P.scale = 0; P.root = 0; P.close = 1; P.sustain = 0;
    P.wave = 0.0f; P.fold = 0.0f; P.mod = 0.0f;
    P.cutoff = 90.0f / 127.0f; P.reso = 0.2f; P.envamt = 0.25f; P.sus = 0.7f; P.volume = 100.0f / 127.0f;
    P.acoef = time_coef(0.005f); P.dcoef = time_coef(0.4f); P.rcoef = time_coef(0.3f);
}
static FM1_AUDIO_CODE void release_voice(struct voice *v) { v->gate = 0; v->held = 0; if (v->stage != ST_OFF) v->stage = ST_RELEASE; }
static FM1_AUDIO_CODE void control(unsigned cc, unsigned value)
{
    float x = (float)value * (1.0f / 127.0f);
    switch (cc) {
    case CC_MOD: P.mod = x; break;
    case CC_VOLUME: P.volume = x; break;
    case CC_SUSTAIN:
        P.sustain = value >= 64;
        if (!P.sustain) for (unsigned i = 0; i < UT_VOICES; ++i) if (voices[i].held) release_voice(&voices[i]);
        break;
    case CC_DIV1: case CC_DIV2: case CC_DIV3: P.div[cc - CC_DIV1] = (uint8_t)(value * 17U / 128U); retune_all(); break;
    case CC_LVL1: case CC_LVL2: case CC_LVL3: P.lvl[cc - CC_LVL1 + 1] = x; break;
    case CC_LVL0: P.lvl[0] = x; break;
    case CC_MODE: P.mode = (uint8_t)(value / 43U); retune_all(); break;
    case CC_SCALE: P.scale = (uint8_t)(value * 12U / 128U); retune_all(); break;
    case CC_ROOT: P.root = (uint8_t)(value * 12U / 128U); retune_all(); break;
    case CC_CLOSE: P.close = value >= 64; retune_all(); break;
    case CC_WAVE: P.wave = x; break;
    case CC_RESO: P.reso = x; break;
    case CC_RELEASE: P.rcoef = time_coef(cc_time(value)); break;
    case CC_ATTACK: P.acoef = time_coef(cc_time(value)); break;
    case CC_CUTOFF: P.cutoff = x; break;
    case CC_DECAY: P.dcoef = time_coef(cc_time(value)); break;
    case CC_SUSTAIN_LVL: P.sus = x; break;
    case CC_ENVAMT: P.envamt = (x - 0.5f) * 2.0f; break;
    case CC_FOLD: P.fold = x; break;
    case CC_ALL_SOUND_OFF:
        for (unsigned i = 0; i < UT_VOICES; ++i) { voices[i].stage = ST_OFF; voices[i].env = 0.0f; voices[i].gate = voices[i].held = 0; }
        break;
    case CC_ALL_NOTES_OFF:
        for (unsigned i = 0; i < UT_VOICES; ++i) release_voice(&voices[i]);
        break;
    default: break;
    }
}
static FM1_AUDIO_CODE void note_on(unsigned note, unsigned velocity)
{
    struct voice *v = 0;
    for (unsigned i = 0; i < UT_VOICES && !v; ++i) if (voices[i].stage != ST_OFF && voices[i].note == note) v = &voices[i];
    for (unsigned i = 0; i < UT_VOICES && !v; ++i) if (voices[i].stage == ST_OFF) v = &voices[i];
    if (!v) {   /* steal the oldest */
        v = &voices[0];
        for (unsigned i = 1; i < UT_VOICES; ++i) if (voices[i].age < v->age) v = &voices[i];
    }
    if (v->stage == ST_OFF) {
        v->env = 0.0f; v->ic1 = v->ic2 = 0.0f;
        for (unsigned i = 0; i < UT_OSCS; ++i) v->o[i].phase = 0.0f;
    }
    v->note = (uint8_t)note; v->gate = 1; v->held = 0; v->stage = ST_ATTACK;
    v->vel = 0.25f + 0.75f * (float)velocity * (1.0f / 127.0f);
    v->age = ++age_counter;
    tune_voice(v);
}
static FM1_AUDIO_CODE void note_off(unsigned note)
{
    for (unsigned i = 0; i < UT_VOICES; ++i) {
        struct voice *v = &voices[i];
        if (v->stage == ST_OFF || v->note != note || !v->gate) continue;
        if (P.sustain) { v->gate = 0; v->held = 1; } else release_voice(v);
    }
}
static FM1_AUDIO_CODE void process_events(void)
{
    unsigned r = q_read;
    while (r != q_write) {
        barrier();
        uint32_t e = queue[r % UT_QUEUE];
        unsigned status = e & 0xf0U, d1 = (e >> 8) & 0x7fU, d2 = (e >> 16) & 0x7fU;
        if (status == 0x90U && d2) note_on(d1, d2);
        else if (status == 0x80U || status == 0x90U) note_off(d1);
        else if (status == 0xb0U) control(d1, d2);
        else if (status == 0xd0U) P.mod = (float)d1 * (1.0f / 127.0f);   /* channel aftertouch */
        ++r;
    }
    q_read = r;
}

/* ---------- MIDI routing task ---------- */
int undertone_midi_accept(const uint8_t *m, size_t n)
{
    if (!m || !n || m[0] < 0x80U || m[0] >= 0xf0U) return 0;
    if ((m[0] & 15U) != UNDERTONE_CHANNEL) return 0;
    unsigned status = m[0] & 0xf0U;
    if (status != 0x80U && status != 0x90U && status != 0xb0U && status != 0xd0U) return 1; /* ours, ignored */
    unsigned w = q_write;
    if (w - q_read >= UT_QUEUE) return 1;   /* full: drop */
    queue[w % UT_QUEUE] = (uint32_t)m[0] | ((uint32_t)(n > 1 ? m[1] : 0) << 8) | ((uint32_t)(n > 2 ? m[2] : 0) << 16);
    barrier();
    q_write = w + 1;
    return 1;
}

/* ---------- audio ---------- */
UT_INLINE float blep(float t, float dt, float inv)
{
    if (t < dt) { t *= inv; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) * inv; return t * t + t + t + 1.0f; }
    return 0.0f;
}
UT_INLINE float osc_tick(struct osc *o, float wave)
{
    float t = o->phase, dt = o->inc, inv = o->inv, s;
    if (wave < 0.5f) {
        float saw = 2.0f * t - 1.0f - blep(t, dt, inv);
        float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
        float sq = (t < 0.5f ? 1.0f : -1.0f) + blep(t, dt, inv) - blep(t2, dt, inv);
        float k = wave * 2.0f;
        s = saw + (sq - saw) * k;
    } else {
        float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
        float sq = (t < 0.5f ? 1.0f : -1.0f) + blep(t, dt, inv) - blep(t2, dt, inv);
        float tri = 1.0f - 4.0f * ut_abs(t - 0.5f);
        float k = (wave - 0.5f) * 2.0f;
        s = sq + (tri - sq) * k;
    }
    t += dt; if (t >= 1.0f) t -= 1.0f;
    o->phase = t;
    return s;
}
/* Triangle wavefolder: identity on [-1, 1], reflects beyond. */
UT_INLINE float fold(float x)
{
    float t = (x + 1.0f) * 0.25f + 8.0f;
    t -= (float)ut_floor(t);
    return 1.0f - 4.0f * ut_abs(t - 0.5f);
}
static FM1_AUDIO_CODE void filter_coefs(struct voice *v)
{
    float octaves = P.cutoff * 10.0f + P.envamt * v->env * 7.0f + ((float)v->note - 60.0f) * (0.5f / 12.0f)
                    + P.mod * 2.0f;
    float hz = 20.0f * ut_exp2(octaves);
    if (hz < 20.0f) hz = 20.0f;
    if (hz > 16000.0f) hz = 16000.0f;
    float g = ut_tan(3.14159265f * hz / UT_RATE);
    float k = 2.0f - 1.94f * P.reso;
    v->a1 = 1.0f / (1.0f + g * (g + k));
    v->a2 = g * v->a1;
    v->a3 = g * v->a2;
}
FM1_AUDIO_CODE void undertone_render_add(int32_t *output)
{
    if (!output) return;
    if (!initialized) { defaults(); initialized = 1; }
    process_events();

    float lvl_sum = P.lvl[0];
    for (unsigned i = 0; i < 3; ++i) if (P.div[i]) lvl_sum += P.lvl[i + 1];
    float norm = lvl_sum > 1.0f ? 1.0f / lvl_sum : 1.0f;
    float lvl[UT_OSCS] = { P.lvl[0] * norm, P.div[0] ? P.lvl[1] * norm : 0.0f,
                           P.div[1] ? P.lvl[2] * norm : 0.0f, P.div[2] ? P.lvl[3] * norm : 0.0f };
    float drive = 1.0f + (P.fold + P.mod * 0.6f) * 5.0f;
    unsigned folding = P.fold > 0.002f || P.mod > 0.002f;
    float gain = P.volume * P.volume * 0.35f * 8388607.0f;
    float wave = P.wave;

    for (unsigned base = 0; base < FM1_BLOCK_FRAMES; base += UT_SUBBLOCK) {
        for (unsigned vi = 0; vi < UT_VOICES; ++vi) {
            struct voice *v = &voices[vi];
            if (v->stage == ST_OFF) continue;
            filter_coefs(v);
            for (unsigned j = 0; j < UT_SUBBLOCK; ++j) {
                /* envelope */
                if (v->stage == ST_ATTACK) {
                    v->env += P.acoef * (1.2f - v->env);
                    if (v->env >= 1.0f) { v->env = 1.0f; v->stage = ST_DECAY; }
                } else if (v->stage == ST_DECAY) {
                    v->env += P.dcoef * (P.sus - v->env);
                } else {
                    v->env -= P.rcoef * v->env;
                    if (v->env < 0.0001f) { v->env = 0.0f; v->stage = ST_OFF; break; }
                }
                /* oscillators */
                float s = 0.0f;
                for (unsigned oi = 0; oi < UT_OSCS; ++oi) {
                    if (lvl[oi] > 0.0f) s += lvl[oi] * osc_tick(&v->o[oi], wave);
                    else { v->o[oi].phase += v->o[oi].inc; if (v->o[oi].phase >= 1.0f) v->o[oi].phase -= 1.0f; }
                }
                if (folding) s = fold(s * drive);
                /* TPT state-variable low-pass */
                float v3 = s - v->ic2;
                float v1 = v->a1 * v->ic1 + v->a2 * v3;
                float v2 = v->ic2 + v->a2 * v->ic1 + v->a3 * v3;
                v->ic1 = 2.0f * v1 - v->ic1;
                v->ic2 = 2.0f * v2 - v->ic2;
                mix[j] += v2 * v->env * v->vel;
            }
        }
        for (unsigned j = 0; j < UT_SUBBLOCK; ++j) {
            float f = mix[j] * gain;
            mix[j] = 0.0f;
            if (f > 16777215.0f) f = 16777215.0f;
            if (f < -16777215.0f) f = -16777215.0f;
            int32_t add = (int32_t)f;
            for (unsigned c = 0; c < 2; ++c) {
                int32_t y = output[2 * (base + j) + c] + add;
                output[2 * (base + j) + c] = y > 8388607 ? 8388607 : y < -8388608 ? -8388608 : y;
            }
        }
    }
}
