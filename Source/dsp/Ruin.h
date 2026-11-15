#pragma once

#include "Common.h"

namespace vessel::dsp
{
//==============================================================================
/** (6) Asymmetric thermionic drive. Runs inside RuinCore's 4x oversampled block. Three bias topologies after the
    classic triode / pentode / bias-starved switch: soft even-order, hard odd-order, and sputtering crossover fuzz.
    Every curve is smooth (no hard knees), so harmonics fall away fast and the 4x headroom contains them. */
class TubeDrive
{
public:
    struct Params { bool on = true; int mode = 0; float drive = 0.3f, bias = 0.15f, mix = 1.0f, outDb = 0.0f; };

    void prepare (float osRate)
    {
        for (auto& d : dc) d.prepare (osRate);
        driveS.prepare (osRate, 30.0f); biasS.prepare (osRate, 30.0f);
        mixS.prepare (osRate, 20.0f); outS.prepare (osRate, 20.0f);
    }

    /** In place at the oversampled rate. The dry reference for the blend is this function's own input, so dry and
        wet pass through the identical up/down filters: a parallel blend cannot comb, whatever the filter phase. */
    void process (float* l, float* r, int m, const Params& p)
    {
        mixS.set (p.on ? p.mix : 0.0f);
        if (! p.on && mixS.cur < 1.0e-4f) { mixS.snap (0.0f); return; }
        driveS.set (p.drive); biasS.set (p.bias);
        outS.set (dbToGain (p.outDb) / (1.0f + 1.6f * p.drive));

        for (int i = 0; i < m; ++i)
        {
            const float d = driveS.next(), b = biasS.next(), mx = mixS.next(), o = outS.next();
            const float g = std::exp2 (d * 6.643856f);   // 0..40 dB
            const float wl = dc[0].process (shape (l[i] * g, b, p.mode));
            const float wr = dc[1].process (shape (r[i] * g, b, p.mode));
            l[i] += mx * (wl * o - l[i]);
            r[i] += mx * (wr * o - r[i]);
        }
    }

    /** The static transfer curve (also used by the UI to draw it). */
    static float shape (float x, float b, int mode) noexcept
    {
        switch (mode)
        {
            case 0:   // triode: soft, asymmetric -> 2nd harmonic
            {
                const float off = 0.45f * b;
                return fastTanh (x + off) - fastTanh (off);
            }
            case 1:   // pentode: harder knee, mostly odd
            {
                const float off = 0.15f * b;
                auto clip = [] (float u) { const float a = std::abs (u); return u / std::pow (1.0f + a * a * std::sqrt (a), 0.4f); };
                return clip (x + off) - clip (off);
            }
            default:  // starved: smooth crossover dead-zone (cubic near zero, linear beyond) + lopsided fuzz
            {
                const float u = 1.5f * x + 0.5f * b;
                const float thr = 0.10f + 0.22f * std::max (0.0f, -b);
                const float v = u - thr * fastTanh (u / thr);
                const float pos = fastTanh (2.2f * v), neg = 0.85f * fastTanh (0.8f * v);
                const float s = 0.5f + 0.5f * fastTanh (v * 20.0f);   // C1 blend between the two half-curves
                return neg + s * (pos - neg);
            }
        }
    }

private:
    DcBlocker dc[2];
    Smooth driveS, biasS, mixS, outS;
};

//==============================================================================
/** (10) Analog bias modulator: programme-dependent supply sag (gain drops + bias walks as current is drawn)
    feeding a magnetic tape curve with simple hysteresis memory and head-gap HF loss. Runs at the 4x rate. */
class BiasModulator
{
public:
    struct Params { bool on = false; float amount = 0.2f, sag = 0.35f, sat = 0.4f, speedMs = 80; };

    void prepare (float rate)
    {
        sr = rate;
        for (auto& d : dc) d.prepare (sr);
        y1[0] = y1[1] = 0.0f;
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
            const float k = kR.next(), is = isR.next(), hyst = hystR.next(), amt = amtR.next(), sag = sagR.next();
            const float e = std::min (2.0f, env.process (0.5f * (l[i] + r[i])) * 2.0f);
            const float sagGain = 1.0f / (1.0f + sag * 3.0f * e);
            const float b = amt + sag * 0.25f * e;
            const float stat = fastTanh (b);

            float* io[2] { l + i, r + i };
            for (int c = 0; c < 2; ++c)
            {
                const float in = *io[c] * sagGain * k + b;
                float y = fastTanh (in + hyst * (y1[c] - 0.5f * in));
                y1[c] = y;
                *io[c] = dc[c].process (head[c].process ((y - stat) * is));
            }
        }
    }

private:
    float sr = 192000.0f, y1[2] {};
    EnvFollower env;
    DcBlocker dc[2];
    OnePole head[2];
    Ramp kR, isR, hystR, amtR, sagR;
};

//==============================================================================
/** The non-linear core of RUIN in one 4x oversampled block: (6) tube → (15) post tilt → (10) tape bias.
    Polyphase IIR half-band stages (lowest latency). Every parallel blend inside (tube mix, bias bypass fade)
    happens in the oversampled domain, so dry and wet always share the same transfer function. The block always
    runs, even with everything off, so switching a stage never changes latency or phase. */
class RuinCore
{
public:
    struct Params { TubeDrive::Params tube; float postTiltDb = 0, postPivot = 1200; BiasModulator::Params bias; };

    void prepare (float sampleRate, int maxBlock)
    {
        osr = 4.0f * sampleRate;
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        os->initProcessing ((size_t) maxBlock);
        os->reset();
        tube.prepare (osr);
        bias.prepare (osr);
        tilt[0].reset(); tilt[1].reset();
        biasFade.prepare (osr);
        dry.setSize (2, maxBlock * 4);
    }

    /** Group delay of the up/down pair at low frequency (fractional samples at the base rate). */
    float latency() const { return os ? os->getLatencyInSamples() : 0.0f; }
    void snapBias (bool on) { biasFade.snap (on); }

    void process (float* l, float* r, int n, const Params& p)
    {
        float* chans[2] { l, r };
        juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
        auto up = os->processSamplesUp (block);
        const int m = (int) up.getNumSamples();
        float* ul = up.getChannelPointer (0);
        float* ur = up.getChannelPointer (1);

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

        os->processSamplesDown (block);
    }

private:
    float osr = 192000.0f;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    TubeDrive tube;
    BiasModulator bias;
    Tilt tilt[2];
    BypassFade biasFade;
    juce::AudioBuffer<float> dry;
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
