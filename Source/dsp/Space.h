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
                x += (float) presence[c].hp * (presG - 1.0f) * fr;
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
        ctrlPrimed = false;
        ctrlTickCount = 0;
        lastDamp = lastLowCut = lastDecay = lastMod = lastSizeQ = -1.0f;
        lastInf = false;
    }

    void process (float* l, float* r, int n, const Params& p, bool isCtrlTick = true)
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
        const float modDepth = p.mod * 0.0024f * sr;

        if (isCtrlTick || ! ctrlPrimed)
        {
            const bool paramsChanged = ! ctrlPrimed
                || std::abs (p.damp - lastDamp) > 1.0e-5f
                || std::abs (p.lowCut - lastLowCut) > 1.0e-3f
                || std::abs (p.decay - lastDecay) > 1.0e-5f
                || std::abs (p.mod - lastMod) > 1.0e-5f
                || std::abs (sizeS.cur - lastSizeQ) > 1.0e-4f
                || p.infinite != lastInf;

            if (paramsChanged)
            {
                if (! ctrlPrimed || std::abs (p.damp - lastDamp) > 1.0e-5f)
                {
                    const float dampHz = 18000.0f * std::pow (0.07f, p.damp);
                    for (auto& d : damp) d.setCutoff (dampHz, sr);
                }
                if (! ctrlPrimed || std::abs (p.lowCut - lastLowCut) > 1.0e-3f)
                {
                    inHp[0].setup (p.lowCut, 0.7f, sr); inHp[1].setup (p.lowCut, 0.7f, sr);
                }
                const float meanLen = 2744.0f * scale * sizeS.target;
                const float gMean = std::pow (10.0f, -3.0f * meanLen / (std::max (0.3f, p.decay) * sr));
                wetNormC = p.infinite ? 0.6f : juce::jlimit (0.35f, 1.6f, std::sqrt (1.0f - gMean * gMean) / 0.36f);

                for (int k = 0; k < kLines; ++k)
                {
                    const float len = kBase[k] * scale * sizeS.cur + modDepth + 2.0f;
                    gC[k] = p.infinite ? 1.0f : std::pow (10.0f, -3.0f * len / (std::max (0.3f, p.decay) * sr));
                    const float w = kTwoPi * (0.07f + 0.05f * (float) k) * (0.3f + p.mod) / sr;
                    rcC[k] = std::cos (w); rsC[k] = std::sin (w);
                }
                lastDamp = p.damp; lastLowCut = p.lowCut; lastDecay = p.decay;
                lastMod = p.mod; lastSizeQ = sizeS.cur; lastInf = p.infinite;
                ctrlPrimed = true;
            }
            // Renormalize rotors every 8 control ticks (64 samples) — keeps unit-circle invariant & cuts sqrt 8x
            if ((ctrlTickCount++ & 7u) == 0u)
            {
                for (int k = 0; k < kLines; ++k)
                {
                    const float norm = 1.0f / std::sqrt (lfoC[k] * lfoC[k] + lfoS[k] * lfoS[k]);
                    lfoC[k] *= norm; lfoS[k] *= norm;
                }
            }
        }
        const float wetNorm = wetNormC;
        const float* g = gC;
        const float* rc = rcC;
        const float* rs = rsC;
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
    alignas (32) float gC[kLines] {}, rcC[kLines] {}, rsC[kLines] {};
    float wetNormC = 1.0f;
    float lastDamp = -1.0f, lastLowCut = -1.0f, lastDecay = -1.0f, lastMod = -1.0f, lastSizeQ = -1.0f;
    uint32_t ctrlTickCount = 0;
    bool cleared = false, ctrlPrimed = false, lastInf = false;
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
        if (! on && std::abs (widthS.cur) < 1.0e-5f)
        {
            widthS.snap (0.0f);
            return;
        }
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

//==============================================================================
/** Authentic BBD (Bucket Brigade Device) Analog Chorus (MN3009 / MN3007 / Dimension-D inspired):
    - Clock-rate-modulated BBD delay lines: tau(t) = N_stg / (2 * f_clk(t)) driven by C-infinity rounded
      analog triangle LFOs in anti-phase stereo (Mode I: 256-stage Juno; Mode II: 512-stage deep + warm
      band-limited regen; Mode III: 4-tap dual-rate Ensemble).
    - Exact complementary 64-bit TPT pre-emphasis (+6 dB @ 1.6 kHz) / de-emphasis (-6 dB @ 1.6 kHz)
      and NE570-style 2:1 / 1:2 true-RMS compander (2-pole ripple-free detector, unity steady-state gain,
      organic analog transient bloom, zero noise-floor pumping).
    - Dark 4th-order analog anti-aliasing (pre-BBD) and clock-coupled anti-imaging (post-BBD) Cytomic
      TPT low-pass filtering (6–10 kHz).
    - 64-bit double-precision ring buffers with 4th-order-tangent 64-bit Cubic Hermite interpolation and
      subtle 64-bit ADAA1 BBD charge-well soft saturation.
    - Reuses VESSEL's Halfband2x polyphase allpass 2x oversampler ONLY on the wet BBD branch when
      sr < 88.2 kHz (zero nested oversampling, 0 samples added dry latency).
    - Sub-120 Hz phase-coherent LR4 crossover discipline keeps bass full-weight and mono-anchored
      even at 100% wet without hollowing out the low end. */
class BbdChorus
{
public:
    enum Mode { Off = 0, ModeI = 1, ModeII = 2, Ensemble = 3 };

    struct Params
    {
        bool  on     = false;
        int   mode   = ModeI;      // 1=Mode I, 2=Mode II, 3=Ensemble
        float rateHz = 0.55f;      // 0.05..8.0 Hz clock-mod LFO rate
        float depth  = 0.55f;      // 0..1 clock-rate modulation depth
        float toneHz = 8000.0f;    // 6000..10000 Hz analog anti-imaging LPF cutoff
        float mix    = 0.50f;      // 0..1 wet blend (above sub-120 Hz crossover)
    };

    void prepare (double sampleRate)
    {
        sr = std::max (8000.0, sampleRate);
        use2x = (sr < 88200.0);
        bbdSr = use2x ? (2.0 * sr) : sr;

        // Allocate 64-bit ring buffers for >= 35 ms at bbdSr (zero audio-thread allocations)
        const int maxSamp = (int) std::ceil (0.035 * bbdSr) + 32;
        for (int c = 0; c < 2; ++c)
            delayBuf[c].prepare (maxSamp);

        // Fixed 160 Hz 64-bit LR4 crossover -> protects entire sub-120 Hz band phase-coherently
        constexpr double kXoverHz = 160.0;
        constexpr double kButterQ = 0.7071067811865476;
        for (int c = 0; c < 2; ++c)
        {
            xSplit1[c].setup (kXoverHz, kButterQ, sr);
            xSplitLp2[c].setup (kXoverHz, kButterQ, sr);
            xSplitHp2[c].setup (kXoverHz, kButterQ, sr);
            wetHp[c].setup (110.0, kButterQ, sr);
            wetDc[c].prepare (sr, 5.0);
        }

        // Exact complementary TPT pre-emphasis (+6.02 dB above 1.6 kHz) and de-emphasis (-6.02 dB)
        const double fcShelf = std::min (1600.0, 0.25 * sr);
        gPreShelf = std::tan (kPiD * fcShelf / sr);
        gDeShelf  = gPreShelf / kPreGain;
        aPreShelf = gPreShelf / (1.0 + gPreShelf);
        aDeShelf  = gDeShelf  / (1.0 + gDeShelf);

        // NE570-style 2-pole true-RMS compander time constants (sample-rate invariant)
        envAtt  = std::exp (-1.0 / (0.012 * sr));   // 12 ms attack
        envRel  = std::exp (-1.0 / (0.065 * sr));   // 65 ms release
        envRip  = std::exp (-1.0 / (0.018 * sr));   // 18 ms 2nd-pole ripple smoother (>110 dB @ 12 kHz)

        reset();
    }

    /** Zero added dry-path latency (Halfband2x runs solely inside the delayed wet BBD branch). */
    float latency() const noexcept { return 0.0f; }

    int activeMode() const noexcept { return lastMode; }

    void reset() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            delayBuf[c].clear();
            os[c].reset();
            xSplit1[c].reset();
            xSplitLp2[c].reset();
            xSplitHp2[c].reset();
            preLp1[c].reset(); preLp2[c].reset();
            postLp1[c].reset(); postLp2[c].reset();
            wetHp[c].reset();
            wetDc[c].reset();
            sPreShelf[c] = sDeShelf[c] = 0.0;
            compE1[c] = compE2[c] = 0.0;
            expE1[c]  = expE2[c]  = 0.0;
            adaaX1[c] = adaaF1[c] = 0.0;
            fbState[c] = 0.0;
            prevDelayA[c] = 0.0;
            prevDelayB[c] = 0.0;
        }
        phase1 = 0.0;
        phase2 = 0.25;
        rateCur = 0.55; depthCur = 0.55; toneCur = 8000.0; mixCur = 0.50;
        dRate = dDepth = dTone = dMix = 0.0;
        lastPreTone = -1.0;
        postSubTick = 0;
        primed = false;
        wasActive = false;
        delayPrimed = false;
    }

    void process (float* l, float* r, int n, const Params& p, bool isCtrlTick = true, int ctrlSpan = 0) noexcept
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        const int mode = std::clamp (p.mode, 0, 3);
        if (! p.on || mode == Off)
        {
            if (wasActive)
            {
                reset();
                wasActive = false;
            }
            return;
        }

        wasActive = true;
        lastMode = mode;

        if (isCtrlTick || ! primed)
        {
            const double tRate  = std::clamp ((double) p.rateHz, 0.05, 8.0);
            const double tDepth = std::clamp ((double) p.depth,  0.0,  1.0);
            const double tTone  = std::clamp ((double) p.toneHz, 6000.0, std::min (10000.0, 0.42 * sr));
            const double tMix   = std::clamp ((double) p.mix,    0.0,  1.0);

            if (! primed)
            {
                rateCur  = tRate;
                depthCur = tDepth;
                toneCur  = tTone;
                mixCur   = tMix;
                dRate = dDepth = dTone = dMix = 0.0;
                primed = true;
            }
            else
            {
                const double inv = 1.0 / (double) std::max (1, ctrlSpan);
                dRate  = (tRate  - rateCur)  * inv;
                dDepth = (tDepth - depthCur) * inv;
                dTone  = (tTone  - toneCur)  * inv;
                dMix   = (tMix   - mixCur)   * inv;
            }
        }

        const double maxDelay = (double) (delayBuf[0].mask - 8);
        const double invSr = 1.0 / sr;
        double dryG = std::cos (0.5 * kPiD * mixCur);
        double wetG = std::sin (0.5 * kPiD * mixCur);
        const bool rampMix = (std::abs (dMix) > 1.0e-12);

        for (int i = 0; i < n; ++i)
        {
            rateCur  = std::clamp (rateCur  + dRate,  0.05, 8.0);
            depthCur = std::clamp (depthCur + dDepth, 0.0,  1.0);
            toneCur  = std::clamp (toneCur  + dTone,  6000.0, std::min (10000.0, 0.42 * sr));
            mixCur   = std::clamp (mixCur   + dMix,   0.0,  1.0);
            if (rampMix)
            {
                const double angle = 0.5 * kPiD * mixCur;
                dryG = std::cos (angle);
                wetG = std::sin (angle);
            }

            const double inL = (double) l[i];
            const double inR = (double) r[i];

            // 1. Sub-120 Hz phase-coherent LR4 split at 160 Hz (xLo + xHi == unity-magnitude allpass)
            xSplit1[0].tick (inL);
            xSplitLp2[0].tick (xSplit1[0].lp);
            xSplitHp2[0].tick (xSplit1[0].hp);
            const double xLoL = xSplitLp2[0].lp;
            const double xHiL = xSplitHp2[0].hp;

            xSplit1[1].tick (inR);
            xSplitLp2[1].tick (xSplit1[1].lp);
            xSplitHp2[1].tick (xSplit1[1].hp);
            const double xLoR = xSplitLp2[1].lp;
            const double xHiR = xSplitHp2[1].hp;

            // Keep sub-120 Hz bass phase-coherent (smoothly mono-anchored as mix engages)
            const double mLo = 0.5 * (xLoL + xLoR);
            const double sLo = 0.5 * (xLoL - xLoR);
            const double subWidth = std::max (0.0, 1.0 - 2.0 * mixCur);
            const double outLoL = mLo + sLo * subWidth;
            const double outLoR = mLo - sLo * subWidth;

            // 2. Advance BBD clock-modulation LFOs (exact C-infinity rounded analog triangle)
            phase1 += rateCur * invSr;
            if (phase1 >= 1.0) phase1 -= std::floor (phase1);
            const double secRate = std::min (9.5, rateCur * 2.1731);
            phase2 += secRate * invSr;
            if (phase2 >= 1.0) phase2 -= std::floor (phase2);

            const double lfo1 = analogTriangle (phase1);
            double fClkAL = 40000.0, fClkAR = 39200.0;
            double fClkNomA = 40000.0;
            double dSecAL = 0.0032, dSecAR = 0.00326;
            double dSecBL = 0.0053, dSecBR = 0.00547;
            double fbGain = 0.0;

            if (mode == ModeI)
            {
                // Juno-60/106 MN3009 (256 stages, anti-phase clock modulation)
                const double oct = 0.72 * depthCur;
                fClkNomA = 40000.0;
                fClkAL = 40000.0 * std::exp2 ( oct * lfo1);
                fClkAR = 39200.0 * std::exp2 (-oct * lfo1);
                dSecAL = 256.0 / (2.0 * fClkAL);
                dSecAR = 256.0 / (2.0 * fClkAR);
            }
            else if (mode == ModeII)
            {
                // Deep MN3007 / CE-1 (512 stages, deep clock sweep + subtle dark analog regen)
                const double oct = 0.65 * depthCur;
                fClkNomA = 52000.0;
                fClkAL = 52000.0 * std::exp2 ( oct * lfo1);
                fClkAR = 50400.0 * std::exp2 (-oct * lfo1);
                dSecAL = 512.0 / (2.0 * fClkAL);
                dSecAR = 512.0 / (2.0 * fClkAR);
                fbGain = 0.14 * depthCur;
            }
            else
            {
                // Ensemble: 4-tap dual-BBD (MN3009 primary + MN3007 quadrature shimmer, Solina/Dimension-D voicing)
                const double lfo2 = analogTriangle (phase2);
                const double octA = 0.65 * depthCur;
                const double octB = 0.28 * depthCur;
                fClkNomA = 42000.0;
                fClkAL = 42000.0 * std::exp2 ( octA * lfo1);
                fClkAR = 41200.0 * std::exp2 (-octA * lfo1);
                const double fClkBL = 56000.0 * std::exp2 ( octB * lfo2);
                const double fClkBR = 54600.0 * std::exp2 (-octB * lfo2);
                dSecAL = 256.0 / (2.0 * fClkAL);
                dSecAR = 256.0 / (2.0 * fClkAR);
                dSecBL = 512.0 / (2.0 * fClkBL);
                dSecBR = 512.0 / (2.0 * fClkBR);
            }

            const double dSampA[2] {
                std::clamp (dSecAL * bbdSr, 4.0, maxDelay),
                std::clamp (dSecAR * bbdSr, 4.0, maxDelay)
            };
            const double dSampB[2] {
                std::clamp (dSecBL * bbdSr, 4.0, maxDelay),
                std::clamp (dSecBR * bbdSr, 4.0, maxDelay)
            };
            if (! delayPrimed)
            {
                for (int c = 0; c < 2; ++c) { prevDelayA[c] = dSampA[c]; prevDelayB[c] = dSampB[c]; }
                delayPrimed = true;
            }

            // Update pre-BBD anti-aliasing and clock-coupled post-BBD anti-imaging LPFs every 8 base samples (odd-block invariant)
            if ((postSubTick++ & 7u) == 0u)
            {
                if (std::abs (toneCur - lastPreTone) > 0.05)
                {
                    lastPreTone = toneCur;
                    for (int c = 0; c < 2; ++c)
                    {
                        preLp1[c].setup (toneCur, kAnalogQ1, sr);
                        preLp2[c].setup (toneCur, kAnalogQ2, sr);
                    }
                }
                const double clkRatio[2] { fClkAL / fClkNomA, fClkAR / fClkNomA };
                for (int c = 0; c < 2; ++c)
                {
                    // Slower BBD clock subtly darkens reconstruction LPF just like analog hardware
                    const double darkMul = std::clamp (0.88 + 0.12 * clkRatio[c], 0.78, 1.06);
                    const double fcPost = std::clamp (toneCur * darkMul, 4500.0, 0.42 * sr);
                    postLp1[c].setup (fcPost, kAnalogQ1, sr);
                    postLp2[c].setup (fcPost, kAnalogQ2, sr);
                }
            }

            const double hiIn[2] { xHiL, xHiR };
            double wetOut[2] { 0.0, 0.0 };
            const double bbdDrive = 0.45 + 0.35 * depthCur;
            const double invBbdDrive = 1.0 / bbdDrive;

            for (int c = 0; c < 2; ++c)
            {
                // 3. Pre-emphasis (+6.02 dB above 1.6 kHz)
                const double lpPre = aPreShelf * hiIn[c] + (1.0 - aPreShelf) * sPreShelf[c];
                sPreShelf[c] = flushDenormal (2.0 * lpPre - sPreShelf[c]);
                const double preEmph = lpPre + kPreGain * (hiIn[c] - lpPre);

                // 4. NE570 2:1 true-RMS compressor (2-pole ripple-free detector, bounded floor)
                const double sqPre = preEmph * preEmph;
                const double cCoef = (sqPre > compE1[c]) ? envAtt : envRel;
                compE1[c] = flushDenormal (sqPre + cCoef * (compE1[c] - sqPre));
                compE2[c] = flushDenormal (compE1[c] + envRip * (compE2[c] - compE1[c]));
                const double rmsPre = std::sqrt (compE2[c] + kCompFloorSq);
                const double gComp = std::sqrt (kCompRef / rmsPre);
                const double compSig = preEmph * gComp;

                // 5. Pre-BBD 4th-order dark analog anti-aliasing LPF + Mode II warm regen injection
                preLp1[c].tick (compSig + fbGain * fbState[c]);
                preLp2[c].tick (preLp1[c].lp);
                const double toBbd = preLp2[c].lp;

                // 6. BBD charge-transfer delay core (2x via Halfband2x when sr < 88.2 kHz, 64-bit Hermite + ADAA1)
                double fromBbd = 0.0;
                if (use2x)
                {
                    double u0, u1;
                    os[c].upsample (toBbd, u0, u1);
                    const double dA0 = 0.5 * (prevDelayA[c] + dSampA[c]);
                    const double dB0 = 0.5 * (prevDelayB[c] + dSampB[c]);
                    const double v0 = tickBbdCore (c, u0, bbdDrive, invBbdDrive, mode, dA0, dB0);
                    const double v1 = tickBbdCore (c, u1, bbdDrive, invBbdDrive, mode, dSampA[c], dSampB[c]);
                    fromBbd = os[c].downsample (v0, v1);
                }
                else
                {
                    fromBbd = tickBbdCore (c, toBbd, bbdDrive, invBbdDrive, mode, dSampA[c], dSampB[c]);
                }
                prevDelayA[c] = dSampA[c];
                prevDelayB[c] = dSampB[c];

                // 7. Post-BBD 4th-order clock-coupled dark analog anti-imaging LPF
                postLp1[c].tick (fromBbd);
                postLp2[c].tick (postLp1[c].lp);
                const double filtBbd = postLp2[c].lp;

                // Store band-limited feedback for Mode II (soft-guarded against runaway)
                fbState[c] = flushDenormal (fastTanhD (filtBbd));

                // 8. NE570 1:2 true-RMS expander (complementary steady-state unity + analog transient bloom)
                const double sqPost = filtBbd * filtBbd;
                const double eCoef = (sqPost > expE1[c]) ? envAtt : envRel;
                expE1[c] = flushDenormal (sqPost + eCoef * (expE1[c] - sqPost));
                expE2[c] = flushDenormal (expE1[c] + envRip * (expE2[c] - expE1[c]));
                const double rmsPost = std::sqrt (expE2[c] + kExpFloorSq);
                const double gExp = rmsPost * kInvCompRef;
                const double expSig = filtBbd * gExp;

                // 9. Exact complementary de-emphasis (-6.02 dB above 1.6 kHz)
                const double lpDe = aDeShelf * expSig + (1.0 - aDeShelf) * sDeShelf[c];
                sDeShelf[c] = flushDenormal (2.0 * lpDe - sDeShelf[c]);
                const double deEmph = lpDe + kInvPreGain * (expSig - lpDe);

                // 10. 64-bit DC block + 110 Hz wet HPF guard (zero sub-120 Hz IM or DC leakage)
                wetHp[c].tick (wetDc[c].process (deEmph));
                wetOut[c] = wetHp[c].hp;
            }

            // Equal-power dry/wet blend on >120 Hz band; phase-coherent sub-120 Hz bass summed at exact unity
            l[i] = (float) (outLoL + dryG * xHiL + wetG * wetOut[0]);
            r[i] = (float) (outLoR + dryG * xHiR + wetG * wetOut[1]);
        }
    }

private:
    /** 64-bit double-precision power-of-two circular delay line with 4th-order-tangent 64-bit Cubic Hermite. */
    struct DelayLineD
    {
        std::vector<double> buf;
        int mask = 0, w = 0;

        void prepare (int maxSamples)
        {
            int sz = 1;
            while (sz < maxSamples + 16) sz <<= 1;
            buf.assign ((size_t) sz, 0.0);
            mask = sz - 1;
            w = 0;
        }
        void clear() noexcept { std::fill (buf.begin(), buf.end(), 0.0); w = 0; }
        void push (double x) noexcept { buf[(size_t) w] = x; w = (w + 1) & mask; }

        /** 64-bit Cubic Hermite interpolation with 4th-order finite-difference tangents (d >= 4.0). */
        double readHermite64 (double d) const noexcept
        {
            const double rp = (double) w - d;
            const int i1 = (int) std::floor (rp);
            const double t = rp - (double) i1;
            const double ym1 = buf[(size_t) ((i1 - 2) & mask)];
            const double y0  = buf[(size_t) ((i1 - 1) & mask)];
            const double y1  = buf[(size_t) ( i1      & mask)];
            const double y2  = buf[(size_t) ((i1 + 1) & mask)];
            const double y3  = buf[(size_t) ((i1 + 2) & mask)];
            const double y4  = buf[(size_t) ((i1 + 3) & mask)];
            constexpr double kInv12 = 1.0 / 12.0;
            const double m1 = (ym1 - 8.0 * y0 + 8.0 * y2 - y3) * kInv12;
            const double m2 = (y0  - 8.0 * y1 + 8.0 * y3 - y4) * kInv12;
            const double c2 = 3.0 * (y2 - y1) - 2.0 * m1 - m2;
            const double c3 = 2.0 * (y1 - y2) + m1 + m2;
            return ((c3 * t + c2) * t + m1) * t + y1;
        }
    };

    static inline double analogTriangle (double phase01) noexcept
    {
        // C-infinity rounded analog triangle LFO (linear constant-detune slopes + smooth RC turnarounds)
        constexpr double kKappa   = 0.96;
        constexpr double kInvAsin = 1.0 / 1.2870022175865687;   // 1 / asin(0.96)
        return std::asin (kKappa * std::sin (kTwoPiD * phase01)) * kInvAsin;
    }

    inline double tickBbdCore (int c, double xIn, double drive, double invDrive, int mode,
                               double dA, double dB) noexcept
    {
        // Subtle 64-bit ADAA1 BBD MOSFET charge-well soft saturation (exact odd symmetry -> zero DC)
        const double u = xIn * drive;
        const double f1 = tanhF1 (u);
        const double du = u - adaaX1[c];
        const double sat = (std::abs (du) > 1.0e-5) ? (f1 - adaaF1[c]) / du
                                                    : fastTanhD (0.5 * (u + adaaX1[c]));
        adaaX1[c] = u;
        adaaF1[c] = f1;

        delayBuf[c].push (sat * invDrive);

        const double yA = delayBuf[c].readHermite64 (dA);
        if (mode == Ensemble)
        {
            const double yB = delayBuf[c].readHermite64 (dB);
            return 0.68 * (yA + yB);
        }
        return yA;
    }

    static constexpr double kPreGain     = 2.0;                         // +6.02 dB pre-emphasis shelf
    static constexpr double kInvPreGain  = 1.0 / kPreGain;
    static constexpr double kCompRef     = 0.25;                        // -12 dBFS compander nominal reference
    static constexpr double kInvCompRef  = 1.0 / kCompRef;
    static constexpr double kCompFloor   = 0.05;                        // -26 dBFS floor -> max +7 dB quiet lift
    static constexpr double kCompFloorSq = kCompFloor * kCompFloor;
    static constexpr double kExpFloorSq  = kCompRef * kCompFloor;       // exact unity product at silence
    static constexpr double kAnalogQ1    = 0.5411961001461970;          // 4th-order analog anti-imaging stage 1
    static constexpr double kAnalogQ2    = 1.0800000000000000;          // silky Bessel-Butterworth analog shoulder

    double sr = 48000.0, bbdSr = 96000.0;
    bool use2x = true;

    DelayLineD delayBuf[2];
    Halfband2x os[2];
    Svf xSplit1[2], xSplitLp2[2], xSplitHp2[2];
    Svf preLp1[2], preLp2[2], postLp1[2], postLp2[2], wetHp[2];
    DcBlocker wetDc[2];

    double gPreShelf = 0.1, gDeShelf = 0.05, aPreShelf = 0.1, aDeShelf = 0.05;
    double sPreShelf[2] {}, sDeShelf[2] {};
    double envAtt = 0.99, envRel = 0.999, envRip = 0.999;
    double compE1[2] {}, compE2[2] {}, expE1[2] {}, expE2[2] {};
    double adaaX1[2] {}, adaaF1[2] {};
    double fbState[2] {};
    double prevDelayA[2] {}, prevDelayB[2] {};

    double phase1 = 0.0, phase2 = 0.25;
    double rateCur = 0.55, depthCur = 0.55, toneCur = 8000.0, mixCur = 0.50;
    double dRate = 0.0, dDepth = 0.0, dTone = 0.0, dMix = 0.0;
    double lastPreTone = -1.0;
    uint32_t postSubTick = 0;
    int lastMode = ModeI;
    bool primed = false, wasActive = false, delayPrimed = false;
};
} // namespace vessel::dsp
