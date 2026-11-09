# Undertone v0.1 for FM1 Open (M-VAVE FM-1)

A subharmonic voice engine added to M-VAVE's official FM1 Open V14 firmware. The FM1 Open instrument (8 tracks, DX7, 303, 808, sampler) is untouched; Undertone plays on top of it on its own MIDI channel.

Each note sounds the played pitch plus up to three subharmonics (f/1 to f/16). In SCALE mode the subharmonics are snapped to a chosen scale, so the chord quality follows the scale degree instead of planing.

C major, subs f/3 and f/5, close voicing:

| Played | JI mode (planing) | SCALE mode |
|---|---|---|
| C | C F G# | C F A (F/C) |
| D | D G A# | D G B (G/D) |
| E | E A C | E A C (Am/E) |
| F | F A# C# | F A D (Dm/F) |
| G | G C D# | G C E (C/G) |
| A | A D F | A D F (Dm/A) |
| B | B E G | B E G (Em/B) |

## Playing it

- MIDI channel **12**, received over TRS, USB or BLE. Set `UNDERTONE_CHANNEL` in `app/undertone.h` to change it (0-based).
- External MIDI only in v0.1. The FM-1's own keys still play the FM1 Open tracks.
- 4-voice polyphonic; each voice is four oscillators.
- Mono engine, sent to both outputs, through the FM-1 master level.

## CC chart (channel 12)

| CC | Parameter | Range / notes |
|---|---|---|
| 1 | Mod wheel | Adds fold and opens the filter |
| Ch. aftertouch | Same as mod wheel | Works with the CC2-to-AT JSFX |
| 7 | Volume | Default 100 |
| 20, 21, 22 | Sub 1/2/3 divisor | 0 = off, then f/1 to f/16 across the range. Defaults 3, 5, 2 |
| 23, 24, 25 | Sub 1/2/3 level | Defaults 0.7, 0.6, 0 |
| 26 | Main oscillator level | Default 0.8 |
| 27 | Harmony mode | 0–42 JI, 43–85 12-ET, 86–127 SCALE (default) |
| 28 | Scale | 12 steps: major, natural minor, dorian, phrygian, lydian, mixolydian, harmonic minor, melodic minor, major pent, minor pent, whole tone, octatonic |
| 29 | Root | 12 steps, C to B |
| 30 | Close voicing | ≥64 on (default): subs raised by octaves into the octave below the note |
| 64 | Sustain pedal | |
| 70 | Wave | Saw → pulse → triangle |
| 71 | Resonance | |
| 72 | Release | 1 ms to 8 s |
| 73 | Attack | 1 ms to 8 s |
| 74 | Cutoff | 20 Hz to 20 kHz, half key tracking |
| 75 | Decay | 1 ms to 8 s |
| 76 | Sustain level | |
| 77 | Filter env amount | 64 = none, bipolar |
| 78 | Fold | Triangle wavefolder after the oscillator mix |
| 120 / 123 | All sound off / all notes off | |

Divisor, mode, scale, root and close-voicing changes retune notes that are already sounding.

For the Sextant-to-Buchla end: wave up (triangle), fold up, cutoff open, and ride CC1 or aftertouch.

## Building (Windows)

The firmware needs JieLi's Windows toolchain, so it has to be built on a Windows PC.

1. Install **JieLi Windows toolchain 2.5.3** from https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe (normally to `C:\JL`).
2. Unzip this source and open PowerShell in the folder.
3. Local test build:
   ```powershell
   .\sdk\Build.ps1 -FirmwareOnly -Toolchain 'C:\JL\pi32'
   ```
   This produces `build\...\generated\FM1_OFFICIAL_OPEN.fwsc`.
4. To get an updater page with this firmware embedded (and the stock V15 restore built in), build the package. It needs an HTTPS URL for the matching source archive, for example this zip on your own GitHub repo:
   ```powershell
   $env:FM1_SOURCE_URL = 'https://github.com/<you>/undertone-fm1/archive/refs/tags/v0.1.zip'
   $env:FM1_PI32_TOOLCHAIN = 'C:\JL\pi32'
   .\build.bat --web
   .\build.bat --package
   ```
   The output is `build\official-web\FM1-Official-Open-Source.html`. Open it in Chrome or Edge, connect the FM-1 by USB, and use its update tab.

The build checks every SDK and toolchain file against `sdk/manifest.json`. If it stops on a manifest check, nothing has been written to the FM-1.

## Before flashing

- Keep `FM-1_015.fwsc` (stock V15). SHA-256: `db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a`. Also download V15 (and V16) from https://www.m-vave.com/download.
- Export any patches or projects you want to keep.
- Keep USB power connected while writing.

## Host test

`host/harness.c` compiles the engine on Linux or macOS, prints the harmony table and renders the demo WAVs:

```
gcc -O2 -Isdk/include -Iapp -o harness host/harness.c -lm && ./harness
```

## Changes to FM1 Open

- `app/undertone.c`, `app/undertone.h`: new.
- `app/midi_router.c`: channel-12 voice messages go to Undertone instead of the instrument.
- `app/board/audio.c`: Undertone is added to each rendered block before the output mix.

GPL-3.0 with the FM1 SDK linking exception, as FM1 Open.
