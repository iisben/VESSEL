#include "Wavetable.h"

namespace vessel::dsp
{
const WavetableBank& WavetableBank::get()
{
    static const WavetableBank bank;
    return bank;
}

namespace
{
/** Harmonic h of frame f: amplitude and phase (radians, applied to a sine). */
void harmonic (int frame, int h, float& amp, float& ph)
{
    const float hf = (float) h;
    ph = 0.0f;
    switch (frame)
    {
        case 0: amp = h == 1 ? 1.0f : 0.0f; break;                                         // sine
        case 1: amp = (h & 1) ? ((((h - 1) / 2) & 1) ? -1.0f : 1.0f) / (hf * hf) : 0.0f; break; // triangle
        case 2: amp = 1.0f / hf; break;                                                     // saw
        case 3: amp = (h & 1) ? 1.0f / hf : 0.0f; break;                                    // square
        case 4: amp = (h % 3 == 0) ? 0.0f : 1.0f / std::pow (hf, 1.2f); break;              // hollow
        case 5:                                                                              // vox (two formant humps)
        {
            auto bump = [hf] (float c, float w) { const float d = (hf - c) / w; return std::exp (-d * d); };
            amp = (1.0f / hf) * (0.6f + 3.0f * bump (4.0f, 1.6f) + 2.2f * bump (11.0f, 3.0f));
            break;
        }
        case 6:                                                                              // 18% pulse (cosine series)
            amp = std::sin (kPi * hf * 0.18f) / hf;
            ph  = 0.5f * kPi;
            break;
        case 7:                                                                              // grit: saw, scrambled phases, lifted mids
        default:
        {
            Rng r ((uint32_t) h * 2654435761u);
            amp = h <= 48 ? 1.0f / std::pow (hf, 0.75f) : 1.0f / hf;
            ph  = r.uni() * kTwoPi;
            break;
        }
    }
}
} // namespace

WavetableBank::WavetableBank (int frames) : nFrames (std::max (1, frames))
{
    data.assign ((size_t) (nFrames * kMips * (kSize + 1)), 0.0f);
}

std::unique_ptr<WavetableBank> WavetableBank::fromFrames (const float* frames, int numFrames)
{
    std::unique_ptr<WavetableBank> b (new WavetableBank (numFrames));
    constexpr int order = 11;
    static_assert ((1 << order) == kSize);
    juce::dsp::FFT fft (order);
    std::vector<float> spec ((size_t) kSize * 2), buf ((size_t) kSize * 2);

    for (int f = 0; f < b->nFrames; ++f)
    {
        std::fill (spec.begin(), spec.end(), 0.0f);
        std::copy_n (frames + (size_t) f * kSize, kSize, spec.begin());
        fft.performRealOnlyForwardTransform (spec.data(), true);   // bins 0..N/2, interleaved re/im
        spec[0] = spec[1] = 0.0f;                                   // DC

        for (int m = 0; m < kMips; ++m)
        {
            // the same partial budget as the built-in table: harmonics 1..min(N/2 >> m, N/2 - 1)
            const int maxH = std::min (std::max (1, (kSize / 2) >> m), kSize / 2 - 1);
            std::fill (buf.begin(), buf.end(), 0.0f);
            std::copy (spec.begin() + 2, spec.begin() + 2 * (maxH + 1), buf.begin() + 2);
            fft.performRealOnlyInverseTransform (buf.data());
            float* t = b->table (f, m);
            std::copy_n (buf.begin(), kSize, t);
            t[kSize] = t[0];
        }
    }

    float peak = 0.0f;
    for (int f = 0; f < b->nFrames; ++f)
    {
        const float* t = b->table (f, 0);
        for (int i = 0; i < kSize; ++i) peak = std::max (peak, std::abs (t[i]));
    }
    if (peak > 1.0e-6f)
    {
        const float norm = 0.9f / peak;
        for (auto& x : b->data) x *= norm;
    }
    return b;
}

WavetableBank::WavetableBank()
{
    data.assign ((size_t) (kFrames * kMips * (kSize + 1)), 0.0f);

    constexpr int order = 11;   // 2^11 = 2048
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) kSize * 2);

    for (int f = 0; f < kFrames; ++f)
    {
        float norm = 1.0f;
        for (int m = 0; m < kMips; ++m)
        {
            const int maxH = std::max (1, (kSize / 2) >> m);
            std::fill (buf.begin(), buf.end(), 0.0f);

            for (int h = 1; h < std::min (maxH + 1, kSize / 2); ++h)
            {
                float a, ph;
                harmonic (f, h, a, ph);
                // a*sin(h*theta + ph)  <=>  X[h] = a * (sin ph, -cos ph)  (up to a common scale)
                buf[(size_t) (2 * h)]     = a * std::sin (ph);
                buf[(size_t) (2 * h + 1)] = -a * std::cos (ph);
            }

            fft.performRealOnlyInverseTransform (buf.data());

            float* t = table (f, m);
            for (int i = 0; i < kSize; ++i) t[i] = buf[(size_t) i];

            if (m == 0)
            {
                float peak = 0.0f;
                for (int i = 0; i < kSize; ++i) peak = std::max (peak, std::abs (t[i]));
                norm = peak > 0.0f ? 0.9f / peak : 1.0f;
            }
            for (int i = 0; i < kSize; ++i) t[i] *= norm;
            t[kSize] = t[0];   // guard sample for interpolation
        }
    }
}
} // namespace vessel::dsp
