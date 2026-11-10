# TacAudio

A gaming-focused binaural (3D headphone) audio engine: precise direction, clean footsteps,
no exaggerated bass. Planned extras beyond commercial virtualisers: footstep boost,
gunshot taming and bass control.


## Status

| Milestone | State |
|---|---|
| **M1** offline renderer: 7.1 / 5.1 / 7.1.4 WAV → binaural WAV | ✅ done |
| **M2** real-time engine: capture / loopback / test scene → WASAPI → headphones | ✅ done (needs a virtual 7.1 device for games) |
| M3 TacProcessor: footstep boost, gunshot tamer, bass control | dynamics + punch + footstep clarity done |
| M4 presets, headphone EQ (AutoEQ), UI | UI done (`tacaudio_gui`); presets / headphone EQ next |

## Build (Windows, Visual Studio 2022/2026)

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
build/Release/tac_tests.exe
```

### Measured HRTFs (SOFA import, on by default)

The configure step downloads **zlib v1.3.2** and **libmysofa v1.3.5** (BSD-3) and copies the
bundled **MIT KEMAR** HRTF (free to use with attribution: Gardner & Martin, MIT Media Lab) to
`build/Release/hrtf/`, where `tacrender` uses it by default. Any SimpleFreeFieldHRIR SOFA file
works, e.g. SADIE II (48 kHz).

For a build with no third-party code: `cmake -S . -B build -DTAC_WITH_SOFA=OFF`
(only the built-in spherical-head HRTF is then available).

## App (`tacaudio_gui`)

```bash
build/Release/tacaudio_gui.exe
```

A dark, card-based app (Dear ImGui with custom-drawn widgets: sidebar navigation, toggle
switches, segmented controls, slim sliders with value boxes, live graphs and meters). Pages:
**Sound**, **Devices** (game audio source, headphone output, exclusive mode), **Performance**
(latency / buffer / CPU / glitch tiles + history) and **About** (credits & licences).
Every option has a ↺ reset-to-default button, plus **Reset all** on the Sound page.
Set the devices once, switch on with the power toggle (top right), then adjust live:

| Control | What it does |
|---|---|
| 3D sound | untick for a plain stereo downmix (instant A/B) |
| Head | HRTF set (`kemar`, `ku100`, `h3`…`h20`); first load ~2.5 s, then cached |
| Room | neutral / balanced / spacious |
| Dynamics + Punch | tame loud sounds and lift quiet ones; punch keeps gunshot impact |
| Footsteps | clarity boost around 3.5 kHz, only while a channel is quiet |
| Volume | output level (instant) |

Changes swap the renderer click-free (new one warms up ~20 ms, then a ~10 ms crossfade).
Settings persist in `%APPDATA%\TacAudio\settings.ini`. Checks without a screen:
`tacaudio_gui.exe --smoke-test [--page 0-3] [--preview]` (hidden window, writes
`%TEMP%\tacaudio_gui_smoke.bmp`; `--preview` fills in sample live data) and
`tacaudio_gui.exe --session-test "<output device>"` (start, live changes, stop; writes
`%TEMP%\tacaudio_session_test.txt`).

## Real-time engine (`tacaudio`)

```bash
tacaudio devices                                    # list outputs/inputs with numbers

# Hear it now, no virtual device needed: built-in 7.1 scenes, looping
tacaudio run --test compass --out "Game"            # or circle / battle
# Games: capture what is played into the virtual 7.1 cable, play on your headphones' device
tacaudio run --loopback "CABLE Input" --out "Game" --exclusive   # lowest latency (recommended)
tacaudio run --loopback "CABLE Input" --out "Game"               # shared: other apps can use "Game" too

# Verify a virtual device really carries 7.1 (inaudible; plays/captures a tone per channel)
tacaudio cable-test --device "CABLE Input"

# Measure the latency TacAudio adds (injects clicks into the cable; they are audible on --out,
# so use a quiet --gain-db, e.g. -40)
tacaudio run --loopback "CABLE Input" --out "Game" --exclusive --latency-test --gain-db -40 --seconds 15
```

### Latency

Measured with `--latency-test` on this PC (click entering the cable → leaving TacAudio's output):

| Output mode | Added latency | Notes |
|---|---|---|
| shared (default) | ~55 ms | GoXLR channels only allow a 10 ms period when shared; includes the Windows mixer |
| `--exclusive` | **~31 ms** | 3 ms device period, Windows mixer bypassed; nothing else can play on that device meanwhile |

The fixed part is VB-CABLE loopback: it delivers 10 ms packets (~9 ms delivery delay)
whatever the engine period. TacAudio's own virtual driver (later) removes that step.

Live status line, once per second:

```
[ 14s] PLAY buf 13.2/12.7 ms  drift +49 ppm  cpu 2.3% (max 11.1%)  lim 0.0 dB  underruns 1  out-late 0  in-gap 10.6 ms
```

| Field | Meaning |
|---|---|
| `PLAY` / `WAIT` | playing, or waiting for input (nothing is being sent to the source) |
| `buf` | input buffer level / current target. The target starts from the input and output packet sizes and **grows 2 ms after each underrun** (max 60 ms), then shrinks 0.5 ms per 20 s without one |
| `drift` | playback-rate correction between the source and output clocks (start-up excess shows here for ~10 s) |
| `dyn` | strongest dynamics taming / lift in the last second (e.g. `-14.2/+6.1` = gunfire pulled down 14 dB, quiet sounds lifted 6 dB) |
| `lim` | strongest output limiting in the last second (0 dB = none) |
| `underruns` | input ran dry (audible glitch); a few at start are normal while the buffer adapts, then it should stop rising |
| `out-late` | output wake-ups missed by more than a period (audible glitch; CPU starvation under game load) |
| `in-gap` | longest gap between input packets in the last second (10 ms is normal for VB-CABLE; much more = the game/PC is stalling audio) |

Options: `--exclusive` (`--exclusive-period-ms`), `--mute` (pipeline runs, device gets silence),
`--out null` (no device), `--latency-ms`, `--out-queue-ms`, `--no-low-latency`, `--no-drift`,
`--gain-db` (default −6: headroom for the HRTF near-ear boost), `--limiter-db` (−1),
`--no-limiter`, `--block` (default 128), `--seconds`, `--latency-test`, plus the HRTF/layout
options below.

### Game setup (one-time)

Games must output **7.1** into a device TacAudio can capture. GoXLR channels (Game/Chat/…)
are stereo-only, so a virtual cable is needed:

1. Install VB-Audio Virtual Cable (free, Microsoft-signed, so anti-cheat friendly) and reboot.
2. `mmsys.cpl` → Playback → *CABLE Input* → Configure → **7.1 Surround**; Properties →
   Advanced → 48000 Hz. Keep *Spatial sound* **off** on CABLE Input.
3. `tacaudio cable-test` must print **PASS** (all 8 channels separate).
4. Send the game to *CABLE Input* per app: *Settings → System → Sound → Volume mixer*.
5. In-game: audio output = **7.1 / Home theater / Surround** (not "headphones" or
   "stereo"), and turn off the game's own 3D-headphone mode so it isn't applied twice.
6. `tacaudio run --loopback "CABLE Input" --out "Game" --exclusive`. TacAudio plays into
   the GoXLR Game channel, so the Game fader still works.

Why loopback and not *CABLE Output*: VB-CABLE's recording side is often stereo-only
through WASAPI, while WASAPI loopback of the 7.1 playback side delivers all 8 channels
(verified with `cable-test`).

Later this cable is replaced by TacAudio's own virtual 7.1 driver (needs Microsoft signing
for anti-cheat compatibility).

Measured on this PC: about 2 % CPU.

## Offline renderer (`tacrender`)

```bash
# Synthetic test scenes (7.1 + binaural) for listening checks
tacrender demo renders

# Render any multichannel WAV (layout from its channel mask)
tacrender render game_capture_71.wav out_binaural.wav --normalize -1

# HRTF choice
tacrender render in.wav out.wav --sofa data/hrtf/SADIE_D1.sofa
tacrender render in.wav out.wav --hrir my_set.thrir
tacrender render in.wav out.wav --spherical                        # built-in model

# Convert a SOFA file once, then load the fast .thrir format
tacrender import-sofa in.sofa out.thrir
```

Options: `--block <n>` (default 256), `--side <deg>` (90), `--back <deg>` (140),
`--lfe-db <dB>` (0), `--gain-db <dB>`, `--normalize <dBFS>`, `--pcm24`.

### Demo scenes

| File | What you should hear |
|---|---|
| `demo_compass_binaural.wav` | footsteps front → front-right → right → back-right → back → back-left → left → front-left (2 s each) |
| `demo_circle_binaural.wav` | footsteps walking clockwise around you, starting in front |
| `demo_battle_binaural.wav` | the same circle + loud gunshot bursts from front-right (the problem M3 will solve) |

Demos are peak-normalised to −1 dBFS: measured HRTFs boost the near ear by up to ~10 dB.

**HRTF equalisation (on by default).** Raw dummy-head data contains the head's own ear-canal
resonance: averaged over all directions, raw MIT KEMAR is about **+18 dB at 2–3 kHz** and
−6 dB in the bass, which sounds harsh and thin. TacAudio applies one diffuse-field EQ filter
(minimum phase, identical for both ears and all directions), so the average response is flat
within ±1.5 dB (300 Hz–14 kHz, tested) while the interaural cues stay unchanged (tested to 0.2 dB).
`--raw-hrtf` disables it for comparison (`renders/raw/` holds raw-HRTF demo renders).

**Heads (HRTF sets).** Everyone hears binaural audio a little differently, so pick the head
that works for you:

```bash
tacrender heads                       # list: mit, ku100, kemar, h3..h20
tacrender audition renders/heads      # same footstep compass through every head
tacaudio run ... --head kemar         # use one
```

Default: **SADIE II D1 (Neumann KU100)** when installed, else the bundled MIT KEMAR.
SADIE II (University of York, Apache-2.0, ~240 MB at 48 kHz) lives in `data/hrtf/sadie2/`
(not in git). Download: Zenodo record 12092466, the `<subject>_HRIR_SOFA.zip` files; keep the
`*_48K_24bit_256tap_FIR_SOFA.sofa` file of each as `data/hrtf/sadie2/<subject>.sofa`.

**Dynamics** (`--dynamics off|light|medium|strong`, M3; `tacaudio` default `medium`). Per
channel, before the 3D stage: loud sounds (gunfire, explosions, killstreaks) are compressed
above a threshold, quiet sounds (distant footsteps) are lifted below another, and nothing below
a noise floor is raised. Channels are independent, so a gun on one side does not duck footsteps
elsewhere.

`--punch 0-100` (default 60) sets how much of each shot's impact passes before taming starts:
the detector blends an instant peak level (punch 0: everything loud is flattened) with a
loudness window of up to 50 ms (punch 100). Short cracks then pass, while sustained loudness
(killstreak signatures, constant fire, explosion rumble) is still reduced by >8 dB (tested).
Battle demo, relative to footsteps:

| Setting | Gun crack | Gun body |
|---|---|---|
| off | 34 dB | 25 dB |
| medium, punch 0 | 24 dB | 15 dB |
| medium, punch 60 (default) | 32 dB | 22 dB |

**Room presets** (`--room`): `neutral` (dry, maximum clarity), `balanced` (small room, 0.28 s
decay, default for `tacaudio`), `spacious` (larger room, 0.5 s). Early reflections plus a
4-line feedback-delay-network reverb; bass below 160 Hz and highs above 9 kHz are kept out of
the room so footsteps stay clean, and the direct sound is untouched for the first 5 ms (tested),
so direction cues arrive first. `--room-db` shifts the room level (e.g. `-3` = subtler).
Preset values are TacAudio's own starting points, to be tuned by ear.

The default MIT KEMAR set has real pinna cues (a source behind is ~3.6 dB darker at 8–12 kHz
than the same angle in front). The `--spherical` model gives correct left/right
(ITD ≈ 0.66 ms, ILD ≈ 10 dB at 90°) but weak front/back and elevation.

## Layout

```
src/core/       dependency-free DSP: fft, convolver (UPOLS), wav, hrtf, layout, panner (VBAP),
                binaural_renderer, scene (test signals), biquad, limiter,
                ring_buffer (lock-free SPSC), varispeed (drift resampler)
src/engine/     real-time engine: ring -> varispeed -> binaural -> limiter (platform independent)
src/platform/   WASAPI capture/loopback/render streams (Windows)
src/sofa/       SOFA import (libmysofa)
apps/common/    shared CLI helpers (args, HRTF selection)
apps/tacrender/ offline CLI
apps/tacaudio/  real-time CLI
tests/          unit tests (each DSP block vs. a slow reference; engine vs. offline; drift simulation)
```

Conventions: azimuth 0° = front, +90° = right, elevation + = up. Channel order follows the
Windows channel mask (7.1 = FL FR FC LFE BL BR SL SR).
