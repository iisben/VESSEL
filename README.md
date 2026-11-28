# VESSEL

[![Latest Release](https://img.shields.io/github/v/release/iisben/VESSEL?color=8b5cf6&label=Latest%20Release)](https://github.com/iisben/VESSEL/releases/latest)

[**Download Latest Release (macOS Universal · Windows x64 · Linux x86_64)**](https://github.com/iisben/VESSEL/releases/latest) · [**All Releases**](https://github.com/iisben/VESSEL/releases)

## What's New in v2.0.5

- **Smoother Dust & Master Output Resonant Filter:** Tamed high-frequency crackle spikes, resonant ring snaps, and hiss fizz in `Dust` (`NoiseInjector`), and added a zero-latency 64-bit TPT ZDF **Master Output Resonant Filter** (`Out Cut` & `Out Res` right on the Master Rail, also routable from LFOs/Macros/XY) for instant analog de-harshing and warm resonant master sweeps across the entire synth and FX chain.
- **Standalone MIDI, Zero-Mic Launch & Computer Keyboard Piano:** Standalone builds (macOS, Windows, Linux) auto-detect and auto-enable all connected & hot-plugged MIDI keyboards, launch with audio input disabled (`0 in / 2 out`), and include a built-in **QWERTY Computer Keyboard Piano** (`KEYS C3` pill in the header).
- **Hardened Host Compliance & Real-Time Reliability:** Passes `pluginval` strictness level 10 on both VST3 & AU, `auval -strict` with zero warnings, Steinberg VST 3.8.1 `validator` (`537/537`), and zero-violation LLVM RTSan / ASan / UBSan / TSan verification.
- **DSP Polish Across All 116 Presets:** Instant idle model switching in the physical-model `Resonator` so the very first strike after switching presets rings at full strength, plus 64-bit 10 Hz Butterworth DC-hygiene filtering after `BitCrusher` and post-clipper `OutputStage` (pre-true-peak ceiling) for clean sub-`-80 dBFS` DC across every factory preset.
- **Boutique Analog Realism DSP Suite:** Nonlinear coupled 64-bit ZDF ladder & dual-diode Sallen-Key filters, stateful magnetic hysteresis & iron transformer saturation (`RuinCore`), two-stage CdS vactrol low-pass gate & audio-rate feedback, polyphonic power-rail sag & 4–8 stage all-pass phase dispersion, and a clock-rate-modulated BBD analog chorus (Modes I/II/Ensemble) with NE570 compander dynamics, dark 6–10 kHz anti-imaging filtering, and sub-120 Hz phase-coherent bass preservation.
- **Core DSP Engine Upgrade:** Band-limited 3-mip/octave Hermite wavetables, through-zero FM, 2nd-order anti-aliased saturation (ADAA2), linear-phase oversampling with host PDC, click-free modulation and envelopes, and 16-voice SIMD optimization.
- **More Sounds & Control:** 20 new flagship presets (116 total), MPE & polyphonic aftertouch support, MIDI Learn, A/B compare, and Undo/Redo.
- **Readable Default Size & Reactive UI:** Enlarged `1388×873` default UI size with sharpened high-contrast typography, header version badge, subtle 3D sculpted knobs, brighter neon-purple accents, and transparent obsidian ferrofluid + crystalline plexus OpenGL backdrop.
- **Linux Support:** Native Linux x86_64 VST3 and Standalone builds added alongside macOS Universal and Windows x64.

---

## About VESSEL

VESSEL is a dark 5-stage synthesizer and effects rack built for cinematic scores, trailers, IDM, witch house, and heavy modern bass design.

You start with a phase-locked dual sub, wavetables, FM, drone, noise, and granular textures, then sculpt the signal through five dedicated stages — **Source → Ruin → Shape → Space → Pressure** — ending in a tight mono low end and true-peak ceiling so every patch comes out mix-ready.

---

## Modules & Interface

![VESSEL — I · Source stage with reactive backdrop and 3D knobs](docs/images/stage0_fm.png)

| II · Ruin (Tilt, Tube, Bias, Crush, VHS, Motor) | III · Shape (Ladders, Follower, Unmask, Vox, Resonator) |
|---|---|
| ![II · Ruin](docs/images/stage1.png) | ![III · Shape](docs/images/stage2_gong.png) |

| IV · Space (Depth, Body, Black Hole, Haas) | V · Pressure (Multiband OTT, Ducker) |
|---|---|
| ![IV · Space](docs/images/stage3.png) | ![V · Pressure](docs/images/stage4.png) |

| Modulation (LFO & Expression Decks) | Preset Browser (116 Factory Presets) |
|---|---|
| ![LFO Deck](docs/images/lfo.png) | ![Preset Browser](docs/images/browser.png) |

---

## Features

- **I · Source:** Phase-locked dual sub oscillators, wavetable engine with 379 bundled CC0 wavetables (Echo Sound Works Core Tables), through-zero 2-op FM, cinematic drone, dust/vinyl noise, and a freeze/stutter granular looper.
- **II · Ruin:** Tilt EQ, 3-topology tube saturation (Triode, Pentode, Starved) with power-supply sag, bit/rate crusher, VHS wow/flutter/dropouts, and a playable tape-stop motor.
- **III · Shape:** Dual self-oscillating 64-bit ZDF filters, envelope follower, sub-carving Unmask filter, vowel/formant shaper, and an 8-mode physical-model resonator (string, tube, bell, bar, drum, plate, bowl, gong).
- **IV · Space:** Front-to-back Z-depth delay, convolution body (built-in rooms + custom IRs), infinite-hold Black Hole reverb, and mono-safe Haas stereo width.
- **V · Pressure & Output:** 3-band upward/downward OTT compression, sub-keyed ducker, mono-below crossover, exciter, soft clipper, and true-peak ceiling.
- **Modulation & Workflow:** 8 named gain-compensated macros per preset, 3 drawable/warped LFOs, drag-and-drop routing, velocity / mod wheel / channel & polyphonic aftertouch / MPE, MIDI Learn, Lights Out mode, and 116 loudness-matched factory presets.

---

## Installation

Download the latest release for your OS from [**GitHub Releases**](https://github.com/iisben/VESSEL/releases/latest) (or browse [all releases](https://github.com/iisben/VESSEL/releases)).

### macOS (Universal: Apple Silicon + Intel)

Download `VESSEL-2.0.5-macOS-Universal.zip` (or the individual `macOS-VST3` / `macOS-AU` zips), unzip, and copy:

- `VESSEL.vst3` → `~/Library/Audio/Plug-Ins/VST3/`
- `VESSEL.component` → `~/Library/Audio/Plug-Ins/Components/`
- `VESSEL.app` → `/Applications/` or `~/Applications/` (optional Standalone)

> Builds are ad-hoc signed. If macOS Gatekeeper blocks a bundle on first load, run:
> ```bash
> xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/VESSEL.vst3 ~/Library/Audio/Plug-Ins/Components/VESSEL.component
> ```

### Windows (64-bit)

Download `VESSEL-2.0.5-Windows-x64.zip`, unzip, and copy the **entire** `VESSEL.vst3` folder into:

```text
C:\Program Files\Common Files\VST3\
```

Keep the folder intact (the 379 wavetables live inside it). A standalone `Standalone\VESSEL.exe` is also included — click **More info → Run anyway** if SmartScreen prompts on first launch.

### Linux (x86_64)

Download `VESSEL-2.0.5-Linux-x86_64.tar.gz`, extract, and copy the **entire** `VESSEL.vst3` folder into:

```bash
tar -xzf VESSEL-2.0.5-Linux-x86_64.tar.gz
mkdir -p ~/.vst3
cp -r VESSEL-2.0.5-Linux-x86_64/VESSEL.vst3 ~/.vst3/
```

Keep the folder intact so the bundled wavetables load cleanly, then rescan VST3 plug-ins in your DAW (REAPER, Bitwig, Ardour, Renoise, etc.). A standalone binary is in `Standalone/VESSEL`.

To build natively from source on Ubuntu/Debian:

```bash
./scripts/build_linux.sh --install
```
