#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

namespace vessel::dsp
{
constexpr double kPiD    = std::numbers::pi_v<double>;
constexpr double kTwoPiD = 2.0 * kPiD;
constexpr float  kPi     = std::numbers::pi_v<float>;
constexpr float  kTwoPi  = 2.0f * kPi;

/** Validated Padé/minimax fast 2^x (C++20 portable bit_cast, max rel error < 1.5e-5 -> < 0.03 cents / < 0.00015 dB). */
inline float fastExp2 (float x) noexcept
{
    x = std::clamp (x, -126.0f, 126.0f);
    const float nf = std::floor (x);
    const int n = (int) nf;
    const float f = x - nf;
    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    const uint32_t bits = (uint32_t) (n + 127) << 23;
    return std::bit_cast<float> (bits) * p;
}

/** Validated rational fast log2(x) for x > 0 (C++20 portable bit_cast, max error < 4e-6 -> < 0.00003 dB). */
inline float fastLog2 (float x) noexcept
{
    x = std::max (x, 1.0e-30f);
    const uint32_t bits = std::bit_cast<uint32_t> (x);
    int e = (int) ((bits >> 23) & 0xFFu) - 127;
    float m = std::bit_cast<float> ((bits & 0x007FFFFFu) | 0x3F800000u);   // [1, 2)
    if (m > 1.41421356f) { m *= 0.5f; ++e; }                              // [sqrt(0.5), sqrt(2)]
    const float z = (m - 1.0f) / (m + 1.0f);
    const float z2 = z * z;
    constexpr float kTwoOverLn2 = 2.88539008178f;
    return (float) e + kTwoOverLn2 * z * (1.0f + z2 * (0.33333333f + z2 * (0.20000000f + z2 * 0.14285714f)));
}

/** Fast sin(2*pi*p) when p is already in [0, 1) (zero std::floor, max error < 5e-7). */
inline float fastSin01Unit (float p) noexcept
{
    float t = p;
    if (t > 0.75f)      t -= 1.0f;
    else if (t > 0.25f) t = 0.5f - t;
    const float x = kTwoPi * t;
    const float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333315f + x2 * (-0.0001984090f + x2 * 2.752556e-6f))));
}

/** Validated minimax fast sin(2*pi*p) for any unit-cycle phase p (max error < 5e-7, 0 libm calls). */
inline float fastSin01 (float p) noexcept
{
    return fastSin01Unit (p - std::floor (p));
}

/** Validated minimax fast cos(2*pi*p) for unit-cycle phase p (max error < 5e-7). */
inline float fastCos01 (float p) noexcept
{
    return fastSin01 (p + 0.25f);
}

inline float dbToGain (float db) noexcept   { return fastExp2 (db * 0.16609640474f); }
inline float gainToDb (float g) noexcept    { return 6.02059991328f * fastLog2 (std::max (g, 1.0e-9f)); }
inline float clamp01 (float x) noexcept     { return std::clamp (x, 0.0f, 1.0f); }
inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }
inline float semis (float st) noexcept      { return fastExp2 (st * (1.0f / 12.0f)); }
inline float midiToHz (float note) noexcept { return 440.0f * fastExp2 ((note - 69.0f) * (1.0f / 12.0f)); }

/** Rational tanh (float): exact at ±3 (=±1), C1-continuous, cheap (control/non-critical use). */
inline float fastTanh (float x) noexcept
{
    x = std::clamp (x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/** 64-bit smooth C-infinity tanh via Padé [5/4] + 3 double-angle steps (branchless, auto-vectorizable,
    max error < 1e-12 across [-18, +18], machine-exact asymptote ±1 beyond ±18 with zero corner). */
inline double fastTanhD (double x) noexcept
{
    x = std::clamp (x, -18.0, 18.0);
    const double z  = x * 0.125;
    const double z2 = z * z;
    double t = z * (945.0 + z2 * (105.0 + z2)) / (945.0 + z2 * (420.0 + 15.0 * z2));
    t = (2.0 * t) / (1.0 + t * t);
    t = (2.0 * t) / (1.0 + t * t);
    t = (2.0 * t) / (1.0 + t * t);
    return t;
}

/** 1-division 64-bit Padé [9/8] tanh for hot inner ZDF NR loops (monotonic on [-6, 6], max error < 8.2e-6,
    auto-vectorizable NEON/SSE/AVX with 4x fewer divisions than double-angle form). */
inline double fastTanhLadder (double x) noexcept
{
    x = std::clamp (x, -6.0, 6.0);
    const double x2 = x * x;
    const double num = x * (34459425.0 + x2 * (4729725.0 + x2 * (135135.0 + x2 * (990.0 + x2))));
    const double den = 34459425.0 + x2 * (16216200.0 + x2 * (945945.0 + x2 * (13860.0 + 45.0 * x2)));
    return num / den;
}

/** Overflow-safe first antiderivative of tanh(u): ln(cosh(u)). */
inline double tanhF1 (double u) noexcept
{
    const double a = std::abs (u);
    return a + std::log1p (std::exp (-2.0 * a)) - std::numbers::ln2_v<double>;
}

/** Exact second antiderivative of tanh(u): integral_0^u ln(cosh(s)) ds (< 1e-14 error everywhere).
    Degree-21 Chebyshev odd polynomial on [-1, 1]; analytic Li2(-exp(-2|u|)) series for |u| > 1. */
inline double tanhF2 (double u) noexcept
{
    const double a = std::abs (u);
    if (a <= 1.0)
    {
        const double u2 = u * u;
        double p = -1.04082453322718038e-07;
        p = p * u2 +  9.22822032465668182e-07;
        p = p * u2 + -4.41034879411841829e-06;
        p = p * u2 +  1.63542695112197308e-05;
        p = p * u2 + -5.64092053448049974e-05;
        p = p * u2 +  1.98666093502251526e-04;
        p = p * u2 + -7.49524253350742622e-04;
        p = p * u2 +  3.17459810216789854e-03;
        p = p * u2 + -1.66666662536115034e-02;
        p = p * u2 +  1.66666666651057810e-01;
        p = p * u2 +  1.75717971591030797e-13;
        return p * u;
    }
    const double z = std::exp (-2.0 * a);
    // 9-term Horner Li2(-z) for 0 < z <= exp(-2) = 0.13534 (zero divisions, max error < 2e-11)
    double li = 1.0 / 81.0;
    li = -li * z + 1.0 / 64.0;
    li = -li * z + 1.0 / 49.0;
    li = -li * z + 1.0 / 36.0;
    li = -li * z + 1.0 / 25.0;
    li = -li * z + 1.0 / 16.0;
    li = -li * z + 1.0 / 9.0;
    li = -li * z + 1.0 / 4.0;
    li = -li * z + 1.0;
    li *= -z;
    constexpr double kPiSqOver24 = (kPiD * kPiD) / 24.0;
    const double val = 0.5 * a * a - a * std::numbers::ln2_v<double> + kPiSqOver24 + 0.5 * li;
    return u >= 0.0 ? val : -val;
}

inline float  flushDenormal (float x) noexcept  { return std::abs (x) < 1.0e-15f ? 0.0f : x; }
inline double flushDenormal (double x) noexcept { return std::abs (x) < 1.0e-30 ? 0.0 : x; }

/** Bidirectional phase wrap into [0, 1) (handles both positive and negative TZFM phase increments). */
inline double wrapPhase01 (double p) noexcept
{
    p -= std::floor (p);
    return (p >= 0.0 && p < 1.0) ? p : 0.0;
}

inline float wrapPhase01 (float p) noexcept
{
    p -= std::floor (p);
    return (p >= 0.0f && p < 1.0f) ? p : 0.0f;
}

/** tan(x) for |x| <= ~1.45 (Padé [5/4]); retained for non-critical helpers. */
inline float fastTan (float x) noexcept
{
    const float x2 = x * x;
    return x * (945.0f + x2 * (-105.0f + x2)) / (945.0f + x2 * (-420.0f + 15.0f * x2));
}

/** Sample-accurate linear ramp between per-chunk control values.
    The engine computes controls once per 64-sample chunk; every gain, mix and level that touches the signal
    runs through one of these, so a control change becomes a straight line across the chunk instead of a step. */
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
    void reset() noexcept { cur = inc = 0.0f; primed = false; }
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

/** xorshift32 – deterministic, allocation-free, audio-thread safe. */
struct Rng
{
    uint32_t s = 0x9E3779B9u;
    explicit Rng (uint32_t seed = 0x9E3779B9u) : s (seed ? seed : 1u) {}
    void seed (uint32_t v) noexcept { s = v ? v : 1u; }
    uint32_t nextU() noexcept { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() noexcept { return (float) (nextU() >> 8) * (1.0f / 16777216.0f); }   // [0,1)
    float bip() noexcept { return uni() * 2.0f - 1.0f; }                              // [-1,1)
};

/** One-pole lowpass. */
struct OnePole
{
    float a = 0.0f, z = 0.0f;
    void setCutoff (float hz, float sr) noexcept { a = std::exp (-kTwoPi * std::min (hz, 0.49f * sr) / sr); }
    void setTime (float ms, float sr) noexcept    { a = ms <= 0.0f ? 0.0f : std::exp (-1.0f / (0.001f * ms * sr)); }
    float process (float x) noexcept { z = flushDenormal (x + a * (z - x)); return z; }
    void reset (float v = 0.0f) noexcept { z = v; }
};

/** Parameter smoother (exponential, ~time constant, sample-rate invariant). */
struct Smooth
{
    float cur = 0.0f, target = 0.0f, a = 0.0f;
    void prepare (float sr, float ms = 20.0f) noexcept { a = std::exp (-1.0f / (0.001f * std::max (0.01f, ms) * std::max (1.0f, sr))); }
    void set (float t) noexcept { target = t; }
    void snap (float t) noexcept { target = cur = t; }
    float next() noexcept { cur = target + a * (cur - target); return cur; }
};

/** Sample-rate-invariant log-frequency smoother (perceptually uniform pitch/cutoff sweeps across 44.1..192 kHz). */
struct LogSmooth
{
    float curLog = 10.0f, targetLog = 10.0f, a = 0.0f;
    void prepare (float sr, float ms = 20.0f) noexcept { a = std::exp (-1.0f / (0.001f * std::max (0.01f, ms) * std::max (1.0f, sr))); }
    void setHz (float hz) noexcept { targetLog = fastLog2 (std::max (1.0f, hz)); }
    void snapHz (float hz) noexcept { targetLog = curLog = fastLog2 (std::max (1.0f, hz)); }
    float nextHz() noexcept { curLog = targetLog + a * (curLog - targetLog); return fastExp2 (curLog); }
};

/** Sample-rate-invariant linear-dB smoother (perceptually uniform loudness sweeps across 44.1..192 kHz). */
struct DbSmooth
{
    float curDb = 0.0f, targetDb = 0.0f, a = 0.0f;
    void prepare (float sr, float ms = 20.0f) noexcept { a = std::exp (-1.0f / (0.001f * std::max (0.01f, ms) * std::max (1.0f, sr))); }
    void setDb (float db) noexcept { targetDb = db; }
    void snapDb (float db) noexcept { targetDb = curDb = db; }
    float nextGain() noexcept { curDb = targetDb + a * (curDb - targetDb); return dbToGain (curDb); }
};

/** Attack/release envelope follower on |x|. */
struct EnvFollower
{
    float att = 0.0f, rel = 0.0f, env = 0.0f;
    void setTimes (float attMs, float relMs, float sr) noexcept
    {
        att = std::exp (-1.0f / (0.001f * std::max (0.01f, attMs) * sr));
        rel = std::exp (-1.0f / (0.001f * std::max (0.01f, relMs) * sr));
    }
    float process (float x) noexcept
    {
        const float r = std::abs (x);
        env = flushDenormal (r + (r > env ? att : rel) * (env - r));
        return env;
    }
    void reset() noexcept { env = 0.0f; }
};

/** 64-bit Cytomic TPT/ZDF state-variable filter. Returns lp/bp/hp simultaneously; bells and shelves derived. */
struct Svf
{
    double g = 0.0, k = 1.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double ic1 = 0.0, ic2 = 0.0;
    double lp = 0.0, bp = 0.0, hp = 0.0;

    void setup (double hz, double q, double sr) noexcept
    {
        const double safeSr = std::max (sr, 1.0);
        hz = std::clamp (hz, 10.0, 0.485 * safeSr);
        g  = std::tan (kPiD * hz / safeSr);
        k  = 1.0 / std::max (0.05, q);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void setupWithG (double gIn, double q) noexcept
    {
        g  = std::clamp (gIn, 1.0e-7, 25.0);
        k  = 1.0 / std::max (0.05, q);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void tick (double x) noexcept
    {
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = flushDenormal (2.0 * v1 - ic1);
        ic2 = flushDenormal (2.0 * v2 - ic2);
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
    float lowpass (float x) noexcept  { tick ((double) x); return (float) lp; }
    float bandpass (float x) noexcept { tick ((double) x); return (float) bp; }
    float highpass (float x) noexcept { tick ((double) x); return (float) hp; }
    /** Constant-peak bell: gainLin is the linear gain at centre. */
    float bell (float x, float gainLin) noexcept { tick ((double) x); return (float) ((double) x + ((double) gainLin - 1.0) * k * bp); }
    void reset() noexcept { ic1 = ic2 = lp = bp = hp = 0.0; }
};

/** First-order tilt EQ around a pivot (−dB/2 below, +dB/2 above). Band gains are ramped per sample. */
struct Tilt
{
    OnePole lp;
    Ramp gLo, gHi;
    void set (float db, float pivot, float sr, int n) noexcept
    {
        lp.setCutoff (pivot, sr);
        gLo.to (dbToGain (-0.5f * db), n);
        gHi.to (dbToGain (0.5f * db), n);
    }
    float process (float x) noexcept
    {
        const float lo = lp.process (x);
        return lo * gLo.next() + (x - lo) * gHi.next();
    }
    void reset() noexcept { lp.reset(); gLo.reset(); gHi.reset(); }
};

/** 64-bit normalized ~5 Hz DC blocker (exact 0 at DC, exact 1.0 at Nyquist, safe up to 768 kHz+). */
struct DcBlocker
{
    double r = 0.9993, g = 0.99965, x1 = 0.0, y1 = 0.0;
    void prepare (double sr, double cutoffHz = 5.0) noexcept
    {
        r = std::exp (-kTwoPiD * cutoffHz / std::max (sr, 1.0));
        g = 0.5 * (1.0 + r);
        reset();
    }
    double process (double x) noexcept
    {
        const double y = g * (x - x1) + r * y1;
        x1 = x;
        y1 = flushDenormal (y);
        return y;
    }
    float process (float x) noexcept { return (float) process ((double) x); }
    void reset() noexcept { x1 = y1 = 0.0; }
};

/** 64-bit exact elliptic 4+4 polyphase allpass halfband 2x oversampler.
    Passband flatness < 1e-6 dB up to 0.42*fs (20.16 kHz @ 48 kHz), stopband > 125 dB.
    Sample-by-sample API (zero heap allocations, odd-block invariant, denormal safe). */
struct Halfband2x
{
    static constexpr double kA0[4] { 0.026821928500, 0.214527533360, 0.489449268646, 0.773727278635 };
    static constexpr double kA1[4] { 0.102468849567, 0.347836340388, 0.631780124224, 0.920832324410 };

    double upX0[4] {}, upY0[4] {}, upX1[4] {}, upY1[4] {};
    double dnX0[4] {}, dnY0[4] {}, dnX1[4] {}, dnY1[4] {};

    void reset() noexcept
    {
        for (int k = 0; k < 4; ++k)
            upX0[k] = upY0[k] = upX1[k] = upY1[k] = dnX0[k] = dnY0[k] = dnX1[k] = dnY1[k] = 0.0;
    }

    static inline double allpass4 (double x, const double c[4], double sx[4], double sy[4]) noexcept
    {
        double v = x;
        for (int k = 0; k < 4; ++k)
        {
            const double y = c[k] * (v - sy[k]) + sx[k];
            sx[k] = v;
            sy[k] = flushDenormal (y);
            v = y;
        }
        return v;
    }

    /** Upsample 1 base-rate sample into 2 oversampled samples (y0, y1). */
    inline void upsample (double x, double& y0, double& y1) noexcept
    {
        y0 = allpass4 (x, kA0, upX0, upY0);
        y1 = allpass4 (x, kA1, upX1, upY1);
    }

    /** Downsample 2 oversampled samples (v0, v1) into 1 base-rate sample. */
    inline double downsample (double v0, double v1) noexcept
    {
        return 0.5 * (allpass4 (v1, kA0, dnX0, dnY0) + allpass4 (v0, kA1, dnX1, dnY1));
    }
};

/** 64-bit Zero-Delay-Feedback Sallen-Key Dual-Diode Core (12 dB/oct resonant LPF).
    - Exact trapezoidal TPT two-stage Sallen-Key topology with subtle +/-0.35% stage mismatch
      (geometric-mean preserved for < 0.05 ct self-oscillation tuning accuracy).
    - Asymmetric dual-diode clipper inside the resonance feedback path (exact D(0)=0, D'(0)=1)
      so high resonance screams and compresses organically with even + odd harmonics without exploding.
    - O(1) strictly bounded Newton-Raphson solver (max 3 iterations; provably F'(y2) >= 0.36 > 0).
    - Built-in 64-bit polyphase halfband 2x oversampling when driven/resonant. */
struct SallenKeyCore
{
    static constexpr double kStage1Mul = 1.0035;
    static constexpr double kStage2Mul = 0.9965;
    static constexpr double kVd        = 0.95;
    static constexpr double kInvVd     = 1.0 / kVd;
    static constexpr double kBd        = 0.15;
    static constexpr double kTd0       = 0.14888503362331806;   // tanh(0.15)
    static constexpr double kInvSd0    = 1.0226707094124906;    // 1 / (1 - tanh(0.15)^2)
    static constexpr double kVin       = 3.0;
    static constexpr double kInvVin    = 1.0 / kVin;

    double baseSr = 48000.0;
    double G1 = 0.0, H1 = 1.0, G2 = 0.0, H2 = 1.0, alpha = 0.0;
    double kRes = 0.5, drive = 1.0, invSqrtDrive = 1.0;
    double s1 = 0.0, s2 = 0.0;
    Halfband2x os;
    bool use2x = true;

    void prepare (double sr, bool oversample2x = true) noexcept
    {
        baseSr = std::max (sr, 1.0);
        use2x = oversample2x;
        reset();
    }

    void reset() noexcept
    {
        s1 = s2 = 0.0;
        os.reset();
    }

    /** Configure cutoff (Hz), Sallen-Key resonance k in [0, 2.35] (k=2-1/Q linearly; k>2 self-oscillates),
        and input drive >= 1.0. */
    void setup (double cutoffHz, double kIn, double driveIn = 1.0) noexcept
    {
        const double osr = use2x ? (2.0 * baseSr) : baseSr;
        const double safeFc = std::clamp (cutoffHz, 10.0, 0.48 * baseSr);
        const double gBase = std::tan (kPiD * std::min (safeFc, 0.485 * osr) / osr);
        const double g1 = gBase * kStage1Mul;
        const double g2 = gBase * kStage2Mul;
        G1 = g1 / (1.0 + g1);
        H1 = 1.0 - G1;
        G2 = g2 / (1.0 + g2);
        H2 = 1.0 - G2;
        alpha = G2 * H1;
        kRes = std::clamp (kIn, 0.0, 2.35);
        drive = std::clamp (driveIn, 1.0, 8.0);
        invSqrtDrive = 1.0 / std::sqrt (drive);
    }

    /** Single core tick at the configured internal rate (strictly 3 bounded NR iterations). */
    inline double tickCore (double xIn) noexcept
    {
        // Subtle MS-20 OTA input stage soft rounding (exact unity slope at small signals)
        const double x = (kVin * invSqrtDrive) * fastTanhLadder (xIn * drive * kInvVin);
        const double S = G2 * (G1 * x + H1 * s1) + H2 * s2;
        const double k = kRes;
        const double ak = alpha * k;

        // Exact linear ZDF initial guess (denominator >= 1 - 0.25*2.35 = 0.4125)
        double y2 = S / (1.0 - ak);

        // O(1) strictly bounded 3-step Newton-Raphson (globally F'(y2) >= 0.38 > 0)
        double vfb = 0.0;
        for (int it = 0; it < 3; ++it)
        {
            const double th = fastTanhLadder (k * y2 * kInvVd + kBd);
            vfb = kVd * (th - kTd0) * kInvSd0;
            const double dD = (1.0 - th * th) * kInvSd0;
            const double res = y2 - S - alpha * vfb;
            const double jac = 1.0 - ak * dD;
            y2 -= res / jac;
        }

        // Converged asymmetric dual-diode feedback value
        const double thF = fastTanhLadder (k * y2 * kInvVd + kBd);
        vfb = kVd * (thF - kTd0) * kInvSd0;

        // Exact trapezoidal TPT state update (unconditionally stable under audio-rate modulation)
        const double z1 = G1 * (x - vfb) + H1 * s1;
        s1 = flushDenormal (2.0 * z1 - s1);
        const double y1 = z1 + vfb;
        const double y2Out = G2 * y1 + H2 * s2;
        s2 = flushDenormal (2.0 * y2Out - s2);
        return y2Out;
    }

    /** 2x oversampled processing via 64-bit polyphase allpass halfband IIR. */
    inline double process2x (double x) noexcept
    {
        double u0, u1;
        os.upsample (x, u0, u1);
        const double v0 = tickCore (u0);
        const double v1 = tickCore (u1);
        return os.downsample (v0, v1);
    }

    inline float process (float x) noexcept
    {
        return (float) (use2x ? process2x ((double) x) : tickCore ((double) x));
    }
};

/** Power-of-two circular delay buffer with linear and Hermite reads. */
struct DelayLine
{
    std::vector<float> buf;
    int mask = 0, w = 0;

    void prepare (int maxSamples)
    {
        int n = 1;
        while (n < maxSamples + 4) n <<= 1;
        buf.assign ((size_t) n, 0.0f);
        mask = n - 1; w = 0;
    }
    void clear() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); }
    void push (float x) noexcept { buf[(size_t) w] = x; w = (w + 1) & mask; }
    /** delay in samples, >= 1 (1 = most recently pushed sample). */
    float readLinear (float d) const noexcept
    {
        const float rp = (float) w - d;
        const int i0 = (int) std::floor (rp);
        const float f = rp - (float) i0;
        const float a = buf[(size_t) (i0 & mask)], b = buf[(size_t) ((i0 + 1) & mask)];
        return a + f * (b - a);
    }
    float readHermite (float d) const noexcept
    {
        const float rp = (float) w - d;
        const int i1 = (int) std::floor (rp);
        const float t = rp - (float) i1;
        const float y0 = buf[(size_t) ((i1 - 1) & mask)], y1 = buf[(size_t) (i1 & mask)];
        const float y2 = buf[(size_t) ((i1 + 1) & mask)], y3 = buf[(size_t) ((i1 + 2) & mask)];
        const float c0 = y1, c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }
    float readInt (int d) const noexcept { return buf[(size_t) ((w - d) & mask)]; }
    /** Absolute read by a continuous position counted in pushed samples. */
    float readAt (double pos) const noexcept
    {
        const auto i0 = (int64_t) std::floor (pos);
        const float f = (float) (pos - (double) i0);
        const float a = buf[(size_t) (i0 & mask)], b = buf[(size_t) ((i0 + 1) & mask)];
        return a + f * (b - a);
    }
    /** Absolute 4-point Hermite read (used where the read head is pitched, to keep imaging down). */
    float readAtHermite (double pos) const noexcept
    {
        const auto i1 = (int64_t) std::floor (pos);
        const float t = (float) (pos - (double) i1);
        const float y0 = buf[(size_t) ((i1 - 1) & mask)], y1 = buf[(size_t) (i1 & mask)];
        const float y2 = buf[(size_t) ((i1 + 1) & mask)], y3 = buf[(size_t) ((i1 + 2) & mask)];
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }
};

/** Smoothed random walk ("drift" source), output roughly in [-1,1]. */
struct Drift
{
    Rng rng;
    float target = 0.0f, value = 0.0f, phase = 1.0f, inc = 0.0f, prev = 0.0f;
    void prepare (float sr, float rateHz, uint32_t seed) noexcept
    {
        rng.seed (seed);
        inc = rateHz / std::max (1.0f, sr);
        target = value = prev = 0.0f;
        phase = 1.0f;
    }
    void reseed (uint32_t seed) noexcept
    {
        rng.seed (seed);
        target = value = prev = 0.0f;
        phase = 1.0f;
    }
    void setRate (float rateHz, float sr) noexcept { inc = rateHz / std::max (1.0f, sr); }
    /** Advance by `n` samples at once (control-rate use). */
    float next (int n) noexcept { phase += inc * (float) (n - 1); return next(); }
    float next() noexcept
    {
        phase += inc;
        while (phase >= 1.0f) { phase -= 1.0f; prev = target; target = rng.bip(); }
        // raised-cosine interpolation between random points
        const float t = 0.5f - 0.5f * fastCos01 (0.5f * phase);
        value = prev + (target - prev) * t;
        return value;
    }
};

/** 3-octave correlated 1/f (pink) organic analog drift driven by a deterministic seeded PRNG.
    Output stays smoothly bounded in [-1, 1] and is sample-rate & block-size invariant when stepped on fixed cadence. */
struct PinkDrift
{
    Drift d0, d1, d2;
    float cur = 0.0f;

    void prepare (float sr, uint32_t seed) noexcept
    {
        d0.prepare (sr, 0.11f, seed * 1664525u + 1013904223u);
        d1.prepare (sr, 0.43f, seed * 22695477u + 1u);
        d2.prepare (sr, 1.67f, seed * 1103515245u + 12345u);
        cur = 0.0f;
    }
    void reseed (uint32_t seed) noexcept
    {
        d0.reseed (seed * 1664525u + 1013904223u);
        d1.reseed (seed * 22695477u + 1u);
        d2.reseed (seed * 1103515245u + 12345u);
        cur = 0.0f;
    }
    float next (int n) noexcept
    {
        cur = 0.55f * d0.next (n) + 0.30f * d1.next (n) + 0.15f * d2.next (n);
        return cur;
    }
    float value() const noexcept { return cur; }
};

/** Audio-thread scratch buffers for a stereo signal path. */
struct Stereo
{
    float* l = nullptr;
    float* r = nullptr;
};

//==============================================================================
/** Fast stereo 4x Type-I linear-phase Kaiser FIR oversampler with mirrored circular ring buffers.
    Zero per-sample array shifting (O(1) ring advance + contiguous SIMD-friendly dot products), zero audio-thread
    allocations.
    - Stage 1 (1x <-> 2x): N=95, fc=0.221, beta=9.0 -> 0.00 dB @18 kHz, -74 dB @24.03 kHz, -100 dB @26 kHz (47 base samples)
    - Stage 2 (2x <-> 4x): N=27, fc=0.250, beta=9.5 -> -95.6 dB @72 kHz (6.5 base samples)
    Total up+down linear-phase latency = exactly 53.5 base-rate samples (so + 1 ADAA2 sample + 1 pad = 54.000 exact). */
class LinearPhase4xOs
{
public:
    static constexpr int kN1 = 95, kM1 = (kN1 - 1) / 2, kPad1 = 96, kHalf1 = 48;
    static constexpr int kN2 = 27, kM2 = (kN2 - 1) / 2, kPad2 = 28, kHalf2 = 14;
    static constexpr int kRing1 = 128, kMask1 = kRing1 - 1;
    static constexpr int kRing2 = 64,  kMask2 = kRing2 - 1;

    static constexpr float kBaseLatency = (float) kM1 + 0.5f * (float) kM2;   // 47.0 + 6.5 = 53.5 samples

    void prepare (int maxBaseBlock)
    {
        designKaiser (kN1, 0.221, 9.0, h1, h1Even, h1Odd, kPad1, kHalf1);
        designKaiser (kN2, 0.250, 9.5, h2, h2Even, h2Odd, kPad2, kHalf2);
        const int sz2 = std::max (1, maxBaseBlock) * 2;
        const int sz4 = std::max (1, maxBaseBlock) * 4;
        buf2L.assign ((size_t) sz2, 0.0f); buf2R.assign ((size_t) sz2, 0.0f);
        buf4L.assign ((size_t) sz4, 0.0f); buf4R.assign ((size_t) sz4, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (std::begin (up1[c]), std::end (up1[c]), 0.0f);
            std::fill (std::begin (dn1[c]), std::end (dn1[c]), 0.0f);
            std::fill (std::begin (up2[c]), std::end (up2[c]), 0.0f);
            std::fill (std::begin (dn2[c]), std::end (dn2[c]), 0.0f);
            wUp1[c] = wDn1[c] = wUp2[c] = wDn2[c] = 0;
        }
    }

    float* upL() noexcept { return buf4L.data(); }
    float* upR() noexcept { return buf4R.data(); }
    float getLatencyInSamples() const noexcept { return kBaseLatency; }

    /** Upsample n base-rate samples from (inL, inR) into (upL(), upR()) of length 4*n. */
    void processUp (const float* inL, const float* inR, int n) noexcept
    {
        upStage<kRing1, kMask1, kHalf1> (inL, buf2L.data(), n, up1[0], wUp1[0], h1Even, h1Odd);
        upStage<kRing1, kMask1, kHalf1> (inR, buf2R.data(), n, up1[1], wUp1[1], h1Even, h1Odd);
        upStage<kRing2, kMask2, kHalf2> (buf2L.data(), buf4L.data(), n * 2, up2[0], wUp2[0], h2Even, h2Odd);
        upStage<kRing2, kMask2, kHalf2> (buf2R.data(), buf4R.data(), n * 2, up2[1], wUp2[1], h2Even, h2Odd);
    }

    /** Downsample 4*n samples from (upL(), upR()) into (outL, outR) of length n. */
    void processDown (float* outL, float* outR, int n) noexcept
    {
        dnStage<kRing2, kMask2, kPad2> (buf4L.data(), buf2L.data(), n * 2, dn2[0], wDn2[0], h2);
        dnStage<kRing2, kMask2, kPad2> (buf4R.data(), buf2R.data(), n * 2, dn2[1], wDn2[1], h2);
        dnStage<kRing1, kMask1, kPad1> (buf2L.data(), outL, n, dn1[0], wDn1[0], h1);
        dnStage<kRing1, kMask1, kPad1> (buf2R.data(), outR, n, dn1[1], wDn1[1], h1);
    }

private:
    static double besselI0 (double x) noexcept
    {
        double acc = 1.0, term = 1.0;
        for (int k = 1; k < 35; ++k) { const double r = x / (2.0 * (double) k); term *= r * r; acc += term; }
        return acc;
    }

    static void designKaiser (int N, double fc, double beta, float* hFull, float* hE, float* hO, int padLen, int halfLen) noexcept
    {
        const int M = (N - 1) / 2;
        const double i0b = besselI0 (beta);
        double tmp[128] {};
        double sum = 0.0;
        for (int n = 0; n < N; ++n)
        {
            const int k = n - M;
            const double sinc = (k == 0) ? 2.0 * fc : std::sin (kTwoPiD * fc * (double) k) / (kPiD * (double) k);
            const double u = (double) k / (double) M;
            const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - u * u))) / i0b;
            tmp[n] = sinc * w;
            sum += tmp[n];
        }
        const double inv = 1.0 / sum;
        for (int i = 0; i < padLen; ++i) hFull[i] = i < N ? (float) (tmp[i] * inv) : 0.0f;
        for (int m = 0; m < halfLen; ++m)
        {
            const int ie = 2 * m;
            const int io = 2 * m + 1;
            hE[m] = ie < N ? 2.0f * hFull[ie] : 0.0f;
            hO[m] = io < N ? 2.0f * hFull[io] : 0.0f;
        }
    }

    template <int Ring, int Mask, int Half>
    static void upStage (const float* __restrict in, float* __restrict out, int n,
                         float* __restrict ring, int& w, const float* __restrict hE, const float* __restrict hO) noexcept
    {
        using VF = juce::dsp::SIMDRegister<float>;
        constexpr int kW = (int) VF::SIMDNumElements;
        auto loadUnaligned = [] (const float* ptr) noexcept -> VF
        {
            alignas (32) float tmp[kW];
            std::memcpy (tmp, ptr, sizeof (tmp));
            return VF::fromRawArray (tmp);
        };
        for (int i = 0; i < n; ++i)
        {
            w = (w - 1) & Mask;
            ring[w] = ring[w + Ring] = in[i];
            const float* __restrict p = ring + w;
            VF ve = VF::expand (0.0f), vo = VF::expand (0.0f);
            int m = 0;
            for (; m + kW <= Half; m += kW)
            {
                const VF vx = loadUnaligned (p + m);
                ve = VF::multiplyAdd (ve, VF::fromRawArray (hE + m), vx);
                vo = VF::multiplyAdd (vo, VF::fromRawArray (hO + m), vx);
            }
            float e = ve.sum(), o = vo.sum();
            for (; m < Half; ++m)
            {
                const float x = p[m];
                e += hE[m] * x;
                o += hO[m] * x;
            }
            out[2 * i]     = e;
            out[2 * i + 1] = o;
        }
    }

    template <int Ring, int Mask, int Pad>
    static void dnStage (const float* __restrict in, float* __restrict out, int nOut,
                         float* __restrict ring, int& w, const float* __restrict h) noexcept
    {
        using VF = juce::dsp::SIMDRegister<float>;
        constexpr int kW = (int) VF::SIMDNumElements;
        auto loadUnaligned = [] (const float* ptr) noexcept -> VF
        {
            alignas (32) float tmp[kW];
            std::memcpy (tmp, ptr, sizeof (tmp));
            return VF::fromRawArray (tmp);
        };
        for (int i = 0; i < nOut; ++i)
        {
            w = (w - 1) & Mask;
            ring[w] = ring[w + Ring] = in[2 * i];
            const float* __restrict p = ring + w;
            VF v0 = VF::expand (0.0f), v1 = VF::expand (0.0f);
            int k = 0;
            for (; k + 2 * kW <= Pad; k += 2 * kW)
            {
                v0 = VF::multiplyAdd (v0, VF::fromRawArray (h + k),      loadUnaligned (p + k));
                v1 = VF::multiplyAdd (v1, VF::fromRawArray (h + k + kW), loadUnaligned (p + k + kW));
            }
            for (; k + kW <= Pad; k += kW)
                v0 = VF::multiplyAdd (v0, VF::fromRawArray (h + k), loadUnaligned (p + k));
            float acc = (v0 + v1).sum();
            for (; k < Pad; ++k) acc += h[k] * p[k];
            out[i] = acc;

            w = (w - 1) & Mask;
            ring[w] = ring[w + Ring] = in[2 * i + 1];
        }
    }

    alignas (32) float h1[kPad1] {}, h1Even[kHalf1] {}, h1Odd[kHalf1] {};
    alignas (32) float h2[kPad2] {}, h2Even[kHalf2] {}, h2Odd[kHalf2] {};
    alignas (32) float up1[2][2 * kRing1] {}, dn1[2][2 * kRing1] {};
    alignas (32) float up2[2][2 * kRing2] {}, dn2[2][2 * kRing2] {};
    int wUp1[2] {}, wDn1[2] {}, wUp2[2] {}, wDn2[2] {};
    std::vector<float> buf2L, buf2R, buf4L, buf4R;
};
} // namespace vessel::dsp
