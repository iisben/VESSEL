#pragma once

#include "Common.h"
#include "Wavetable.h"

namespace vessel::dsp
{
//==============================================================================
/** (3) Cinematic drone: six slowly drifting partials over the lowest held note, a resonant body filter
    and a breath layer. Has its own very slow gate so it sustains beyond the notes that feed it. */
class DroneEngine
{
public:
    struct Params
    {
        bool on = true, hold = false;
        float level = 0.35f, tone = 0.4f, motion = 0.5f, spread = 0.6f, attack = 3.0f, release = 6.0f;
        int octave = -12;
    };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        for (int i = 0; i < kPartials; ++i)
        {
            ampDrift[(size_t) i].prepare (sr, 0.1f, (uint32_t) (i * 131 + 7));
            pitchDrift[(size_t) i].prepare (sr, 0.07f, (uint32_t) (i * 977 + 3));
            phase[(size_t) i] = (float) i / (float) kPartials;
        }
        noteSmooth.setTime (250.0f, sr);
        noteSmooth.reset (0.0f);
        body.reset(); bodyR.reset(); breath.reset();
        levelR.reset();
        breathRng.seed (4242u);
        gate = 0.0f;
        latched = -1;
        sampleCount = 0;
    }

    void reseed (uint32_t seedVal) noexcept
    {
        for (int i = 0; i < kPartials; ++i)
        {
            ampDrift[(size_t) i].reseed (seedVal + (uint32_t) (i * 131 + 7));
            pitchDrift[(size_t) i].reseed (seedVal + (uint32_t) (i * 977 + 3));
        }
        breathRng.seed (seedVal ^ 4242u);
        sampleCount = 0;
    }

    /** rootNote < 0 means nothing held. */
    void process (float* outL, float* outR, int n, int rootNote, const Params& p,
                  bool isCtrlTick = true, int ctrlSpan = 0)
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        if (rootNote >= 0) latched = rootNote;
        const bool gateOn = p.on && (rootNote >= 0 || (p.hold && latched >= 0));
        const float tc = gateOn ? p.attack : p.release;
        const float gCoef = std::exp (-1.0f / (std::max (0.02f, tc) * 0.33f * sr));

        if (latched < 0 || (! gateOn && gate < 1.0e-5f)) { gate = gateOn ? gate : 0.0f; if (! gateOn) return; }
        if (noteSmooth.z <= 0.0f) noteSmooth.reset ((float) latched);

        if (isCtrlTick)
        {
            for (int k = 0; k < kPartials; ++k)
            {
                ampDrift[(size_t) k].setRate (0.03f + p.motion * 0.45f, sr);
                pitchDrift[(size_t) k].setRate (0.02f + p.motion * 0.2f, sr);
            }
            if (gate < 1.0e-5f) levelR.snap (p.level * 0.22f);
            else levelR.to (p.level * 0.22f, ctrlSpan);

            body.setup (midiToHz ((float) latched + (float) p.octave) * fastExp2 (1.5f + p.tone * 5.5f), 0.9f + p.tone * 1.5f, sr);
            bodyR.setup (midiToHz ((float) latched + (float) p.octave) * fastExp2 (1.6f + p.tone * 5.5f), 0.9f + p.tone * 1.5f, sr);
            breath.setup (midiToHz ((float) latched + (float) p.octave) * 4.0f, 6.0f, sr);
        }

        const float morph = 0.05f + p.tone * 0.32f;   // sine → triangle → saw region of the bank
        static constexpr float offs[kPartials] { 0.0f, 0.0f, 7.0f, 12.0f, 19.0f, 24.0f };
        static constexpr float wts [kPartials] { 1.0f, 0.8f, 0.42f, 0.5f, 0.22f, 0.16f };
        static constexpr float pans[kPartials] { -0.3f, 0.3f, -0.7f, 0.6f, -0.9f, 0.9f };

        for (int i = 0; i < n; ++i)
        {
            const float root = noteSmooth.process ((float) latched) + (float) p.octave;
            if ((sampleCount++ & 15u) == 0u)
            {
                const float rootHz = midiToHz (root);
                const float lock = std::min (1.0f, (rootHz / 120.0f) * (rootHz / 120.0f));
                for (int k = 0; k < kPartials; ++k)
                {
                    const float det = ((k == 1 ? 1.0f : (k == 0 ? -1.0f : 0.0f)) * p.spread * 0.14f
                                       + pitchDrift[(size_t) k].next (16) * p.motion * 0.09f) * (k < 2 ? lock : 1.0f);
                    const float hz = midiToHz (root + offs[k] + det);
                    if (hz >= 0.48f * sr)
                    {
                        incC[k] = 0.0f; mipC[k] = WavetableBank::kMips - 1;
                        glC[k] = grC[k] = 0.0f;
                        continue;
                    }
                    incC[k] = hz / sr;
                    mipC[k] = WavetableBank::mipFor (hz, sr);
                    const float amp = wts[k] * (1.0f - p.motion * 0.7f * (0.5f + 0.5f * ampDrift[(size_t) k].next (16)));
                    const float pan = pans[k] * p.spread;
                    glC[k] = amp * std::sqrt (0.5f - 0.5f * pan);
                    grC[k] = amp * std::sqrt (0.5f + 0.5f * pan);
                }
            }

            gate = flushDenormal ((gateOn ? 1.0f : 0.0f) + gCoef * (gate - (gateOn ? 1.0f : 0.0f)));
            float l = 0.0f, r = 0.0f;
            for (int k = 0; k < kPartials; ++k)
            {
                auto& ph = phase[(size_t) k];
                const float sm = bank.readMorph (morph, mipC[k], ph);
                ph += incC[k];
                if (ph >= 1.0f) ph -= std::floor (ph);
                if (! (ph >= 0.0f && ph < 1.0f)) ph = 0.0f;
                l += sm * glC[k];
                r += sm * grC[k];
            }

            const float b = breath.bandpass (breathRng.bip()) * (0.2f + 0.8f * p.motion) * 0.6f;
            l = body.lowpass (l + b);
            r = bodyR.lowpass (r + b);

            const float g = gate * levelR.next();
            outL[i] += l * g;
            outR[i] += r * g;
        }
    }

private:
    static constexpr int kPartials = 6;
    float sr = 48000.0f, gate = 0.0f;
    int latched = -1;
    uint64_t sampleCount = 0;
    float incC[kPartials] {}, glC[kPartials] {}, grC[kPartials] {};
    int mipC[kPartials] {};
    std::array<float, kPartials> phase {};
    std::array<Drift, kPartials> ampDrift, pitchDrift;
    OnePole noteSmooth;
    Svf body, bodyR, breath;
    Rng breathRng;
    Ramp levelR;
    const WavetableBank& bank = WavetableBank::get();
};

//==============================================================================
/** (4) Dust & vinyl noise injector: pink tape hiss, crackle pops with ringing, mains hum + harmonics.
    The level breathes with the programme ("dynamic noise floor"). */
class NoiseInjector
{
public:
    struct Params { bool on = true; float hiss = 0.12f, crackle = 0.15f, hum = 0.0f, tone = 0.55f, dyn = 0.5f; float mains = 50.0f; };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        follower.setTimes (10.0f, 350.0f, sr);
        follower.reset();
        for (int c = 0; c < 2; ++c)
        {
            hissHp[c].reset(); toneLp[c].reset(); popLp[c].reset(); ring[c].reset();
            pinkState[c][0] = pinkState[c][1] = pinkState[c][2] = 0.0f;
        }
        hissHp[0].setup (900.0f, 0.6f, sr); hissHp[1].setup (900.0f, 0.6f, sr);
        humDrift.prepare (sr, 0.4f, 99u);
        humPhase = 0.0f;
        hissR.reset(); humR.reset(); crackR.reset();
        rng.seed (0xC0FFEEu);
    }

    void reseed (uint32_t seedVal) noexcept
    {
        humDrift.reseed (seedVal ^ 99u);
        rng.seed (seedVal ^ 0xC0FFEEu);
    }

    void process (float* outL, float* outR, const float* key, int n, const Params& p,
                  bool isCtrlTick = true, int ctrlSpan = 0)
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        if (isCtrlTick)
        {
            hissR.to (p.on ? p.hiss * p.hiss * 0.35f : 0.0f, ctrlSpan);
            humR.to (p.on ? p.hum * p.hum * 0.22f : 0.0f, ctrlSpan);
            crackR.to (p.on ? p.crackle : 0.0f, ctrlSpan);
        }
        if (! p.on && hissR.cur <= 0.0f && humR.cur <= 0.0f && crackR.cur <= 0.0f) return;
        if (isCtrlTick)
        {
            const float toneHz = 1500.0f * fastExp2 (p.tone * 3.6f);
            for (int c = 0; c < 2; ++c) { toneLp[c].setup (toneHz, 0.6f, sr); popLp[c].setup (toneHz, 0.7f, sr); }
        }

        const float popRate = p.crackle * p.crackle * 70.0f / sr;
        const float dustRate = p.crackle * 400.0f / sr;

        for (int i = 0; i < n; ++i)
        {
            const float env = follower.process (key[i]);
            const float dynG = (1.0f - p.dyn) + p.dyn * std::min (1.0f, env * 4.0f);

            float l = 0.0f, r = 0.0f;
            const float hissG = hissR.next(), humG = humR.next(), crackG = crackR.next();

            if (hissG > 0.0f)
            {
                l += toneLp[0].lowpass (hissHp[0].highpass (pink (0))) * hissG;
                r += toneLp[1].lowpass (hissHp[1].highpass (pink (1))) * hissG;
            }

            if (crackG > 0.0f)
            {
                float il = 0.0f, ir = 0.0f;
                if (rng.uni() < popRate)
                {
                    const float a = std::pow (rng.uni(), 2.5f) * 3.0f * (rng.uni() < 0.5f ? -1.0f : 1.0f);
                    const float pan = rng.uni();
                    il = a * std::sqrt (1.0f - pan); ir = a * std::sqrt (pan);
                    const float fc = 700.0f + rng.uni() * 5200.0f;
                    ring[0].setup (fc, 2.5f, sr); ring[1].setup (fc * 1.07f, 2.5f, sr);
                }
                if (rng.uni() < dustRate)
                {
                    const float a = rng.bip() * 0.25f;
                    if (rng.uni() < 0.5f) il += a; else ir += a;
                }
                l += popLp[0].lowpass (il * 0.6f + ring[0].bandpass (il)) * crackG;
                r += popLp[1].lowpass (ir * 0.6f + ring[1].bandpass (ir)) * crackG;
            }

            if (humG > 0.0f)
            {
                humPhase += p.mains * (1.0f + humDrift.next() * 0.002f) / sr;
                if (humPhase >= 1.0f) humPhase -= 1.0f;
                const float t = kTwoPi * humPhase;
                float h = 0.6f * std::sin (t) + 0.3f * std::sin (2.0f * t) + 0.25f * std::sin (3.0f * t) + 0.1f * std::sin (5.0f * t);
                h = fastTanh (1.8f * h + 0.3f) - 0.29f;   // transformer buzz (asymmetric)
                l += h * humG; r += h * humG;
            }

            outL[i] += l * dynG;
            outR[i] += r * dynG;
        }
    }

private:
    float pink (int c) noexcept
    {
        auto& b = pinkState[c];
        const float w = rng.bip();
        b[0] = 0.99765f * b[0] + w * 0.0990460f;
        b[1] = 0.96300f * b[1] + w * 0.2965164f;
        b[2] = 0.57000f * b[2] + w * 1.0526913f;
        return (b[0] + b[1] + b[2] + w * 0.1848f) * 0.25f;
    }

    float sr = 48000.0f, humPhase = 0.0f;
    EnvFollower follower;
    Svf hissHp[2], toneLp[2], popLp[2], ring[2];
    float pinkState[2][3] {};
    Drift humDrift;
    Rng rng;
    Ramp hissR, humR, crackR;
};

//==============================================================================
/** (2) Granular micro-looper. Live grain cloud (pitch/time) on the incoming signal; Freeze or the stutter
    clock captures the last division and loops it through overlapping windowed grains. */
class GranularLooper
{
public:
    struct Params
    {
        bool on = false, freeze = false;
        float sizeMs = 90, density = 18, pitch = 0, stretch = 1, spray = 0.15f, reverse = 0, chance = 0, mix = 0.5f;
        float divBeats = 0.25f;
    };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        const int len = (int) (sr * 6.0f);
        bufL.prepare (len); bufR.prepare (len);
        writeCount = 0; loopActive = false; frozenLatch = false;
        for (auto& g : grains) g.active = false;
        for (size_t i = 0; i < hannT.size(); ++i) { const float sn = std::sin (kPi * (float) i / 512.0f); hannT[i] = sn * sn; }
        rng.seed (777u);
        beatPos = 0.0; lastSlot = -1; spawnCounter = 0.0f; scan = 0.0; loopLen = 64;
        mixR.reset();
    }

    void reseed (uint32_t seedVal) noexcept { rng.seed (seedVal ^ 777u); }

    /** Called once per block with the host transport, before process(). */
    void setTransport (bool playing, double ppq, double bpmIn)
    {
        bpm = bpmIn > 1.0 ? bpmIn : 120.0;
        if (playing) beatPos = ppq;
        hostPlaying = playing;
    }

    bool isLooping() const noexcept { return loopActive; }

    void process (float* l, float* r, int n, const Params& p, bool isCtrlTick = true, int ctrlSpan = 0)
    {
        if (ctrlSpan <= 0) ctrlSpan = n;
        if (! p.on)
        {
            // keep recording so engaging it is instant
            for (int i = 0; i < n; ++i) { bufL.push (l[i]); bufR.push (r[i]); ++writeCount; }
            loopActive = false; frozenLatch = false;
            beatPos += (double) n * bpm / (60.0 * sr);
            return;
        }

        const int grainLen = std::max (16, (int) (p.sizeMs * 0.001f * sr));
        const float rate = semis (p.pitch);
        const float overlap = p.density * p.sizeMs * 0.001f;
        const float norm = 1.0f / std::pow (std::max (0.5f, 0.5f * overlap), 0.7f);
        const double beatInc = bpm / (60.0 * sr);
        const int divSamples = std::max (64, (int) (p.divBeats * 60.0 / bpm * sr));
        if (isCtrlTick) mixR.to (p.mix, ctrlSpan);

        for (int i = 0; i < n; ++i)
        {
            // ── loop capture logic
            if (p.freeze && ! frozenLatch) { engage (divSamples * 2); frozenLatch = true; }
            if (! p.freeze && frozenLatch) { frozenLatch = false; loopActive = false; }

            beatPos += beatInc;
            const auto slot = (int64_t) std::floor (beatPos / (double) p.divBeats);
            if (slot != lastSlot)
            {
                lastSlot = slot;
                if (! frozenLatch)
                {
                    loopActive = false;
                    if (p.chance > 0.0f && rng.uni() < p.chance) engage (divSamples);
                }
            }

            if (! loopActive) { bufL.push (l[i]); bufR.push (r[i]); ++writeCount; }
            else
            {
                scan += (double) p.stretch;
                if (scan >= (double) loopLen) scan -= (double) loopLen;
            }

            // ── grain scheduler
            spawnCounter -= 1.0f;
            if (spawnCounter <= 0.0f)
            {
                spawn (grainLen, rate, p);
                spawnCounter += sr / std::max (0.5f, p.density) * (1.0f + 0.5f * p.spray * rng.bip());
            }

            float wl = 0.0f, wr = 0.0f;
            for (auto& g : grains)
            {
                if (! g.active) continue;
                const float w = hann ((float) g.age / (float) g.len);
                double pos = g.pos;
                if (g.looped)
                {
                    double rel = std::fmod (pos - (double) loopStart, (double) loopLen);
                    if (rel < 0.0) rel += (double) loopLen;
                    pos = (double) loopStart + rel;
                }
                const double idx = pos - (double) (writeCount - bufL.w) ;   // map absolute → ring index
                wl += bufL.readAtHermite (idx) * w * g.gl;
                wr += bufR.readAtHermite (idx) * w * g.gr;
                g.pos += (double) g.rate;
                if (++g.age >= g.len) g.active = false;
            }

            const float mix = mixR.next();
            l[i] = l[i] * (1.0f - mix) + wl * norm * mix;
            r[i] = r[i] * (1.0f - mix) + wr * norm * mix;
        }
    }

private:
    /** Hann window from a 512-point table (replaces a sin() per grain per sample). Filled in prepare(). */
    float hann (float t) const noexcept
    {
        const float x = std::clamp (t, 0.0f, 1.0f) * 512.0f;
        const int i = (int) x;
        return hannT[(size_t) i] + (x - (float) i) * (hannT[(size_t) i + 1] - hannT[(size_t) i]);
    }
    std::array<float, 514> hannT {};

    struct Grain { double pos = 0; float rate = 1, gl = 1, gr = 1; int len = 0, age = 0; bool active = false, looped = false; };

    void engage (int len)
    {
        loopLen = std::min<int64_t> (len, (int64_t) (bufL.buf.size() / 2));
        loopStart = writeCount - loopLen;
        scan = 0.0;
        loopActive = true;
    }

    void spawn (int len, float rate, const Params& p)
    {
        for (auto& g : grains)
        {
            if (g.active) continue;
            const bool rev = rng.uni() < p.reverse;
            g.rate = rev ? -rate : rate;
            g.len = len; g.age = 0; g.active = true;
            const float pan = 0.5f + 0.5f * p.spray * rng.bip();
            g.gl = std::sqrt (1.0f - pan) * 1.41f; g.gr = std::sqrt (pan) * 1.41f;

            if (loopActive)
            {
                g.looped = true;
                g.pos = (double) loopStart + scan + (double) (p.spray * rng.uni() * (float) loopLen);
                if (rev) g.pos += (double) len * rate;
            }
            else
            {
                g.looped = false;
                const double reach = (double) len * std::max (1.0f, rate) + 64.0;
                const double sprayBack = (double) (p.spray * rng.uni() * 0.4f * sr);
                g.pos = (double) writeCount - reach - sprayBack;
                if (rev) g.pos += (double) len * rate;
            }
            return;
        }
    }

    float sr = 48000.0f, spawnCounter = 0.0f;
    DelayLine bufL, bufR;
    int64_t writeCount = 0, loopStart = 0, loopLen = 1, lastSlot = -1;
    double scan = 0.0, beatPos = 0.0, bpm = 120.0;
    bool loopActive = false, frozenLatch = false, hostPlaying = false;
    std::array<Grain, 40> grains;
    Rng rng;
    Ramp mixR;
};
} // namespace vessel::dsp
