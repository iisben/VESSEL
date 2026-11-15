#pragma once

#include "Common.h"

namespace vessel::dsp
{
//==============================================================================
/** (18) Z-axis micro-delay. Negative depth pushes the source back (precedence pre-delay, a cluster of
    0.4–15 ms micro reflections, air absorption, level drop). Positive depth pulls it forward (presence shelf,
    reflections suppressed, a little level). */
class ZAxis
{
public:
    void prepare (float sampleRate)
    {
        sr = sampleRate;
        dl[0].prepare ((int) (sr * 0.05f)); dl[1].prepare ((int) (sr * 0.05f));
        absorb[0].reset(); absorb[1].reset();
        presence[0].reset(); presence[1].reset();
        depthS.prepare (sr, 40.0f);
        depthS.snap (0.0f);
    }

    void process (float* l, float* r, int n, bool on, float depth)
    {
        depthS.set (on ? depth : 0.0f);
        if (! on && std::abs (depthS.cur) < 1.0e-4f)
        {
            for (int i = 0; i < n; ++i) { dl[0].push (l[i]); dl[1].push (r[i]); }
            return;
        }
        static constexpr float tapsMs[2][6] { { 0.43f, 1.7f, 3.1f, 5.9f, 8.7f, 13.3f },
                                              { 0.61f, 2.3f, 3.9f, 6.7f, 10.1f, 14.6f } };
        static constexpr float tapG[6] { 0.55f, 0.42f, 0.33f, 0.25f, 0.18f, 0.12f };

        const float d = depthS.cur;
        const float back = std::max (0.0f, -d), front = std::max (0.0f, d);
        const float air = 18000.0f * std::pow (0.22f, back);
        for (int c = 0; c < 2; ++c)
        {
            absorb[c].setup (air, 0.6f, sr);
            presence[c].setup (3200.0f, 0.7f, sr);
        }
        const float presG = dbToGain (front * 5.0f);

        for (int i = 0; i < n; ++i)
        {
            const float dd = depthS.next();
            const float b = std::max (0.0f, -dd), fr = std::max (0.0f, dd);
            dl[0].push (l[i]); dl[1].push (r[i]);
            float* io[2] { l + i, r + i };
            for (int c = 0; c < 2; ++c)
            {
                const float pre = 1.0f + b * 0.009f * sr;   // up to 9 ms precedence push
                float x = dl[c].readLinear (pre);
                float refl = 0.0f;
                for (int k = 0; k < 6; ++k) refl += dl[c].readLinear (pre + tapsMs[c][k] * 0.001f * sr * (1.0f + b)) * tapG[k];
                x = x * (1.0f - 0.45f * b) + refl * b * 0.75f;
                x = absorb[c].lowpass (x);
                presence[c].tick (x);
                x += presence[c].hp * (presG - 1.0f) * fr;
                *io[c] = x * (1.0f + 0.2f * fr);
            }
        }
    }

private:
    float sr = 48000.0f;
    DelayLine dl[2];
    Svf absorb[2], presence[2];
    Smooth depthS;
};

//==============================================================================
/** Procedural impulse responses for (17): physical spaces synthesised at the current sample rate. */
struct ImpulseFactory
{
    static juce::AudioBuffer<float> make (int type, float size, double sampleRate)
    {
        const float sr = (float) sampleRate;
        float seconds = 1.0f;
        switch (type)
        {
            case 0: seconds = 2.2f; break;   // concrete pipe
            case 1: seconds = 0.12f; break;  // old TV speaker
            case 2: seconds = 3.0f; break;   // stairwell
            case 3: seconds = 4.0f; break;   // water tank
            default: seconds = 0.08f; break; // cassette shell
        }
        seconds *= size;
        const int len = std::max (64, (int) (seconds * sr));
        juce::AudioBuffer<float> ir (2, len);
        ir.clear();
        Rng rng ((uint32_t) (type * 1013 + 17));

        for (int c = 0; c < 2; ++c)
        {
            float* y = ir.getWritePointer (c);
            switch (type)
            {
                case 0:   // pipe: dense noise burst into a feedback comb at the pipe's round trip, darkening each pass
                {
                    const float tripMs = (8.3f + (float) c * 0.37f) * size;
                    DelayLine comb; comb.prepare ((int) (sr * 0.05f));
                    OnePole lp; lp.setCutoff (2600.0f, sr);
                    Svf body; body.setup (95.0f / size, 3.0f, sr);
                    for (int i = 0; i < len; ++i)
                    {
                        const float t = (float) i / sr;
                        const float exc = (i == 0 ? 1.0f : 0.0f) + rng.bip() * 0.35f * std::exp (-t / 0.004f);
                        const float fb = comb.readLinear (tripMs * 0.001f * sr);
                        const float v = exc + lp.process (fb) * 0.88f;
                        comb.push (v);
                        y[i] = (v + body.bandpass (v) * 0.6f) * std::exp (-t / (0.45f * size));
                    }
                    break;
                }
                case 1:   // small cone in a plastic cabinet: narrow band, cone break-up peaks, tiny box reflections
                case 4:
                {
                    const bool tv = type == 1;
                    Svf hp, lp, peakA, peakB;
                    hp.setup (tv ? 290.0f : 420.0f, 0.8f, sr);
                    lp.setup (tv ? 4800.0f : 3300.0f, 0.9f, sr);
                    peakA.setup ((tv ? 920.0f : 1400.0f) / size, 5.0f, sr);
                    peakB.setup ((tv ? 2850.0f : 2300.0f) / size, 7.0f, sr);
                    const int r1 = (int) ((tv ? 1.3f : 0.6f) * 0.001f * sr * size);
                    const int r2 = (int) ((tv ? 2.1f : 0.9f) * 0.001f * sr * size);
                    const int r3 = (int) ((tv ? 3.4f : 1.4f) * 0.001f * sr * size);
                    for (int i = 0; i < len; ++i)
                    {
                        float x = (i == 0 ? 1.0f : 0.0f) + (i == r1 ? 0.45f : 0.0f) + (i == r2 + c ? -0.3f : 0.0f) + (i == r3 ? 0.2f : 0.0f);
                        x = lp.lowpass (hp.highpass (x));
                        x += peakA.bandpass (x) * 0.9f + peakB.bandpass (x) * 0.6f;
                        y[i] = x;
                    }
                    break;
                }
                case 2:   // stairwell: bright diffuse tail with a flutter echo between parallel walls
                {
                    DelayLine flutter; flutter.prepare ((int) (sr * 0.1f));
                    const float flMs = (23.0f + (float) c * 1.3f) * size;
                    OnePole lp; lp.setCutoff (7000.0f, sr);
                    for (int i = 0; i < len; ++i)
                    {
                        const float t = (float) i / sr;
                        const float tail = rng.bip() * std::exp (-t / (0.55f * size)) * 0.5f;
                        const float fb = flutter.readLinear (flMs * 0.001f * sr);
                        const float v = (i == 0 ? 1.0f : 0.0f) + tail * 0.4f + lp.process (fb) * 0.62f;
                        flutter.push (v);
                        y[i] = v * std::exp (-t / (0.9f * size)) + tail;
                    }
                    break;
                }
                case 3:   // water tank: inharmonic metal modes ringing over a dark wash
                default:
                {
                    float freqs[24], decays[24], amps[24], phases[24];
                    for (int k = 0; k < 24; ++k)
                    {
                        freqs[k] = (70.0f + 2800.0f * std::pow (rng.uni(), 2.2f)) / size;
                        decays[k] = (0.4f + 2.2f * rng.uni()) * size;
                        amps[k] = 0.05f + 0.1f * rng.uni();
                        phases[k] = rng.uni() * kTwoPi;
                    }
                    OnePole lp; lp.setCutoff (1800.0f, sr);
                    for (int i = 0; i < len; ++i)
                    {
                        const float t = (float) i / sr;
                        float v = 0.0f;
                        for (int k = 0; k < 24; ++k) v += amps[k] * std::sin (kTwoPi * freqs[k] * t + phases[k]) * std::exp (-t / decays[k]);
                        v += lp.process (rng.bip()) * 0.6f * std::exp (-t / (1.1f * size));
                        y[i] = (i == 0 ? 0.5f : 0.0f) + v;
                    }
                    break;
                }
            }
            // gentle fade-out on the last 5% so truncation never clicks
            const int fade = std::max (8, len / 20);
            for (int i = 0; i < fade; ++i) y[len - 1 - i] *= (float) i / (float) fade;
        }
        return ir;
    }
};

//==============================================================================
/** (16) "Black Hole" reverb: input diffusion → 8-line modulated FDN with a Hadamard mixer, per-line damping,
    true infinite hold (unity loop gain with a soft limiter guarding the loop). */
class BlackHole
{
public:
    struct Params { bool on = true, infinite = false; float size = 0.7f, decay = 8, damp = 0.45f, mod = 0.4f, preMs = 30, lowCut = 120, mix = 0.3f; };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        const float scale = sr / 48000.0f;
        for (int i = 0; i < kLines; ++i)
        {
            lines[(size_t) i].prepare ((int) ((kBase[i] * 2.6f + 260.0f) * scale) + 256);
            damp[(size_t) i].reset();
            lfoC[i] = std::cos (kTwoPi * (float) i / (float) kLines);
            lfoS[i] = std::sin (kTwoPi * (float) i / (float) kLines);
        }
        // Right diffuser uses kDiff[i] * scale * sz * 1.07 (up to 2.73x): allocate 3.1x + 128 so >= 96 kHz never wraps
        for (int i = 0; i < 4; ++i) { diff[0][i].prepare ((int) (kDiff[i] * scale * 3.1f) + 128); diff[1][i].prepare ((int) (kDiff[i] * scale * 3.1f) + 128); }
        pre[0].prepare ((int) (sr * 0.55f)); pre[1].prepare ((int) (sr * 0.55f));
        inHp[0].reset(); inHp[1].reset();
        sizeS.prepare (sr, 300.0f); sizeS.snap (0.7f);
        preS.prepare (sr, 60.0f); preS.snap (std::max (1.0f, 30.0f * 0.001f * sr));
        mixS.prepare (sr, 30.0f); mixS.snap (0.0f);
        cleared = false;
    }

    void process (float* l, float* r, int n, const Params& p)
    {
        mixS.set (p.on ? p.mix : 0.0f);
        if (! p.on && mixS.cur < 1.0e-4f)
        {
            mixS.snap (0.0f);
            if (! cleared)
            {   // once fully off, drop the tail so switching back on never resurrects old audio
                for (auto& d : lines) d.clear();
                for (auto& side : diff) for (auto& d : side) d.clear();
                pre[0].clear(); pre[1].clear();
                inHp[0].reset(); inHp[1].reset();
                for (auto& d : damp) d.reset();
                cleared = true;
            }
            return;
        }
        const float preTarget = std::clamp (p.preMs * 0.001f * sr, 1.0f, sr * 0.5f);
        if (cleared) preS.snap (preTarget);
        else         preS.set (preTarget);
        cleared = false;

        sizeS.set (0.25f + 2.3f * p.size * p.size);
        const float scale = sr / 48000.0f;
        const float dampHz = 18000.0f * std::pow (0.07f, p.damp);
        for (auto& d : damp) d.setCutoff (dampHz, sr);
        inHp[0].setup (p.lowCut, 0.7f, sr); inHp[1].setup (p.lowCut, 0.7f, sr);
        const float modDepth = p.mod * 0.0024f * sr;

        // Constant-loudness tail: steady-state FDN energy grows as 1/(1-g²), so scale the wet by sqrt(1-g²),
        // referenced to an 8 s hall. Long decays no longer swamp the mix; short rooms are not starved.
        const float meanLen = 2744.0f * scale * sizeS.target;
        const float gMean = std::pow (10.0f, -3.0f * meanLen / (std::max (0.3f, p.decay) * sr));
        const float wetNorm = p.infinite ? 0.6f : juce::jlimit (0.35f, 1.6f, std::sqrt (1.0f - gMean * gMean) / 0.36f);

        // Control rate: per-line loop gain (from each line's mean length) and the modulation rotors.
        // The loop gain varies by < 0.1 dB across the modulation excursion, so per-chunk is exact enough,
        // and it removes 8 pow() calls per sample.
        alignas (32) float g[kLines], rc[kLines], rs[kLines];
        for (int k = 0; k < kLines; ++k)
        {
            const float len = kBase[k] * scale * sizeS.cur + modDepth + 2.0f;
            g[k] = p.infinite ? 1.0f : std::pow (10.0f, -3.0f * len / (std::max (0.3f, p.decay) * sr));
            const float w = kTwoPi * (0.07f + 0.05f * (float) k) * (0.3f + p.mod) / sr;
            rc[k] = std::cos (w); rs[k] = std::sin (w);
            const float norm = 1.0f / std::sqrt (lfoC[k] * lfoC[k] + lfoS[k] * lfoS[k]);   // keep the rotors on the unit circle
            lfoC[k] *= norm; lfoS[k] *= norm;
        }
        const float hn = 1.0f / std::sqrt ((float) kLines);

        for (int i = 0; i < n; ++i)
        {
            const float sz = sizeS.next();
            const float m = mixS.next();
            const float preD = preS.next();

            pre[0].push (l[i]); pre[1].push (r[i]);
            float in[2] { inHp[0].highpass (pre[0].readLinear (preD)), inHp[1].highpass (pre[1].readLinear (preD)) };

            // input diffusion: 4 allpasses per side
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 4; ++k)
                {
                    auto& d = diff[c][k];
                    const float dlen = kDiff[k] * scale * sz * (c ? 1.07f : 1.0f);
                    const float v = d.readLinear (dlen);
                    const float w = in[c] + 0.62f * v;
                    d.push (w);
                    in[c] = v - 0.62f * w;
                }

            // modulation rotors (lane-parallel) → line lengths → reads
            alignas (32) float len[kLines], y[kLines], h[kLines];
            for (int k = 0; k < kLines; ++k)
            {
                const float c0 = lfoC[k], s0 = lfoS[k];
                lfoC[k] = c0 * rc[k] - s0 * rs[k];
                lfoS[k] = c0 * rs[k] + s0 * rc[k];
                len[k] = kBase[k] * scale * sz + modDepth * lfoS[k] + modDepth + 2.0f;
            }
            for (int k = 0; k < kLines; ++k) y[k] = lines[(size_t) k].readLinear (len[k]);

            // Hadamard 8x8 (fast Walsh–Hadamard), normalised
            std::copy (y, y + kLines, h);
            for (int stride = 1; stride < kLines; stride <<= 1)
                for (int s = 0; s < kLines; s += stride << 1)
                    for (int j = s; j < s + stride; ++j)
                    {
                        const float a = h[j], b = h[j + stride];
                        h[j] = a + b; h[j + stride] = a - b;
                    }

            for (int k = 0; k < kLines; ++k)
            {
                float v = damp[(size_t) k].process (h[k] * hn) * g[k];
                if (p.infinite) v = fastTanh (v * 0.9f) / 0.9f;   // soft guard: unity gain hold without runaway
                const float inj = p.infinite ? 0.0f : ((k & 1) ? in[1] : in[0]) * 0.35f;
                lines[(size_t) k].push (v + inj);
            }

            const float wl = (y[0] - y[2] + y[4] - y[6] + 0.5f * (y[1] + y[5])) * 0.45f * wetNorm;
            const float wr = (y[1] - y[3] + y[5] - y[7] + 0.5f * (y[2] + y[6])) * 0.45f * wetNorm;
            // send-style blend: dry stays mostly present, so 100% is a wash rather than a mute
            const float dryG = 1.0f - 0.35f * m * m;
            l[i] = l[i] * dryG + m * wl;
            r[i] = r[i] * dryG + m * wr;
        }
    }

private:
    static constexpr int kLines = 8;
    static constexpr float kBase[kLines] { 1427.0f, 1777.0f, 2137.0f, 2459.0f, 2903.0f, 3301.0f, 3779.0f, 4271.0f };
    static constexpr float kDiff[4] { 142.0f, 107.0f, 379.0f, 277.0f };

    float sr = 48000.0f;
    std::array<DelayLine, kLines> lines;
    std::array<OnePole, kLines> damp;
    float lfoC[kLines] {}, lfoS[kLines] {};
    bool cleared = false;
    DelayLine diff[2][4], pre[2];
    Svf inHp[2];
    Smooth sizeS, preS, mixS;
};

//==============================================================================
/** (19) Mono-safe Haas widener. Classic Haas delays one channel and combs when summed to mono. Here the
    delayed copy of the high-passed mid is injected into the SIDE only: L+R is mathematically untouched,
    so the mono fold-down has zero cancellation, while each ear still gets an inter-aural time difference. */
class HaasWidener
{
public:
    void prepare (float sampleRate)
    {
        sr = sampleRate;
        dl.prepare ((int) (sr * 0.04f));
        hp.reset();
        timeS.prepare (sr, 60.0f); timeS.snap (0.0f);
        widthS.prepare (sr, 30.0f); widthS.snap (0.0f);
    }

    void process (float* l, float* r, int n, bool on, float timeMs, float width, float xover)
    {
        widthS.set (on ? width : 0.0f);
        timeS.set (std::max (0.1f, timeMs) * 0.001f * sr);
        hp.setup (xover, 0.7f, sr);
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]);
            float s = 0.5f * (l[i] - r[i]);
            dl.push (hp.highpass (m));
            const float w = widthS.next();
            s += w * 0.9f * dl.readLinear (1.0f + timeS.next());
            l[i] = m + s;
            r[i] = m - s;
        }
    }

private:
    float sr = 48000.0f;
    DelayLine dl;
    Svf hp;
    Smooth timeS, widthS;
};
} // namespace vessel::dsp
