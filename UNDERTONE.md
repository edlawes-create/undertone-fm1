# Undertone v0.2 — custom firmware for the M-VAVE FM-1

A dedicated instrument: three sound modules and a shared FX bus, played from the FM-1's own keys and knobs. The stock FM1 Open synths and UI are gone from the panel. M-VAVE's USB update, recovery, storage, USB audio/MIDI and Bluetooth layers remain underneath (they are the only way to reflash or restore the unit).

## Modules

**VOICE** — subharmonic polysynth (4 voices). Each note sounds its pitch plus up to three subharmonics (f/1–f/16). Harmony modes: JI (pure undertones), 12-ET, or SCALE (subs snapped to a scale, so chords follow the scale degree). **COAST** morphs from East (saw/pulse into a resonant, envelope-swept filter) to West (filter becomes a low-pass gate, the wavefolder becomes the timbre source).

**PERC** — West Coast percussion (6 voices). Each strike pings a sine modulator FM-ing a sine carrier, through a wavefolder and a vactrol-style low-pass gate. Pitch sweep, noise burst, and BOUNCE (bouncing-ball retriggers). Every key is a pitch; SPREAD sets the interval between keys.

**DRONE** — eight sustained voices in four pairs on the eight white keys F3–F4. Each pair has a pitch; SPREAD detunes the pair for beating. MOD cross-modulates (ROUTE: within pairs, ring of pairs, chaotic sum, or LFO). SHARP sets slow swells vs sharp plucks. Latch voices with HOLD. Best through the FX bus.

**FX** (shared) — drive, modulated delay (10–735 ms, feedback with tone), and a global LFO (pitch, tone or FX time; sine, triangle, square, sample & hold).

## Controls

| Control | Action |
|---|---|
| SELECT | Module: VOICE / PERC / DRONE |
| ALGORITHM | Page (the last three pages are FX, FX MOD, LFO) |
| KNOB 1–4 | The four values on screen (turn faster for bigger steps) |
| PRESETS | Six presets per module; on FX/LFO pages, six FX presets |
| HOME | First page of the module |
| SCL | Harmony page (VOICE) |
| ENV | Envelope (VOICE) / gate (PERC) / mod (DRONE) |
| EDIT | Next page |
| FX | FX page, press again for FX MOD |
| LFO | LFO page |
| OCT− / OCT+ | Octave (VOICE, PERC) |
| PLAY | HOLD: notes and drone voices latch; tap a held key again to release it |
| GLO | All notes off (also clears HOLD) |
| MASTER | Output level |

Keys and HOLD state light the key LEDs; in DRONE the LEDs show which voices are sounding.

External MIDI (USB, TRS, BLE): notes on any channel play the selected module. DRONE responds to F3, G3, A3, B3, C4, D4, E4, F4. CC120/123 stop everything.

## Not yet

- Settings are not saved across power cycles (presets are built in; tweaks reset at power-on).
- SAVE, ARP, SEQ and REC are unused.
- Not yet run on hardware: CPU headroom with all modules sounding at once is unknown. If the screen shows AUDIO OVERLOAD, use fewer voices or less FX.
- The factory DX7 bank is trimmed to banks 1–2 (unused by this firmware; kept for the storage layer).

## Building

Same as FM1 Open: JieLi toolchain 2.5.3, then `sdk\Build.ps1 -FirmwareOnly -Toolchain 'C:\JL\pi32'` for a test build, or `build.bat --web` and `build.bat --package` with `FM1_SOURCE_URL` set to this tag's source archive for the updater page.

## Source layout

- `app/screen.c` — panel, display and presets (replaces the stock UI; the fixed build gives this file the native compilation the SDK control loop needs).
- `app/unit.c` — event queues, global LFO, FX bus and output.
- `app/mod_voice.c`, `app/mod_perc.c`, `app/mod_drone.c` — the engines (all audio code runs from SRAM).
- `app/dsp.h` — shared inline DSP helpers (no libc in the audio path).
- `app/midi_router.c` — external MIDI goes to the unit.
- `app/board/audio.c` — the unit is mixed into each audio block before the master stage.

GPL-3.0 with the FM1 SDK linking exception, as FM1 Open.
