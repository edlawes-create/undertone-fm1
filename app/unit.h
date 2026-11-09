/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Undertone unit: three sound modules (VOICE, PERC, DRONE) and a shared FX
 * bus, played from the FM-1 panel and external MIDI. Parameters are 0..127.
 *
 * Threading: unit_note/unit_param/unit_select are producer calls. The panel
 * (control task) and MIDI (routing task) each use their own single-producer
 * queue; the audio interrupt is the only consumer. */
#ifndef UNIT_H
#define UNIT_H
#include <stddef.h>
#include <stdint.h>
#include "fm1.h"

enum { MOD_VOICE, MOD_PERC, MOD_DRONE, MOD_FX, MOD_COUNT };
enum { SRC_PANEL, SRC_MIDI };
#define UNIT_PARAMS 24          /* per module */

/* VOICE parameters */
enum {
    V_WAVE, V_FOLD, V_CUTOFF, V_RESO,
    V_COAST, V_ENVAMT, V_DRIVE, V_GLIDE,
    V_DIV1, V_DIV2, V_DIV3, V_CLOSE,
    V_LVL0, V_LVL1, V_LVL2, V_LVL3,
    V_MODE, V_SCALE, V_ROOT, V_DETUNE,
    V_ATTACK, V_DECAY, V_SUSTAIN, V_RELEASE,
};
/* PERC parameters */
enum {
    P_TUNE, P_RATIO, P_FM, P_FOLD,
    P_DECAY, P_TONE, P_SWEEP, P_NOISE,
    P_SPREAD, P_BOUNCE, P_VELO, P_LEVEL,
};
/* DRONE parameters */
enum {
    D_PITCH1, D_PITCH2, D_PITCH3, D_PITCH4,
    D_MOD, D_ROUTE, D_SPREAD, D_SHARP,
    D_WAVE, D_LFORATE, D_LFODEPTH, D_LEVEL,
};
/* Shared FX parameters */
enum {
    F_DRIVE, F_TIME, F_FEEDBACK, F_MIX,
    F_MODRATE, F_MODDEPTH, F_TONE, F_SEND,
    F_LFORATE, F_LFODEPTH, F_LFOTARGET, F_LFOSHAPE,
};

void unit_init(void);   /* control task, before audio starts using the unit */
/* velocity 0 = note off. note is MIDI pitch, module MOD_VOICE..MOD_DRONE. */
int unit_note(unsigned source, unsigned module, unsigned note, unsigned velocity);
int unit_param(unsigned source, unsigned module, unsigned param, unsigned value);
int unit_all_off(unsigned source);
/* Module that external MIDI notes play; written by the UI. */
extern volatile unsigned unit_midi_module;
/* Routing task: consume a channel message for the unit. Returns 1 when the
 * message was taken (it must not reach the stock instrument). */
int unit_midi(const uint8_t *message, size_t bytes);
/* Read-only for the UI: which drone voices are sounding (bit mask). */
unsigned unit_drone_active(void);
FM1_AUDIO_CODE void unit_render_add(int32_t *output);

/* Module engines, called only from the audio interrupt via unit.c.
 * Each render ADDS one block (frames <= 128) of mono float into mix.
 * mod: global LFO output, already scaled by depth and routed. */
#define UNIT_BLOCK 128
struct unit_mod { float pitch, tone; };
FM1_AUDIO_CODE void voice_init(void);
FM1_AUDIO_CODE void voice_note(unsigned note, unsigned velocity);
FM1_AUDIO_CODE void voice_param(unsigned param, unsigned value);
FM1_AUDIO_CODE void voice_off(void);
FM1_AUDIO_CODE void voice_render(float *mix, const struct unit_mod *mod);
FM1_AUDIO_CODE void perc_init(void);
FM1_AUDIO_CODE void perc_note(unsigned note, unsigned velocity);
FM1_AUDIO_CODE void perc_param(unsigned param, unsigned value);
FM1_AUDIO_CODE void perc_off(void);
FM1_AUDIO_CODE void perc_render(float *mix, const struct unit_mod *mod);
FM1_AUDIO_CODE void drone_init(void);
FM1_AUDIO_CODE void drone_note(unsigned voice, unsigned on);
FM1_AUDIO_CODE void drone_param(unsigned param, unsigned value);
FM1_AUDIO_CODE void drone_off(void);
FM1_AUDIO_CODE void drone_render(float *mix, const struct unit_mod *mod);
FM1_AUDIO_CODE unsigned drone_active(void);
/* Parameter defaults, shared by the engines and the UI. */
extern const uint8_t unit_defaults[MOD_COUNT][UNIT_PARAMS];

#endif
