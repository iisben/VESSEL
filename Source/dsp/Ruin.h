#pragma once

#include "Common.h"

namespace vessel::dsp
{
//==============================================================================
/** (6) Asymmetric thermionic drive with 64-bit double ADAA2 (2nd-order antiderivative antialiasing).
    Runs inside RuinCore's 4x Type-I linear-phase FIR oversampled block. Three bias topologies after the
    classic triode / pentode / bias-starved switch, each with exact closed-form F0, F1, and F2 antiderivatives,
    calibrated midpoint fallbacks, 64-bit ~5 Hz DC blocking, 1-sample phase-aligned dry mix, and click-free
    mode-switch crossfade. */
class TubeDrive
{
public:
    struct Params { bool on = true; int mode = 0; float drive = 0.3f, bias = 0.15f, mix = 1.0f, outDb = 0.0f; };

    static constexpr double kEps1 = 1.0e-5;
    static constexpr double kEps2 = 1.0e-4;

    void prepare (float osRate)
    {
        for (auto& d : dc) d.prepare ((double) osRate, 5.0);
        driveS.prepare (osRate, 30.0f); driveS.snap (0.0f);
        biasS.prepare (osRate, 30.0f);  biasS.snap (0.0f);
        mixS.prepare (osRate, 20.0f);   mixS.snap (0.0f);
        outS.prepare (osRate, 20.0f);   outS.snap (0.0f);
        modeStep = 1.0f / std::max (1.0f, 0.015f * osRate);
        resetState();
    }

    void resetState() noexcept
    {
        for (auto& d : dc) d.reset();
        for (int c = 0; c < 2; ++c)
        {
            x1[c] = x2[c] = f1_1[c] = f2_1[c] = f2_2[c] = bPrev[c] = 0.0;
            modePrev[c] = -1;
            dryPrev[c] = 0.0f;
        }
        curMode = oldMode = 0;
        modeFade = 0.0f;
        modePrimed = false;
    }

    /** Exact static transfer curve F0(x, b, mode) in 64-bit double. */
    static double adaaF0 (double x, double b, int mode) noexcept
    {
        switch (mode)
        {
            case 0:   // triode: soft, asymmetric -> 2nd harmonic
            {
                const double off = 0.45 * b;
                return fastTanhD (x + off) - fastTanhD (off);
            }
            case 1:   // pentode: sharper knee, odd-dominant harmonic bite (exact F1 & F2)
            {
                constexpr double alpha = 0.40;
                const double off = 0.15 * b;
                auto base = [] (double u) noexcept
                {
                    const double t = fastTanhD (u);
                    const double t2 = t * t;
                    return t + alpha * t2 * t * (1.0 - t2);
                };
                return base (x + off) - base (off);
            }
            default:  // starved: C^2 cubic crossover dead-zone + lopsided +1/-0.85 fuzz
            {
                const double u = 1.5 * x + 0.5 * b;
                const double thr = 0.10 + 0.22 * std::max (0.0, -b);
                const double invDen = 1.0 / (1.0 + 2.0 * thr);
                const bool pos = (u >= 0.0);
                const double k = (pos ? 1.6 : 0.95) * invDen;
                const double a = pos ? 1.0 : 0.85;
                const double t = fastTanhD (k * u);
                return a * t * t * t;
            }
        }
    }

    /** Exact first antiderivative F1(x, b, mode) such that dF1/dx == adaaF0(x, b, mode). */
    static double adaaF1 (double x, double b, int mode) noexcept
    {
        switch (mode)
        {
            case 0:
            {
                const double off = 0.45 * b;
                return tanhF1 (x + off) - x * fastTanhD (off);
            }
            case 1:
            {
                constexpr double alpha = 0.40;
                const double off = 0.15 * b;
                const double t0 = fastTanhD (off);
                const double t0_2 = t0 * t0;
                const double c0 = t0 + alpha * t0_2 * t0 * (1.0 - t0_2);
                const double u = x + off;
                const double t = fastTanhD (u);
                const double t2 = t * t;
                return tanhF1 (u) + (0.25 * alpha) * t2 * t2 - x * c0;
            }
            default:
            {
                const double u = 1.5 * x + 0.5 * b;
                const double thr = 0.10 + 0.22 * std::max (0.0, -b);
                const double invDen = 1.0 / (1.0 + 2.0 * thr);
                const bool pos = (u >= 0.0);
                const double k = (pos ? 1.6 : 0.95) * invDen;
                const double a = pos ? 1.0 : 0.85;
                const double s = 1.5 * k;
                const double w = k * u;
                const double t = fastTanhD (w);
                return (a / s) * (tanhF1 (w) - 0.5 * t * t);
            }
        }
    }

    /** Exact second antiderivative F2(x, b, mode) such that dF2/dx == adaaF1(x, b, mode). */
    static double adaaF2 (double x, double b, int mode) noexcept
    {
        switch (mode)
        {
            case 0:
            {
                const double off = 0.45 * b;
                return tanhF2 (x + off) - 0.5 * x * x * fastTanhD (off);
            }
            case 1:
            {
                constexpr double alpha = 0.40;
                const double off = 0.15 * b;
                const double t0 = fastTanhD (off);
                const double t0_2 = t0 * t0;
                const double c0 = t0 + alpha * t0_2 * t0 * (1.0 - t0_2);
                const double u = x + off;
                const double t = fastTanhD (u);
                return tanhF2 (u) + (0.25 * alpha) * (u - t - (1.0 / 3.0) * t * t * t) - 0.5 * x * x * c0;
            }
            default:
            {
                const double u = 1.5 * x + 0.5 * b;
                const double thr = 0.10 + 0.22 * std::max (0.0, -b);
                const double invDen = 1.0 / (1.0 + 2.0 * thr);
                const bool pos = (u >= 0.0);
                const double k = (pos ? 1.6 : 0.95) * invDen;
                const double a = pos ? 1.0 : 0.85;
                const double s = 1.5 * k;
                const double w = k * u;
                const double t = fastTanhD (w);
                return (a / (s * s)) * (tanhF2 (w) - 0.5 * (w - t));
            }
        }
    }

    /** 1st-order ADAA evaluation with calibrated midpoint fallback (exact continuity across |dx| ~ kEps1). */
    static double adaa1Eval (double x, double xPrev, double b, int mode) noexcept
    {
        const double dx = x - xPrev;
        if (std::abs (dx) > kEps1)
            return (adaaF1 (x, b, mode) - adaaF1 (xPrev, b, mode)) / dx;
        return adaaF0 (0.5 * (x + xPrev), b, mode);
    }

    /** 2nd-order ADAA split-half kernel (no division by x0 - x2 -> immune to sine-peak cancellation). */
    static double adaa2Half (double a, double c, double fa2, double fc2, double fc1, double b, int mode) noexcept
    {
        const double d = a - c;
        if (std::abs (d) > kEps2)
            return (fa2 - fc2 - d * fc1) / (d * d);
        return 0.5 * adaaF0 ((a + 2.0 * c) * (1.0 / 3.0), b, mode);
    }

    static double adaa2Eval (double x0, double x1v, double x2v, double b, int mode) noexcept
    {
        const double f1_1v = adaaF1 (x1v, b, mode);
        const double f2_0v = adaaF2 (x0,  b, mode);
        const double f2_1v = adaaF2 (x1v, b, mode);
        const double f2_2v = adaaF2 (x2v, b, mode);
        return adaa2Half (x0, x1v, f2_0v, f2_1v, f1_1v, b, mode)
             + adaa2Half (x2v, x1v, f2_2v, f2_1v, f1_1v, b, mode);
    }

    /** The static transfer curve (also used by the UI and QA naive reference). */
    static float shape (float x, float b, int mode) noexcept
    {
        return (float) adaaF0 ((double) x, (double) b, mode);
    }

    /** In place at the oversampled rate. Always preserves an exact 1-sample oversampled delay (matching ADAA2's
        triangular kernel center at x[n-1]) so dry and wet stay 100% phase-aligned at every mix setting. */
    void process (float* l, float* r, int m, const Params& p)
    {
        mixS.set (p.on ? p.mix : 0.0f);
        if (! p.on && mixS.cur < 1.0e-4f)
        {
            mixS.snap (0.0f);
            for (int i = 0; i < m; ++i)
            {
                const float dl = dryPrev[0]; dryPrev[0] = l[i]; l[i] = dl;
                const float dr = dryPrev[1]; dryPrev[1] = r[i]; r[i] = dr;
            }
            return;
        }
        driveS.set (p.drive); biasS.set (p.bias);
        outS.set (dbToGain (p.outDb) / (1.0f + 1.6f * p.drive));

        if (! modePrimed) { curMode = oldMode = p.mode; modeFade = 0.0f; modePrimed = true; }
        else if (p.mode != curMode) { oldMode = curMode; curMode = p.mode; modeFade = 1.0f; }

        for (int i = 0; i < m; ++i)
        {
            const float d  = driveS.next(), b = biasS.next(), mx = mixS.next(), o = outS.next();
            const double g = std::exp2 ((double) d * 6.643856);   // 0..40 dB
            const double bd = (double) b;

            const float dryL = dryPrev[0]; dryPrev[0] = l[i];
            const float dryR = dryPrev[1]; dryPrev[1] = r[i];

            const double inL = (double) l[i] * g;
            const double inR = (double) r[i] * g;

            double yl = stepChannel (0, inL, bd, curMode);
            double yr = stepChannel (1, inR, bd, curMode);

            if (modeFade > 0.0f)
            {
                // x1[c] now holds in, x2[c] holds prevIn; evaluate previous curve on shared history for click-free switch
                const double oldL = adaa2Eval (inL, prevX1[0], prevX2[0], bd, oldMode);
                const double oldR = adaa2Eval (inR, prevX1[1], prevX2[1], bd, oldMode);
                yl += (double) modeFade * (oldL - yl);
                yr += (double) modeFade * (oldR - yr);
                modeFade = std::max (0.0f, modeFade - modeStep);
            }

            const float wl = (float) dc[0].process (yl);
            const float wr = (float) dc[1].process (yr);
            l[i] = dryL + mx * (wl * o - dryL);
            r[i] = dryR + mx * (wr * o - dryR);
        }
    }

private:
    double stepChannel (int c, double x0, double b, int mode) noexcept
    {
        prevX1[c] = x1[c];
        prevX2[c] = x2[c];

        // Refresh cached antiderivatives if bias or mode moved (prevents parameter-modulation cancellation glitches)
        if (std::abs (b - bPrev[c]) > 1.0e-12 || mode != modePrev[c])
        {
            f1_1[c] = adaaF1 (x1[c], b, mode);
            f2_1[c] = adaaF2 (x1[c], b, mode);
            f2_2[c] = adaaF2 (x2[c], b, mode);
        }

        const double f1_0 = adaaF1 (x0, b, mode);
        const double f2_0 = adaaF2 (x0, b, mode);

        const double y = adaa2Half (x0,    x1[c], f2_0,    f2_1[c], f1_1[c], b, mode)
                       + adaa2Half (x2[c], x1[c], f2_2[c], f2_1[c], f1_1[c], b, mode);

        x2[c] = x1[c]; f2_2[c] = f2_1[c];
        x1[c] = x0;    f1_1[c] = f1_0; f2_1[c] = f2_0;
        bPrev[c] = b;  modePrev[c] = mode;
        return y;
    }

    DcBlocker dc[2];
    Smooth driveS, biasS, mixS, outS;
    double x1[2] {}, x2[2] {}, prevX1[2] {}, prevX2[2] {};
    double f1_1[2] {}, f2_1[2] {}, f2_2[2] {}, bPrev[2] {};
    int modePrev[2] { -1, -1 };
    float dryPrev[2] {};
    int curMode = 0, oldMode = 0;
    float modeFade = 0.0f, modeStep = 1.0f / 2880.0f;
    bool modePrimed = false;
};

//==============================================================================
/** (10) Analog bias modulator: programme-dependent supply sag (gain drops + bias walks as current is drawn)
    feeding a 64-bit magnetic tape curve with hysteresis memory and head-gap HF loss. Runs at the 4x rate. */
class BiasModulator
{
public:
    struct Params { bool on = false; float amount = 0.2f, sag = 0.35f, sat = 0.4f, speedMs = 80; };

    void prepare (float rate)
    {
        sr = rate;
        for (auto& d : dc) d.prepare ((double) sr, 5.0);
        y1[0] = y1[1] = 0.0;
        xPrev[0] = xPrev[1] = 0.0;
        head[0].reset(); head[1].reset();
        kR.reset(); isR.reset(); hystR.reset(); amtR.reset(); sagR.reset();
        env.reset();
    }

    void process (float* l, float* r, int m, const Params& p)
    {
        env.setTimes (4.0f, p.speedMs, sr);
        const float kT = 1.0f + p.sat * 5.0f;
        kR.to (kT, m); isR.to (1.0f / std::sqrt (kT), m);
        hystR.to (0.35f * p.sat, m); amtR.to (0.3f * p.amount, m); sagR.to (p.sag, m);
        const float hf = std::min (20000.0f * (1.0f - 0.55f * p.sat), 0.45f * sr);
        head[0].setCutoff (hf, sr); head[1].setCutoff (hf, sr);

        for (int i = 0; i < m; ++i)
        {
            const double k = (double) kR.next(), is = (double) isR.next(), hyst = (double) hystR.next();
            const double amt = (double) amtR.next(), sag = (double) sagR.next();
            const double e = (double) std::min (2.0f, env.process (0.5f * (l[i] + r[i])) * 2.0f);
            const double sagGain = 1.0 / (1.0 + sag * 3.0 * e);
            const double b = amt + sag * 0.25 * e;
            const double stat = fastTanhD (b);

            float* io[2] { l + i, r + i };
            for (int c = 0; c < 2; ++c)
            {
                const double in = (double) (*io[c]) * sagGain * k + b;
                double y;
                if (hyst < 1.0e-5)
                {
                    // Pure feedforward path -> 64-bit ADAA1 of tanh
                    const double dx = in - xPrev[c];
                    y = std::abs (dx) > 1.0e-5 ? (tanhF1 (in) - tanhF1 (xPrev[c])) / dx
                                               : fastTanhD (0.5 * (in + xPrev[c]));
                    xPrev[c] = in;
                }
                else
                {
                    // Feedback hysteresis path -> smooth 64-bit C-infinity tanh (never ADAA inside feedback)
                    y = fastTanhD (in + hyst * (y1[c] - 0.5 * in));
                    xPrev[c] = in;
                }
                y1[c] = y;
                *io[c] = (float) dc[c].process ((double) head[c].process ((float) ((y - stat) * is)));
            }
        }
    }

private:
    float sr = 192000.0f;
    double y1[2] {}, xPrev[2] {};
    EnvFollower env;
    DcBlocker dc[2];
    OnePole head[2];
    Ramp kR, isR, hystR, amtR, sagR;
};

//==============================================================================
/** The non-linear core of RUIN in one 4x Type-I linear-phase FIR oversampled block:
    (6) ADAA2 tube -> (15) post tilt -> (10) tape bias.
    Uses equiripple linear-phase halfband FIR + exact integer oversampled padding (0..3 samples at 4x)
    so total active latency is an exact linear-phase integer (0-sample PDC error, phase-exact sub alignment),
    and bypasses at 0-sample latency when both nonlinear modules are off at prepareToPlay. */
class RuinCore
{
public:
    struct Params { TubeDrive::Params tube; float postTiltDb = 0, postPivot = 1200; BiasModulator::Params bias; };

    void prepare (float sampleRate, int maxBlock)
    {
        baseSr = sampleRate;
        osr = 4.0f * sampleRate;
        os.prepare (maxBlock);
        const int q = (int) std::llround (((double) os.getLatencyInSamples() + 0.25) * 4.0);
        pad4x = (4 - (q & 3)) & 3;
        tube.prepare (osr);
        bias.prepare (osr);
        tilt[0].reset(); tilt[1].reset();
        biasFade.prepare (osr);
        dry.setSize (2, maxBlock * 4);
        resetPads();
        osActive = true;   // standalone callers (QA / lab) default to active; processor snap() configures true 0-latency bypass
    }

    /** Exact linear-phase integer latency in base-rate samples (0 when bypassed). */
    float latency() const
    {
        if (! osActive) return 0.0f;
        return os.getLatencyInSamples() + (float) (1 + pad4x) * 0.25f;
    }

    void snapBias (bool on) { biasFade.snap (on); }

    /** Synchronous state snap (called from prepareToPlay / state load): configures exact 0-latency bypass when dry. */
    void snap (bool tubeOn, bool biasOn)
    {
        osActive = tubeOn || biasOn;
        biasFade.snap (biasOn);
        os.reset();
        tube.resetState();
        tilt[0].reset(); tilt[1].reset();
        resetPads();
    }

    void process (float* l, float* r, int n, const Params& p)
    {
        if (! osActive && ! p.tube.on && ! p.bias.on)
        {
            // True 0-latency 1x bypass path (linear post-tilt only)
            tilt[0].set (p.postTiltDb, p.postPivot, baseSr, n);
            tilt[1].set (p.postTiltDb, p.postPivot, baseSr, n);
            for (int i = 0; i < n; ++i) { l[i] = tilt[0].process (l[i]); r[i] = tilt[1].process (r[i]); }
            return;
        }
        if (! osActive)
        {
            osActive = true;
            os.reset();
            tube.resetState();
            tilt[0].reset(); tilt[1].reset();
            resetPads();
        }

        os.processUp (l, r, n);
        const int m = n * 4;
        float* ul = os.upL();
        float* ur = os.upR();

        tube.process (ul, ur, m, p.tube);

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

        if (pad4x > 0)
        {
            for (int i = 0; i < m; ++i)
            {
                const float pl = padBuf[0][padIdx]; padBuf[0][padIdx] = ul[i]; ul[i] = pl;
                const float pr = padBuf[1][padIdx]; padBuf[1][padIdx] = ur[i]; ur[i] = pr;
                if (++padIdx >= pad4x) padIdx = 0;
            }
        }

        os.processDown (l, r, n);
    }

private:
    void resetPads() noexcept
    {
        for (int c = 0; c < 2; ++c) for (int k = 0; k < 4; ++k) padBuf[c][k] = 0.0f;
        padIdx = 0;
    }

    float baseSr = 48000.0f, osr = 192000.0f;
    LinearPhase4xOs os;
    TubeDrive tube;
    BiasModulator bias;
    Tilt tilt[2];
    BypassFade biasFade;
    juce::AudioBuffer<float> dry;
    float padBuf[2][4] {};
    int pad4x = 0, padIdx = 0;
    bool osActive = true;
};

//==============================================================================
/** (9) Bit crusher + sample-rate reducer with optional anti-alias pre filter and reconstruction post filter.
    Aliasing here is the point of the effect; the pre filter is the user's control over how much. */
class BitCrusher
{
public:
    struct Params { bool on = false; float bits = 12, rate = 22050, pre = 0, post = 0.3f, mix = 1; };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        acc = 0.0f; held[0] = held[1] = 0.0f;
        for (int c = 0; c < 2; ++c) { preA[c].reset(); preB[c].reset(); post[c].reset(); }
        mixR.reset(); preR.reset();
    }

    void process (float* l, float* r, int n, const Params& p)
    {
        if (! p.on) return;
        const float rate = std::min (p.rate, sr);
        for (int c = 0; c < 2; ++c)
        {
            preA[c].setup (rate * 0.45f, 0.7f, sr);
            preB[c].setup (rate * 0.45f, 0.7f, sr);
            post[c].setup (lerp (20000.0f, rate * 0.5f, p.post), 0.7f, sr);
        }
        const float levels = std::exp2 (p.bits - 1.0f);
        const float inc = rate / sr;
        mixR.to (p.mix, n); preR.to (p.pre, n);

        for (int i = 0; i < n; ++i)
        {
            const float mix = mixR.next(), preAmt = preR.next();
            float* io[2] { l + i, r + i };
            acc += inc;
            const bool take = acc >= 1.0f;
            if (take) acc -= 1.0f;

            for (int c = 0; c < 2; ++c)
            {
                const float x = *io[c];
                const float pre = lerp (x, preB[c].lowpass (preA[c].lowpass (x)), preAmt);
                if (take) held[c] = std::round (pre * levels) / levels;
                float y = held[c];
                if (p.post > 0.0f) y = post[c].lowpass (y);
                *io[c] = x + mix * (y - x);
            }
        }
    }

private:
    float sr = 48000.0f, acc = 0.0f, held[2] {};
    Svf preA[2], preB[2], post[2];
    Ramp mixR, preR;
};

//==============================================================================
/** (7) VHS wow & flutter: periodic wow, random capstan drift, high-rate flutter, oxide-loss HF roll-off
    and random dropouts. A modulated Hermite delay line, same modulation on both channels (one tape). */
class VhsTransport
{
public:
    struct Params { bool on = true; float wow = 0.2f, flutter = 0.15f, drift = 0.3f, rate = 0.6f, age = 0.25f; };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        dl[0].prepare ((int) (sr * 0.05f)); dl[1].prepare ((int) (sr * 0.05f));
        slow.prepare (sr, 0.15f, 11u);
        fast.prepare (sr, 11.0f, 23u);
        wowDrift.prepare (sr, 0.9f, 37u);
        dropS.prepare (sr, 25.0f); dropS.snap (1.0f);
        wowPhase = flutPhase = dropDepth = 0.0f;
        dropLeft = 0;
        lp[0].reset(); lp[1].reset();
        wowR.reset(); driftR.reset(); flutR.reset();
        rng.seed (5150u);
    }

    void process (float* l, float* r, int n, const Params& p)
    {
        if (! p.on)
        {   // keep the tape loop fed, so switching on (under the bypass crossfade) never replays stale audio
            for (int i = 0; i < n; ++i) { dl[0].push (l[i]); dl[1].push (r[i]); }
            return;
        }
        const float base = 0.012f * sr;
        wowR.to (p.wow * 0.0035f * sr, n); driftR.to (p.drift * 0.005f * sr, n); flutR.to (p.flutter * 0.00028f * sr, n);
        const float ageHz = 20000.0f * std::pow (0.2f, p.age);
        lp[0].setup (ageHz, 0.6f, sr); lp[1].setup (ageHz, 0.6f, sr);
        const float dropRate = p.age * p.age * 0.8f / sr;

        for (int i = 0; i < n; ++i)
        {
            wowPhase += p.rate / sr;           if (wowPhase >= 1.0f) wowPhase -= 1.0f;
            flutPhase += 9.3f / sr;            if (flutPhase >= 1.0f) flutPhase -= 1.0f;

            const float wow  = wowR.next() * (0.65f * std::sin (kTwoPi * wowPhase) + 0.35f * wowDrift.next());
            const float drft = driftR.next() * slow.next();
            const float flut = flutR.next() * (0.7f * std::sin (kTwoPi * flutPhase) + 0.3f * fast.next());
            const float d = std::max (2.0f, base + wow + drft + flut);

            if (dropLeft > 0) { --dropLeft; dropS.set (0.25f + 0.4f * dropDepth); }
            else
            {
                dropS.set (1.0f);
                if (rng.uni() < dropRate) { dropLeft = (int) ((0.03f + 0.1f * rng.uni()) * sr); dropDepth = rng.uni(); }
            }
            const float g = dropS.next();

            dl[0].push (l[i]); dl[1].push (r[i]);
            l[i] = lp[0].lowpass (dl[0].readHermite (d)) * g;
            r[i] = lp[1].lowpass (dl[1].readHermite (d)) * g;
        }
    }

private:
    float sr = 48000.0f, wowPhase = 0, flutPhase = 0, dropDepth = 0;
    int dropLeft = 0;
    DelayLine dl[2];
    Drift slow, fast, wowDrift;
    Svf lp[2];
    Smooth dropS;
    Ramp wowR, driftR, flutR;
    Rng rng;
};

//==============================================================================
/** (8) Tape start/stop motor. A varispeed read head trails the write head.
    Stop: speed falls 1→0 (ease-out) and the transport parks.
    Start: s(u) = 6u − 9u² + 4u³ — starts at rest, overshoots ~25% like a real capstan motor, and arrives at
    unity with zero accumulated lag (∫s du = 1), so the motor adds no latency once it is up to speed. */
class TapeMotor
{
public:
    enum class Mode { Off, Start, Stop, Both };
    struct Params { Mode mode = Mode::Off; float startMs = 350, stopMs = 900; };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        buf[0].prepare ((int) (sr * 10.0f)); buf[1].prepare ((int) (sr * 10.0f));
        state = State::Running; writeCount = 0; readPos = -1.0; xfade = 0;
    }

    void noteOnFromSilence (const Params& p)
    {
        if (p.mode == Mode::Start || p.mode == Mode::Both) begin (State::Starting);
        else if (state != State::Running) begin (State::Running);
    }

    void allReleased (const Params& p)
    {
        if (p.mode == Mode::Stop || p.mode == Mode::Both)
            if (state == State::Running || state == State::Starting) { state = State::Stopping; u = 0.0f; }
    }

    float speedForDisplay() const noexcept { return state == State::Stopped ? 0.0f : speed; }

    void process (float* l, float* r, int n, const Params& p)
    {
        if (p.mode == Mode::Off && state == State::Running && xfade == 0)
        {
            for (int i = 0; i < n; ++i) { buf[0].push (l[i]); buf[1].push (r[i]); ++writeCount; }
            readPos = (double) (writeCount - 1);
            return;
        }

        const float startInc = 1.0f / (std::max (5.0f, p.startMs) * 0.001f * sr);
        const float stopInc  = 1.0f / (std::max (5.0f, p.stopMs) * 0.001f * sr);

        for (int i = 0; i < n; ++i)
        {
            buf[0].push (l[i]); buf[1].push (r[i]); ++writeCount;
            float gain = 1.0f;

            switch (state)
            {
                case State::Running:  speed = 1.0f; readPos = (double) (writeCount - 1); break;
                case State::Stopping:
                    u += stopInc;
                    speed = std::pow (std::max (0.0f, 1.0f - u), 1.6f);
                    readPos += speed;
                    gain = std::min (1.0f, speed * 10.0f);
                    if (u >= 1.0f) { state = State::Stopped; speed = 0.0f; }
                    break;
                case State::Stopped:  speed = 0.0f; gain = 0.0f; break;
                case State::Starting:
                    u += startInc;
                    if (u >= 1.0f) { state = State::Running; speed = 1.0f; readPos = (double) (writeCount - 1); break; }
                    speed = u * (6.0f + u * (-9.0f + 4.0f * u));
                    readPos += speed;
                    gain = std::min (1.0f, u * 25.0f);
                    break;
            }

            readPos = std::min (readPos, (double) (writeCount - 1));
            float ol = buf[0].readAt (readPos) * gain;
            float orr = buf[1].readAt (readPos) * gain;

            if (xfade > 0)
            {
                const float f = (float) xfade / (float) kXfade;
                oldPos += oldSpeed;
                oldPos = std::min (oldPos, (double) (writeCount - 1));
                ol  = ol  * (1.0f - f) + buf[0].readAt (oldPos) * oldGain * f;
                orr = orr * (1.0f - f) + buf[1].readAt (oldPos) * oldGain * f;
                --xfade;
            }
            l[i] = ol; r[i] = orr;
        }
    }

private:
    enum class State { Running, Stopping, Stopped, Starting };
    static constexpr int kXfade = 256;

    void begin (State s)
    {
        oldPos = readPos; oldSpeed = speed;
        oldGain = state == State::Stopped ? 0.0f : 1.0f;
        xfade = kXfade;
        state = s; u = 0.0f;
        readPos = (double) (writeCount - 1);
        speed = s == State::Running ? 1.0f : 0.0f;
    }

    float sr = 48000.0f, u = 0.0f, speed = 1.0f, oldSpeed = 1.0f, oldGain = 1.0f;
    State state = State::Running;
    DelayLine buf[2];
    int64_t writeCount = 0;
    double readPos = -1.0, oldPos = 0.0;
    int xfade = 0;
};
} // namespace vessel::dsp
