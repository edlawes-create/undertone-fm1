/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Undertone: subharmonic voice engine for FM1 Open.
 * Each note sounds the played pitch plus up to three subharmonics (f/1..f/16).
 * Harmony modes: JI (pure undertones, fixed chord shape), 12-ET (rounded,
 * fixed shape) or SCALE (subs snapped to a scale, so the chord quality
 * changes with the scale degree). Played on its own MIDI channel, mixed on
 * top of the FM1 Open instrument. Parameters are MIDI CCs (see UNDERTONE.md). */
#ifndef FM1_UNDERTONE_H
#define FM1_UNDERTONE_H
#include <stddef.h>
#include <stdint.h>
#include "fm1.h"

/* 0-based MIDI channel. 11 = MIDI channel 12 (FM1 Open uses 1-7 and 10). */
#define UNDERTONE_CHANNEL 11U

/* MIDI routing task. Returns 1 when the message belongs to Undertone and must
 * not be posted to the FM1 instrument; 0 otherwise. Never blocks. */
int undertone_midi_accept(const uint8_t *message, size_t bytes);

/* Audio ISR. Adds one block of Undertone into the rendered Q23 stereo block. */
FM1_AUDIO_CODE void undertone_render_add(int32_t *output);

#endif
