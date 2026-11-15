#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vessel::dsp
{
constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

inline float dbToGain (float db) noexcept   { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept    { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }
inline float clamp01 (float x) noexcept     { return std::clamp (x, 0.0f, 1.0f); }
inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }
inline float semis (float st) noexcept      { return std::exp2 (st / 12.0f); }
inline float midiToHz (float note) noexcept { return 440.0f * std::exp2 ((note - 69.0f) / 12.0f); }

/** Rational tanh: exact at ±3 (=±1), C1-continuous, cheap. */
inline float fastTanh (float x) noexcept
{
    x = std::clamp (x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float flushDenormal (float x) noexcept { return std::abs (x) < 1.0e-15f ? 0.0f : x; }

/** tan(x) for |x| <= ~1.45 (Padé [5/4]); filter prewarping only. Max relative error 3e-5 at 1.4 rad. */
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

/** Parameter smoother (exponential, ~time constant). */
struct Smooth
{
    float cur = 0.0f, target = 0.0f, a = 0.0f;
    void prepare (float sr, float ms = 20.0f) noexcept { a = std::exp (-1.0f / (0.001f * ms * sr)); }
    void set (float t) noexcept { target = t; }
    void snap (float t) noexcept { target = cur = t; }
    float next() noexcept { cur = target + a * (cur - target); return cur; }
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

/** Cytomic TPT state-variable filter. Returns lp/bp/hp simultaneously; bells and shelves derived. */
struct Svf
{
    float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    float ic1 = 0.0f, ic2 = 0.0f;
    float lp = 0.0f, bp = 0.0f, hp = 0.0f;

    void setup (float hz, float q, float sr) noexcept
    {
        hz = std::clamp (hz, 5.0f, 0.48f * sr);
        g  = std::tan (kPi * hz / sr);
        k  = 1.0f / std::max (0.05f, q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void setupWithG (float gIn, float q) noexcept
    {
        g = gIn; k = 1.0f / std::max (0.05f, q);
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    void tick (float x) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = flushDenormal (2.0f * v1 - ic1);
        ic2 = flushDenormal (2.0f * v2 - ic2);
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
    float lowpass (float x) noexcept  { tick (x); return lp; }
    float bandpass (float x) noexcept { tick (x); return bp; }
    float highpass (float x) noexcept { tick (x); return hp; }
    /** Constant-peak bell: gainLin is the linear gain at centre. */
    float bell (float x, float gainLin) noexcept { tick (x); return x + (gainLin - 1.0f) * k * bp; }
    void reset() noexcept { ic1 = ic2 = 0.0f; }
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

struct DcBlocker
{
    float r = 0.995f, x1 = 0.0f, y1 = 0.0f;
    void prepare (float sr) noexcept { r = std::exp (-kTwoPi * 8.0f / sr); reset(); }
    float process (float x) noexcept { const float y = x - x1 + r * y1; x1 = x; y1 = flushDenormal (y); return y; }
    void reset() noexcept { x1 = y1 = 0.0f; }
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
        rng.seed (seed); inc = rateHz / sr;
    }
    void setRate (float rateHz, float sr) noexcept { inc = rateHz / sr; }
    /** Advance by `n` samples at once (control-rate use). */
    float next (int n) noexcept { phase += inc * (float) (n - 1); return next(); }
    float next() noexcept
    {
        phase += inc;
        if (phase >= 1.0f) { phase -= 1.0f; prev = target; target = rng.bip(); }
        // raised-cosine interpolation between random points
        const float t = 0.5f - 0.5f * std::cos (kPi * phase);
        value = prev + (target - prev) * t;
        return value;
    }
};

/** Audio-thread scratch buffers for a stereo signal path. */
struct Stereo
{
    float* l = nullptr;
    float* r = nullptr;
};
} // namespace vessel::dsp
