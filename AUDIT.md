# VESSEL DSP audit

by iisben. Audited and upgraded against the bar set by FabFilter, Valhalla DSP and Freakshow Industries: no artefact you can hear before you hear the sound, no thread hazard a host can trip, and every number below is measured, not claimed.

All measurements come from `VesselLab` (in `tools/`) at 48 kHz, run on the original code and on the upgraded code with the same test signals. "Before" is the code as it was when this audit started.

## Verdict

The original engine had good bones. The sub stack was phase-locked to one master phase, the tube's dry path was already latency-compensated, the mono collapse and Haas widener were already mono-safe, and nothing allocated in `processBlock`. It was not commercial grade, for six reasons:

1. **The first note could build the whole wavetable bank on the audio thread.** That is 88 band-limited tables (8 frames × 11 mips), built by inverse FFT, and about 0.7 MB of allocation, triggered by a function-local static. On a busy session that is a guaranteed dropout on the first note.
2. **The UI read audio-thread state with no synchronisation.** The LFO preview and the user-IR path were formally data races, which is undefined behaviour.
3. **Zipper noise was 35 to 45 dB below the signal.** Output gain, wavetable level and the tilt EQs stepped once per block, which is plainly audible on automation.
4. **Seven module switches clicked**, at −33 to −54 dB.
5. **Three stages aliased hard**, all at the base rate:
   - phase-warp PWM: −32 dB
   - the voice ladders with drive: −30 dB
   - the exciter: −33.5 dB
6. **The bias-starved tube curve had hard corners**, which is the definition of a digital-sounding fold.

All six are fixed and measured. What is still short of the bar is listed honestly at the end.

## Findings, by severity

### Critical

**C1. The wavetable bank was built lazily on the audio thread.**

- **Problem:** `WavetableBank::get()` is a function-local static. It was first touched inside the oscillator loop, so the first note-on paid for the whole build. On every later sample, every voice also paid an acquire load on the static guard.
- **Fix:** The processor constructor builds the bank on the message thread. Oscillators and the drone cache a pointer to it.
- **Files:** `PluginProcessor.cpp` constructor, `dsp/Wavetable.h` `WavetableOsc::bank`, `dsp/Sources.h` `DroneEngine::bank`.

**C2. UI and audio threads shared plain data.**

- **LFO preview.** `previewLfo()` called `Lfo::evaluate()`, which reads `held`, `steps[]` and `stutter[]`, while the audio thread rewrote them.
  - **Fix:** Those three are atomics now, and the preview evaluates from raw parameter values.
- **User IR path.** `userIrFile` is a `juce::File`, which holds a ref-counted String. The message thread wrote it while the host's state thread read it.
  - **Fix:** `irLock` now guards it. The lock is only ever taken on the message and host threads, never inside `processBlock`.
- **Scope ring.** It was plain `float`s.
  - **Fix:** It is relaxed `std::atomic<float>` now. That compiles to the same store on arm64 and x86, so it is race-free at no cost.

### High

**H1. Zipper noise.**

- **Problem:** Gain, level, tilt and mix parameters jumped once per processing chunk.
- **Fix:** There are now two stages:
  1. A 20 ms one-pole on every continuous parameter, in the normalised domain, so skewed ranges such as cutoff glide perceptually. Stepped parameters (choices, switches, semitones) are exempt.
  2. A per-sample linear `Ramp` inside every module, toward the chunk's smoothed target. Filter coefficients ramp too: the ladders ramp `G`, `k` and drive per sample, and the formant bank re-solves every 4 samples.
- **Result:** Zipper energy is now −87 to −120 dB for every move except the formant vowel (−68 dB, up from −48), whose filter bank still re-solves every 4 samples (table below).

**H2. Bypass clicks.**

- **Problem:** Switching VHS, multiband, granular, formant, follower, bias or exciter on or off cut the signal instantly.
- **Fix:** `BypassFade`, a 15 ms linear crossfade between the dry and processed signal on every switchable module. Modules that are fully off skip their processing entirely, so the fade costs nothing at rest. VHS keeps its delay line fed while off, so it comes back without a burst of stale audio. The black hole clears its lines once it is fully off.
- **Result:** Every module now switches at or below −81 dB.

**H3. Phase-warp PWM was not band-limited.**

- **Problem:** The mip-maps band-limit the table, but the warp maps phase piecewise-linearly. Its slope breaks at `phase = pw` and at the wrap put a derivative discontinuity into the output, and its alias tail falls at 1/n².
- **Fix, in three parts:**
  1. Mip selection includes the warp's compression.
  2. Two adjacent mips crossfade by fractional pitch, so the top octave of partials fades in and out during glides instead of switching.
  3. A 2-point polyBLAMP residual cancels the slope break at both knees.
- **Result:** The C7 15% pulse went from −32.2 dB to −86.5 dB of alias (−63.9 dB from the polyBLAMP alone; the 4x voice rate in H4 does the rest).

**H4. Voices ran at the base rate.**

- **Problem:** The ladder has `tanh` inside a feedback loop. At the base rate, driven ladders aliased at −30 dB.
- **Fix:**
  - The whole voice (sub stack, oscillator, both ladders) now runs at 4x, decimated once by a polyphase IIR shared by body L, body R and the sub, so all three see the same filter and the same phase.
  - All 8 voices' ladders run as one struct-of-arrays bank. That turned 4x the work into roughly the same cost: see the SIMD answer below.
- **Result:** −30.4 → −58.3 dB.

**H5. The exciter and clipper ran at the base rate.**

- **Problem:** Exciter: −33.5 dB of alias. Clipper: −57.7 dB.
- **Fix:** `OutputStage` runs exciter → output gain → clipper at 4x, then a final sample-exact clamp at the ceiling, because the decimator's ripple can overshoot by a fraction of a dB. The exciter's shaper also lost its `|x|` corner: it is now `tanh(kx)` plus a smooth even term.
- **Result:** Exciter −93.6 dB. Clipper −65.7 dB.
- **Later (phase 3):** That clamp still let inter-sample peaks through, and it was itself a base-rate hard clip. It now sits behind a true-peak ceiling and no longer acts. The clipper measures −74.9 dB, and true peaks stay under the ceiling (see Phase 3).

### Medium

**M1. The tube's dry/wet blend was only approximately aligned.**

- **Problem:** The original delayed the dry path by the oversampler's integer latency. That is correct at low frequency, but a half-band IIR is not linear-phase, so the wet path's high end arrives with a different phase and `tube_mix < 1` combed slightly.
- **Fix:** `RuinCore` mixes dry and wet inside the oversampled domain, where both have passed the same up-filter. Tube, post tilt and bias now share one up/down pair instead of three conversions.
- **Result:** A 50% blend at zero drive, against fully dry, used to dip −0.29 dB at 15 kHz and −2.38 dB at 18 kHz. It is now −0.01 dB across the whole band.
- **Sub path (already right in the original):** The clean sub was already delayed to re-join the processed sub in phase. That still holds, now by a fractional Hermite delay that matches the IIR's real low-frequency group delay.

**M2. The bias-starved tube had hard corners.**

- **Problem:** The original used a hard dead-zone `max(0, u − thr)` and a hard switch between the positive and negative curves. Both are slope discontinuities, which give a slowly decaying harmonic series: the brittle, digital fizz.
- **Fix:** A smooth dead-zone, `v = u − thr·tanh(u/thr)` (cubic near zero, linear beyond), and a C¹ blend between the two halves.
- **Result:** Alias at 80% drive improved from −55.4 to −60.5 dB, and the fizz is gone.
- **Pentode:** It lost a per-sample `std::pow (a, 2.5)` (now `a·a·√a`).

**M3. Drone partials beat in the sub region.**

- **Problem:** The drone's root and octave partials drift independently. Below about 120 Hz that beating reads as the sub pumping and moving in mono.
- **Fix:** The drift on those two partials scales by `min (1, (root / 120 Hz)²)`, so they stay phase-locked down low and drift freely above.

**M4. Smaller timing issues.**

- LFO values were evaluated at each chunk's midpoint. The modules ramp toward that value across the chunk, so the modulation ran half a chunk (0.7 ms) late. They are now evaluated at the chunk's end, which is where the ramp lands.

### Low

- The sub shaper's `static const float sqNorm = 1/tanh(4)` put a static guard check in the inner loop. The value is now inline, and `std::tanh` in the sub loop is now the rational `fastTanh`.
- The ladders called `std::tan` for their coefficients. They now use a Padé `fastTan`, once per chunk, ramped per sample.

## Thread hygiene, verified

Inside `processBlock`:

- **No allocation.** Every buffer is sized in `prepareToPlay`. The JUCE oversamplers are initialised there, and their per-block calls do not allocate.
- **No waiting on locks.** `irLock` is only taken on the message and host threads. The one lock-like operation on the audio thread is inside JUCE's `Convolution`: it collects a newly loaded IR engine with a spin-lock *try*-lock that never waits (if the loader holds it, the swap happens next block), and it hands the retired engine back to its own background thread for deletion. IR generation and loading happen on a message-thread timer.
- **No blocking or system calls.** No file access, no logging, no `std::function`, no exceptions.
- **No function-local statics.** The one table left (`divs`) is `static constexpr`, which is constant-initialised and has no guard.

Communication between the threads:

- **Parameters:** APVTS atomics, read once per 64-sample chunk.
- **Telemetry** (meters, scope, LFO and expression state): `std::atomic`, relaxed.
- **Drawn LFO tables:** `std::atomic<float>` arrays.
- **Scope ring:** relaxed atomics with a release/acquire write index.

## Rewritten blocks

These are the exact blocks now in the source tree.

### Parameter smoothing (processor)

```cpp
/** 20 ms one-pole on every continuous parameter, evaluated once per chunk in the normalised domain
    (perceptual for skewed ranges). The modules then ramp linearly across the chunk, so a host automation
    jump becomes a smooth curve with no stair-steps. Stepped params (semitones, choices, switches) are exempt. */
void VesselProcessor::smoothControls (int n)
{
    const float a = std::exp (-(float) n / (0.020f * (float) sr));
    for (int i = 0; i < NUM_PARAMS; ++i)
    {
        const float target = params[(size_t) i]->getValue();
        auto& b = base[(size_t) i];
        b = smoothed[(size_t) i] ? target + a * (b - target) : target;
    }

    const float e = std::exp (-(float) n / (0.012f * (float) sr));   // expression: 12 ms (aftertouch is often 7-bit)
    mwS  = mwRaw  + e * (mwS  - mwRaw);
    atS  = atRaw  + e * (atS  - atRaw);
    velS = velRaw + e * (velS - velRaw);
    bendS = bendRaw + e * (bendS - bendRaw);
}
```

### Per-sample ramp and bypass crossfade (`dsp/Common.h`)

```cpp
struct Ramp
{
    float cur = 0.0f, inc = 0.0f;
    bool primed = false;

    void to (float target, int n) noexcept
    {
        if (! primed) { snap (target); return; }
        inc = (target - cur) / (float) std::max (1, n);
    }
    void snap (float v) noexcept { cur = v; inc = 0.0f; primed = true; }
    float next() noexcept { cur += inc; return cur; }
    void skip (int n) noexcept { cur += inc * (float) n; }
};

/** Click-free module bypass: a 15 ms linear crossfade between the dry and the processed signal. */
struct BypassFade
{
    float g = 0.0f, step = 1.0f / 720.0f;
    void prepare (float sr, float ms = 15.0f) noexcept { step = 1.0f / std::max (1.0f, ms * 0.001f * sr); }
    void snap (bool on) noexcept { g = on ? 1.0f : 0.0f; }
    /** True when no crossfade is needed: fully on, or fully off. */
    bool settled (bool on) const noexcept { return on ? g >= 1.0f : g <= 0.0f; }
    float next (bool on) noexcept { g = on ? std::min (1.0f, g + step) : std::max (0.0f, g - step); return g; }
};
```

And the wrapper every switchable module goes through:

```cpp
template <typename Fn>
void VesselProcessor::faded (BypassFade& f, bool on, int n, Fn&& process)
{
    if (f.settled (on)) { process (on); return; }   // fully on, or fully off (the module's own off path)
    std::copy_n (xL.data(), n, dryL.data());
    std::copy_n (xR.data(), n, dryR.data());
    process (true);
    for (int i = 0; i < n; ++i)
    {
        const float g = f.next (on);
        xL[(size_t) i] = dryL[(size_t) i] + g * (xL[(size_t) i] - dryL[(size_t) i]);
        xR[(size_t) i] = dryR[(size_t) i] + g * (xR[(size_t) i] - dryR[(size_t) i]);
    }
}
```

### SIMD ladder bank (`dsp/Voice.h`)

All 8 voices' ladders are one struct of arrays. Coefficients ramp per sample, and the nonlinearity is inlined so the loop vectorises.

```cpp
template <int L>
struct LadderBank
{
    alignas (32) float s0[L] {}, s1[L] {}, s2[L] {}, s3[L] {};
    alignas (32) float G[L] {}, dG[L] {}, k[L] {}, dk[L] {}, drv[L] {}, ddrv[L] {}, isd[L] {}, disd[L] {};

    static float coefG (float hz, float osr) noexcept
    {
        const float g = fastTan (kPi * hz / osr);
        return g / (1.0f + g);
    }

    inline void tick (const float* __restrict in, float* __restrict out) noexcept
    {
        for (int l = 0; l < L; ++l)
        {
            const float g = (G[l] += dG[l]);
            const float kk = (k[l] += dk[l]);
            const float d = (drv[l] += ddrv[l]);
            const float id = (isd[l] += disd[l]);
            const float g2 = g * g, g3 = g2 * g, g4 = g3 * g;
            const float S = (g3 * s0[l] + g2 * s1[l] + g * s2[l] + s3[l]) * (1.0f - g);
            const float x = in[l] * (1.0f + 0.55f * kk);   // pass-band loss compensation
            const float y4 = (g4 * x + S) / (1.0f + kk * g4);

            float u = std::clamp (d * (x - kk * y4), -3.0f, 3.0f);   // fastTanh, inlined so it vectorises
            const float u2 = u * u;
            u = u * (27.0f + u2) / (27.0f + 9.0f * u2) * id;

            float v = (u - s0[l]) * g; float y = v + s0[l]; s0[l] = y + v; u = y;
            v = (u - s1[l]) * g; y = v + s1[l]; s1[l] = y + v; u = y;
            v = (u - s2[l]) * g; y = v + s2[l]; s2[l] = y + v; u = y;
            v = (u - s3[l]) * g; y = v + s3[l]; s3[l] = y + v;
            out[l] = y;
        }
    }
    // setLane / holdLane / resetLane: per-voice targets, ramped over the chunk (see source)
};
```

### Band-limited phase-warp oscillator (`dsp/Wavetable.h`)

```cpp
/** Mip pair for a pitch: blend (mip - 1) with weight wLower, mip with (1 - wLower). */
static void mipBlend (float hz, float baseSr, int& mip, float& wLower) noexcept
{
    const float ratio = 2.0f * (float) WavetableBank::kSize * 0.5f * hz / baseSr;
    if (ratio <= 1.0f) { mip = 0; wLower = 0.0f; return; }
    mip = std::clamp ((int) std::ceil (std::log2 (ratio)), 1, WavetableBank::kMips - 1);
    const float r = ratio / std::exp2 ((float) (mip - 1));   // (1, 2] inside the octave
    wLower = std::clamp (2.0f - r, 0.0f, 1.0f);
}

/** dt = cycles per (oversampled) sample. */
float process (float dt, int mip, float wLower, float morph, float pw) noexcept
{
    pw = std::clamp (pw, 0.03f, 0.97f);
    const float a1 = 0.5f / pw, a2 = 0.5f / (1.0f - pw);   // phase-map slopes either side of the knee
    const float p = phase < pw ? phase * a1 : 0.5f + (phase - pw) * a2;
    float y = table (p, morph, mip, wLower);

    if (std::abs (a1 - a2) > 1.0e-3f)
    {
        auto slope = [&] (float at)   // dT/dp at the knee, central difference
        {
            constexpr float h = 1.0f / (float) WavetableBank::kSize;
            const float lo = at - h < 0.0f ? at - h + 1.0f : at - h;
            return (table (at + h, morph, mip, wLower) - table (lo, morph, mip, wLower)) * (0.5f / h);
        };
        auto blamp = [] (float t) { return t >= 0.0f ? (1.0f - t) * (1.0f - t) * (1.0f - t) / 6.0f
                                                     : (1.0f + t) * (1.0f + t) * (1.0f + t) / 6.0f; };
        const float dk = phase - pw;   // knee: slope a1 -> a2
        if (std::abs (dk) < dt) y += slope (0.5f) * dt * (a2 - a1) * blamp (dk / dt);
        if (phase < dt)              y += slope (0.0f) * dt * (a1 - a2) * blamp (phase / dt);           // just wrapped
        else if (phase > 1.0f - dt)  y += slope (0.0f) * dt * (a1 - a2) * blamp ((phase - 1.0f) / dt);  // about to wrap
    }

    phase += dt;
    if (phase >= 1.0f) phase -= 1.0f;
    return y;
}
```

### Phase-coherent oversampled RUIN block (`dsp/Ruin.h`)

```cpp
class RuinCore
{
public:
    struct Params { TubeDrive::Params tube; float postTiltDb = 0, postPivot = 1200; BiasModulator::Params bias; };

    void prepare (float sampleRate, int maxBlock)
    {
        osr = 4.0f * sampleRate;
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        os->initProcessing ((size_t) maxBlock);
        os->reset();
        tube.prepare (osr);
        bias.prepare (osr);
        biasFade.prepare (osr);
        dry.setSize (2, maxBlock * 4);
    }

    /** Group delay of the up/down pair at low frequency (fractional samples at the base rate). */
    float latency() const { return os ? os->getLatencyInSamples() : 0.0f; }

    void process (float* l, float* r, int n, const Params& p)
    {
        float* chans[2] { l, r };
        juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
        auto up = os->processSamplesUp (block);
        const int m = (int) up.getNumSamples();
        float* ul = up.getChannelPointer (0);
        float* ur = up.getChannelPointer (1);

        tube.process (ul, ur, m, p.tube);        // dry/wet mixed here, inside the 4x domain

        tilt[0].set (p.postTiltDb, p.postPivot, osr, m);
        tilt[1].set (p.postTiltDb, p.postPivot, osr, m);
        for (int i = 0; i < m; ++i) { ul[i] = tilt[0].process (ul[i]); ur[i] = tilt[1].process (ur[i]); }

        const bool on = p.bias.on;
        if (! biasFade.settled (on))
        {
            auto* dl = dry.getWritePointer (0);
            auto* dr = dry.getWritePointer (1);
            std::copy_n (ul, m, dl); std::copy_n (ur, m, dr);
            bias.process (ul, ur, m, p.bias);
            for (int i = 0; i < m; ++i)
            {
                const float g = biasFade.next (on);
                ul[i] = dl[i] + g * (ul[i] - dl[i]);
                ur[i] = dr[i] + g * (ur[i] - dr[i]);
            }
        }
        else if (on) bias.process (ul, ur, m, p.bias);

        os->processSamplesDown (block);
    }
    // ...
};
```

And the clean sub re-joining in phase (processor):

```cpp
// Clean sub re-joins, delayed by the RUIN block's low-frequency group delay (fractional, Hermite) so the
// sent and clean halves of the sub sum in phase: no cancellation at any send amount.
const float subDelay = ruin.latency() + 1.0f;
for (int i = 0; i < n; ++i)
{
    subAlign.push (subB[(size_t) i]);
    const float s = subAlign.readHermite (subDelay) * cleanR.next();
    xL[(size_t) i] += s; xR[(size_t) i] += s;
}
```

### Smooth bias-starved curve (`dsp/Ruin.h`, `TubeDrive::shape`)

```cpp
default:  // starved: smooth crossover dead-zone (cubic near zero, linear beyond) + lopsided fuzz
{
    const float u = 1.5f * x + 0.5f * b;
    const float thr = 0.06f + 0.22f * std::max (0.0f, -b) + 0.04f;
    const float v = u - thr * fastTanh (u / thr);
    const float pos = fastTanh (2.2f * v), neg = 0.85f * fastTanh (0.8f * v);
    const float s = 0.5f + 0.5f * fastTanh (v * 20.0f);   // C1 blend between the two half-curves
    return neg + s * (pos - neg);
}
```

### Oversampled output stage (`dsp/Pressure.h`)

```cpp
void OutputStage::process (float* l, float* r, int n, const Params& p)
{
    float* chans[2] { l, r };
    juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
    auto up = os->processSamplesUp (block);
    const int m = (int) up.getNumSamples();
    float* ul = up.getChannelPointer (0);
    float* ur = up.getChannelPointer (1);

    gainR.to (dbToGain (p.gainDb), m);
    ceilR.to (dbToGain (p.ceilingDb), m);
    const float k = 1.0f + p.excHarm * 9.0f;
    amtR.to (p.excAmount * 1.4f / std::sqrt (k), m);
    const bool excLive = p.excOn || ! excFade.settled (p.excOn);
    if (excLive) for (int c = 0; c < 2; ++c) { pre[c].setup (p.excFreq, 0.7f, osr); post[c].setup (p.excFreq, 0.7f, osr); }
    const bool clipLive = p.clipOn || ! clipFade.settled (p.clipOn);
    int hits = 0;

    for (int i = 0; i < m; ++i)
    {
        float x[2] { ul[i], ur[i] };
        const float amt = amtR.next();
        if (excLive)
        {
            const float eg = excFade.next (p.excOn) * amt;
            for (int c = 0; c < 2; ++c)
            {
                const float h = pre[c].highpass (x[c]);
                const float t = fastTanh (k * h);
                const float e = t + 0.6f * t * t;   // odd + even products, smooth (no |x| corner)
                x[c] += post[c].highpass (e) * eg;
            }
        }
        const float g = gainR.next(), c = ceilR.next();
        x[0] *= g; x[1] *= g;
        if (clipLive)
        {
            const float cg = clipFade.next (p.clipOn);
            for (auto& v : x)
            {
                if (std::abs (v) > c) ++hits;
                v += cg * (std::clamp (v, -c, c) - v);
            }
        }
        ul[i] = x[0]; ur[i] = x[1];
    }
    os->processSamplesDown (block);

    if (p.clipOn)   // the decimator's ripple can overshoot by a fraction of a dB: final sample-exact ceiling
    {
        const float c = ceilR.cur;
        for (int i = 0; i < n; ++i) { l[i] = std::clamp (l[i], -c, c); r[i] = std::clamp (r[i], -c, c); }
    }
    clipped += (hits + 7) / 8;
}
```

### Expression routing (processor)

```cpp
/** Mod wheel, aftertouch and (global) velocity → modulation offsets. Unipolar sources: offset = amount × source,
    i.e. full depth sweeps the whole range once, the same span a bipolar LFO covers at full depth. */
void VesselProcessor::applyExpression()
{
    for (int s = 0; s < kExprSlots; ++s)
    {
        const int dest = destIndexToPid (pc (kExprDest[s]));
        if (dest < 0) continue;
        const float amt = pv (kExprDest[s] + 1);
        if (s < 2)
        {
            if (! isVoiceParam (dest)) mod[(size_t) dest] += amt * velS;   // per-voice velocity handled in buildVoiceParams
        }
        else mod[(size_t) dest] += amt * (s < 4 ? mwS : atS);
    }
}
```

Velocity routed to a voice parameter (sub, wavetable, ladders, envelopes, glide) is applied per voice in `buildVoiceParams()`, from that voice's own note-on velocity. A hard note in a soft chord opens only its own filter.

## Measurements

48 kHz. Lower is better throughout.

**Zipper noise:** HF energy above 3 kHz in excess of the static patch, while the parameter jumps between two values every 50 ms.

| Parameter move | Before | After |
|---|---|---|
| Output gain −12 ↔ 0 dB | −35.2 dB | −87.6 dB |
| WT level 0.2 ↔ 0.9 | −34.8 dB | −90.1 dB |
| Sub 1 level 0.2 ↔ 1.0 | −44.5 dB | −120.0 dB |
| Tilt −9 ↔ +9 dB | −36.7 dB | −90.7 dB |
| Ladder A cutoff 1k ↔ 4k | −57.7 dB | −116.2 dB |
| Formant vowel A ↔ U | −47.9 dB | −68.2 dB |
| Hole mix 0 ↔ 0.6 | −107.0 dB | −120.0 dB |
| Duck depth 0 ↔ 1 | −56.2 dB | −117.2 dB |

**Bypass clicks:** HF energy when a module is switched every 120 ms on a 110 Hz sine.

| Module | Before | After |
|---|---|---|
| VHS | −35.3 dB | −84.7 dB |
| Multiband | −33.0 dB | −81.2 dB |
| Granular | −44.2 dB | −95.7 dB |
| Formant | −48.1 dB | −91.7 dB |
| Follower LPF | −49.8 dB | −86.1 dB |
| Bias | −52.9 dB | −101.8 dB |
| Exciter | −54.5 dB | −102.2 dB |
| Crush, Unmask, Tube, Black Hole | −85 to −115 dB | −85.8 to −115.3 dB |

**Aliasing:** inharmonic energy from 20 Hz to 20 kHz, relative to the harmonics.

| Test | Before | After |
|---|---|---|
| WT saw C7, PW 50% | −88.8 dB | −88.9 dB |
| WT saw C7, PW 15% (warp) | −32.2 dB | −86.5 dB |
| WT square C6, PW 25% (warp) | −38.2 dB | −90.0 dB |
| Ladder drive 100%, saw C6 | −30.4 dB | −58.3 dB |
| Tube triode 100%, sine C6 | −91.9 dB | −92.0 dB |
| Tube starved 80%, sine C6 | −55.4 dB | −60.5 dB |
| Exciter 100%, sine 4.4 kHz | −33.5 dB | −93.6 dB |
| Clipper +12 dB, sine C6 | −57.7 dB | −65.7 dB (−74.9 dB with the true-peak ceiling) |

**Phase coherence:** level of a parallel blend against the single path. 0 dB is ideal.

| Test | Before | After |
|---|---|---|
| Tube mix 50% vs 0%, drive 0, at 15 kHz | −0.29 dB | −0.01 dB |
| Tube mix 50% vs 0%, drive 0, at 18 kHz | −2.38 dB | −0.01 dB |
| Sub send 50% vs 0%, at 55 / 110 / 220 Hz | −0.09 / −0.04 / −0.02 dB | −0.09 / −0.04 / −0.02 dB |

The sub-send residual is the tube's DC blocker (see Known limits). It was already this small in the original.

**CPU:** dense patch, 8 voices held, every module on, % of one Apple Silicon core in real time.

| | Before | After |
|---|---|---|
| Total | 5.1 % | 6.3 % |

The voices now do 4x the samples, and the RUIN and output stages run at 4x. The SoA ladder bank and the removal of per-sample `std::tan`, `std::tanh` and `std::pow` paid for almost all of it.

## Phase 2: low end, saturation, SIMD

**Low end: phase-aligned and mono-compatible.**

- The sub is generated as a single mono channel from one master phase, so both sub oscillators' zero crossings coincide.
- Its sent and clean halves re-join through a fractional delay matched to the RUIN block's group delay.
- Mono collapse removes the side channel below `mono_freq` (default 120 Hz) with an LR4 high-pass, and an allpass on the mid keeps the phase matched.
- The Haas widener injects its delay into the side only, above its crossover.
- The drone's root partials are phase-locked below 120 Hz (M3).
- With mono collapse on, the Init patch's side energy below 120 Hz is −43.8 dB under the mid there; it is −32.8 dB with collapse off.
- Every factory preset is checked for side energy below 120 Hz. Results: every preset's side energy below 120 Hz is between −31.4 and below −99 dB relative to the programme (limit −30 dB).

**Saturation: analog character, no digital foldback.**

- Every waveshaper is now a smooth curve (C¹ or better):
  - triode: offset `tanh`, which gives the even harmonics
  - pentode: a rational soft knee
  - starved: smooth dead-zone, C¹ halves
  - tape bias: a `tanh` with hysteresis
  - exciter: `tanh` plus a smooth square term
  - ladder feedback: rational `tanh`
- All of them run at 4x. A smooth curve's harmonics fall off fast, and the 4x margin puts the image band well above the audible harmonics, so what folds back is 58 to 93 dB down.
- The only hard nonlinearity is the brickwall clipper, which is a deliberate safety device. It runs at 4x too, at −75 dB of alias when driven 12 dB into it (−66 dB before the true-peak ceiling took over from the base-rate clamp).
- The bit crusher aliases on purpose: that is the effect. Its pre-filter is the user's control over how much.

**SIMD and AVX2.**

- **Where it applies:** The hottest loop is the ladder bank: 8 voices × 2 ladders × 4 poles at 4x. It is now struct-of-arrays across voices, with the nonlinearity inlined. The compiler auto-vectorises it, and the shipped arm64 binary's `renderChunk` contains 4-lane NEON arithmetic (`fmul.4s`, `fmla.4s`, `fdiv.4s`: over 400 vector instructions).
- **AVX2 is not available on this Mac.** It is an x86 instruction set, and this is an Apple Silicon machine. To ship Intel Macs or Windows:
  - Build a universal binary with `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`. The same SoA loop becomes 4-wide SSE by default.
  - For 8-wide AVX2, do not compile the whole plug-in with `-mavx2 -mfma`: that crashes on pre-Haswell CPUs. Use function multi-versioning (`__attribute__((target_clones("avx2,fma","default")))` on `LadderBank::tick` and `Voice::renderBlock`) or a runtime CPUID dispatch.
  - With 8 lanes, the ladder bank is exactly one AVX2 register per state variable.
- **Where it does not pay:**
  - Wavetable reads are gather-bound (each voice reads a different table position), so the oscillator stays scalar per voice.
  - Convolution already runs on JUCE's FFT, which is vDSP on macOS.
  - The black hole FDN mixes its 8 lines with a fast Walsh–Hadamard transform: 24 adds per sample, too little arithmetic to gain from hand-written SIMD.

## Phase 3: movement, mapping, physical modelling

**LFOs.** Each of the three LFOs gained:

- four shapes: Ramp Down, Wander (smooth random: a cosine glide between random targets), Pump (ducks on the beat and swells back, like a sidechain) and Stairs (4 steps). That makes 11 shapes.
- a trigger mode: Free, Retrigger (restarts the cycle and the fade-in on every note) or One-shot (runs one cycle and holds).
- start phase, fade-in (0 to 10 s, from the trigger), output smoothing (a one-pole that rounds square, S&H and stairs into glides). In Free mode the fade-in restarts only on a note from silence, so legato playing keeps the movement going and a unipolar switch.
- four destinations instead of two.

The new parameters are appended to the parameter table, so saved sessions and presets keep their meaning. The routing reads them in the same 64-sample chunk as before, so there is no new per-sample cost.

| LFO test (`VesselLab lfo`) | Expected | Measured |
|---|---|---|
| Bipolar range, 11 shapes × 3 warps | −1 .. 1 | −1.000 .. 1.000 |
| Unipolar range, 11 shapes × 3 warps | 0 .. 1 | 0.000 .. 1.000 |
| Retrigger: saw right after note-on | −1 | −1.0000 |
| One-shot: ramp-down after 6 cycles | −1, held | −1.0000 |
| Fade-in: square at half the fade time | 0.5 | 0.500 |
| Smooth 50 %: square rise 10-90 % (τ 127 ms) | ~280 ms | 278.7 ms |

**Mapping.** Routes stay ordinary parameters (a destination choice and an amount), so a mapping made in the interface is saved with the session, automatable and undoable. Two gestures make one:

- Drag an LFO tab or an expression header onto any control.
- Right-click a control and tick a source.

Either way, the route goes in the source's first free slot. A source with no free slot says so instead of overwriting one.

**Resonator (feature 26, `dsp/Resonator.h`).** An 8-voice physical model that follows the notes, placed after Unmask and before the RUIN sub send. It has five models:

- String: Karplus-Strong with a one-pole loss filter and a first-order allpass for stiffness (dispersion). Tuning compensates the phase delay of both filters at the fundamental.
- Tube: an inverting closed bore, so odd harmonics, with a soft-saturating reflection.
- Bell, Bar and Membrane: banks of 5 to 7 two-pole modes, using the measured ratios of a church bell, a free bar and a circular membrane. Stiffness stretches the ratios.

Each voice is excited by the synth bus (Input) and by a per-note noise or impulse strike, shaped by the strike position. Decay is set as T60 and converted to loop gain per note. The damper shortens the decay on release, and the sustain pedal holds it.

| Model (`VesselLab res`) | Worst tuning | Worst T60 error | Strike peak |
|---|---|---|---|
| String | −0.04 ct | 9.4 % | −4.3 dBFS |
| Tube | 0.05 ct | 0.2 % | −5.3 dBFS |
| Bell | 0.25 ct | 3.1 % | −4.6 dBFS |
| Bar | 0.29 ct | 0.0 % | −4.9 dBFS |
| Membrane | 0.28 ct | 0.2 % | −3.9 dBFS |

Stability stress: white noise at 0 dBFS into the input at 100 %, decay 20 s, stiffness 100 %, eight notes from C1 to F7 held for 8 s, then released with the damper at 100 % into 4 s of silence.

- Every model, bright and dark, stays finite.
- Peaks reach +1.8 to +5.3 dBFS inside the module. That is driven far past any preset, and the module's own soft limiter and the output stage contain it.
- Nine of the ten tails fall to the −240 dBFS floor. The dark string's tail is −107 dBFS.

Toggling the resonator every 120 ms measures −75.4 dB, against −77.1 dB for the same patch held on, so the switch adds no click of its own. The figure is the module's own ringing content above 3 kHz. On the dense CPU patch (8 voices, every module on), the resonator costs about 0.1 % of a core. The patch's total was 6.29 % before this phase and measures 6.35 to 6.73 % across runs, which is within the run-to-run noise of this measurement.

The six new presets come at the end of the bank, so the original 57 keep their program numbers. They pass the same gates as the rest (see PRESETS.md).

**True-peak ceiling (`dsp/Pressure.h`, `TruePeakCeiling`).** The 4x clipper bounds the oversampled signal. Its half-band decimator, and the base-rate clamp after it, still let the reconstructed waveform overshoot between samples: over 4 dB on clipped noise, and 1.1 dB on the brightest new pluck. The output stage now ends with a look-ahead true-peak ceiling:

- **Detector:** for each interval between two samples, it rebuilds the waveform at 16x with a 64-tap-per-phase Kaiser-windowed sinc, flat to about 22 kHz at 48 kHz. The gain that interval needs is ceiling ÷ peak, aiming 0.02 dB under the ceiling.
- **Gain:** a box average, W = 0.8 ms long, of a running minimum of those needs over W + 64 samples, with the audio delayed to match. Each output sample's gain is therefore no higher than any interval whose reconstruction reaches that sample needs. The gain moves only in straight 0.8 ms ramps and is the same for both channels.
- **Below the ceiling:** the gain is exactly 1, and the stage is a bit-exact delay.
- **Cost:** a cheap bound (the largest tap at each position times the sample magnitudes) skips the full 16-phase rebuild wherever the peak cannot reach the ceiling. The rebuild itself runs on SIMD lanes, one phase per lane.

| Signal into the clipper (ceiling −1 dBFS) | Before: 4x meter / 16x reference | After: 4x / 16x | Most reduction |
|---|---|---|---|
| fs/4 sine at 45°, +3 dB | +0.11 / +0.15 dBTP | −1.05 / −1.02 dBTP | 1.6 dB |
| 11 kHz sine, +12 dB | +1.32 / +1.32 | −1.15 / −1.02 | 3.0 dB |
| Saw C6, +12 dB | +0.78 / +0.79 | −1.08 / −1.03 | 3.2 dB |
| White-noise bursts, +12 dB | +4.34 / +4.34 | −1.02 / −1.01 | 7.1 dB |
| 55 Hz kick, +12 dB | −0.98 / −0.98 | −1.02 / −1.02 | 0.1 dB |

- **Below the ceiling:** bit-exact (largest difference from the delayed input: 0).
- **Latency:** 101 samples at 48 kHz (2.1 ms), 98 at 44.1 kHz, 140 at 96 kHz, 217 at 192 kHz. Reported to the host, and the same with the clipper on or off.
- **CPU:** 0.23 % of a core on a loud chord that hits the ceiling, and 0.49 % in the worst case (full-scale noise, every interval near the ceiling).
- **Clipper alias:** with the base-rate clamp no longer clipping, the alias figure for the clipper driven 12 dB went from −65.7 to −74.9 dB.
- **Factory bank:** all 63 presets now sit at or under −1.0 dBTP, both at audition velocity and with every expression source at maximum. Before, the worst audition peak was −0.6 dBTP and the worst stress peak −0.3. Loudness moved by at most 0.01 LU, expression depths by at most 0.1 dB, and no preset needed a new gain.

## Phase 4: cinematic bank, three engine fixes

The Cinematic category (25 presets, see the README) was built with a new `VesselCheck audit` mode. It runs the bank gates on a bank file without a rebuild and measures character: low-end and top-end share, width above 300 Hz, tail, and PLR. It also draws a spectrogram contact sheet. Reading those spectrograms turned up three engine bugs, all older than this phase:

**E1. Note-off click on zero-sustain envelopes (`dsp/Voice.h`).**
- **Cause:** the voices refresh their envelope parameters every control block. `juce::ADSR::setParameters` then recomputes the release rate from the *sustain* level, not from the level at note-off. With sustain 0 that rate is 0, and a zero rate in the release stage counts as finished, so the envelope dropped to silence on the first refresh after note-off.
- **Effect:** a click whenever a note was let go before its decay finished. With other sustain levels, a note released during its attack or decay released at the wrong speed.
- **Fix:** `Adsr` is `juce::ADSR` with one change: the release keeps the slope it started with.
- **Result:** on Abyssal Drop's note-off, the click above 2 kHz (5 ms window) went from −40.9 dB to −114.6 dB, the noise floor.
- **Affected:** 12 factory presets have zero amp sustain (plucks, keys, hits), and 5 have zero filter-envelope sustain.

**E2. Silent first note in Mono and Legato (`PluginProcessor.cpp`).**
- **Cause:** the voice mode was applied at the next render chunk, after the block's MIDI.
- **Effect:** in the first block after a preset or mode change, a note at that block's first sample started as a poly voice. The switch to mono then released it before its attack had risen, so it was silent. This hits an offline bounce that starts on a note, and the QA renders.
- **Fix:** the mode is now applied when the processor prepares (with the other snapped state) and before every note-on.
- **Affected:** 23 presets are Mono or Legato, among them most of the Sub and Bass categories, whose audition starts on a note at 0. With its first note now heard, Velvet Mud's wheel route (wah lift and tape saturation) measured 0.6 dB, under the 1 dB mark. The wheel now lifts the wah and thickens the triangle toward a saw.

**E3. Lost first strike on the resonator (`dsp/Resonator.h`).**
- **Cause:** a fresh resonator starts on the String model and fades over to the preset's model. The fade clears the strings' state, including a strike queued by a note in the same block.
- **Effect:** the first hit after loading a Bell, Bar, Membrane or Tube preset was dry.
- **Fix:** the processor's prepare and state-load snap now adopt the model at once, as they already did for the other bypass fades.
- **Affected:** 9 presets.

All 88 presets were recalibrated and re-checked after the fixes.

## Phase 5: wavetable library, FM operator, three resonator models

**Imported wavetables (`dsp/Tables.*`, `tools/make_tables.py`).** The 379 Echo Sound Works Core Tables (CC0) are stored as 16-bit FLAC (36 MB), decoded and band-limited on demand: per frame, one forward FFT, then 11 inverse FFTs keeping harmonics 1 to min(1024 >> mip, 1023), the same partial budget as the built-in table.
- **Real-time safety:** the audio thread only stores the wanted index and loads the current table pointer, both lock-free. A background thread decodes and swaps; preset loads, state restores and prepare decode synchronously, so a bounce starting on a note already has its table. A table is freed only after it has been off the audio thread's pointer for 2 s.
- **Race found while writing it:** the loader read the wanted index before taking the lock, so a preset load landing in between could be overwritten by the stale request. It now reads it under the lock.
- **Conversion bug found by the test:** `afconvert` writes an empty FLAC for 4096 samples or fewer, so the 22 two-frame tables came out as 42-byte files. Short tables are now padded with copies of their last frame (the list keeps the real frame count, and the loader reads only those), and the converter fails on any file under 1 kB.
- **Crash found by the install test:** with a table file missing (a damaged install), the reader was handed a null stream and the host crashed on any preset using that table (HostProbe: segfault). A missing or unreadable file now falls back to the built-in table. A/B on the built VST3 with `HostProbe <vst3> "Feedback Hydra"`: bundle intact 37.967062 dB render signature (repeatable), tables folder removed 38.306475 dB, one file corrupted 38.306475 dB, no crash in either. So the plug-in does read its tables from its own bundle.
- **`VesselLab tables`:** all 379 decode (3.3 ms average, 4.3 ms worst) and play finite. At C6 with the morph halfway, the inharmonic energy has a median of −89.4 dB. The worst is Vocal - Monks at −57.5 dB: it keeps unusual energy in harmonics 24 to 32 (25 to 34 kHz at C6), and a trace of it passes the decimator's stopband. It is inaudible under the harmonics.

**FM operator (`dsp/Voice.h`).** Phase modulation of the wavetable oscillator by a per-voice sine, added after the PWM warp.
- **Aliasing:** phase modulation of β radians at fm lifts harmonic h of the carrier to h × (f0 + β·fm). The first version picked the mip for f0 alone. A saw carrier at C6, 2:1, index 0.8 then measured −21.2 dB inharmonic. The mip is now chosen for the peak instantaneous frequency (feedback counts as a steeper modulator), and the same case measures −90.6 dB. Sine carriers measure −80 to −91 dB across ratios 1/4 to 14 and pitches C3 to C7.
- **Clicks:** the on/off switch jumped the index within one 64-sample chunk, a pitch chirp. It now glides over 15 ms. Toggling every 0.25 s, the largest sample step is 0.90 x that of the steady FM sound, against 1.50 x before. An index sweep 0-1-0 driven block by block peaks at 0.97 x a static index-1 render.
- **Idle cost:** with the index and envelope at rest the operator is skipped, and its ramps and envelope still advance, so it resumes on time.

**Plate, Bowl, Gong (`dsp/Resonator.h`).** Modal, 12 modes each (the maximum went from 7 to 12; Bell, Bar and Membrane are unchanged and measure as before).
- **Plate:** a simply supported rectangle, aspect 1.37, with modes weighted by |sin(mπx) sin(nπy)| at the strike point. In tune within 0.28 ct, T60 exact.
- **Bowl:** Rayleigh's thin-ring modes split into doublets (0.15 to 1.35 %, set by stiffness), weighted 0.8 / 0.6. The beat measures 0.71 Hz against 0.73 Hz expected at A3 with stiffness 0.15, and 1.77 against 1.91 Hz at 0.6. The swing is 16.4 dB. The lower half of each doublet first fell under the bell's "hum rings longer" rule (ratio below 1). That rule now applies below 0.9 only, and the swing went from 6 dB to the intended 16 dB.
- **Gong:** above a low-mode level of 0.05, the low modes drive the upper modes with noise. Each upper mode's drive is scaled by its analytic AR(2) noise gain, so the constant sets a mode amplitude whatever the decay, and the drive saturates (tanh²) so a loud sympathetic input cannot run it away. It is feed-forward only, so it cannot oscillate. At full velocity the upper band peaks 150 to 200 ms after the strike, 5.0 dB above its onset. At 0.3 velocity it stays within 0.8 dB. The pitch sits up to 1.2 % high while loud (+2.7 ct in the tuning test) and settles as the ring decays.
- **Stability:** white noise at 0 dBFS into every model at 100 % input, 20 s decay and full stiffness stays finite, and the tails are silent after release.

**UI.** The SOURCE stage has a sixth column (FM, with a Phase Bend display) and a table menu by category. The violet is brighter and leans toward magenta (OKLCH L 0.70, C 0.19, h 302; the shader's ink matches): with blue already at its maximum, the chroma could not rise at the old hue. The waveform displays drew the table at mip 3 (128 harmonics), which flattened tables with most of their energy above harmonic 64 (Formant Bot 01 has 86 % there). They now draw at near full band, four points per pixel, scaled to the frame's own peak.

## Phase 6: macros, a redesigned bank, readability

**Macros (processor, `Parameters.h`).** 104 parameters appended (348 in all): `macro1..8` and six routes each. Applied after expression and before the LFOs, in the normalised domain, so a macro can drive an LFO's rate or warp and is smoothed like every other parameter. A macro at zero costs nothing (skipped). Names (24 characters) live outside the parameter tree, guarded by a lock for the UI, and are saved in a `MACROS` child of the state; an older session has none and loads with blank names. Macros are deliberately not modulation destinations, so no cycle can form.

**Macro gates (`VesselCheck`).** Each preset is rendered with all macros at 0, then with each macro alone at 1, then with all eight at 1 under full velocity, wheel and pressure. Each macro must move the level-normalised spectrum (shape or movement) by at least 1 dB, must not move integrated loudness by more than 6 LU, and the all-at-full stress render must not reach 0 dBFS. Every preset except Init must name and route all eight.

**Flux joins the timbre distance.** The movement term was the spread of each band's level over the phrase, which cannot tell a 0.1 Hz drift from a 5 Hz throb of the same depth: a Motion macro that sped a preset's LFO up twenty-fold measured 0.2 to 0.9 dB although the change is plain to hear. The distance is now the largest of three: shape, spread, and flux (the mean frame-to-frame change of each band, 85 ms frames at a 43 ms hop, each step capped at 6 dB and levels floored 80 dB under the loudest band, so note starts and ends and the noise floor between notes cannot swamp it; an RMS version tried first was dominated by the note edges and moved nothing). The pass mark stays 1 dB, and it applies to the expression gates too, which can only rise.

**The bank (fifth pass).** Measured with a feature script on the QA renders (1/3-octave spectrum, movement, width, air, nearest neighbour in the same category), before and after:

| Measure (median) | Before | After |
|---|---|---|
| Presets with a same-category neighbour closer than 4 dB | 18 | 0 |
| Side/mid above 300 Hz (width) | −12.8 dB | −7.9 dB |
| Energy above 4 kHz: pads / drones / textures / leads | −30.9 / −42.4 / −28.5 / −16.9 dB | −23.7 / −34.6 / −17.7 / −11.3 dB |
| Presets playing an imported table | a third shared the built-in saw frame | 74 of 96 |

The low end: pads, drones, textures, keys, leads and plucks carried about 40 % of their energy below 100 Hz, from the default sub stack (sub 1 at 0.8 and sub 2 at 0.45, an octave down) and drones an octave or two down. The sub is capped per category (0.15 to 0.3) and the second sub removed there, except where the sub is the sound (Pressure Ritual, Skin Ritual and the three newest resonator presets). Anything below the category's air floor got an exciter, anything below its width floor got Haas above the mono crossover, and anything with no movement got a slow table drift. The bland and near-duplicate presets were redesigned one by one with their own tables, motion and space (the README lists the new sources); Event Horizon is renamed Black Infinity, as the old name belongs to a film.

**Macros in the bank.** Built from target values (a macro at full lands on a musical setting whatever the starting point) in eight fixed roles. Making each one audible took ten measured passes; what the gate taught:
- **Motion** speeding an LFO up is heard but was not measured (see Flux). It also needs depth: it deepens and quickens the table's own PWM, and the slow LFO it speeds up must reach a layer you hear (the sub's saturation on subs, the ladder on two drones). On struck models (taiko, thunder sheet, bells, strings) the ring sits after everything a voice LFO reaches, so Motion wakes a *parked tremolo*: an LFO retriggered at the top of a 0.01 Hz sine, which holds still at rest and trembles at 4.5 Hz at full.
- **Texture** under a big wet sound is masked if it is only noise: on dense categories it adds a crushed, decimated layer (dry at rest, the crusher at zero mix is bit-exact).
- **Tone** has to open every stage that holds the top down: both ladders (in series, B at its 5 kHz default capped what A opened), an old tape's oxide loss, a crusher's own filters, the drone and dust (which bypass the ladders), the formants of a voice, and the damping of a wet tail. On basses Tone is Growl: resonance and drive too.
- **Loudness:** the sixth route of every macro is an `out_gain` offset measured from the audit (−0.85 × the macro's loudness change, accumulated over passes). Before it, Dirt, Weight and Transform moved some presets by up to 9.9 LU; now the largest change in the bank is 3.0 LU (median per preset 1.3).
- Result: every macro on every preset changes the timbre by at least 1.0 dB (median per preset of its weakest macro 1.6 dB), and all eight at full on top of full expression peak at −1.0 dBFS.

**UI.** Captions and values under 12.5 pt draw one point larger with 20 % less tracking (same width), the secondary text colour (pewter) is brighter at `#a4adc0`, the seams are lifted to `#343b49`, and centred caps now account for the font's extra kerning. The resize grip (`drawCornerResizer`) was grey hatching; it is now a violet wedge with two hairline brackets and three diamonds growing toward the corner, brighter on hover and frost while dragging. The modulation deck opens on the MACRO page: eight macro knobs, names with a drag diamond and route ticks, and the selected macro's six routes. Double-click a name to rename it.

## Known limits

- Driven ladders at 100% still alias at −58.3 dB, and the starved tube at 80% at −60.5 dB. Fully transparent results would need 8x or ADAA on the ladder feedback, which is 2x the voice cost again. On a sub-bass synth at −58 dB under a C6 saw, this sits under the noise floor of the tape stage.
- With VHS on, the whole signal runs about 12 ms late: the wow needs a centre delay to swing around. This is not reported to the host, because the latency would change every time VHS is switched, and hosts handle changing latency badly. If tight timing matters, delay by 12 ms whether VHS is on or off and report it.
- Tube mode switches (triode / pentode / starved) change curves instantly. Switching while a note sustains can tick. A curve crossfade would fix it.
- The tube's 8 Hz DC blocker gives the processed half of the sub a phase lead over the clean half: 15° at 30 Hz, 8° at 55 Hz. When the two halves sum at equal level, that costs 0.07 dB at 30 Hz. It is inaudible, but it is not zero.
- The VST3 exposes 2,430 parameters (348 of them VESSEL's own), because JUCE's VST3 wrapper adds MIDI CC emulation parameters. Hosts handle this fine. Disable `JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS` if a host's automation list is too long.
- The string model's loop gain is capped at 0.99995. A Karplus-Strong loop also resonates at 0 Hz, and full loss compensation there would let DC build up. On high notes, where the loop is short and the cap bites hardest, the string rings up to 9.4 % shorter than the T60 asked for (A5 at a 1.5 s decay rings 1.36 s; A1 to A4 are exact). Every other model and note in the test is within 3.1 %.
- Physical-model voices are separate from synth voices: 8 of them, stolen oldest-first with a 5 ms fade. A ninth held note cuts the oldest ring short, as on a real instrument with too few strings.
- Expression routing now has a panel: the EXPR tab of the modulation deck.
- The true-peak ceiling costs 2.2 ms of look-ahead at 44.1 kHz, 2.1 ms at 48 kHz and 1.5 ms at 96 kHz. It is reported to the host and constant, but it adds to the monitoring path when playing live. Shortening it means shorter gain ramps (more audible pumping on heavy reduction) or a shorter reconstruction filter, which under-reads content above about 19 kHz. The 24-tap version tried first let clipped noise through 0.9 dB over the ceiling.
- The ceiling is held to within 0.02 dB on a 16x reconstruction. An analogue reconstruction filter with a different response, or content right at Nyquist, can read a few hundredths of a dB differently. Before the stage existed, two of the new plucks measured +0.1 and −0.0 dBTP under stress, and their treble routing was cut back (Iron Strings' wheel moves the pluck point instead of tilting it bright; Bone Marimba's tilt route is halved). Those routing changes stay.
