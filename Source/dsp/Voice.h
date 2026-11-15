#pragma once

#include "Common.h"
#include "Wavetable.h"

namespace vessel::dsp
{
//==============================================================================
/** Linear ADSR, identical to juce::ADSR except for one thing: the release keeps the slope it started with (level at
    note-off / release time) when the parameters are refreshed mid-release. juce::ADSR recomputes it from the sustain
    level, so with sustain = 0 the first refresh after note-off ends the release at once, a click on every note let go
    before its decay has finished, and a release begun mid-attack or mid-decay runs at the wrong speed. The voices
    refresh their parameters every control block, so this matters for every zero-sustain patch.                    */
class Adsr
{
public:
    using Parameters = juce::ADSR::Parameters;

    // as juce::ADSR: rates from the default parameters, so a voice's very first noteOn (before its first parameter
    // refresh) still enters the attack stage instead of jumping to sustain
    Adsr() noexcept { recalculateRates(); }

    void setSampleRate (double sr) noexcept               { sampleRate = sr; }
    void setParameters (const Parameters& p) noexcept     { parameters = p; recalculateRates(); }
    bool isActive() const noexcept                        { return state != State::idle; }
    void reset() noexcept                                 { level = 0.0f; state = State::idle; }

    void noteOn() noexcept
    {
        if (attackRate > 0.0f)     state = State::attack;
        else if (decayRate > 0.0f) { level = 1.0f; state = State::decay; }
        else                       { level = parameters.sustain; state = State::sustain; }
    }

    void noteOff() noexcept
    {
        if (state == State::idle) return;
        if (parameters.release <= 0.0f) { reset(); return; }
        releaseFrom = level;
        releaseRate = rate (releaseFrom, parameters.release);
        state = State::release;
    }

    float getNextSample() noexcept
    {
        switch (state)
        {
            case State::idle:    return 0.0f;
            case State::attack:  level += attackRate; if (level >= 1.0f) { level = 1.0f; next(); } break;
            case State::decay:   level -= decayRate;  if (level <= parameters.sustain) { level = parameters.sustain; next(); } break;
            case State::sustain: level = parameters.sustain; break;
            case State::release: level -= releaseRate; if (level <= 0.0f) next(); break;
        }
        return level;
    }

private:
    enum class State { idle, attack, decay, sustain, release };

    float rate (float distance, float seconds) const noexcept
    {
        return seconds > 0.0f ? (float) (distance / (seconds * sampleRate)) : -1.0f;
    }

    void recalculateRates() noexcept
    {
        attackRate  = rate (1.0f, parameters.attack);
        decayRate   = rate (1.0f - parameters.sustain, parameters.decay);
        releaseRate = rate (state == State::release ? releaseFrom : parameters.sustain, parameters.release);
        if ((state == State::attack && attackRate <= 0.0f)
            || (state == State::decay && (decayRate <= 0.0f || level <= parameters.sustain))
            || (state == State::release && parameters.release <= 0.0f))
            next();
    }

    void next() noexcept
    {
        if (state == State::attack)       state = decayRate > 0.0f ? State::decay : State::sustain;
        else if (state == State::decay)   state = State::sustain;
        else if (state == State::release) reset();
    }

    State state = State::idle;
    Parameters parameters;
    double sampleRate = 44100.0;
    float level = 0.0f, releaseFrom = 0.0f, attackRate = 0.0f, decayRate = 0.0f, releaseRate = 0.0f;
};

//==============================================================================
/** Lane-parallel bank of 24 dB/oct zero-delay-feedback ladders (Zavalishin topology, tanh inside the loop).
    One lane per voice, structure-of-arrays: every operation in tick() is the same across lanes with no
    branches, so the compiler emits NEON (arm64) or SSE/AVX (x86) for the whole bank. Coefficients glide
    linearly per sample between control updates, so cutoff sweeps are continuous.
    Resonance 0..1 maps to k = 0..4.3: self-oscillation from ~0.93, bounded by the tanh.                     */
template <int L>
struct LadderBank
{
    alignas (32) float s0[L] {}, s1[L] {}, s2[L] {}, s3[L] {};
    alignas (32) float G[L] {}, dG[L] {}, k[L] {}, dk[L] {}, drv[L] {}, ddrv[L] {}, isd[L] {}, disd[L] {};

    static float coefG (float hz, float osr) noexcept
    {
        hz = std::clamp (hz, 5.0f, 0.45f * osr);
        const float g = fastTan (kPi * hz / osr);
        return g / (1.0f + g);
    }

    void resetLane (int l) noexcept { s0[l] = s1[l] = s2[l] = s3[l] = 0.0f; }

    void setLane (int l, float Gt, float kt, float driveT, int m, bool snap) noexcept
    {
        const float isdT = 1.0f / std::sqrt (driveT);
        if (snap) { G[l] = Gt; k[l] = kt; drv[l] = driveT; isd[l] = isdT; dG[l] = dk[l] = ddrv[l] = disd[l] = 0.0f; return; }
        const float inv = 1.0f / (float) m;
        dG[l] = (Gt - G[l]) * inv; dk[l] = (kt - k[l]) * inv;
        ddrv[l] = (driveT - drv[l]) * inv; disd[l] = (isdT - isd[l]) * inv;
    }
    void holdLane (int l) noexcept { dG[l] = dk[l] = ddrv[l] = disd[l] = 0.0f; }

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
};

struct VoiceParams
{
    // sub (1)
    bool subOn = true;
    int sub1Oct = 1, sub2Oct = 2;          // octaves below the played note
    float sub1Level = 0.8f, sub2Level = 0.45f, sub1Shape = 0, sub2Shape = 0.25f, subWeight = 0.3f;
    // wavetable (5)
    bool wtOn = true;
    float wtLevel = 0.55f, morph = 0.35f, pw = 0.5f, pwm = 0.3f, pwmRate = 0.35f, wtTune = 0.0f, drift = 0.25f;
    // ladders (11)
    bool fltOn = true;
    int route = 0;
    float cutA = 1800, resA = 0.35f, cutB = 5000, resB = 0.2f, fDrive = 0.25f, envAmt = 0.35f, keyTrack = 0.3f;
    juce::ADSR::Parameters amp, filt;
    float glideMs = 0.0f, bend = 0.0f, velAmp = 0.65f;
    // FM operator (27)
    bool fmOn = false;
    float fmRatio = 1.0f, fmFine = 0.0f, fmIndex = 0.3f, fmFb = 0.0f, fmEnv = 0.0f, fmDecay = 0.4f, fmKey = 0.0f;
};

//==============================================================================
/** One voice of the oversampled engine: sub stack, wavetable oscillator and envelopes.
    Its ladder pair lives in the manager's lane banks so all voices filter in parallel. */
class Voice
{
public:
    void prepare (float baseSampleRate, float osFactor, uint32_t seed)
    {
        sr = baseSampleRate;
        osr = osFactor * baseSampleRate;
        ampEnv.setSampleRate (osr);
        filtEnv.setSampleRate (osr);
        drift.prepare (osr, 0.27f, seed);
        rng.seed (seed * 7919u + 13u);
        pwmPhase = rng.uni();
        reset();
    }

    void reset()
    {
        ampEnv.reset(); filtEnv.reset();
        active = false; gateHeld = false; note = -1; lastEnv = 0.0f; fresh = true;
        subPhase = 0.0; wt.phase = 0.0f; fmPhase = fmY1 = fmY2 = fmEnvLevel = fmGate = 0.0f;
        for (auto* r : { &sub1L, &sub2L, &sub1S, &sub2S, &weight, &wtL, &morph, &pwBase, &pwmDepth,
                         &cutA, &cutB, &resA, &resB, &fDrive, &envAmt, &keyTrack, &velGain, &bend,
                         &wtTuneR, &spreadPan, &fmIdx, &fmEnvAmt, &fmFbR })
            r->reset();
    }

    void start (int midiNote, float vel, bool retrigger, bool snapPitch)
    {
        const bool wasSilent = ! active || lastEnv < 1.0e-3f;
        note = midiNote;
        velocity = vel;
        targetNote = (float) midiNote;
        if (snapPitch || wasSilent) currentNote = targetNote;

        if (wasSilent)
        {
            subPhase = 0.0;        // phase-aligned: sub stack and wavetable start together at a zero crossing
            wt.phase = 0.0f;
            pan = rng.uni() - 0.5f;
            fresh = true;          // controls snap to this note's values instead of ramping from the last note
            ladderSnap = true;
        }
        if (retrigger || wasSilent) { ampEnv.noteOn(); filtEnv.noteOn(); fmEnvLevel = 1.0f; }
        if (wasSilent) { fmPhase = 0.0f; fmY1 = fmY2 = 0.0f; }
        active = true;
        age = 0;
    }

    void glideTo (int midiNote) { note = midiNote; targetNote = (float) midiNote; }
    /** The table the wavetable oscillator reads (the subs always use the built-in sine). Audio thread, per block. */
    void setTable (const WavetableBank* b) noexcept { wt.bank = b; }
    void release() { ampEnv.noteOff(); filtEnv.noteOff(); }
    bool isActive() const noexcept { return active; }
    int  getNote() const noexcept { return note; }
    float getVelocity() const noexcept { return velocity; }

    bool gateHeld = false;
    uint32_t age = 0;

    /** Chunk-rate control update; n2 = oversampled samples in the chunk. */
    void control (const VoiceParams& p, int n2)
    {
        ampEnv.setParameters (p.amp);
        filtEnv.setParameters (p.filt);

        auto set = [this, n2] (Ramp& r, float v) { if (fresh) r.snap (v); else r.to (v, n2); };
        set (sub1L, p.subOn ? p.sub1Level : 0.0f);
        set (sub2L, p.subOn ? p.sub2Level : 0.0f);
        set (sub1S, p.sub1Shape); set (sub2S, p.sub2Shape); set (weight, p.subWeight);
        set (wtL, p.wtOn ? p.wtLevel : 0.0f);
        set (morph, p.morph); set (pwBase, p.pw); set (pwmDepth, p.pwm);
        set (cutA, std::log2 (std::max (10.0f, p.cutA))); set (cutB, std::log2 (std::max (10.0f, p.cutB)));
        set (resA, p.resA); set (resB, p.resB); set (fDrive, p.fDrive);
        set (envAmt, p.envAmt); set (keyTrack, p.keyTrack);
        set (velGain, 1.0f - p.velAmp + p.velAmp * velocity);
        set (bend, p.bend);
        set (wtTuneR, p.wtTune);
        set (spreadPan, p.drift);
        // the FM switch glides (15 ms): an index jumping within one chunk is a pitch chirp, heard as a click
        const float gateTarget = p.fmOn ? 1.0f : 0.0f;
        if (fresh) fmGate = gateTarget;
        else fmGate += (gateTarget - fmGate) * (1.0f - std::exp (-(float) n2 / (0.015f * osr)));
        if (! p.fmOn && fmGate < 1.0e-3f) fmGate = 0.0f;
        set (fmIdx, p.fmIndex * fmGate);
        set (fmEnvAmt, p.fmEnv * fmGate);
        set (fmFbR, p.fmFb);
        fmOn = fmGate > 0.0f;
        fmRatio = p.fmRatio * semis (p.fmFine * 0.01f);
        fmEnvMul = std::exp (-1.0f / (std::max (0.005f, p.fmDecay) * osr));
        fmKey = p.fmKey;

        glideCoef = p.glideMs <= 0.5f ? 0.0f : std::exp (-1.0f / (0.001f * p.glideMs * osr));
        pwmRate = p.pwmRate;
        driftAmt = p.drift;
        wtTune = p.wtTune;
        s1Mul = 1 << (3 - p.sub1Oct);   // relative to the master phase at note/8
        s2Mul = 1 << (3 - p.sub2Oct);
        fresh = false;
    }

    /** Renders m oversampled samples: oscillator → oscLM[i*L + lane], amp gain → gainLM, sub summed into sub.
        Returns the filter-envelope value at the end of the block. */
    template <int L>
    float renderBlock (int m, int lane, float* oscLM, float* gainLM, float* sub)
    {
        // ── control rate (once per block): pitch, drift, mip, PWM LFO
        glideM = (glideCoef > 0.0f) ? std::pow (glideCoef, (float) m) : 0.0f;
        currentNote = targetNote + glideM * (currentNote - targetNote);
        bend.skip (m);
        wtTuneR.skip (m);
        noteBent = currentNote + bend.cur;
        const float hz = midiToHz (noteBent);
        const float d = drift.next (m) * driftAmt;
        const float oscHz = hz * semis (wtTuneR.cur + d * 0.18f);
        const float dt = std::clamp (oscHz / osr, 1.0e-7f, 0.48f);
        const float dSub = std::clamp (hz / (8.0f * osr), 0.0f, 0.48f);

        const float pwmStart = pwmLfo;
        pwmPhase += pwmRate * (float) m / osr;
        pwmPhase -= std::floor (pwmPhase);
        if (! (pwmPhase >= 0.0f && pwmPhase < 1.0f)) pwmPhase = 0.0f;
        pwmLfo = std::sin (kTwoPi * pwmPhase);
        const float pwmInc = (pwmLfo - pwmStart) / (float) m;

        // FM: modulator at ratio x the oscillator's pitch; the index (squared, so small values are fine-grained)
        // in radians, halved per octave above C3 at full key tracking, and live only while it has something to do
        const bool fmLive = fmOn && (fmIdx.cur > 0.0f || fmIdx.inc != 0.0f || (fmEnvAmt.cur > 0.0f && fmEnvLevel > 1.0e-4f));
        const float fmDt = std::min (0.45f, dt * fmRatio);
        const float fmKeyGain = std::clamp (std::exp2 (-fmKey * (noteBent - 48.0f) / 12.0f), 0.25f, 2.0f);
        constexpr float kMaxIndex = 12.0f / kTwoPi;   // cycles of phase deviation at index 1
        if (! fmLive)
        {   // keep the ramps and the index envelope on time while the operator is idle
            fmIdx.skip (m); fmEnvAmt.skip (m); fmFbR.skip (m);
            if (fmOn) fmEnvLevel *= std::pow (fmEnvMul, (float) m);
        }

        // Phase modulation of beta radians at fm raises harmonic h's instantaneous frequency to h * (f0 + beta * fm):
        // the mip is chosen for that peak, so a bright carrier darkens as the index rises instead of folding over.
        // (Feedback steepens the modulator, which raises its slope the same way.)
        float fmSpread = 1.0f;
        if (fmLive)
        {
            const float xEnd = fmIdx.cur + fmIdx.inc * (float) m;
            const float xPeak = std::min (1.25f, std::max (fmIdx.cur, xEnd) + std::max (fmEnvAmt.cur, fmEnvAmt.cur + fmEnvAmt.inc * (float) m) * fmEnvLevel);
            const float fbPeak = std::max (fmFbR.cur, fmFbR.cur + fmFbR.inc * (float) m);
            fmSpread = 1.0f + kTwoPi * kMaxIndex * xPeak * xPeak * fmKeyGain * fmRatio * (1.0f + 1.5f * fbPeak);
        }

        // squeeze-aware mip pair, from the narrowest duty this block can reach
        const float pwMid = std::clamp (pwBase.cur + pwmDepth.cur * 0.44f * pwmLfo + d * 0.03f, 0.03f, 0.97f);
        int mip; float wLower;
        WavetableOsc::mipBlend (oscHz * fmSpread * std::max (0.5f / pwMid, 0.5f / (1.0f - pwMid)), sr, mip, wLower);

        const bool subLive = sub1L.cur > 0.0f || sub2L.cur > 0.0f || sub1L.inc != 0.0f || sub2L.inc != 0.0f;
        const bool wtLive  = wtL.cur > 0.0f || wtL.inc != 0.0f;
        const auto& bank = *core;   // the subs' sine
        float pwmV = pwmStart, fenv = 0.0f;


        for (int i = 0; i < m; ++i)
        {
            const float env = ampEnv.getNextSample();
            fenv = filtEnv.getNextSample();
            lastEnv = env;
            const float vg = velGain.next();

            // sub-harmonic stack: one master phase, everything derived from it (phase-coherent)
            const float a = sub1L.next(), b = sub2L.next(), sh1 = sub1S.next(), sh2 = sub2S.next(), w = weight.next();
            if (subLive)
            {
                const double ph1 = subPhase * (double) s1Mul, ph2 = subPhase * (double) s2Mul;
                const float s = subShape (bank, (float) (ph1 - std::floor (ph1)), sh1) * a
                              + subShape (bank, (float) (ph2 - std::floor (ph2)), sh2) * b;
                const float wN = 1.0f + 3.0f * w;
                sub[i] += fastTanh (s * wN) / fastTanh (wN) * 0.36f * env * vg;
            }
            subPhase += (double) dSub;
            if (subPhase >= 1.0) subPhase -= 1.0;

            // wavetable
            const float lvl = wtL.next(), mo = morph.next();
            pwmV += pwmInc;
            const float pw = pwBase.next() + pwmDepth.next() * 0.44f * pwmV + d * 0.03f;
            float pm = 0.0f;
            if (fmLive)
            {
                const float x = std::min (1.25f, fmIdx.next() + fmEnvAmt.next() * fmEnvLevel);
                fmEnvLevel *= fmEnvMul;
                // DX-style feedback: the operator's last two outputs averaged into its own phase (no hunting)
                float ph = fmPhase + 0.22f * fmFbR.next() * 0.5f * (fmY1 + fmY2);
                ph -= std::floor (ph);
                const float y = bank.read (0, 0, ph) * (1.0f / 0.9f);
                fmY2 = fmY1; fmY1 = y;
                fmPhase += fmDt;
                fmPhase -= std::floor (fmPhase);
                if (! (fmPhase >= 0.0f && fmPhase < 1.0f)) fmPhase = 0.0f;
                pm = kMaxIndex * x * x * fmKeyGain * y;
            }
            oscLM[i * L + lane] = wtLive ? wt.process (dt, mip, wLower, mo, pw, pm) * lvl : 0.0f;
            gainLM[i * L + lane] = env * vg * 0.5f;
        }
        return fenv;
    }

    /** Ladder targets for the end of the current block. */
    void filterTargets (float fenv, int m, float& cA, float& rA, float& cB, float& rB, float& drive)
    {
        for (auto* r : { &cutA, &cutB, &resA, &resB, &fDrive, &envAmt, &keyTrack }) r->skip (m);
        const float octs = envAmt.cur * 7.0f * fenv + keyTrack.cur * (noteBent - 60.0f) / 12.0f;
        const float hi = 0.45f * sr;
        cA = std::clamp (std::exp2 (cutA.cur + octs), 10.0f, hi);
        cB = std::clamp (std::exp2 (cutB.cur + octs), 10.0f, hi);
        rA = clamp01 (resA.cur); rB = clamp01 (resB.cur);
        const float dr = clamp01 (fDrive.cur);
        drive = 1.0f + 6.0f * dr * dr;
    }

    void pans (float& l, float& r) const noexcept
    {
        const float p = pan * spreadPan.cur;
        l = std::sqrt (0.5f - 0.5f * p);
        r = std::sqrt (0.5f + 0.5f * p);
    }

    void endChunk() noexcept
    {
        ++age;
        if (! ampEnv.isActive()) { active = false; note = -1; lastEnv = 0.0f; }
    }

    bool ladderSnap = false;

private:
    static float subShape (const WavetableBank& bank, float ph, float shape) noexcept
    {
        const float s = bank.read (0, 0, ph) * (1.0f / 0.9f);   // sine frame (normalised to 0.9 peak)
        const float tri = ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
        if (shape <= 0.5f) return lerp (s, tri, shape * 2.0f);
        return lerp (tri, fastTanh (4.0f * s), shape * 2.0f - 1.0f);
    }

    float sr = 48000.0f, osr = 96000.0f;
    Adsr ampEnv, filtEnv;
    WavetableOsc wt;
    Drift drift;
    Rng rng;
    double subPhase = 0.0;
    float pwmPhase = 0.0f, pwmLfo = 0.0f, pan = 0.0f;
    float currentNote = 60.0f, targetNote = 60.0f, noteBent = 60.0f, velocity = 1.0f, lastEnv = 0.0f;
    float glideCoef = 0.0f, glideM = 0.0f, pwmRate = 0.35f, driftAmt = 0.25f, wtTune = 0.0f;
    int s1Mul = 4, s2Mul = 2;
    Ramp sub1L, sub2L, sub1S, sub2S, weight, wtL, morph, pwBase, pwmDepth;
    Ramp cutA, cutB, resA, resB, fDrive, envAmt, keyTrack, velGain, bend, wtTuneR, spreadPan;
    Ramp fmIdx, fmEnvAmt, fmFbR;
    const WavetableBank* core = &WavetableBank::get();
    bool fmOn = false;
    float fmGate = 0.0f;
    float fmPhase = 0.0f, fmY1 = 0.0f, fmY2 = 0.0f, fmEnvLevel = 0.0f, fmEnvMul = 0.999f, fmRatio = 1.0f, fmKey = 0.0f;
    int note = -1;
    bool active = false, fresh = true;
};

//==============================================================================
/** Poly / mono / legato allocation with sustain pedal and a held-note stack.
    Renders all voices into oversampled buses; the processor decimates them once (body L/R + sub together, so their
    phase relationship survives the filter). */
class VoiceManager
{
public:
    static constexpr int kVoices = 8;
    static constexpr int kOsLog2 = 2;               // voices run at 4x the host rate
    static constexpr int kOs = 1 << kOsLog2;
    static constexpr int kSub = 8 * kOs;            // oversampled samples per control block (8 host samples)

    void prepare (float baseSr)
    {
        osr = (float) kOs * baseSr;
        for (int i = 0; i < kVoices; ++i) voices[(size_t) i].prepare (baseSr, (float) kOs, (uint32_t) (i * 7717 + 101));
        fltFade.prepare (osr, 10.0f);
        fltFade.snap (true);
        allOff();
    }

    void allOff()
    {
        for (int v = 0; v < kVoices; ++v) { voices[(size_t) v].reset(); ladA.resetLane (v); ladB.resetLane (v); }
        stackSize = 0; sustain = false;
        std::fill (std::begin (sustained), std::end (sustained), false);
    }

    void setTable (const WavetableBank* b) noexcept { for (auto& v : voices) v.setTable (b); }

    void setMode (int m) noexcept
    {
        if (m != mode) { for (auto& v : voices) v.release(); mode = m; }
    }

    void noteOn (int note, float vel)
    {
        if (note < 0 || note >= 128) return;
        pushNote (note);
        sustained[note] = false;
        noteVel[note] = vel;

        if (mode == 0)
        {
            Voice* target = nullptr;
            for (auto& v : voices) if (v.isActive() && v.getNote() == note) { target = &v; break; }
            if (! target) for (auto& v : voices) if (! v.isActive()) { target = &v; break; }
            if (! target)
            {   // steal: prefer released voices, then the oldest
                uint32_t best = 0;
                for (auto& v : voices)
                {
                    const uint32_t score = v.age + (v.gateHeld ? 0u : 1000000u);
                    if (! target || score > best) { target = &v; best = score; }
                }
            }
            target->start (note, vel, true, false);
            target->gateHeld = true;
            for (auto& v : voices) if (&v != target) ++v.age;
        }
        else
        {
            auto& v = voices[0];
            const bool legato = mode == 2 && stackSize > 1 && v.isActive();
            v.start (note, vel, ! legato, false);
            v.gateHeld = true;
        }
    }

    void noteOff (int note)
    {
        if (note < 0 || note >= 128) return;
        if (sustain) { sustained[note] = true; return; }
        releaseNote (note);
    }

    void setSustain (bool on)
    {
        sustain = on;
        if (! on)
            for (int n = 0; n < 128; ++n)
                if (sustained[n]) { sustained[n] = false; releaseNote (n); }
    }

    /** n2 oversampled samples. vp holds each voice's own parameter set (velocity routing is per voice). */
    void render (float* bodyL, float* bodyR, float* sub, int n2, const std::array<VoiceParams, kVoices>& vp, bool fltOn, int route)
    {
        bool any = false;
        for (int v = 0; v < kVoices; ++v)
        {
            auto& voice = voices[(size_t) v];
            if (voice.isActive()) { voice.control (vp[(size_t) v], n2); any = true; }
            else { ladA.holdLane (v); ladB.holdLane (v); }
        }
        if (! any) return;

        alignas (32) float osc[kSub * kVoices], gain[kSub * kVoices], a[kVoices], b[kVoices], y[kVoices];
        float panL[kVoices], panR[kVoices];

        for (int start = 0; start < n2; start += kSub)
        {
            const int m = std::min (kSub, n2 - start);
            std::fill_n (osc, m * kVoices, 0.0f);
            std::fill_n (gain, m * kVoices, 0.0f);

            for (int v = 0; v < kVoices; ++v)
            {
                auto& voice = voices[(size_t) v];
                if (! voice.isActive()) { panL[v] = panR[v] = 0.0f; continue; }
                const float fenv = voice.renderBlock<kVoices> (m, v, osc, gain, sub + start);
                float cA, rA, cB, rB, drive;
                voice.filterTargets (fenv, m, cA, rA, cB, rB, drive);
                const bool snap = voice.ladderSnap;
                if (snap) { ladA.resetLane (v); ladB.resetLane (v); voice.ladderSnap = false; }
                ladA.setLane (v, LadderBank<kVoices>::coefG (cA, osr), 4.3f * rA, drive, m, snap);
                ladB.setLane (v, LadderBank<kVoices>::coefG (cB, osr), 4.3f * rB, drive, m, snap);
                voice.pans (panL[v], panR[v]);
            }

            const bool runFilters = fltOn || ! fltFade.settled (fltOn);
            for (int i = 0; i < m; ++i)
            {
                const float* x = osc + i * kVoices;
                if (runFilters)
                {
                    switch (route)
                    {
                        case 0:  ladA.tick (x, a); ladB.tick (a, y); break;
                        case 1:  ladA.tick (x, a); ladB.tick (x, b); for (int l = 0; l < kVoices; ++l) y[l] = 0.5f * (a[l] + b[l]); break;
                        default: ladA.tick (x, y); break;
                    }
                    const float g = fltFade.next (fltOn);
                    for (int l = 0; l < kVoices; ++l) y[l] = x[l] + g * (y[l] - x[l]);
                }
                else std::copy_n (x, kVoices, y);

                const float* gl = gain + i * kVoices;
                float sl = 0.0f, sr2 = 0.0f;
                for (int l = 0; l < kVoices; ++l) { const float s = y[l] * gl[l]; sl += s * panL[l]; sr2 += s * panR[l]; }
                bodyL[start + i] += sl;
                bodyR[start + i] += sr2;
            }
        }
        for (auto& v : voices) if (v.isActive()) v.endChunk();
    }

    bool anyHeld() const noexcept { return stackSize > 0; }
    int lowestHeld() const noexcept
    {
        int lo = 128;
        for (int i = 0; i < stackSize; ++i) lo = std::min (lo, stack[i]);
        return lo == 128 ? -1 : lo;
    }
    int lastHeld() const noexcept { return stackSize > 0 ? stack[stackSize - 1] : -1; }
    bool anyActive() const noexcept
    {
        for (auto& v : voices) if (v.isActive()) return true;
        return false;
    }
    bool voiceActive (int v) const noexcept { return voices[(size_t) v].isActive(); }
    float voiceVelocity (int v) const noexcept { return voices[(size_t) v].getVelocity(); }

private:
    void pushNote (int note)
    {
        removeFromStack (note);
        if (stackSize < 128) stack[stackSize++] = note;
    }
    void removeFromStack (int note)
    {
        int w = 0;
        for (int i = 0; i < stackSize; ++i) if (stack[i] != note) stack[w++] = stack[i];
        stackSize = w;
    }
    void releaseNote (int note)
    {
        removeFromStack (note);
        if (mode == 0)
        {
            for (auto& v : voices)
                if (v.isActive() && v.getNote() == note && v.gateHeld) { v.release(); v.gateHeld = false; }
        }
        else
        {
            auto& v = voices[0];
            if (stackSize > 0)
            {
                const int back = stack[stackSize - 1];
                if (v.getNote() != back)
                {
                    if (mode == 2) v.glideTo (back);
                    else           v.start (back, noteVel[back], true, false);   // keeps the held note's own velocity
                }
            }
            else if (v.gateHeld) { v.release(); v.gateHeld = false; }
        }
    }

    std::array<Voice, kVoices> voices;
    LadderBank<kVoices> ladA, ladB;
    BypassFade fltFade;
    float osr = 96000.0f;
    int stack[128] {};
    float noteVel[128] {};
    int stackSize = 0;
    int mode = 0;
    bool sustain = false;
    bool sustained[128] {};
};
} // namespace vessel::dsp
