# VESSEL

VESSEL is a dark synth and effects rack for scores, trailers, IDM and witch house: the stuff that rumbles under
a scene, the hit before the title card, the pad that sounds like it's coming through a wall.

The idea is simple. You start with a clean, phase-locked sub, then drag the sound through five stages (Source,
Ruin, Shape, Space and Pressure) until it's as broken, huge or haunted as you want. It still comes out the other
side mix-ready, with a tight mono low end and no clipping surprises. There are 379 wavetables, an FM operator, a
physical-model resonator, eight macros on every preset, and velocity, mod wheel and aftertouch that actually
change the sound.

## What's inside

- **Sources:** two sub-harmonic generators locked to one phase, so the low end never fights itself. There's a
  wavetable oscillator with 379 free tables, a 2-operator FM voice, a slow cinematic drone, dust and vinyl noise,
  and a granular looper that can freeze or stutter in time.
- **Ruin:** tube drive in three flavours (triode, pentode and a starved, sputtering one), power-supply sag, a bit
  crusher, VHS wobble and dropouts, and a tape stop you can play from the keyboard.
- **Shape:** two self-oscillating ladder filters, an envelope follower, a filter that carves room for the sub, a
  vowel/cello formant, and a physical-model resonator. The resonator can be a string, tube, bell, bar, drum skin,
  plate, singing bowl or gong.
- **Space:** a tiny delay for front-to-back depth, convolution (built-in rooms or your own IRs), the Black Hole
  reverb that can hold forever, and a stereo widener that keeps mono intact.
- **Pressure:** 3-band upward and downward compression, mono below a crossover, a ducker keyed by the sub, an
  exciter, a clipper and a true-peak ceiling at the end.
- **Modulation:** eight named macros, three LFOs with 11 shapes (you can draw your own), and velocity, mod wheel and
  aftertouch routes. All 104 parameters can be automated from your DAW.
- **No clicks or zipper noise:** every knob is smoothed and every on/off switch crossfades.
- **96 factory presets**, all levelled to the same loudness, so A/B-ing presets is honest.

## Installing it (macOS)

Grab the latest build from [Releases](https://github.com/iisben/VESSEL/releases/latest): there's a zip for the
VST3 and one for the AU, both Universal (Apple Silicon and Intel). Unzip them and copy:

- `VESSEL.vst3` into `~/Library/Audio/Plug-Ins/VST3/`
- `VESSEL.component` into `~/Library/Audio/Plug-Ins/Components/`

## Installing it (Windows)

Download the `Windows-VST3` zip from [Releases](https://github.com/iisben/VESSEL/releases/latest) and copy the whole
`VESSEL.vst3` folder into `C:\Program Files\Common Files\VST3\`. Keep the folder intact, because the wavetables
live inside it. There's also a standalone `VESSEL.exe` in the `Standalone` folder. It isn't code-signed, so
SmartScreen may warn you the first time ("More info", then "Run anyway").

## Expression

Every preset responds to how you play. Velocity, mod wheel and aftertouch each get two routes to any continuous
parameter:

## Macros

Every preset has eight macros, and they're always in the same order, so once you learn the layout it works
everywhere. Each one has a name that fits the sound:

| Macro | Role | Typical routes |
|---|---|---|
| 1 | Tone (Bloom, Bite, Fury, Snap) | filter cutoffs, tilt, exciter |
| 2 | Motion (Drift, Throb, Unrest, Warble) | speeds up the preset's LFOs, warps them, adds tape wow |
| 3 | Dirt (Heat, Grit, Snarl, Rust) | tube drive and bias, filter drive, crusher, FM feedback |
| 4 | Weight (Depth, Mass, Abyss, Floor) | sub level, drone, a darker tone |
| 5 | Signature (per preset: Choir, Engine, Tine, Skin, Vowel) | whatever makes that sound itself: table morph, vowel, resonator, FM index, grain pitch |
| 6 | Texture (Haze, Ash, Dust, Static) | hiss, crackle, tape age |
| 7 | Space (Arena, Vastness, Distance, Room) | reverb size and mix, depth, width (the low end stays mono) |
| 8 | Transform (per preset: Leviathan, Requiem, Riptide, Afterglow) | a whole scene change across several modules |

Each macro also carries a hidden volume correction, so turning it changes the character, not the level. Macros
are automatable like everything else, so you can draw them in your DAW or map them to a hardware knob.

## The presets

There are 96, all by iisben, in `Presets/FactoryBank.xml`. `PRESETS.md` lists every one with its routing and
measurements.

I wanted each one to stand on its own. Most play a wavetable picked for that sound: big brass under the braams, a
Rhodes under Rotted Tines, a didgeridoo under Buried Giant. Pads and keys keep their sub in check so they leave
room in a mix. Nothing just sits still, and no two presets in a category sound alike.


## Wavetables

Besides its own built-in analog table, VESSEL ships with the 379 **Echo Sound Works Core Tables**, which Echo Sound
Works generously released into the public domain (CC0 1.0). Thank you! They come in 11 categories: Analog, Basics,
Chill, Digital, FM, Growl, Instruments, Metallic, Spectral, One Shots and Vocal.

`tools/make_tables.py <source folder> <repo root>` converts a folder of Serum-format tables into the compressed
`Wavetables/` folder and `Source/dsp/TableList.h`. Switching tables never stalls the audio: the new table loads
in the background while the old one keeps playing.

For the nitty-gritty of the DSP audit and known limits, see `AUDIT.md`.

## Status

VESSEL is built and tested on macOS (Apple Silicon) as a Universal binary. The AU passes Apple's `auval`, the VST3
survives `HostProbe`'s torture tests, all 379 wavetables play, and all 96 presets pass `VesselCheck`. The Windows
VST3 is built on GitHub Actions and passes the same `HostProbe` tests there, but it hasn't been tried in a DAW on
Windows yet. Linux is untested, and none of the builds are code-signed (no Developer ID or notarisation on macOS).
