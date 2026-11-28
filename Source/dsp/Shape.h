#pragma once

#include "Common.h"

namespace vessel::dsp
{
//==============================================================================
/** (12) Envelope-follower lowpass: 64-bit ZDF Sallen-Key Dual-Diode Core (12 dB/oct, 2x oversampled).
    The programme's own amplitude opens (or, with negative depth, closes) the resonant LPF;
    high resonance pushes into asymmetric dual-diode clipping and organic self-oscillation. */
class FollowerFilter
{
public:
    struct Params { bool on = false; float cutoff = 300, depth = 0.6f, res = 0.3f, attackMs = 5, releaseMs = 180, mix = 1; };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        env.reset(); envN = 0.0f; sampleCount = 0;
        for (int c = 0; c < 2; ++c)
        {
            f[c].prepare ((double) sampleRate, true);
            dc[c].prepare ((double) sampleRate, 5.0);
        }
        cutR.reset(); depthR.reset(); resR.reset(); mixR.reset();
    }
    float lastEnv() const noexcept { return envN; }

    void process (float* l, float* r, int n, const Params& p, bool isCtrlTick = true, int ctrlSpan = 0)
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        if (! p.on) { envN = 0.0f; return; }
        if (isCtrlTick)
        {
            env.setTimes (p.attackMs, p.releaseMs, sr);
            cutR.to (fastLog2 (std::max (20.0f, p.cutoff)), ctrlSpan); depthR.to (p.depth, ctrlSpan);
            resR.to (clamp01 (p.res), ctrlSpan); mixR.to (p.mix, ctrlSpan);
        }

        for (int i = 0; i < n; ++i)
        {
            const float lc = cutR.next(), dp = depthR.next(), rv = resR.next(), mix = mixR.next();
            envN = std::min (1.0f, env.process (0.5f * (l[i] + r[i])) * 3.0f);
            if ((sampleCount++ & 7u) == 0u)
            {
                const double fc = (double) fastExp2 (lc + dp * 7.0f * envN);
                // Calibrated to match linear SVF Q = 0.6 + 9*res for existing presets (res <= 0.35),
                // smoothly pushing above k = 2.0 into asymmetric dual-diode scream at high resonance.
                const double rd = (double) rv;
                const double r2 = rd * rd;
                const double kRes = std::min (2.28, (2.0 - 1.0 / (0.6 + 9.0 * rd)) + 0.38 * r2 * r2);
                const double drv = 1.0 + 0.65 * r2;
                f[0].setup (fc, kRes, drv);
                f[1].setup (fc, kRes, drv);
            }
            const float yl = (float) dc[0].process (f[0].process2x ((double) l[i]));
            const float yr = (float) dc[1].process (f[1].process2x ((double) r[i]));
            l[i] += mix * (yl - l[i]);
            r[i] += mix * (yr - r[i]);
        }
    }

private:
    float sr = 48000.0f, envN = 0.0f;
    uint64_t sampleCount = 0;
    EnvFollower env;
    SallenKeyCore f[2];
    DcBlocker dc[2];
    Ramp cutR, depthR, resR, mixR;
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
        env.reset(); lastCut = 0.0f; g1 = g2 = 1.0f; sampleCount = 0;
        keyLp.reset();
        for (int c = 0; c < 2; ++c) { b1[c].reset(); b2[c].reset(); }
    }
    float reductionDb() const noexcept { return lastCut; }

    void process (float* l, float* r, const float* key, int n, const Params& p, bool isCtrlTick = true)
    {
        if (! p.on) { lastCut = 0.0f; return; }
        if (isCtrlTick)
        {
            env.setTimes (2.0f, p.releaseMs, sr);
            keyLp.setup (p.freq * 1.5f, 0.7f, sr);
        }

        for (int i = 0; i < n; ++i)
        {
            const float e = std::min (1.0f, env.process (keyLp.lowpass (key[i])) * 2.5f);
            if ((sampleCount++ & 15u) == 0u)
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
    uint64_t sampleCount = 0;
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
        sampleCount = 0;
        for (int c = 0; c < 2; ++c) for (int k = 0; k < 4; ++k) f[c][k].reset();
        vowelR.reset(); bodyR.reset(); shiftR.reset(); resR.reset(); mixR.reset();
    }

    void process (float* l, float* r, int n, const Params& p, bool isCtrlTick = true, int ctrlSpan = 0)
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        if (! p.on) return;
        if (isCtrlTick)
        {
            vowelR.to (std::clamp (p.vowel, 0.0f, 4.0f), ctrlSpan); bodyR.to (p.body, ctrlSpan); shiftR.to (p.shift, ctrlSpan);
            resR.to (p.res, ctrlSpan); mixR.to (p.mix, ctrlSpan);
        }

        for (int i = 0; i < n; ++i)
        {
            const float v = vowelR.next(), b = bodyR.next(), sh = shiftR.next(), re = resR.next(), mix = mixR.next();
            if ((sampleCount++ & 3u) == 0u) update (v, b, sh, re);   // resonators re-tuned every 4 absolute samples

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
            gains[k] = dbToGain (gainsDb[k]) * (float) f[0][k].k;   // k·bp = unity-peak bandpass
        }
        makeup = 1.6f + res * 1.2f;
    }

    float sr = 48000.0f, gains[4] {}, makeup = 1.6f;
    uint64_t sampleCount = 0;
    Svf f[2][4];
    Ramp vowelR, bodyR, shiftR, resR, mixR;
};

//==============================================================================
/** Zero-gain-change cascaded 4-to-8-stage tunable all-pass phase dispersion network.
    Uses 64-bit double Cytomic TPT/ZDF 2nd-order all-pass sections:
      H_ap(s) = (s^2 - k*w0*s + w0^2) / (s^2 + k*w0*s + w0^2) = 1 - 2*k*H_bp(s)
    Properties:
    - Exact unity magnitude |H(e^{jw})| == 1.0 (0.000 dB ripple across 0..Nyquist)
      for every stage count (4..8), frequency (40..2000 Hz), pinch/Q, and sample rate.
    - Exact +1.0 DC gain (zero polarity flip, zero DC offset injection).
    - Strictly phase-matched L & R coefficients preserve sub-120 Hz mono correlation
      (0.000 deg inter-channel phase skew).
    - Sample-rate invariant across 44.1-192 kHz, odd-block invariant, 0 added latency,
      0 CPU when bypassed. */
class PhaseDisperser
{
public:
    static constexpr int kMaxStages = 8;

    struct Params
    {
        bool  on     = false;
        int   stages = 6;       // 4..8 cascaded 2nd-order TPT all-pass sections
        float freqHz = 180.0f;  // dispersion pivot frequency (40..2000 Hz)
        float pinch  = 0.50f;   // 0..1 group-delay pinch (Q = 0.50..2.80)
    };

    void prepare (double sampleRate) noexcept
    {
        sr = std::max (sampleRate, 1.0);
        reset();
    }

    void reset() noexcept
    {
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < kMaxStages; ++s)
                ic1[c][s] = ic2[c][s] = 0.0;
        gCur = kCur = 0.0;
        dg = dk = 0.0;
        primed = false;
        wasActive = false;
        lastStages = 0;
    }

    int activeStages() const noexcept { return lastStages >= 4 ? lastStages : 6; }

    void process (float* __restrict l, float* __restrict r, int n, const Params& p,
                  bool isCtrlTick = true, int ctrlSpan = 0) noexcept
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        const int numStages = std::clamp (p.stages, 4, kMaxStages);
        if (! p.on)
        {
            if (wasActive) reset();
            return;
        }
        wasActive = true;

        if (numStages < lastStages)
        {
            for (int c = 0; c < 2; ++c)
                for (int s = numStages; s < kMaxStages; ++s)
                    ic1[c][s] = ic2[c][s] = 0.0;
        }
        lastStages = numStages;

        if (isCtrlTick || ! primed)
        {
            const double fc = std::clamp ((double) p.freqHz, 20.0, 0.45 * sr);
            const double pin = std::clamp ((double) p.pinch, 0.0, 1.0);
            const double q = 0.50 + 2.30 * pin * pin;
            const double gT = std::tan (kPiD * fc / sr);
            const double kT = 1.0 / q;

            if (! primed || (std::abs (gT - gCur) <= 1.0e-7 && std::abs (kT - kCur) <= 1.0e-6))
            {
                gCur = gT;
                kCur = kT;
                dg = 0.0;
                dk = 0.0;
                primed = true;
            }
            else
            {
                const double inv = 1.0 / (double) std::max (1, ctrlSpan);
                dg = (gT - gCur) * inv;
                dk = (kT - kCur) * inv;
            }
        }

        double s1L[kMaxStages], s2L[kMaxStages], s1R[kMaxStages], s2R[kMaxStages];
        for (int s = 0; s < numStages; ++s)
        {
            s1L[s] = ic1[0][s]; s2L[s] = ic2[0][s];
            s1R[s] = ic1[1][s]; s2R[s] = ic2[1][s];
        }

        if (dg == 0.0 && dk == 0.0)
        {
            const double g = gCur;
            const double k = kCur;
            const double a1 = 1.0 / (1.0 + g * (g + k));
            const double a2 = g * a1;
            const double a3 = g * a2;
            const double twoK = 2.0 * k;

            for (int i = 0; i < n; ++i)
            {
                double xl = (double) l[i];
                double xr = (double) r[i];
                for (int s = 0; s < numStages; ++s)
                {
                    const double v3l = xl - s2L[s];
                    const double v1l = a1 * s1L[s] + a2 * v3l;
                    const double v2l = s2L[s] + a2 * s1L[s] + a3 * v3l;
                    s1L[s] = 2.0 * v1l - s1L[s];
                    s2L[s] = 2.0 * v2l - s2L[s];
                    xl -= twoK * v1l;

                    const double v3r = xr - s2R[s];
                    const double v1r = a1 * s1R[s] + a2 * v3r;
                    const double v2r = s2R[s] + a2 * s1R[s] + a3 * v3r;
                    s1R[s] = 2.0 * v1r - s1R[s];
                    s2R[s] = 2.0 * v2r - s2R[s];
                    xr -= twoK * v1r;
                }
                l[i] = (float) xl;
                r[i] = (float) xr;
            }
        }
        else
        {
            double g = gCur, k = kCur;
            for (int i = 0; i < n; ++i)
            {
                g = std::max (1.0e-6, g + dg);
                k = std::max (0.20, k + dk);
                const double a1 = 1.0 / (1.0 + g * (g + k));
                const double a2 = g * a1;
                const double a3 = g * a2;
                const double twoK = 2.0 * k;

                double xl = (double) l[i];
                double xr = (double) r[i];
                for (int s = 0; s < numStages; ++s)
                {
                    const double v3l = xl - s2L[s];
                    const double v1l = a1 * s1L[s] + a2 * v3l;
                    const double v2l = s2L[s] + a2 * s1L[s] + a3 * v3l;
                    s1L[s] = 2.0 * v1l - s1L[s];
                    s2L[s] = 2.0 * v2l - s2L[s];
                    xl -= twoK * v1l;

                    const double v3r = xr - s2R[s];
                    const double v1r = a1 * s1R[s] + a2 * v3r;
                    const double v2r = s2R[s] + a2 * s1R[s] + a3 * v3r;
                    s1R[s] = 2.0 * v1r - s1R[s];
                    s2R[s] = 2.0 * v2r - s2R[s];
                    xr -= twoK * v1r;
                }
                l[i] = (float) xl;
                r[i] = (float) xr;
            }
            gCur = g;
            kCur = k;
        }

        for (int s = 0; s < numStages; ++s)
        {
            ic1[0][s] = flushDenormal (s1L[s]);
            ic2[0][s] = flushDenormal (s2L[s]);
            ic1[1][s] = flushDenormal (s1R[s]);
            ic2[1][s] = flushDenormal (s2R[s]);
        }
    }

private:
    double sr = 48000.0;
    double ic1[2][kMaxStages] {}, ic2[2][kMaxStages] {};
    double gCur = 0.0, kCur = 0.0, dg = 0.0, dk = 0.0;
    bool primed = false, wasActive = false;
    int lastStages = 0;
};
} // namespace vessel::dsp
