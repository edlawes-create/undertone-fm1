/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm_patch.h"
#include "fm_factory.inc"

/* Expand once during startup, before the audio owners are running. MIDI
 * program changes then read SRAM without decompression or heap allocation. */
/* Undertone build: only factory banks 1-2 are stored (flash space). Programs
 * 64-127 mirror 0-63 unless the user loads their own bank 3/4. */
enum { FACTORY_STORED = 64 };
static uint8_t factory_raw[FACTORY_STORED*128];
static unsigned factory_ready;
static FM1_DSP_CODE void factory_init(void)
{
    if (factory_ready) return;
    uint8_t *out=factory_raw;
    for (unsigned at=0;at<sizeof factory_packed && out<factory_raw+sizeof factory_raw;) {
        unsigned token=factory_packed[at++];
        if (token<128) *out++=(uint8_t)token;
        else {
            unsigned low=factory_packed[at++],distance=low&127U;
            if (low&128U) distance|=(unsigned)factory_packed[at++]<<7;
            for (unsigned n=token-128+3;n;--n) { *out=out[-(int)distance]; ++out; }
        }
    }
    factory_ready=1;
}
FM1_DSP_CODE const uint8_t *groove_fm_factory_raw(unsigned program)
{
    factory_init();
    return factory_raw+(program%FACTORY_STORED)*128;
}
FM1_DSP_CODE void groove_fm_factory(uint8_t p[156], unsigned program)
{
    groove_fm_unpack(p,groove_fm_factory_raw(program));
}
