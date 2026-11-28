#pragma once

#include "Common.h"

namespace vessel::dsp
{
//==============================================================================
/** Analog Organic Bus:
    1. Subtle tape/chassis micro-instability (1/f pink drift modulating a Hermite delay line above 120 Hz,
       leaving sub-120 Hz phase-locked and mono-anchored).
    2. Voice-gated analog thermal floor (-72..-48 dBFS band-limited pink thermal bed that smoothly fades
       to exact 0.0 when no voices are active, preserving < -120 dBFS idle silence).
    3. Optional independent sub-120 Hz LR4 M/S mono-anchoring discipline.
    Driven by a deterministic seeded PRNG (resettable in QA mode). Fully bypassed with 0 latency and 0 CPU
    when amount == 0, thermalFloor == 0, and monoAnchor == false. */
class OrganicBus
{
public:
    struct Params
    {
        float amount = 0.0f;         // 0..1 chassis/tape micro-instability & organic warmth
        float thermalFloor = 0.0f;   // 0..1 voice-gated analog thermal noise floor
        bool  monoAnchor = false;    // dedicated sub-120 Hz LR4 M/S mono discipline
    };

    void prepare (float sampleRate, int maxBlock, uint32_t seedVal = 0xA4A106u)
    {
        sr = std::max (1.0f, sampleRate);
        dl[0].prepare ((int) (0.01f * sr) + 32);
        dl[1].prepare ((int) (0.01f * sr) + 32);
        juce::dsp::ProcessSpec spec { (double) sr, (juce::uint32) std::max (1, maxBlock), 1 };
        subSide.prepare (spec); subSide.reset(); subSide.setCutoffFrequency (120.0f);
        subMid.prepare (spec);  subMid.reset();  subMid.setCutoffFrequency (120.0f);
        for (int c = 0; c < 2; ++c)
        {
            thermHp[c].setup (80.0, 0.7071, (double) sr);
            thermLp[c].setup (std::min (14000.0, 0.42 * (double) sr), 0.7071, (double) sr);
            thermHp[c].reset();
            thermLp[c].reset();
            pinkState[c][0] = pinkState[c][1] = pinkState[c][2] = 0.0f;
        }
        gateAtt = std::exp (-1.0f / (0.018f * sr));
        gateRel = std::exp (-1.0f / (0.110f * sr));
        amtR.reset(); floorR.reset();
        reseed (seedVal);
    }

    void reseed (uint32_t seedVal) noexcept
    {
        rng.seed (seedVal ? seedVal : 0xA4A106u);
        wowDrift.prepare (sr, seedVal ^ 0x51A7u);
        azDrift.prepare (sr, seedVal ^ 0x9E3779B9u);
        dl[0].clear(); dl[1].clear();
        subSide.reset(); subMid.reset();
        for (int c = 0; c < 2; ++c)
        {
            thermHp[c].reset(); thermLp[c].reset();
            pinkState[c][0] = pinkState[c][1] = pinkState[c][2] = 0.0f;
        }
        voiceGate = 0.0f;
        wowCur = azCur = 0.0f;
        amtR.reset(); floorR.reset();
        wasActive = false;
    }

    /** Process n base-rate samples. Control updates execute strictly on fixed isCtrlTick boundaries. */
    void process (float* l, float* r, int n, bool anyVoiceActive, const Params& p,
                  bool isCtrlTick = true, int ctrlSpan = 0) noexcept
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        const bool wantInstab = p.amount > 1.0e-5f;
        const bool wantFloor  = p.thermalFloor > 1.0e-5f;
        const bool wantMono   = p.monoAnchor;

        if (! wantInstab && ! wantFloor && ! wantMono && ! wasActive)
            return;

        if (isCtrlTick)
        {
            amtR.to (wantInstab ? p.amount : 0.0f, ctrlSpan);
            floorR.to (wantFloor ? p.thermalFloor : 0.0f, ctrlSpan);
            wowCur = wowDrift.next (ctrlSpan);
            azCur  = azDrift.next (ctrlSpan);
        }

        const bool runInstab = wantInstab || amtR.cur > 1.0e-5f;
        const bool runFloor  = wantFloor  || floorR.cur > 1.0e-5f || voiceGate > 1.0e-6f;
        if (! runInstab && ! runFloor && ! wantMono)
        {
            if (wasActive)
            {
                dl[0].clear(); dl[1].clear();
                subSide.reset(); subMid.reset();
                voiceGate = 0.0f;
                wasActive = false;
            }
            return;
        }
        wasActive = true;

        const float baseDelay = 1.5f + 0.00022f * sr;   // ~0.22 ms nominal micro-head offset (0 latency when off)
        const float modSpan   = 0.00018f * sr;          // ±0.18 ms subtle chassis/tape drift
        const float azSpan    = 0.000035f * sr;         // ±0.035 ms subtle head azimuth skew
        const float gateTarget = anyVoiceActive ? 1.0f : 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float amt = amtR.next();
            const float flr = floorR.next();

            // 1. Voice-gated analog thermal floor (-72..-48 dBFS, decays cleanly to exact 0.0 when idle)
            const float gCoef = (gateTarget > voiceGate) ? gateAtt : gateRel;
            voiceGate = flushDenormal (gateTarget + gCoef * (voiceGate - gateTarget));
            if (voiceGate < 1.0e-6f) voiceGate = 0.0f;

            if (flr > 1.0e-5f && voiceGate > 0.0f)
            {
                const float gain = voiceGate * dbToGain (-72.0f + 24.0f * flr);
                l[i] += thermLp[0].lowpass (thermHp[0].highpass (pink (0))) * gain;
                r[i] += thermLp[1].lowpass (thermHp[1].highpass (pink (1))) * gain;
            }

            // 2. Subtle chassis/tape micro-instability (blended smoothly on the audio path)
            if (amt > 1.0e-5f)
            {
                dl[0].push (l[i]);
                dl[1].push (r[i]);
                const float dL = std::max (1.0f, baseDelay + amt * (wowCur * modSpan - azCur * azSpan));
                const float dR = std::max (1.0f, baseDelay + amt * (wowCur * modSpan + azCur * azSpan));
                const float wl = dl[0].readHermite (dL);
                const float wr = dl[1].readHermite (dR);
                const float blend = std::min (1.0f, amt * 1.5f);
                l[i] += blend * (wl - l[i]);
                r[i] += blend * (wr - r[i]);
            }

            // 3. Dedicated sub-120 Hz LR4 M/S mono-anchored bass discipline
            if (wantMono)
            {
                const float m = 0.5f * (l[i] + r[i]);
                const float s = 0.5f * (l[i] - r[i]);
                float mlo, mhi, slo, shi;
                subMid.processSample (0, m, mlo, mhi);
                subSide.processSample (0, s, slo, shi);
                const float mAllpass = mlo + mhi;
                l[i] = mAllpass + shi;
                r[i] = mAllpass - shi;
            }
        }
    }

private:
    float pink (int c) noexcept
    {
        auto& b = pinkState[c];
        const float w = rng.bip();
        b[0] = 0.99765f * b[0] + w * 0.0990460f;
        b[1] = 0.96300f * b[1] + w * 0.2965164f;
        b[2] = 0.57000f * b[2] + w * 1.0526913f;
        return (b[0] + b[1] + b[2] + w * 0.1848f) * 0.25f;
    }

    float sr = 48000.0f;
    DelayLine dl[2];
    juce::dsp::LinkwitzRileyFilter<float> subSide, subMid;
    Svf thermHp[2], thermLp[2];
    float pinkState[2][3] {};
    PinkDrift wowDrift, azDrift;
    Rng rng { 0xA4A106u };
    Ramp amtR, floorR;
    float voiceGate = 0.0f, gateAtt = 0.99f, gateRel = 0.999f;
    float wowCur = 0.0f, azCur = 0.0f;
    bool wasActive = false;
};

//==============================================================================
/** Additive procedural texture & sub-harmonic layer.
    Three dark-IDM character modes summed cleanly at the voice/bus boundary:
      0: Tape Air      — mechanical capstan flutter + filtered stereo pink air bed (voice-gated).
      1: Sub Weight    — phase-coherent mono-anchored sub-harmonic octave weight (-1/-2 oct, voice-gated).
      2: Metal Exciter — note-triggered 6-mode inharmonic metallic transient exciter.
    Bypassed at 0% mix with 0 latency and 0 CPU so V1 presets remain intact. */
class ProceduralTextureBus
{
public:
    enum Mode { TapeAir = 0, SubWeight = 1, MetalExciter = 2 };

    struct Params
    {
        int   mode  = TapeAir;
        float mix   = 0.0f;
        float tone  = 0.5f;
        float drive = 0.35f;
    };

    void prepare (float sampleRate, uint32_t seedVal = 0x7E8702u)
    {
        sr = std::max (1.0f, sampleRate);
        gateAtt = std::exp (-1.0f / (0.012f * sr));
        gateRel = std::exp (-1.0f / (0.140f * sr));
        keyEnv.setTimes (8.0f, 220.0f, sr);
        for (int c = 0; c < 2; ++c)
        {
            airBp[c].reset(); airHp[c].reset();
            airHp[c].setup (180.0, 0.7071, (double) sr);
            pinkState[c][0] = pinkState[c][1] = pinkState[c][2] = 0.0f;
        }
        subLp.reset();
        subDc.prepare ((double) sr, 5.0);
        reseed (seedVal);
    }

    void reseed (uint32_t seedVal) noexcept
    {
        rng.seed (seedVal ? seedVal : 0x7E8702u);
        mechDrift.prepare (sr, 5.5f, seedVal ^ 0x13579BDFu);
        for (int c = 0; c < 2; ++c)
        {
            airBp[c].reset(); airHp[c].reset();
            pinkState[c][0] = pinkState[c][1] = pinkState[c][2] = 0.0f;
        }
        subLp.reset(); subDc.reset(); keyEnv.reset();
        mixR.reset(); toneR.reset(); drvR.reset();
        voiceGate = 0.0f;
        subPhase = 0.0;
        subNote = 48.0f;
        mechLfo = 0.0f;
        for (int k = 0; k < kMetalModes; ++k) { mY1[k] = mY2[k] = 0.0f; }
        metalActive = false;
        metalBurstLeft = 0;
        pendingStrike = false;
        strikeVel = 0.0f;
        strikeNote = 60;
    }

    /** Triggered on MIDI note-on (audio thread). */
    void noteOn (int midiNote, float velocity) noexcept
    {
        strikeNote = midiNote;
        strikeVel = velocity;
        pendingStrike = true;
    }

    void process (float* outL, float* outR, const float* keyMono, int n,
                  bool anyVoiceActive, int lowestHeldNote, const Params& p,
                  bool isCtrlTick = true, int ctrlSpan = 0) noexcept
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        const bool wantOn = p.mix > 1.0e-5f;

        if (! wantOn && mixR.cur <= 1.0e-5f && ! metalActive)
        {
            pendingStrike = false;
            return;
        }

        if (lowestHeldNote >= 0) subNote = (float) lowestHeldNote;

        if (isCtrlTick)
        {
            mixR.to (wantOn ? p.mix : 0.0f, ctrlSpan);
            toneR.to (p.tone, ctrlSpan);
            drvR.to (p.drive, ctrlSpan);
            mechLfo = mechDrift.next (ctrlSpan);

            if (p.mode == TapeAir)
            {
                const float fc = 400.0f * fastExp2 (p.tone * 4.5f);   // 400 Hz .. 9 kHz
                const float q = 0.85f + p.drive * 2.2f;
                airBp[0].setup (fc, q, (double) sr);
                airBp[1].setup (fc * 1.08, q, (double) sr);
            }
            else if (p.mode == SubWeight)
            {
                const float fc = 65.0f * fastExp2 (p.tone * 2.1f);    // 65 .. 280 Hz
                subLp.setup (fc, 0.7071, (double) sr);
            }
            else if (p.mode == MetalExciter)
            {
                const float baseHz = 420.0f * fastExp2 (p.tone * 3.2f) * fastExp2 (((float) strikeNote - 60.0f) * 0.04f);
                const float t60 = 0.030f + p.drive * p.drive * 0.220f;
                static constexpr float kRatios[kMetalModes] { 1.0f, 1.4142f, 2.312f, 3.173f, 4.621f, 6.183f };
                for (int k = 0; k < kMetalModes; ++k)
                {
                    const float f = std::clamp (baseHz * kRatios[k], 80.0f, 0.44f * sr);
                    const float th = kTwoPi * f / sr;
                    const float r = std::exp (-6.9077553f / (sr * t60 / (1.0f + 0.35f * (float) k)));
                    mC1[k] = 2.0f * r * std::cos (th);
                    mC2[k] = r * r;
                    mGin[k] = 2.0f * (1.0f - r) * std::sin (th);
                }
            }
        }

        if (pendingStrike)
        {
            pendingStrike = false;
            if (p.mode == MetalExciter && wantOn)
            {
                metalBurstLeft = std::max (8, (int) (0.0025f * sr));
                metalBurstAmp = (0.25f + 0.75f * strikeVel) * (0.6f + 0.8f * p.drive);
                metalActive = true;
            }
        }

        const float gateTarget = anyVoiceActive ? 1.0f : 0.0f;
        const double subDt = std::clamp ((double) midiToHz (subNote - 12.0f) / (double) sr, 1.0e-6, 0.25);

        for (int i = 0; i < n; ++i)
        {
            const float mx = mixR.next();
            const float tn = toneR.next();
            const float dr = drvR.next();
            (void) tn;

            const float gCoef = (gateTarget > voiceGate) ? gateAtt : gateRel;
            voiceGate = flushDenormal (gateTarget + gCoef * (voiceGate - gateTarget));
            if (voiceGate < 1.0e-6f) voiceGate = 0.0f;

            if (mx <= 1.0e-5f) continue;

            if (p.mode == TapeAir)
            {
                const float env = std::min (1.0f, keyEnv.process (keyMono[i]) * 2.2f);
                const float g = voiceGate * (0.25f + 0.75f * env) * (0.85f + 0.15f * mechLfo);
                if (g > 1.0e-6f)
                {
                    float nl = airBp[0].bandpass (airHp[0].highpass (pink (0)));
                    float nr = airBp[1].bandpass (airHp[1].highpass (pink (1)));
                    const float sat = 1.0f + 3.0f * dr;
                    nl = fastTanh (nl * sat) / sat;
                    nr = fastTanh (nr * sat) / sat;
                    outL[i] += nl * g * mx * 0.28f;
                    outR[i] += nr * g * mx * 0.28f;
                }
            }
            else if (p.mode == SubWeight)
            {
                if (voiceGate > 0.0f)
                {
                    const float s1 = fastSin01 ((float) subPhase);
                    const float s2 = fastSin01 ((float) (subPhase * 0.5));
                    const float w = 1.0f + 2.5f * dr;
                    const float raw = fastTanh ((0.72f * s1 + 0.38f * s2) * w) / fastTanh (w);
                    const float sub = subLp.lowpass (subDc.process (raw)) * voiceGate * mx * 0.34f;
                    outL[i] += sub;   // strictly mono-anchored L == R
                    outR[i] += sub;
                }
                subPhase += subDt;
                if (subPhase >= 2.0) subPhase -= 2.0;
            }
            else   // MetalExciter
            {
                if (metalActive)
                {
                    float exc = 0.0f;
                    if (metalBurstLeft > 0)
                    {
                        exc = rng.bip() * metalBurstAmp * ((float) metalBurstLeft / std::max (1.0f, 0.0025f * sr));
                        --metalBurstLeft;
                    }
                    float ml = 0.0f, mr = 0.0f, pk = 0.0f;
                    for (int k = 0; k < kMetalModes; ++k)
                    {
                        const float y = flushDenormal (mC1[k] * mY1[k] - mC2[k] * mY2[k] + mGin[k] * exc);
                        mY2[k] = mY1[k];
                        mY1[k] = y;
                        pk = std::max (pk, std::abs (y));
                        const float pan = (k & 1) ? 0.65f : 0.35f;
                        ml += y * pan;
                        mr += y * (1.0f - pan);
                    }
                    const float sat = 1.0f + 2.5f * dr;
                    outL[i] += fastTanh (ml * sat) / sat * mx * 0.32f;
                    outR[i] += fastTanh (mr * sat) / sat * mx * 0.32f;
                    if (metalBurstLeft <= 0 && pk < 1.0e-6f)
                    {
                        metalActive = false;
                        for (int k = 0; k < kMetalModes; ++k) mY1[k] = mY2[k] = 0.0f;
                    }
                }
            }
        }
    }

private:
    float pink (int c) noexcept
    {
        auto& b = pinkState[c];
        const float w = rng.bip();
        b[0] = 0.99765f * b[0] + w * 0.0990460f;
        b[1] = 0.96300f * b[1] + w * 0.2965164f;
        b[2] = 0.57000f * b[2] + w * 1.0526913f;
        return (b[0] + b[1] + b[2] + w * 0.1848f) * 0.25f;
    }

    static constexpr int kMetalModes = 6;
    float sr = 48000.0f;
    EnvFollower keyEnv;
    Svf airBp[2], airHp[2], subLp;
    DcBlocker subDc;
    Drift mechDrift;
    Rng rng { 0x7E8702u };
    Ramp mixR, toneR, drvR;
    float pinkState[2][3] {};
    float voiceGate = 0.0f, gateAtt = 0.99f, gateRel = 0.999f;
    float subNote = 48.0f, mechLfo = 0.0f;
    double subPhase = 0.0;
    float mC1[kMetalModes] {}, mC2[kMetalModes] {}, mGin[kMetalModes] {};
    float mY1[kMetalModes] {}, mY2[kMetalModes] {};
    bool metalActive = false, pendingStrike = false;
    int metalBurstLeft = 0, strikeNote = 60;
    float metalBurstAmp = 0.0f, strikeVel = 0.0f;
};
} // namespace vessel::dsp
