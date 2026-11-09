/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VOICE: subharmonic polysynth. Each note sounds the played pitch plus up to
 * three subharmonics (f/1..f/16). Harmony: JI (pure undertones), 12-ET, or
 * SCALE (subs snapped to a scale, so chord quality follows the degree).
 *
 * COAST morphs the voice architecture:
 *   East (0)  bright saw/pulse into a resonant filter swept by the envelope.
 *   West (127) the filter becomes a low-pass gate that opens with the
 *             envelope, resonance falls away, and the wavefolder takes over
 *             as the timbre source (Buchla-style "complex" tone). */
#include "unit.h"
#include "dsp.h"

#define VOICES 4
#define OSCS 4
#define SUB 16      /* control-rate interval in frames */

enum { MODE_JI, MODE_ET, MODE_SCALE };
enum { ST_OFF, ST_ATTACK, ST_DECAY, ST_RELEASE };

static const float sub_semis[17] FM1_AUDIO_DATA = {
    0.0f, 0.0f, 12.0f, 19.0195500f, 24.0f, 27.8631371f, 31.0195500f, 33.6882590f,
    36.0f, 38.0391000f, 39.8631371f, 41.5131794f, 43.0195500f, 44.4052766f,
    45.6882590f, 46.8826871f, 48.0f
};
#define B(x) (1U << (x))
static const uint16_t scales[12] FM1_AUDIO_DATA = {
    B(0)|B(2)|B(4)|B(5)|B(7)|B(9)|B(11),       /* major */
    B(0)|B(2)|B(3)|B(5)|B(7)|B(8)|B(10),       /* natural minor */
    B(0)|B(2)|B(3)|B(5)|B(7)|B(9)|B(10),       /* dorian */
    B(0)|B(1)|B(3)|B(5)|B(7)|B(8)|B(10),       /* phrygian */
    B(0)|B(2)|B(4)|B(6)|B(7)|B(9)|B(11),       /* lydian */
    B(0)|B(2)|B(4)|B(5)|B(7)|B(9)|B(10),       /* mixolydian */
    B(0)|B(2)|B(3)|B(5)|B(7)|B(8)|B(11),       /* harmonic minor */
    B(0)|B(2)|B(3)|B(5)|B(7)|B(9)|B(11),       /* melodic minor */
    B(0)|B(2)|B(4)|B(7)|B(9),                  /* major pentatonic */
    B(0)|B(3)|B(5)|B(7)|B(10),                 /* minor pentatonic */
    B(0)|B(2)|B(4)|B(6)|B(8)|B(10),            /* whole tone */
    B(0)|B(1)|B(3)|B(4)|B(6)|B(7)|B(9)|B(10)   /* octatonic */
};
#undef B

struct osc { float phase, inc, inv, target; };
struct voice {
    struct osc o[OSCS];
    struct d_svf f;
    float env, vel, pitch;
    uint32_t age;
    uint8_t note, gate, held, stage;
};
static struct voice voices[VOICES];
static uint8_t raw[UNIT_PARAMS];
static uint8_t div_[3], mode, scale_, root, close_, sustain_pedal;
static float lvl[OSCS], wave, fold, cutoff, reso, coast, envamt, drive, glide_coef, detune;
static float acoef, dcoef, rcoef, sus;
static uint32_t age_counter;

/* ---------- harmony ---------- */
static FM1_AUDIO_CODE float snap_to_scale(float target)
{
    unsigned mask = scales[scale_];
    int base = d_floor(target), best = base;
    float best_distance = 1.0e9f;
    for (int c = base - 6; c <= base + 7; ++c) {
        int pc = c - (int)root;
        while (pc < 0) pc += 12;
        while (pc >= 12) pc -= 12;
        if (!(mask & (1U << pc))) continue;
        float d = d_abs((float)c - target);
        if (d < best_distance) { best_distance = d; best = c; }
    }
    return (float)best;
}
static FM1_AUDIO_CODE float sub_pitch(float note, unsigned divisor)
{
    float pure = note - sub_semis[divisor], p;
    if (mode == MODE_JI) p = pure;
    else if (mode == MODE_ET) p = (float)d_floor(pure + 0.5f);
    else p = snap_to_scale(pure);
    if (close_) while (p < note - 12.0f) p += 12.0f;
    return p;
}
static FM1_AUDIO_CODE void retarget(struct voice *v, unsigned snap)
{
    float note = (float)v->note;
    float pitch[OSCS];
    pitch[0] = note;
    for (unsigned i = 0; i < 3; ++i) pitch[i + 1] = div_[i] ? sub_pitch(note, div_[i]) : note;
    for (unsigned i = 0; i < OSCS; ++i) {
        /* small spread between oscillators for width */
        float spread = detune * (i == 0 ? 0.0f : (i & 1U) ? 1.0f : -1.0f);
        v->o[i].target = pitch[i] + spread;
        if (snap) v->o[i].inc = 0.0f;
    }
}
static FM1_AUDIO_CODE void retune_all(void)
{
    for (unsigned i = 0; i < VOICES; ++i) if (voices[i].stage != ST_OFF) retarget(&voices[i], 0);
}

/* ---------- parameters ---------- */
FM1_AUDIO_CODE void voice_param(unsigned p, unsigned v)
{
    if (p >= UNIT_PARAMS) return;
    raw[p] = (uint8_t)v;
    float x = d_unit(v);
    switch (p) {
    case V_WAVE: wave = x; break;
    case V_FOLD: fold = x; break;
    case V_CUTOFF: cutoff = x; break;
    case V_RESO: reso = x; break;
    case V_COAST: coast = x; break;
    case V_ENVAMT: envamt = (x - 0.5f) * 2.0f; break;
    case V_DRIVE: drive = x; break;
    case V_GLIDE: glide_coef = v ? d_coef(0.002f + x * x * 1.5f) * (float)SUB : 1.0f; if (glide_coef > 1.0f) glide_coef = 1.0f; break;
    case V_DIV1: case V_DIV2: case V_DIV3: div_[p - V_DIV1] = (uint8_t)(v * 17U / 128U); retune_all(); break;
    case V_CLOSE: close_ = v >= 64U; retune_all(); break;
    case V_LVL0: case V_LVL1: case V_LVL2: case V_LVL3: lvl[p - V_LVL0] = x; break;
    case V_MODE: mode = (uint8_t)(v * 3U / 128U); retune_all(); break;
    case V_SCALE: scale_ = (uint8_t)(v * 12U / 128U); retune_all(); break;
    case V_ROOT: root = (uint8_t)(v * 12U / 128U); retune_all(); break;
    case V_DETUNE: detune = (x - 0.5f) * 0.3f; retune_all(); break;   /* +-0.15 semitone */
    case V_ATTACK: acoef = d_coef(d_cc_time(v) * 0.4f); break;
    case V_DECAY: dcoef = d_coef(d_cc_time(v) * 0.4f); break;
    case V_SUSTAIN: sus = x; break;
    case V_RELEASE: rcoef = d_coef(d_cc_time(v) * 0.4f); break;
    default: break;
    }
}
FM1_AUDIO_CODE void voice_init(void)
{
    for (unsigned p = 0; p < UNIT_PARAMS; ++p) voice_param(p, unit_defaults[MOD_VOICE][p]);
    sustain_pedal = 0;
}
static FM1_AUDIO_CODE void release_voice(struct voice *v) { v->gate = 0; if (v->stage != ST_OFF) v->stage = ST_RELEASE; }
FM1_AUDIO_CODE void voice_off(void)
{
    for (unsigned i = 0; i < VOICES; ++i) release_voice(&voices[i]);
}
FM1_AUDIO_CODE void voice_note(unsigned note, unsigned velocity)
{
    if (!velocity) {
        for (unsigned i = 0; i < VOICES; ++i)
            if (voices[i].stage != ST_OFF && voices[i].note == note && voices[i].gate) release_voice(&voices[i]);
        return;
    }
    struct voice *v = 0;
    for (unsigned i = 0; i < VOICES && !v; ++i) if (voices[i].stage != ST_OFF && voices[i].note == note) v = &voices[i];
    for (unsigned i = 0; i < VOICES && !v; ++i) if (voices[i].stage == ST_OFF) v = &voices[i];
    if (!v) { v = &voices[0]; for (unsigned i = 1; i < VOICES; ++i) if (voices[i].age < v->age) v = &voices[i]; }
    unsigned fresh = v->stage == ST_OFF;
    if (fresh) {
        v->env = 0.0f; v->f.ic1 = v->f.ic2 = 0.0f;
        for (unsigned i = 0; i < OSCS; ++i) v->o[i].phase = (float)i * 0.25f;
    }
    v->note = (uint8_t)note; v->gate = 1; v->stage = ST_ATTACK;
    v->vel = 0.3f + 0.7f * d_unit(velocity);
    v->age = ++age_counter;
    retarget(v, fresh || glide_coef >= 1.0f);
}

/* ---------- audio ---------- */
FM1_AUDIO_CODE void voice_render(float *mix, const struct unit_mod *mod)
{
    float lsum = lvl[0];
    for (unsigned i = 0; i < 3; ++i) if (div_[i]) lsum += lvl[i + 1];
    float norm = lsum > 1.0f ? 1.0f / lsum : 1.0f;
    float l[OSCS];
    l[0] = lvl[0] * norm;
    for (unsigned i = 0; i < 3; ++i) l[i + 1] = div_[i] ? lvl[i + 1] * norm : 0.0f;

    /* Coast morph: West adds fold and turns the filter into a gate. */
    float west = coast, east = 1.0f - coast;
    float fold_amt = d_clamp(fold + west * 0.35f + mod->tone * 0.5f, 0.0f, 1.0f);
    float drive_fold = 1.0f + fold_amt * 6.0f;
    unsigned folding = fold_amt > 0.002f;
    float res = reso * (1.0f - 0.8f * west);
    float wave_eff = wave;
    float pre = 1.0f + drive * 4.0f;

    for (unsigned base = 0; base < UNIT_BLOCK; base += SUB) {
        for (unsigned vi = 0; vi < VOICES; ++vi) {
            struct voice *v = &voices[vi];
            if (v->stage == ST_OFF) continue;
            /* pitch: glide towards targets, apply LFO */
            for (unsigned i = 0; i < OSCS; ++i) {
                struct osc *o = &v->o[i];
                float target_hz = d_midi_hz(o->target + mod->pitch);
                float target_inc = target_hz / DSP_RATE;
                if (target_inc > 0.40f) target_inc = 0.40f;
                if (target_inc < 0.00002f) target_inc = 0.00002f;
                if (o->inc <= 0.0f) o->inc = target_inc;
                else o->inc += (target_inc - o->inc) * glide_coef;
                o->inv = 1.0f / o->inc;
            }
            /* filter / low-pass gate */
            float east_oct = cutoff * 10.0f + envamt * v->env * 7.0f;
            float west_oct = cutoff * 5.0f + v->env * 8.5f;
            float octaves = east * east_oct + west * west_oct + ((float)v->note - 60.0f) * (0.5f / 12.0f) + mod->tone * 3.0f;
            d_svf_set(&v->f, 20.0f * d_exp2(octaves), res);

            for (unsigned j = 0; j < SUB; ++j) {
                if (v->stage == ST_ATTACK) {
                    v->env += acoef * (1.2f - v->env);
                    if (v->env >= 1.0f) { v->env = 1.0f; v->stage = ST_DECAY; }
                } else if (v->stage == ST_DECAY) {
                    v->env += dcoef * ((v->gate ? sus : 0.0f) - v->env);
                    if (!v->gate) v->stage = ST_RELEASE;
                } else {
                    v->env -= rcoef * v->env;
                    if (v->env < 0.0001f) { v->env = 0.0f; v->stage = ST_OFF; break; }
                }
                float s = 0.0f;
                for (unsigned oi = 0; oi < OSCS; ++oi) {
                    struct osc *o = &v->o[oi];
                    float t = o->phase, dt = o->inc;
                    if (l[oi] > 0.0f) {
                        float out;
                        if (wave_eff < 0.5f) {
                            float saw = 2.0f * t - 1.0f - d_blep(t, dt, o->inv);
                            float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
                            float sq = (t < 0.5f ? 1.0f : -1.0f) + d_blep(t, dt, o->inv) - d_blep(t2, dt, o->inv);
                            out = saw + (sq - saw) * (wave_eff * 2.0f);
                        } else {
                            float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
                            float sq = (t < 0.5f ? 1.0f : -1.0f) + d_blep(t, dt, o->inv) - d_blep(t2, dt, o->inv);
                            float tri = 1.0f - 4.0f * d_abs(t - 0.5f);
                            out = sq + (tri - sq) * ((wave_eff - 0.5f) * 2.0f);
                        }
                        s += l[oi] * out;
                    }
                    t += dt; if (t >= 1.0f) t -= 1.0f;
                    o->phase = t;
                }
                if (folding) s = d_fold(s * drive_fold);
                if (drive > 0.002f) s = d_sat(s * pre);
                float y = d_svf_lp(&v->f, s);
                /* West: the gate's amplitude follows the envelope more steeply */
                float amp = east * v->env + west * v->env * v->env;
                mix[base + j] += y * amp * v->vel * 0.5f;
            }
        }
    }
}
