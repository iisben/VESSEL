#pragma once

#include "Common.h"

namespace vessel::dsp
{
//==============================================================================
/** (21) Three-band upward + downward compressor (OTT-style). Linkwitz–Riley 4th-order split with an
    allpass-compensated low band, so the bands sum flat when the dynamics are idle. */
class MultibandOtt
{
public:
    struct Params { bool on = false; float depth = 0.4f, time = 1, up = 0.6f, down = 0.8f, lox = 120, hix = 2500;
                    float lowDb = 0, midDb = 0, highDb = 0, inDb = 0, outDb = 0; };

    void prepare (float sampleRate, int maxBlock)
    {
        sr = sampleRate;
        juce::dsp::ProcessSpec spec { (double) sr, (juce::uint32) maxBlock, 2 };
        for (auto* f : { &xA, &xB, &apC }) { f->prepare (spec); f->reset(); }
        for (auto& e : env) e.reset();
        lastGainDb = { 0, 0, 0 };
        sampleCount = 0;
        lastLox = lastHix = lastTime = -1.0f;
        for (auto& r : bandR) r.reset();
        inGR.reset(); outGR.reset(); depthR.reset(); upR.reset(); downR.reset();
    }

    std::array<float, 3> bandGainDb() const noexcept { return lastGainDb; }

    void process (float* l, float* r, int n, const Params& p, bool isCtrlTick = true, int ctrlSpan = 0)
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        if (! p.on) { lastGainDb = { 0, 0, 0 }; return; }
        if (isCtrlTick)
        {
            const float maxFc = std::max (40.0f, 0.45f * sr);
            const float lox = std::clamp (p.lox, 20.0f, maxFc * 0.5f);
            const float hix = std::clamp (std::max (p.hix, lox * 2.0f), lox * 1.5f, maxFc);
            if (std::abs (lox - lastLox) > 1.0e-3f) { lastLox = lox; xA.setCutoffFrequency (lox); }
            if (std::abs (hix - lastHix) > 1.0e-3f) { lastHix = hix; xB.setCutoffFrequency (hix); apC.setCutoffFrequency (hix); }

            if (std::abs (p.time - lastTime) > 1.0e-5f)
            {
                lastTime = p.time;
                static constexpr float att[3] { 12.0f, 6.0f, 2.5f }, rel[3] { 170.0f, 110.0f, 70.0f };
                for (int b = 0; b < 3; ++b) env[b].setTimes (att[b] * p.time, rel[b] * p.time, sr);
            }
            bandR[0].to (p.lowDb, ctrlSpan); bandR[1].to (p.midDb, ctrlSpan); bandR[2].to (p.highDb, ctrlSpan);
            inGR.to (dbToGain (p.inDb), ctrlSpan); outGR.to (dbToGain (p.outDb), ctrlSpan);
            depthR.to (p.depth, ctrlSpan); upR.to (p.up, ctrlSpan); downR.to (p.down, ctrlSpan);
        }

        for (int i = 0; i < n; ++i)
        {
            const float inG = inGR.next(), outG = outGR.next(), depth = depthR.next(), upA = upR.next(), downA = downR.next();
            const float bandG[3] { bandR[0].next(), bandR[1].next(), bandR[2].next() };
            float band[3][2];
            float x[2] { l[i] * inG, r[i] * inG };
            for (int c = 0; c < 2; ++c)
            {
                float lo, hi, mid, high, apLo, apHi;
                xA.processSample (c, x[c], lo, hi);
                xB.processSample (c, hi, mid, high);
                apC.processSample (c, lo, apLo, apHi);
                band[0][c] = apLo + apHi;
                band[1][c] = mid;
                band[2][c] = high;
            }

            float outL = 0.0f, outR = 0.0f;
            const bool meterTick = ((sampleCount++ & 31u) == 0u);
            for (int b = 0; b < 3; ++b)
            {
                const float lev = gainToDb (env[b].process (std::max (std::abs (band[b][0]), std::abs (band[b][1]))));
                float g = 0.0f;
                if (lev > kDownThr) g -= (lev - kDownThr) * (1.0f - 1.0f / 8.0f) * downA;
                if (lev < kUpThr)
                {
                    const float gate = clamp01 ((lev + 72.0f) / 14.0f);   // never pump up the noise floor
                    g += std::min (24.0f, (kUpThr - lev) * (1.0f - 1.0f / 3.0f)) * upA * gate;
                }
                g = g * depth + bandG[b];
                if (meterTick) lastGainDb[(size_t) b] = g;
                const float gl = dbToGain (g);
                outL += band[b][0] * gl;
                outR += band[b][1] * gl;
            }
            l[i] = outL * outG;
            r[i] = outR * outG;
        }
    }

private:
    static constexpr float kDownThr = -22.0f, kUpThr = -38.0f;
    float sr = 48000.0f, lastLox = -1.0f, lastHix = -1.0f, lastTime = -1.0f;
    uint64_t sampleCount = 0;
    juce::dsp::LinkwitzRileyFilter<float> xA, xB, apC;
    EnvFollower env[3];
    std::array<float, 3> lastGainDb {};
    Ramp bandR[3], inGR, outGR, depthR, upR, downR;
};

//==============================================================================
/** (22) M/S spatial collapse: removes the side channel below the threshold with an LR4 high-pass, and passes
    the mid through the matching LR4 allpass so mid and side keep identical phase above the split. */
class MonoCollapse
{
public:
    void prepare (float sampleRate, int maxBlock)
    {
        sr = sampleRate;
        lastFc = -1.0f;
        juce::dsp::ProcessSpec spec { (double) sampleRate, (juce::uint32) maxBlock, 1 };
        side.prepare (spec); side.reset();
        mid.prepare (spec);  mid.reset();
    }

    void process (float* l, float* r, int n, bool on, float freq)
    {
        if (! on) return;
        const float fc = std::clamp (freq, 20.0f, std::max (25.0f, 0.45f * sr));
        if (std::abs (fc - lastFc) > 1.0e-3f)
        {
            lastFc = fc;
            side.setCutoffFrequency (fc);
            mid.setCutoffFrequency (fc);
        }
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]), s = 0.5f * (l[i] - r[i]);
            float mlo, mhi, slo, shi;
            mid.processSample (0, m, mlo, mhi);
            side.processSample (0, s, slo, shi);
            const float m2 = mlo + mhi;   // allpass
            l[i] = m2 + shi;
            r[i] = m2 - shi;
        }
    }

private:
    float sr = 48000.0f, lastFc = -1.0f;
    juce::dsp::LinkwitzRileyFilter<float> side, mid;
};

//==============================================================================
/** (23) Sidechain ducker for the drone/texture bus.
    Follow: tracks the sub bus amplitude (sample-synchronous, no look-ahead, same gain on L and R).
    Pump:   fires on every note-on, phase-aligned to the sub oscillator reset, and releases exponentially. */
class Ducker
{
public:
    void prepare (float sampleRate) { sr = sampleRate; env.reset(); depthR.reset(); pump = 0.0f; lastGain = 1.0f; }
    void trigger() noexcept { pump = 1.0f; }
    float currentGain() const noexcept { return lastGain; }

    void process (float* l, float* r, const float* key, int n, bool on, int mode, float depth, float releaseMs,
                  bool isCtrlTick = true, int ctrlSpan = 0)
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        if (isCtrlTick) depthR.to (on ? depth : 0.0f, ctrlSpan);
        if (! on && depthR.cur <= 0.0f) { pump = 0.0f; env.reset(); lastGain = 1.0f; return; }
        if (isCtrlTick) env.setTimes (1.0f, releaseMs, sr);
        const float pumpCoef = std::exp (-1.0f / (0.001f * releaseMs * sr));
        for (int i = 0; i < n; ++i)
        {
            float e;
            if (mode == 0) e = std::min (1.0f, env.process (key[i]) * 2.2f);
            else { e = pump; pump = flushDenormal (pump * pumpCoef); }
            const float g = 1.0f - depthR.next() * e;
            l[i] *= g; r[i] *= g;
            lastGain = g;
        }
    }

private:
    float sr = 48000.0f, pump = 0.0f, lastGain = 1.0f;
    EnvFollower env;
    Ramp depthR;
};

//==============================================================================
/** True-peak ceiling. The 4x clipper bounds the oversampled signal, but its decimation filter and the base-rate clamp
    can still leave the reconstructed (inter-sample) waveform above the ceiling: over 4 dB on clipped noise, about
    1 dB on bright plucks. This stage looks ahead, rebuilds the waveform between samples at 16x (a Kaiser-windowed
    sinc, 64 taps per phase, flat to 0.46 fs so it sees what a true-peak meter sees), and applies one gain to both
    channels, aiming 0.02 dB under the ceiling.

    Each interval between two samples gets the gain it needs. The gain applied is a box average, W samples long, of a
    running minimum of those needs taken over W + 64 samples, with the signal delayed to match. That window is wide
    enough that every sample inside an interval's 64-tap reconstruction window is held at or below the interval's gain,
    and the gain only ever moves in straight ramps W samples long, so the stage adds no clipping of its own. Where
    nothing approaches the ceiling the gain is exactly 1 and the stage is a pure delay. The delay is the same whether the
    stage is on or off, so the latency reported to the host never changes. */
class TruePeakCeiling
{
public:
    static constexpr int kHalf = 32, kTaps = 2 * kHalf, kPhases = 16;

    void prepare (float sr)
    {
        W = juce::jlimit (8, kMaxW, (int) std::lround (sr * 0.0008f));   // 0.8 ms ramps
        H = W + kTaps;                                                   // minimum spans the ramp plus the kernel
        delay = 2 * kHalf + W - 1;                                       // detector look-ahead + kernel + ramp
        l1max = 0.0f;
        const double beta = 8.0, i0b = bessel0 (beta);
        for (int p = 1; p < kPhases; ++p)
        {
            // the point s + p/16 from x[s - (kHalf-1)] .. x[s + kHalf]
            const double f = (double) p / kPhases;
            double dc = 0.0, l1 = 0.0;
            double h[kTaps];
            for (int k = 0; k < kTaps; ++k)
            {
                const double d = f + (kHalf - 1) - k, u = d / (double) kHalf;
                const double sinc = std::sin (juce::MathConstants<double>::pi * d) / (juce::MathConstants<double>::pi * d);
                h[k] = sinc * (u * u < 1.0 ? bessel0 (beta * std::sqrt (1.0 - u * u)) / i0b : 0.0);
                dc += h[k];
            }
            for (int k = 0; k < kTaps; ++k) { coef[k][p] = (float) (h[k] / dc); l1 += std::abs (h[k] / dc); }
            l1max = std::max (l1max, (float) l1);
        }
        for (int k = 0; k < kTaps; ++k)
        {
            coef[k][0] = 0.0f;   // phase 0 is the sample itself, read directly
            tapMax[k] = 0.0f;
            for (int p = 1; p < kPhases; ++p) tapMax[k] = std::max (tapMax[k], std::abs (coef[k][p]));
        }
        reset();
    }

    void reset()
    {
        std::fill (std::begin (xl), std::end (xl), 0.0f);
        std::fill (std::begin (xr), std::end (xr), 0.0f);
        std::fill (std::begin (avg), std::end (avg), 1.0f);
        sum = (double) W; head = tail = 0; t = 0; ai = 0; hot = 0; idle = W; minGain = 1.0f;
    }

    int latency() const noexcept { return delay; }
    int rampLength() const noexcept { return W; }
    /** Lowest gain applied since the last call (1 = untouched). */
    float takeMinGain() noexcept { return std::exchange (minGain, 1.0f); }

    /** In place. ceiling <= 0 turns the detector off: the gain ramps back to 1 and only the delay remains. */
    void process (float* l, float* r, int n, float ceiling) noexcept
    {
        const float cT = ceiling > 0.0f ? ceiling * kMargin : std::numeric_limits<float>::max();
        for (int i = 0; i < n; ++i, ++t)
        {
            const int wi = (int) (t & kMask);
            xl[wi] = xl[wi + kRing] = l[i];   // mirrored, so any window is contiguous
            xr[wi] = xr[wi + kRing] = r[i];

            // 1. the gain interval s = t - kHalf needs; its window x[s-11 .. s+12] ends at t.
            //    A window whose samples are all below ceiling / (largest phase L1 norm) cannot reconstruct above it.
            if (std::max (std::abs (l[i]), std::abs (r[i])) * l1max > cT) hot = kTaps;
            float need = 1.0f;
            if (hot > 0)
            {
                --hot;
                const int si = (int) ((t - kHalf) & kMask);
                float pk = std::max (std::abs (xl[si]), std::abs (xr[si]));
                const int w0 = (int) ((t - (kTaps - 1)) & kMask);
                const float* wl = xl + w0;
                const float* wr = xr + w0;
                // ≥ every phase's |value|: the largest tap at each position times the sample (4 lanes, no reassociation)
                float bl[4] {};
                for (int k = 0; k < kTaps; k += 4)
                    for (int j = 0; j < 4; ++j) bl[j] += tapMax[k + j] * std::max (std::abs (wl[k + j]), std::abs (wr[k + j]));
                if (bl[0] + bl[1] + bl[2] + bl[3] > cT)
                {
                    // all 16 phases side by side in SIMD lanes (NEON / SSE): each tap's sample is broadcast and
                    // multiplied into one aligned coefficient row
                    V a[kVecs], b[kVecs];
                    for (int v = 0; v < kVecs; ++v) { a[v] = V::expand (0.0f); b[v] = V::expand (0.0f); }
                    for (int k = 0; k < kTaps; ++k)
                    {
                        const V va = V::expand (wl[k]), vb = V::expand (wr[k]);
                        for (int v = 0; v < kVecs; ++v)
                        {
                            const V c = V::fromRawArray (coef[k] + v * kLanes);
                            a[v] = V::multiplyAdd (a[v], c, va);
                            b[v] = V::multiplyAdd (b[v], c, vb);
                        }
                    }
                    alignas (32) float ra[kPhases], rb[kPhases];
                    for (int v = 0; v < kVecs; ++v) { a[v].copyToRawArray (ra + v * kLanes); b[v].copyToRawArray (rb + v * kLanes); }
                    for (int p = 1; p < kPhases; ++p) pk = std::max ({ pk, std::abs (ra[p]), std::abs (rb[p]) });
                }
                if (pk > cT) need = cT / pk;
            }

            // 2. running minimum of the needs over the last H intervals (monotonic queue)
            const std::int64_t s = t - kHalf;
            while (tail != head && qv[(tail - 1) & kQMask] >= need) --tail;
            qv[tail & kQMask] = need; qi[tail & kQMask] = s; ++tail;
            while (qi[head & kQMask] <= s - H) ++head;
            const float hold = qv[head & kQMask];

            // 3. box average of the minimum over W: straight ramps
            sum += (double) hold - (double) avg[ai];
            avg[ai] = hold;
            if (++ai == W) ai = 0;
            idle = hold < 1.0f ? 0 : idle + 1;
            if (idle >= W) sum = (double) W;   // exactly 1 again once the window is all ones
            const float g = idle >= W ? 1.0f : (float) (sum / (double) W);
            minGain = std::min (minGain, g);

            // 4. the sample this gain belongs to: every interval whose kernel reaches it lies inside every minimum
            //    that was averaged into g
            const int ri = (int) ((t - delay) & kMask);
            l[i] = xl[ri] * g; r[i] = xr[ri] * g;
        }
    }

private:
    static double bessel0 (double x)
    {
        double acc = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); acc += term; }
        return acc;
    }

    using V = juce::dsp::SIMDRegister<float>;
    static constexpr int kLanes = (int) V::SIMDNumElements;
    static constexpr int kVecs = kPhases / kLanes;
    static_assert (kPhases % kLanes == 0 && kTaps % 4 == 0);
    static constexpr int kMaxW = 400;                  // 0.8 ms at up to 500 kHz
    static constexpr float kMargin = 0.99770f;         // aim 0.02 dB under the ceiling
    static constexpr int kRing = 1024;                 // > kMaxW + 2 * kTaps
    static constexpr std::int64_t kMask = kRing - 1;
    static constexpr std::int64_t kQMask = 511;        // queue never holds more than H + 1
    alignas (32) float coef[kTaps][kPhases] {};
    float tapMax[kTaps] {};
    float l1max = 2.0f;
    float xl[2 * kRing] {}, xr[2 * kRing] {}, avg[kMaxW] {}, qv[512] {};
    std::int64_t qi[512] {};
    std::int64_t t = 0, head = 0, tail = 0;
    double sum = 0.0;
    float minGain = 1.0f;
    int W = 38, H = 38 + kTaps, delay = 2 * kHalf + 37, hot = 0, idle = 0, ai = 0;
};

//==============================================================================
/** Output stage, 4x Type-I linear-phase FIR oversampled:
    (24) 64-bit ADAA1 harmonic exciter -> output gain -> (25) 64-bit ADAA2 brickwall clipper -> 16x true-peak ceiling.
    Exact integer linear-phase latency when active (0-sample PDC error) and true 0-latency bypass when both exciter
    and clipper are off at prepareToPlay. */
class OutputStage
{
public:
    struct Params { bool excOn = false; float excFreq = 6000, excAmount = 0.25f, excHarm = 0.4f; bool clipOn = true; float ceilingDb = -0.3f, gainDb = 0; };

    void prepare (float sampleRate, int maxBlock)
    {
        osr = 4.0f * sampleRate;
        os.prepare (maxBlock);
        const int q = (int) std::llround (((double) os.getLatencyInSamples() + 0.25) * 4.0);
        pad4x = (4 - (q & 3)) & 3;
        pre[0].reset(); pre[1].reset();
        post[0].reset(); post[1].reset();
        gainR.reset(); ceilR.reset(); amtR.reset();
        clipped = 0;
        excFade.prepare (osr); clipFade.prepare (osr);
        tp.prepare (sampleRate);
        resetAdaa();
        osActive = true;
        tpActive = true;
    }

    /** Exact linear-phase integer latency in base-rate samples (0 when bypassed). */
    float latency() const
    {
        float lat = 0.0f;
        if (osActive) lat += os.getLatencyInSamples() + (float) (1 + pad4x) * 0.25f;
        if (tpActive) lat += (float) tp.latency();
        return lat;
    }

    void snap (bool excOn, bool clipOn)
    {
        excFade.snap (excOn);
        clipFade.snap (clipOn);
        osActive = excOn || clipOn;
        tpActive = clipOn;
        os.reset();
        tp.reset();
        pre[0].reset(); pre[1].reset();
        post[0].reset(); post[1].reset();
        resetAdaa();
    }

    int clipped = 0;
    /** Lab A/B only: false leaves the true-peak stage as a pure delay. */
    bool truePeak = true;
    TruePeakCeiling tp;

    void process (float* l, float* r, int n, const Params& p, bool isCtrlTick = true, int ctrlSpan = 0)
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        if (! osActive && ! p.excOn && ! p.clipOn)
        {
            // True 0-latency 1x bypass path (ramped output gain only)
            if (isCtrlTick) gainR.to (dbToGain (p.gainDb), ctrlSpan);
            for (int i = 0; i < n; ++i)
            {
                const float g = gainR.next();
                l[i] *= g; r[i] *= g;
            }
            return;
        }
        if (! osActive)
        {
            osActive = true;
            os.reset();
            pre[0].reset(); pre[1].reset();
            post[0].reset(); post[1].reset();
            resetAdaa();
        }
        if (p.clipOn && ! tpActive)
        {
            tpActive = true;
            tp.reset();
        }

        os.processUp (l, r, n);
        const int m = n * 4;
        const int mCtrl = ctrlSpan * 4;
        float* ul = os.upL();
        float* ur = os.upR();

        const double k = 1.0 + (double) p.excHarm * 9.0;
        const bool excLive = p.excOn || ! excFade.settled (p.excOn);
        if (isCtrlTick)
        {
            gainR.to (dbToGain (p.gainDb), mCtrl);
            ceilR.to (dbToGain (p.ceilingDb), mCtrl);
            amtR.to (p.excAmount * 1.4f / std::sqrt ((float) k), mCtrl);
            if (excLive) for (int c = 0; c < 2; ++c) { pre[c].setup (p.excFreq, 0.7, osr); post[c].setup (p.excFreq, 0.7, osr); }
        }
        const bool clipLive = p.clipOn || ! clipFade.settled (p.clipOn);
        int hits = 0;

        for (int i = 0; i < m; ++i)
        {
            float x[2] { ul[i], ur[i] };
            const float amt = amtR.next();
            if (excLive)
            {
                const float eg = excFade.next (p.excOn) * amt;
                for (int c = 0; c < 2; ++c)
                {
                    const double u = k * (double) pre[c].highpass (x[c]);
                    const double e = adaa1Exciter (c, u);
                    x[c] += post[c].highpass ((float) e) * eg;
                }
            }
            const float g = gainR.next(), c = std::max (1.0e-5f, ceilR.next());
            x[0] *= g; x[1] *= g;

            // Phase-aligned 1-oversampled-sample dry delay matching ADAA2 clipper's triangular kernel center
            const float dry0 = clipDry[0]; clipDry[0] = x[0];
            const float dry1 = clipDry[1]; clipDry[1] = x[1];

            if (clipLive)
            {
                const float cg = clipFade.next (p.clipOn);
                const double invC = 1.0 / (double) c;
                const float dry[2] { dry0, dry1 };
                for (int ch = 0; ch < 2; ++ch)
                {
                    if (std::abs (x[ch]) > c) ++hits;
                    const double yc = adaa2Clip (ch, (double) x[ch] * invC) * (double) c;
                    x[ch] = dry[ch] + cg * ((float) yc - dry[ch]);
                }
            }
            else
            {
                x[0] = dry0; x[1] = dry1;
            }

            if (pad4x > 0)
            {
                const float pl = padBuf[0][padIdx]; padBuf[0][padIdx] = x[0]; x[0] = pl;
                const float pr = padBuf[1][padIdx]; padBuf[1][padIdx] = x[1]; x[1] = pr;
                if (++padIdx >= pad4x) padIdx = 0;
            }

            ul[i] = x[0]; ur[i] = x[1];
        }
        os.processDown (l, r, n);

        // inter-sample peaks left by the decimation; then the sample clamp, which is now only a backstop
        if (tpActive)
            tp.process (l, r, n, p.clipOn && truePeak ? ceilR.cur : 0.0f);
        if (p.clipOn)
        {
            const float c = ceilR.cur;
            for (int i = 0; i < n; ++i) { l[i] = std::clamp (l[i], -c, c); r[i] = std::clamp (r[i], -c, c); }
        }
        clipped += (hits + 7) / 8;   // report in base-rate samples (4x, two channels)
    }

private:
    static double excF0 (double u) noexcept { const double t = fastTanhD (u); return t + 0.6 * t * t; }
    static double excF1 (double u) noexcept { return tanhF1 (u) + 0.6 * (u - fastTanhD (u)); }

    double adaa1Exciter (int c, double u) noexcept
    {
        const double f1 = excF1 (u);
        const double du = u - excX1[c];
        const double y = std::abs (du) > 1.0e-5 ? (f1 - excF1Prev[c]) / du
                                                : excF0 (0.5 * (u + excX1[c]));
        excX1[c] = u;
        excF1Prev[c] = f1;
        return y;
    }

    static double clipF0 (double u) noexcept { return std::clamp (u, -1.0, 1.0); }
    static double clipF1 (double u) noexcept
    {
        if (u < -1.0) return -u - 0.5;
        if (u >  1.0) return  u - 0.5;
        return 0.5 * u * u;
    }
    static double clipF2 (double u) noexcept
    {
        if (u < -1.0) return -0.5 * u * u - 0.5 * u - (1.0 / 6.0);
        if (u >  1.0) return  0.5 * u * u - 0.5 * u + (1.0 / 6.0);
        return (1.0 / 6.0) * u * u * u;
    }
    static double clipHalf (double a, double c, double fa2, double fc2, double fc1) noexcept
    {
        const double d = a - c;
        if (std::abs (d) > 1.0e-4)
            return (fa2 - fc2 - d * fc1) / (d * d);
        return 0.5 * clipF0 ((a + 2.0 * c) * (1.0 / 3.0));
    }

    double adaa2Clip (int ch, double u0) noexcept
    {
        const double u1 = clipX1[ch], u2 = clipX2[ch];
        const double f2_0 = clipF2 (u0);
        const double f1_1 = clipF1 (u1);
        const double f2_1 = clipF2 (u1);
        const double f2_2 = clipF2 (u2);
        const double y = clipHalf (u0, u1, f2_0, f2_1, f1_1)
                       + clipHalf (u2, u1, f2_2, f2_1, f1_1);
        clipX2[ch] = u1;
        clipX1[ch] = u0;
        return y;
    }

    void resetAdaa() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            excX1[c] = excF1Prev[c] = 0.0;
            clipX1[c] = clipX2[c] = 0.0;
            clipDry[c] = 0.0f;
            for (int k = 0; k < 4; ++k) padBuf[c][k] = 0.0f;
        }
        padIdx = 0;
    }

    float osr = 192000.0f;
    LinearPhase4xOs os;
    Svf pre[2], post[2];
    BypassFade excFade, clipFade;
    Ramp gainR, ceilR, amtR;
    double excX1[2] {}, excF1Prev[2] {}, clipX1[2] {}, clipX2[2] {};
    float clipDry[2] {}, padBuf[2][4] {};
    int pad4x = 0, padIdx = 0;
    bool osActive = true, tpActive = true;
};
} // namespace vessel::dsp
