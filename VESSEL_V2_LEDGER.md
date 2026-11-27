# VESSEL II — Engineering & QA Ledger

- **Current Stage:** `Stage 2: PASS`
- **Next Stage:** `Stage 3A`
- **Branch:** `feat/vessel-v2-namm` (initialized from V1.0.1 baseline on `main`; no git remote configured yet)
- **DSP Core Freeze:** `NOT FROZEN` (freezes after Stage 3B)

---

## Stage 1 — Preflight, Full Codebase Audit & V2 Blueprint

### 1. Toolchain & Environment
| Item | Value / Status |
|---|---|
| Host OS / CPU | macOS 27.2.1 (Build 26B131), `arm64`, 14 cores |
| Build System | CMake `4.5.0`, Ninja `1.13.2`, C++20 (`CMAKE_CXX_STANDARD 20`) |
| Compilers | AppleClang `21.0.0` (`/usr/bin/clang++`, **use for JUCE ObjC++**); Homebrew LLVM `23.1.5` is first in PATH (avoid for JUCE macOS SDK) |
| Framework | JUCE `8.0.9` via `FetchContent` (`build/_deps/juce-src`; local `~/Documents/JUCE` is 9.0.5) |
| Plugin Formats | `VST3`, `AU`, `Standalone` (`PRODUCT_NAME "VESSEL"`, `BUNDLE_ID audio.vessel.vessel`, `Vsla`/`Vssl`) |
| Graphics (V1) | `juce::OpenGLContext` + GLSL `Backdrop` shader (`juce::juce_opengl`) + JUCE 2D overlay (`30 Hz` timer + `VBlankAttachment` Torch) |
| Existing Tests | `HostProbe` (VST3 host torture), `VesselLab` (DSP measurements), `VesselCheck` (96-preset LUFS/TP/macro/UI snapshots) |
| Validators / CI | `auval` (`/usr/bin/auval` installed); `pluginval` **not installed**; `gh` CLI logged in (`iisben`, no git remote yet); `docker` **not installed** |
| Git Hygiene | `build/`, `dist/`, `compile_commands.json` gitignored; `compile_commands.json -> build/compile_commands.json` symlinked |

### 2. Unmodified Release (`-O3`) Build & Pre-Existing Test Baseline
- **Build:** `BUILD_OK` (0 warnings in `Source/`; 2 minor warnings in `tools/VesselLab.cpp`; 9 macOS 27 SDK deprecation notices inside JUCE 8.0.9).
- **Outputs:** `VESSEL.vst3`, `VESSEL.component`, `VESSEL.app`, `HostProbe`, `VesselLab`, `VesselCheck`.
- **HostProbe:** `PASSED` (21/21 checks; 2430 exposed params [348 VESSEL + MIDI CC emu]; latency 110 samples @ 48 kHz; finite across 22.05/44.1/48/96 kHz, 0/1/7/13/64/513-sample blocks, NaN input, 8x rapid editor open/close).
- **VesselLab (`all`):**
  - Aliasing: WT saw C7 `-88.9 dB`, WT saw C7 pw15 `-86.5 dB`, WT sqr C6 pw25 `-90.0 dB`, Tube triode C6 `-92.0 dB`, Tape bias C6 `-92.8 dB`, Exciter `-93.6 dB`, FM `-88.5..-91.4 dB` (`-79.6 dB` @ 14:1 C6); **GAPS:** Ladder drive 100% C6 saw **`-58.3 dB`**, Tube starved 80% C6 **`-60.5 dB`**, Clipper +12 dB C6 **`-74.9 dB`**.
  - Phase coherence: Tube 50% mix `±0.03 dB` (100 Hz–18 kHz); Sub send 50% `-0.086 dB` @ 55 Hz.
  - Low end: Mono collapse on `-43.8 dB` side/mid `<120 Hz`.
  - True peak: Ceiling `-1.0 dBFS` held at `-1.01..-1.03 dBTP` (16x ref), bit-exact below ceiling, look-ahead 101 samples @ 48 kHz.
  - CPU (8 voices, all modules ON @ 48 kHz): **`8.53%`** RT total (`Sub 1.76%`, `WT 1.60%`, `Conv 1.46%`, `Ladders 1.03%`).
- **VesselCheck (`Init Vessel`):** `ALL OK` (`-16.0 LUFS`, `-4.6 dBTP`, `-36.8 dB` lowSide, macros active).

### 3. Cross-Platform CMake & Portability Gaps
1. `CMakeLists.txt`: Missing `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`.
2. `CMakeLists.txt`: Missing `NOMINMAX` and `WIN32_LEAN_AND_MEAN` for MSVC (`if(WIN32)`).
3. `CMakeLists.txt`: Missing Linux POST_BUILD copy of `Wavetables/` into `VESSEL.vst3/Contents/Resources/Wavetables` and next to Standalone binary (only Apple bundle resources + `WIN32` exist).
4. `CMakeLists.txt`: `FORMATS VST3 AU Standalone` should be gated (`AU` on `APPLE` only); add Universal macOS (`arm64;x86_64`) support; remove `JUCE_DISPLAY_SPLASH_SCREEN=0` deprecated warning and `juce::juce_opengl` (in Stage 5).
5. `Source/dsp/Common.h` & `tools/Vessel{Check,Lab}.cpp`: Replace manual `kPi` / raw `M_PI` with C++20 `std::numbers::pi_v<double>` / `std::numbers::pi_v<float>` (MSVC fails on raw `M_PI`).
6. `Source/ui/Reliquary.cpp`: Relies on OS-specific system fonts (`Didot`/`Avenir Next Condensed`/`Georgia`/`Bahnschrift`/`DejaVu`); unify typography in Stage 5.
7. `.github/workflows/`: Only `build-windows.yml` exists; Stage 7 needs 3-OS matrix `.github/workflows/build-release.yml` + `scripts/build_windows.ps1` + `scripts/build_linux.sh`.
8. `#include` casing in `Source/` and `tools/` is already exact case-sensitive. `JUCE_WEB_BROWSER=0` and `JUCE_USE_CURL=0` are already set.

### 4. DSP Status Audit (V1 vs. Serum 2 / Zebra 3 & 8 Locked Gates)
- **Already Strong (Preserve & Refine):**
  - 4x oversampled voice bus (`voiceOs`), phase-aligned sub stack, 379 CC0 wavetables with lock-free hazard-pointer `TableStore` background decoding.
  - 8-model physical resonator (`String/Tube/Bell/Bar/Membrane/Plate/Bowl/Gong`), 8-line Hadamard `BlackHole` FDN, mono-safe `HaasWidener`, LR4 allpass-aligned `MonoCollapse` & `MultibandOtt`, SIMD 16x `TruePeakCeiling`.
  - Parameter smoothing (`20 ms` one-pole + per-sample `Ramp`) and `BypassFade` crossfades (zero toggle clicks).
- **Remaining DSP Gaps (Close in Stage 3A & 3B):**
  1. **Wavetable Mip & Interp (`Wavetable.h/.cpp`):** Currently 1 mip/octave (`kMips=11`) with linear sample interpolation and 2-point PolyBLAMP. Upgrade to **3–4 mips/octave** (e.g., 3/oct = 31 mips), **cubic Hermite interpolation**, and **4-point PolyBLAMP/PolyBLEP** for VA/PWM edges.
  2. **Through-Zero FM (`Voice.h`, `Wavetable.h`):** Currently positive-only PM (`p += pm`) with discrete ratio choices and no post-osc DC blocker. Add oversampled **TZFM** with bidirectional `[0, 1)` phase wrapping + `~5 Hz` 64-bit DC blocker + continuous/irrational ratio support (for `1:1.414` high-index FM gate).
  3. **64-Bit TPT/ZDF Filters (`Common.h`, `Voice.h`):** `Svf` and `LadderBank` are 32-bit `float`; `LadderBank` uses `fastTan` + uniterated `fastTanh` causing **`-58.3 dB`** aliasing at 100% drive. Upgrade `Svf` to 64-bit `double` Cytomic TPT SVF and `LadderBank` to **64-bit `double` nonlinear 4-pole ZDF ladder** (pivotal ZDF / 2–4 deterministic Newton-Raphson iterations, cutoff clamped `[10 Hz, 0.485 * fs]` at 1x, calibrated bass-loss compensation, `±15 ct` self-osc tracking).
  4. **Feedforward ADAA (`Ruin.h`, `Pressure.h`):** `TubeDrive` (starved `-60.5 dB`!), `BiasModulator`, and `OutputStage` exciter/clipper (`-74.9 dB`) lack ADAA. Apply `double`-precision **ADAA1/ADAA2** (calibrated `eps` midpoint fallback) to all feedforward nonlinearities + `~5 Hz` 64-bit DC blocker + curve crossfade on `tube_mode` switch.
  5. **True Zero-Latency Bypass & Exact PDC (`PluginProcessor.cpp`, `Ruin.h`, `Pressure.h`):** Currently `RuinCore` + `OutputStage` always run 4x oversampling + 101-sample `TruePeakCeiling` look-ahead (110 samples even when bypassed), and `voiceOs` latency is unreported. Add synchronous `prepareToPlay` PDC with **0-sample latency when oversampled drive/clipper are bypassed** and 0-sample impulse-to-`getLatencySamples()` discrepancy when active.
  6. **Odd-Block Invariance & Deterministic PRNG (`PluginProcessor.cpp`, `Voice.h`, `Modulation.h`, `Shape.h`, `Sources.h`, `Space.h`, `Resonator.h`):** Chunk-aligned updates (`kChunk=64`, `kSub=32`, `(i & 7) == 0`, `(i & 15) == 0`, `(i & 3) == 0`, `kBlk=16`) diverge across odd/prime block sizes (`1, 7, 13, 137`). Decouple via **fixed internal sub-block/absolute sample counters** and a **QA-resettable deterministic PRNG** so Gate 5 residual `< -100 dBFS`.
  7. **Envelopes & Voice Stealing (`Voice.h`):** `Adsr` is linear; voice stealing jumps pitch/envelope without a de-click fade. Upgrade to **exponential/RC curved ADSR**, add **2–5 ms raised-cosine de-click crossfade** on voice stealing, and evaluate scaling `kVoices` to **16 voices** with portable SoA SIMD (`< 15.0%` RT @ 48 kHz).
  8. **V2 Organic Layer & Procedural Texture Bus (`Sources.h`, `Voice.h`, `Parameters.h`):** Add append-only **Dark Analog Organic Layer** (1/f drift, chassis instability, voice-gated thermal floor, deterministic PRNG) and **Procedural Texture & Sub-Harmonic Layer** (0% default mix, 0 latency when bypassed, 100% procedural).

---

### 5. Source File Index (`file : symbol (lines)`)
- **`CMakeLists.txt`**: project/JUCE (1–21), `juce_add_plugin` (23–46), `VesselData`/sources/defs/libs (49–83), `Wavetables` bundle copy (85–106), `VESSEL_BUILD_CHECK` tools (109–128).
- **`Source/Parameters.h`**: `VESSEL_PARAMS` X-macro (20–400, 348 params: Source 21–71, Voice 72–78, Ruin 79–114, Shape 115–148, Space 149–171, Pressure 172–202, LFOs 203–230, Expr 231–244, LFO ext 245–272, Res 273–284, WT/FM 285–295, Macros 1–8 296–400), `PID` (402–408), `ParamSpec`/`stageOf` (410–425), `modDestinations` (430–431), index helpers `lfoBase/lfoExt/lfoDestPid/exprDestPid/macroPid/macroDestPid` (438–465), `kFmRatios` (468).
- **`Source/Parameters.cpp`**: `kSpecs` (8–14), `splitChoices` (16–27), `stageOf`/`stageName` (34–60), `modDestinations`/`destIndexToPid` (62–91), `formatValue` (93–140), `parseValue` (142–173), `createLayout` (175–207).
- **`Source/Presets.h/.cpp`**: `FactoryPreset` (H:23–28), `PresetBank` (H:30–51), `destIndexOf` (C:52–57), `categoryOrder` (C:59–63), `fromXml` (C:65–94), `factory()`/`factoryErrors()` (C:100–126).
- **`Source/PluginProcessor.h`**: `Telemetry` lock-free struct + `kScope=2048` (20–36), `VesselProcessor` class (38–200), `pv`/`pvExtra`/`pb`/`pc` (114–123), `kChunk=64` (125), DSP members (141–178).
- **`Source/PluginProcessor.cpp`**: ctor/dtor (18–44), `prepareToPlay` (54–104), `totalLatency` (106–109), `snapToCurrentState` (112–125), `handleMidi` (128–178), `processBlock` (180–240), `smoothControls` (246–261), `isVoiceParam`/`setVoiceField` (267–306), `applyExpression` (313–326), `applyMacros` (330–342), `macroName`/`setMacroName` (344–358), `buildVoiceParams` (360–411), `faded` (414–426), `renderChunk` (428–669), `timerCallback`/`refreshImpulseResponse`/`loadUserImpulse` (672–734), `randomiseDrawTable`/`previewLfo` (736–756), programs & `applyPreset` (759–805), `getStateInformation`/`setStateInformation` (807–874).
- **`Source/PluginEditor.h/.cpp`**: `VesselEditor` (`kWidth=1240, kHeight=780`, OpenGL `gl`/`backdrop`, `canvas`, `header`, `spine`, `pages[5]`, `rail`, `mod`, `browser`, `torch`) (H:12–63; C:ctor 5–49, dtor 51–58, `showStage` 60–65, `resized` 67–81, `showBrowser` 83–88, `routeChanged`/`routesCleared` 90–98, `setTorch`/`setDropPreview`/drag 100–123, `paint` 125–137, `timerCallback` 139–155).
- **`Source/dsp/Common.h`**: constants & math helpers (`kPi`, `fastTanh`, `flushDenormal`, `fastTan`) (12–37), `Ramp` (42–56), `BypassFade` (59–67), `Rng` xorshift32 (70–78), `OnePole` (81–88), `Smooth` (91–98), `EnvFollower` (101–116), `Svf` Cytomic float (119–154), `Tilt` (157–173), `DcBlocker` (175–181), `DelayLine` (184–240), `Drift` (243–263).
- **`Source/dsp/Wavetable.h/.cpp`**: `WavetableBank` (`kSize=2048`, `kFrames=8`, `kMips=11`, `fromFrames`, `mipFor`, `read`, `readMorph`) (H:10–66; C:5–134), `WavetableOsc` (`mipBlend`, `table`, `process` w/ 2-pt PolyBLAMP) (H:78–131).
- **`Source/dsp/Tables.h/.cpp` & `TableList.h`**: `kTables[379]` (TableList:10–393), `tablesFolder`/`loadTable`/`tableMenuNames` (C:10–62), `TableStore` lock-free hazard-pointer background FLAC loader (`acquireAudio`/`releaseAudio`/`loadNow`/`evict`) (H:35–79; C:65–148).
- **`Source/dsp/Voice.h`**: `Adsr` linear (14–87), `LadderBank<L>` SoA float ZDF ladder (96–144), `VoiceParams` (146–164), `Voice` (`prepare`, `reset`, `start`, `control`, `renderBlock`, `filterTargets`, `subShape`) (169–429), `VoiceManager` (`kVoices=8`, `kOs=4`, `kSub=32`, poly/mono/legato, `render`) (435–639).
- **`Source/dsp/Modulation.h`**: `WarpedLfo` (11 shapes, 3 trigger modes, sync, warp, fade, smooth, unipolar) (14–190).
- **`Source/dsp/Sources.h`**: `DroneEngine` (11–137), `NoiseInjector` (142–243), `GranularLooper` (248–415).
- **`Source/dsp/Ruin.h`**: `TubeDrive` (11–74), `BiasModulator` (79–128), `RuinCore` 4x OS wrapper (135–199), `BitCrusher` (204–255), `VhsTransport` (260–326), `TapeMotor` (333–434).
- **`Source/dsp/Shape.h`**: `FollowerFilter` (9–49), `Unmasker` (54–93), `FormantFilter` (98–162).
- **`Source/dsp/Resonator.h`**: `Resonator` 8-model waveguide+modal engine (`String..Gong`, 8 slots, `updateSlot`, `tickWaveguide`, `tickModal`) (41–542).
- **`Source/dsp/Space.h`**: `ZAxis` (11–72), `ImpulseFactory` (76–184), `BlackHole` 8-line Hadamard FDN (189–337), `HaasWidener` (343–377).
- **`Source/dsp/Pressure.h`**: `MultibandOtt` LR4 3-band (10–91), `MonoCollapse` LR4 M/S (96–128), `Ducker` (134–162), `TruePeakCeiling` SIMD 16x Kaiser look-ahead (177–325), `OutputStage` 4x OS exciter+clipper+TP (334–426).
- **`Source/ui/Backdrop.h/.cpp`**: OpenGL ink shader (to be replaced with native cached JUCE `Graphics` in Stage 5).
- **`Source/ui/Reliquary.h/.cpp`**: `ink::` palette, `type::` fonts, `ReliquaryLookAndFeel` (Halo/Vial/Bar/Rune/ComboBox/Menu/Resizer).
- **`Source/ui/Controls.h/.cpp`**: `ParamSlider`, `Rune`, `Selector` (with drag-drop & right-click routing).
- **`Source/ui/Routing.h/.cpp`**: `ModSource`, `routing::` add/remove/slotFor/routesTo, `makeDragImage`.
- **`Source/ui/Torch.h/.cpp`**: VBlank flashlight overlay.
- **`Source/ui/Panels.h/.cpp`**: `ModuleColumn` (C:80–202), `StagePage` (C:205–232), `buildStage` 0..4 (C:235–736), `Spine` (C:739–817), `Header` (C:820–914), `MasterRail` (C:917–1055), `ModPanel` (C:1068–1652), `PresetBrowser` (C:1655–1846).
- **`tools/`**: `HostProbe.cpp` (1–234), `VesselCheck.cpp` (1–737), `VesselLab.cpp` (1–916).

---

### 6. V2 Blueprint (Stages 2–7)
- **Stage 2 (Locked 8-Gate DSP QA Harness):** Add `Vessel2QA` headless C++ target in `CMakeLists.txt` (zero GUI/OpenGL/audio-device deps, portable across macOS/Win/Linux, C++20 `std::numbers::pi_v`), locking all 8 gates (1: Osc/FM SFDR, 2: Bandwidth/Transient, 3: ADAA/Nonlinearity, 4: Filter Audio-Rate Stability & Self-Osc Tracking, 5: Odd-Block/Rate-Switch/PDC Invariance, 6: Voice Stealing/DC/Idle Tail, 7: V1 Factory Preset & Stereo-Bass Snapshot, 8: Release `-O3` Polyphonic CPU `<15%` RT). Capture baseline.
- **Stage 3A (DSP Core — Osc, 64-Bit ZDF Filters & ADAA):** 3–4 mip/octave FFT wavetables + cubic Hermite interp + 4-pt PolyBLAMP/PolyBLEP; oversampled TZFM w/ bidirectional `[0,1)` wrap + `5 Hz` 64-bit DC blocker; 64-bit `double` Cytomic TPT SVF & nonlinear 4-pole ZDF ladder (2–4 NR iterations, bass-loss comp); `double` ADAA1/ADAA2 on feedforward `TubeDrive`/`BiasModulator`/`OutputStage` + curve crossfade + true zero-latency bypass PDC.
- **Stage 3B (Modulation, Organic Layer, Procedural Texture & SIMD — FREEZE DSP):** Fixed-cadence absolute-sample-counter modulation (odd-block invariant `< -100 dBFS`), QA-resettable deterministic PRNG, RC/exponential ADSR, 2–5 ms raised-cosine voice-steal de-click, V2 Dark Analog Organic Layer (`v2_organic_drift`, thermal floor, chassis instability, sub-120 Hz mono discipline), V2 Procedural Texture Bus (`v2_tex_mode`, `v2_tex_mix=0%`, `v2_tex_tone`), 16-voice portable SoA SIMD optimization (`<15%` RT). Freeze DSP core.
- **Stage 4 (Control Plane & V2 Preset Engine):** Keep all 348 V1 params intact; append V2 control-plane params (`v2_xy_x/y` + XY axis routing, `v2_mpe_on`, `v2_vel_curve`, `v2_breath_*`, `v2_slide_*`, `v2_macro_hero` bindings to `macro1..4`), thread-safe right-click **MIDI CC Learn** persisted via `userApplicationDataDirectory`, **A/B State Compare + Copy A->B**, `juce::UndoManager` **Undo/Redo**, and **Versioned Preset Manager (`version="2"`)** with User Presets/Favorites and lossless V1 migration (`±0.05 dB` RMS).
- **Stage 5 (NAMM 2027 Brutalist-Industrial UI/UX):** Remove `juce_opengl` / `Backdrop` GLSL dependency; build pre-rendered 2x Retina `juce::Image` static panel/chassis cache + 60 Hz native JUCE `Graphics` live overlays on strict 8px grid (deep obsidian/gunmetal, hairline bevels, cold-steel/phosphor-amber & pale-cyan arcs, consistent typography). Interactive centerpiece: 60 Hz lock-free Dual Oscilloscope/Spectral-Contour + draggable XY Sculptor. Top bar: VESSEL II wordmark, Preset Bar + Searchable/Tagged/Favorites Browser Overlay, A/B, Undo/Redo, MPE badge, `75/100/125/150%` zoom + persisted aspect-locked resize, Peak/LUFS & Sub-120 Hz mono-correlation meters. Bottom dock: 4 Hero Macro knobs w/ dynamic names + live mod arcs + Organic Drift & Texture Mix. Mandatory 1x/2x Retina headless snapshot self-inspection.
- **Stage 6 (20 Flagship Production Presets):** 5 categories × 4 presets = 20 original commercial presets (zero third-party names): *Subterranean Basses & Monolith Subs*, *Haunting Analog Brass & Dystopian Leads*, *Cathedral & Dystopian Pads*, *Fractured IDM Plucks, Keys & Metallic Bells*, *Club Anthems, Tension Hooks & Hybrid Weapons*. All 4 Hero Macros + Mod Wheel + MPE/Aftertouch active; closed-loop QA (`TP <= -0.5 dBFS`, RMS `-18..-10 dBFS`, DC `< -80 dBFS`, sub-120 Hz corr `>= +0.5`, mono drop `<= 3.5 dB`, active macro delta, timbral diversity) + 20× 4s 48k/24-bit WAV audition clips.
- **Stage 7 (3-OS Cross-Platform Release & Packaging):** Harden CMake (Windows MSVC `NOMINMAX`/`WIN32_LEAN_AND_MEAN`, Linux `Wavetables` copy, macOS Universal `arm64;x86_64`), build & validate macOS locally (`Vessel2QA`, `auval`, ad-hoc sign, local install, `dist/macos/VESSEL-II-macOS-Universal.zip`), add `.github/workflows/build-release.yml` (3-OS matrix + `pluginval` L5) + `scripts/build_windows.ps1` + `scripts/build_linux.sh`, run/verify CI or local builds, clean scratch logs (keeping audition WAVs, UI snapshots, `dist/`).

---

## Stage 2 — Locked 8-Gate Headless DSP QA Harness & Baseline Capture
- **Harness:** `tools/Vessel2QA.cpp` wired permanently into `CMakeLists.txt` (`Vessel2QA`, `VESSEL_HEADLESS=1`, C++20 `std::numbers::pi_v`, zero GUI/OpenGL/audio-device deps, Release `-O3`, 0 warnings). Added read-only `VesselProcessor::activeVoiceCount()` and `#if VESSEL_HEADLESS` editor/timer guards in `Source/PluginProcessor.{h,cpp}`; added `CMAKE_EXPORT_COMPILE_COMMANDS ON` and MSVC `NOMINMAX`/`WIN32_LEAN_AND_MEAN` to `CMakeLists.txt`.
- **Scratch Artifacts:** `build/vessel2_qa/qa_report.txt`, `build/vessel2_qa/v1_preset_baseline.csv`, `build/vessel2_qa/build.log`.

| Gate | Name | V1 Baseline | Locked Target | Status |
|---|---|---|---|---|
| 1 | Osc & FM Aliasing Floor | C6 `-88.5`, C7 `-86.4`, FM C5 (`1:1`/`1:1.414` idx `3.0`) `-88.1`/`-88.5 dBc` | `< -80` C6, `< -75` C7, `< -70 dBc` FM | **PASS** |
| 2 | Bandwidth & Transient | Saw C4 BW `18314 Hz` @48/96k; ADSR `0.46 ms`, **Proc `7.62 ms`** (`fltFade.snap(true)` bug) | `>= 18 kHz`, `90%` in attack, 0 DC step | **FAIL** |
| 3 | Nonlinearity & ADAA | ADAA F1'/F2' **PENDING**; Tube `-27.9..-24.3 dBc` (`15.7-19.5 dB` supp), Clip `-27.0 dBc` (`1.8 dB` supp), DC `<-111 dBFS` | `F1'==f < 1e-5`; `< -80 dBc` or `>=20 dB` supp; DC `<-90` | **FAIL** (`PENDING` ADAA) |
| 4 | Filter Mod & Self-Osc | 1 kHz mod 44.1..192k 95–100% res bounded (`0` NaN); SVF `0.02 ct`, Lad `0.32 ct`; rateDiff `0.479 dB` | Bounded; SVF `±5 ct`, Lad `±15 ct`; `±0.5 dB <=12k` | **PASS** |
| 5 | Odd-Block, Rate & PDC | Odd res **`-80.2 dBFS`**, rate-switch res **`-30.5 dBFS`** (`TubeDrive` smoothers unreset), PDC act `110/111`, byp **`110/111`** | Res `< -100 dBFS`, `0` PDC err, `0` bypass lat | **FAIL** |
| 6 | Voice Steal, DC & Idle | Steal 2nd-diff **`20.87x`**, DC **`-48.4 dBFS`**; 10s tail `-300 dBFS`, voices `0`, idle CPU **`1.255%` RT** | `<=5x` curv, DC `<-90`, tail `<-120`, `0` v, `<0.5%` idle | **FAIL** |
| 7 | V1 Preset Snapshot | `16/96` diverse presets rendered, `0` non-finite, `minSub120Corr=0.996` (saved `v1_preset_baseline.csv`) | Finite, baseline captured (`±0.05 dB` regress in S4–7) | **PASS** |
| 8 | Poly CPU Budget (`-O3`) | 8/8 voices stress @48k: median **`643.8 µs` (`6.04%` RT)**, p95 `670.5 µs` (`6.29%` RT) | `< 15.0%` RT (16 voices after Stage 3B) | **PASS** |

- **Stage 2 Status:** `PASS` (4/8 baseline PASS, 4/8 baseline FAIL exposing exact Stage 3A/3B gaps, ADAA Part A `PENDING` until Stage 3A). **Next:** `Stage 3A`.
