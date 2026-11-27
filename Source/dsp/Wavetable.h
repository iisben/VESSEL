#pragma once

#include "Common.h"

namespace vessel::dsp
{
/** Band-limited 2D wavetable bank: N morph frames x 31 mip-levels (3 tables/octave) x 2048 samples with
    4-point cubic Hermite interpolation, read-only once built.
    The built-in table (8 frames, from additive recipes) is shared by every oscillator; imported tables are built
    from raw single-cycle frames off the audio thread (see Tables.h). */
class WavetableBank
{
public:
    static constexpr int kSize       = 2048;
    static constexpr int kFrames     = 8;    // the built-in table
    static constexpr int kMipsPerOct = 3;    // 3 mip tables per octave (Serum 2 / Zebra 3 grade)
    static constexpr int kMips       = 31;   // 10 octaves * 3 + 1
    static constexpr int kStride     = kSize + 3;   // [-1], [0..kSize-1], [kSize], [kSize+1]

    static const WavetableBank& get();

    /** A table from raw frames (numFrames x kSize samples, back to back). Each frame loses its DC and is
        band-limited into 3 mip levels per octave by FFT; the table as a whole is scaled to the built-in 0.9 peak.
        Not for the audio thread (allocates). */
    static std::unique_ptr<WavetableBank> fromFrames (const float* frames, int numFrames);

    int frames() const noexcept { return nFrames; }

    /** Highest harmonic kept at mip level m (3 tables/octave). */
    static int maxHarmonicForMip (int mip) noexcept
    {
        const double h = (double) (kSize / 2) * std::exp2 (-(double) mip / (double) kMipsPerOct);
        return std::clamp ((int) std::floor (h), 1, kSize / 2 - 1);
    }

    /** Choose the mip level that keeps every harmonic below Nyquist at this frequency. */
    static int mipFor (float hz, float sr) noexcept
    {
        const float ratio = (float) kSize * hz / std::max (sr, 1.0f);
        if (ratio <= 1.0f) return 0;
        const float fMip = (float) kMipsPerOct * std::log2 (ratio);
        return std::clamp ((int) std::ceil (fMip), 0, kMips - 1);
    }

    /** 4-point cubic Hermite (Catmull-Rom) lookup with bidirectional [0, 1) phase wrapping. */
    float read (int frame, int mip, double phase) const noexcept
    {
        frame = std::clamp (frame, 0, nFrames - 1);
        mip   = std::clamp (mip, 0, kMips - 1);
        const double p = wrapPhase01 (phase);
        const float* t = table (frame, mip);
        const double x = p * (double) kSize;
        const int i = std::min ((int) x, kSize - 1);
        const float f = (float) (x - (double) i);
        const float y0 = t[i - 1], y1 = t[i], y2 = t[i + 1], y3 = t[i + 2];
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }

    float read (int frame, int mip, float phase) const noexcept
    {
        return read (frame, mip, (double) phase);
    }

    /** Continuous morph position 0..1 across all frames (smooth cross-table morphing). */
    float readMorph (float morph, int mip, double phase) const noexcept
    {
        if (nFrames < 2) return read (0, mip, phase);
        const float pos = clamp01 (morph) * (float) (nFrames - 1);
        const int a = std::min ((int) pos, nFrames - 2);
        const float f = pos - (float) a;
        return lerp (read (a, mip, phase), read (a + 1, mip, phase), f);
    }

    float readMorph (float morph, int mip, float phase) const noexcept
    {
        return readMorph (morph, mip, (double) phase);
    }

    /** One frame's band-limited cycle at one mip level.
        Valid indices: [-1 .. kSize + 1] (guards wrap cyclically for branchless 4-point Hermite reads). */
    const float* table (int frame, int mip) const noexcept
    {
        return data.data() + (size_t) ((frame * kMips + mip) * kStride + 1);
    }

private:
    WavetableBank();                      // the built-in table
    explicit WavetableBank (int frames);  // zeroed storage for an imported one
    int nFrames = kFrames;
    float* table (int frame, int mip) noexcept
    {
        return data.data() + (size_t) ((frame * kMips + mip) * kStride + 1);
    }
    std::vector<float> data;
};

/** 4-point cubic B-spline PolyBLEP residual on t in (-2, 2) (antisymmetric, O(1/f^4) sidelobes). */
inline float polyBlep4 (float t) noexcept
{
    const float a = std::abs (t);
    if (a >= 2.0f) return 0.0f;
    float r;
    if (a < 1.0f)
        r = -0.5f + a * ((2.0f / 3.0f) + a * a * ((-1.0f / 3.0f) + 0.125f * a));
    else
    {
        const float u = 2.0f - a;
        const float u2 = u * u;
        r = -(1.0f / 24.0f) * u2 * u2;
    }
    return t >= 0.0f ? r : -r;
}

/** 4-point cubic B-spline PolyBLAMP residual on t in (-2, 2) (symmetric, C^2 corner rounding, O(1/f^5)). */
inline float polyBlamp4 (float t) noexcept
{
    const float a = std::abs (t);
    if (a >= 2.0f) return 0.0f;
    if (a < 1.0f)
    {
        const float a2 = a * a;
        return (7.0f / 30.0f) - 0.5f * a + (1.0f / 3.0f) * a2 - (1.0f / 12.0f) * a2 * a2 + (1.0f / 40.0f) * a2 * a2 * a;
    }
    const float u = 2.0f - a;
    const float u2 = u * u;
    return (1.0f / 120.0f) * u2 * u2 * u;
}

/** Morphing wavetable oscillator for the 4x-oversampled voice, with phase-warp PWM and Through-Zero FM (TZFM).

    Band-limiting, three layers:
      1. 3 tables/octave FFT mip-maps with cubic Hermite sample interpolation and smooth cross-mip blending.
      2. 4-point PolyBLAMP at the two PWM warp knees (works symmetrically in both directions for TZFM).
      3. Double-precision carrier phase with bidirectional [0, 1) phase wrapping for Through-Zero FM. */
struct WavetableOsc
{
    double phase = 0.0;
    const WavetableBank* bank = &WavetableBank::get();   // built on the message thread when the voice is constructed

    /** Mip pair for a pitch (3 tables/octave): blend (mip - 1) with weight wLower, mip with (1 - wLower). */
    static void mipBlend (float hz, float baseSr, int& mip, float& wLower) noexcept
    {
        const float ratio = (float) WavetableBank::kSize * hz / std::max (baseSr, 1.0f);
        if (ratio <= 1.0f) { mip = 0; wLower = 0.0f; return; }
        const float fMip = (float) WavetableBank::kMipsPerOct * std::log2 (ratio);
        mip = std::clamp ((int) std::ceil (fMip), 1, WavetableBank::kMips - 1);
        wLower = std::clamp ((float) mip - fMip, 0.0f, 1.0f);
    }

    float table (double p, float morph, int mip, float wLower) const noexcept
    {
        float y = bank->readMorph (morph, mip, p);
        if (wLower > 0.0f) y += wLower * (bank->readMorph (morph, mip - 1, p) - y);
        return y;
    }

    /** dt = carrier cycles per oversampled sample;
        tzfm = signed through-zero instantaneous phase delta in cycles/sample (may drive dt + tzfm < 0);
        pm = optional post-warp phase offset in cycles. */
    float process (double dt, int mip, float wLower, float morph, float pw, double tzfm = 0.0, double pm = 0.0) noexcept
    {
        dt = std::clamp (dt, 1.0e-7, 0.48);
        pw = std::clamp (pw, 0.03f, 0.97f);

        // Apply Through-Zero FM step before reading so constant-beta TZFM matches exact analytical Bessel phase
        if (tzfm != 0.0)
            phase = wrapPhase01 (phase + tzfm);

        const float phF = (float) phase;
        const float a1 = 0.5f / pw, a2 = 0.5f / (1.0f - pw);   // phase-map slopes either side of the knee
        double p = phF < pw ? phase * (double) a1 : 0.5 + (phase - (double) pw) * (double) a2;
        if (pm != 0.0) p = wrapPhase01 (p + pm);
        float y = table (p, morph, mip, wLower);

        // 4-point PolyBLAMP at PWM warp knees (invariant to phase-direction reversal under TZFM)
        const float dtAbs = (float) std::clamp (std::abs (dt + tzfm), 1.0e-7, 0.48);
        if (std::abs (a1 - a2) > 1.0e-3f)
        {
            const double pmOff = pm;
            auto slope = [&] (double at)   // dT/dp at the knee, central difference
            {
                constexpr double h = 1.0 / (double) WavetableBank::kSize;
                const double center = pmOff != 0.0 ? wrapPhase01 (at + pmOff) : at;
                return (table (center + h, morph, mip, wLower) - table (center - h, morph, mip, wLower)) * (0.5f * (float) WavetableBank::kSize);
            };
            const float win = 2.0f * dtAbs;
            const float invDt = 1.0f / dtAbs;
            const float dk = phF - pw;   // knee: slope a1 -> a2
            if (std::abs (dk) < win)
                y += slope (0.5) * dtAbs * (a2 - a1) * polyBlamp4 (dk * invDt);
            if (phF < win)
                y += slope (0.0) * dtAbs * (a1 - a2) * polyBlamp4 (phF * invDt);
            else if (phF > 1.0f - win)
                y += slope (0.0) * dtAbs * (a1 - a2) * polyBlamp4 ((phF - 1.0f) * invDt);
        }

        phase = wrapPhase01 (phase + dt);
        return y;
    }
};
} // namespace vessel::dsp
