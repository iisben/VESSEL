#pragma once

#include "Common.h"

namespace vessel::dsp
{
//==============================================================================
/** (12) Envelope-follower lowpass: the programme's own amplitude opens (or, with negative depth, closes) a resonant LPF. */
class FollowerFilter
{
public:
    struct Params { bool on = false; float cutoff = 300, depth = 0.6f, res = 0.3f, attackMs = 5, releaseMs = 180, mix = 1; };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        env.reset(); envN = 0.0f;
        f[0].reset(); f[1].reset();
        cutR.reset(); depthR.reset(); qR.reset(); mixR.reset();
    }
    float lastEnv() const noexcept { return envN; }

    void process (float* l, float* r, int n, const Params& p)
    {
        if (! p.on) { envN = 0.0f; return; }
        env.setTimes (p.attackMs, p.releaseMs, sr);
        cutR.to (std::log2 (std::max (20.0f, p.cutoff)), n); depthR.to (p.depth, n);
        qR.to (0.6f + p.res * 9.0f, n); mixR.to (p.mix, n);

        for (int i = 0; i < n; ++i)
        {
            const float lc = cutR.next(), dp = depthR.next(), q = qR.next(), mix = mixR.next();
            envN = std::min (1.0f, env.process (0.5f * (l[i] + r[i])) * 3.0f);
            if ((i & 7) == 0)
            {
                const float fc = std::exp2 (lc + dp * 7.0f * envN);
                f[0].setup (fc, q, sr); f[1].setup (fc, q, sr);
            }
            l[i] += mix * (f[0].lowpass (l[i]) - l[i]);
            r[i] += mix * (f[1].lowpass (r[i]) - r[i]);
        }
    }

private:
    float sr = 48000.0f, envN = 0.0f;
    EnvFollower env;
    Svf f[2];
    Ramp cutR, depthR, qR, mixR;
};

//==============================================================================
/** (13) Spectral unmasking: a dynamic EQ on everything except the sub. Keyed by the sub bus, it digs a bell
    at the sub's band (and a shallower one at its 2nd harmonic) only while the sub is actually sounding. */
class Unmasker
{
public:
    struct Params { bool on = true; float freq = 110, depthDb = 9, releaseMs = 120; };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        env.reset(); lastCut = 0.0f; g1 = g2 = 1.0f;
        keyLp.reset();
        for (int c = 0; c < 2; ++c) { b1[c].reset(); b2[c].reset(); }
    }
    float reductionDb() const noexcept { return lastCut; }

    void process (float* l, float* r, const float* key, int n, const Params& p)
    {
        if (! p.on) { lastCut = 0.0f; return; }
        env.setTimes (2.0f, p.releaseMs, sr);
        keyLp.setup (p.freq * 1.5f, 0.7f, sr);

        for (int i = 0; i < n; ++i)
        {
            const float e = std::min (1.0f, env.process (keyLp.lowpass (key[i])) * 2.5f);
            if ((i & 15) == 0)
            {
                lastCut = p.depthDb * e;
                g1 = dbToGain (-lastCut);
                g2 = dbToGain (-0.5f * lastCut);
                for (int c = 0; c < 2; ++c) { b1[c].setup (p.freq, 0.8f, sr); b2[c].setup (p.freq * 2.0f, 1.2f, sr); }
            }
            l[i] = b2[0].bell (b1[0].bell (l[i], g1), g2);
            r[i] = b2[1].bell (b1[1].bell (r[i], g1), g2);
        }
    }

private:
    float sr = 48000.0f, lastCut = 0.0f, g1 = 1.0f, g2 = 1.0f;
    EnvFollower env;
    Svf keyLp, b1[2], b2[2];
};

//==============================================================================
/** (14) Formant filter: four parallel resonators morphing continuously through A-E-I-O-U,
    blendable toward cello body resonances, with formant shift and resonance. */
class FormantFilter
{
public:
    struct Params { bool on = false; float vowel = 0, body = 0.3f, shift = 0, res = 0.5f, mix = 0.6f; };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        for (int c = 0; c < 2; ++c) for (int k = 0; k < 4; ++k) f[c][k].reset();
        vowelR.reset(); bodyR.reset(); shiftR.reset(); resR.reset(); mixR.reset();
    }

    void process (float* l, float* r, int n, const Params& p)
    {
        if (! p.on) return;
        vowelR.to (std::clamp (p.vowel, 0.0f, 4.0f), n); bodyR.to (p.body, n); shiftR.to (p.shift, n);
        resR.to (p.res, n); mixR.to (p.mix, n);

        for (int i = 0; i < n; ++i)
        {
            const float v = vowelR.next(), b = bodyR.next(), sh = shiftR.next(), re = resR.next(), mix = mixR.next();
            if ((i & 3) == 0) update (v, b, sh, re);   // resonators re-tuned every 4 samples from ramped controls

            float wl = 0.0f, wr = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                wl += f[0][k].bandpass (l[i]) * gains[k];
                wr += f[1][k].bandpass (r[i]) * gains[k];
            }
            l[i] += mix * (wl * makeup - l[i]);
            r[i] += mix * (wr * makeup - r[i]);
        }
    }

private:
    void update (float vowel, float body, float shiftSt, float res) noexcept
    {
        static constexpr float vowels[5][4] {
            { 730, 1090, 2440, 3400 },   // A
            { 530, 1840, 2480, 3500 },   // E
            { 270, 2290, 3010, 3500 },   // I
            { 570,  840, 2410, 3300 },   // O
            { 300,  870, 2240, 3200 } }; // U
        static constexpr float cello[4] { 215, 590, 1150, 2650 };
        static constexpr float gainsDb[4] { 0.0f, -4.0f, -9.0f, -14.0f };

        const int a = std::min (3, (int) vowel);
        const float t = vowel - (float) a;
        const float shift = semis (shiftSt);
        const float q = 3.0f + res * 17.0f;
        for (int k = 0; k < 4; ++k)
        {
            const float fv = lerp (vowels[a][k], vowels[a + 1][k], t);
            const float fc = lerp (fv, cello[k], body) * shift;
            const float qk = q * std::sqrt (fc / 1000.0f) * 0.8f + 1.0f;
            f[0][k].setup (fc, qk, sr); f[1][k].setup (fc, qk, sr);
            gains[k] = dbToGain (gainsDb[k]) * f[0][k].k;   // k·bp = unity-peak bandpass
        }
        makeup = 1.6f + res * 1.2f;
    }

    float sr = 48000.0f, gains[4] {}, makeup = 1.6f;
    Svf f[2][4];
    Ramp vowelR, bodyR, shiftR, resR, mixR;
};
} // namespace vessel::dsp
