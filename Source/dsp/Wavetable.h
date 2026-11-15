#pragma once

#include "Common.h"

namespace vessel::dsp
{
/** Band-limited 2D wavetable bank: N morph frames x 11 octave mip-levels x 2048 samples, read-only once built.
    The built-in table (8 frames, from additive recipes) is shared by every oscillator; imported tables are built
    from raw single-cycle frames off the audio thread (see Tables.h). */
class WavetableBank
{
public:
    static constexpr int kSize   = 2048;
    static constexpr int kFrames = 8;    // the built-in table
    static constexpr int kMips   = 11;

    static const WavetableBank& get();

    /** A table from raw frames (numFrames x kSize samples, back to back). Each frame loses its DC and is
        band-limited into the mip levels by FFT; the table as a whole is scaled to the built-in 0.9 peak, so
        level changes from frame to frame survive. Not for the audio thread (allocates, ~0.2 ms per frame). */
    static std::unique_ptr<WavetableBank> fromFrames (const float* frames, int numFrames);

    int frames() const noexcept { return nFrames; }

    /** Choose the mip level that keeps every harmonic below Nyquist at this frequency. */
    static int mipFor (float hz, float sr) noexcept
    {
        const float ratio = 2.0f * (float) kSize * 0.5f * hz / sr;   // = 2048 * hz / sr
        if (ratio <= 1.0f) return 0;
        return std::clamp ((int) std::ceil (std::log2 (ratio)), 0, kMips - 1);
    }

    float read (int frame, int mip, float phase) const noexcept
    {
        frame = std::clamp (frame, 0, nFrames - 1);
        mip   = std::clamp (mip, 0, kMips - 1);
        float p = phase - std::floor (phase);
        if (! (p >= 0.0f && p < 1.0f)) p = 0.0f;
        const float* t = table (frame, mip);
        const float x = p * (float) kSize;
        const int i = std::min ((int) x, kSize - 1);
        const float f = x - (float) i;
        return t[i] + f * (t[i + 1] - t[i]);
    }

    /** Continuous morph position 0..1 across all frames (neighbouring frames are crossfaded). */
    float readMorph (float morph, int mip, float phase) const noexcept
    {
        if (nFrames < 2) return read (0, mip, phase);
        const float pos = clamp01 (morph) * (float) (nFrames - 1);
        const int a = std::min ((int) pos, nFrames - 2);
        const float f = pos - (float) a;
        return lerp (read (a, mip, phase), read (a + 1, mip, phase), f);
    }

    /** One frame's band-limited cycle at one mip level (kSize + 1 samples, the last a guard copy of the first). */
    const float* table (int frame, int mip) const noexcept { return data.data() + (size_t) ((frame * kMips + mip) * (kSize + 1)); }

private:
    WavetableBank();                      // the built-in table
    explicit WavetableBank (int frames);  // zeroed storage for an imported one
    int nFrames = kFrames;
    float* table (int frame, int mip) noexcept { return data.data() + (size_t) ((frame * kMips + mip) * (kSize + 1)); }
    std::vector<float> data;
};

/** Morphing wavetable oscillator for the 4x-oversampled voice, with phase-warp PWM.

    Band-limiting, three layers:
      1. Mip selection accounts for the warp: a narrow duty compresses one half-cycle, multiplying its top partial.
      2. Mip crossfade: two adjacent mips are blended by fractional pitch, so the top octave of partials fades
         continuously during glides instead of switching (no brightness steps). Partials above the base-rate
         Nyquist only ever exist in the 4x domain and are removed by the voice decimator.  
      3. polyBLAMP at the two warp knees: the piecewise-linear phase map has a slope break at phase = pw and at
         the wrap, which puts a derivative discontinuity into the output. A 2-point band-limited ramp residual
         cancels its 1/n² alias tail.                                                                           */
struct WavetableOsc
{
    float phase = 0.0f;
    const WavetableBank* bank = &WavetableBank::get();   // built on the message thread when the voice is constructed

    /** Mip pair for a pitch: blend (mip - 1) with weight wLower, mip with (1 - wLower). */
    static void mipBlend (float hz, float baseSr, int& mip, float& wLower) noexcept
    {
        const float ratio = 2.0f * (float) WavetableBank::kSize * 0.5f * hz / baseSr;
        if (ratio <= 1.0f) { mip = 0; wLower = 0.0f; return; }
        mip = std::clamp ((int) std::ceil (std::log2 (ratio)), 1, WavetableBank::kMips - 1);
        const float r = ratio / std::exp2 ((float) (mip - 1));   // (1, 2] inside the octave
        wLower = std::clamp (2.0f - r, 0.0f, 1.0f);
    }

    float table (float p, float morph, int mip, float wLower) const noexcept
    {
        float y = bank->readMorph (morph, mip, p);
        if (wLower > 0.0f) y += wLower * (bank->readMorph (morph, mip - 1, p) - y);
        return y;
    }

    /** dt = cycles per (oversampled) sample; pm = phase modulation in cycles (FM), added after the warp. */
    float process (float dt, int mip, float wLower, float morph, float pw, float pm = 0.0f) noexcept
    {
        dt = std::clamp (dt, 1.0e-7f, 0.48f);
        pw = std::clamp (pw, 0.03f, 0.97f);
        const float a1 = 0.5f / pw, a2 = 0.5f / (1.0f - pw);   // phase-map slopes either side of the knee
        float p = phase < pw ? phase * a1 : 0.5f + (phase - pw) * a2;
        if (pm != 0.0f) { p += pm; p -= std::floor (p); if (! (p >= 0.0f && p < 1.0f)) p = 0.0f; }
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
        phase -= std::floor (phase);
        if (! (phase >= 0.0f && phase < 1.0f)) phase = 0.0f;
        return y;
    }
};
} // namespace vessel::dsp
