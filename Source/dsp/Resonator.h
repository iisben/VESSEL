#pragma once

#include "Common.h"

namespace vessel::dsp
{
/** (26) Physical-model resonator.

    Eight note-following resonators, played by the keyboard and excited two ways: by the synth bus running
    through them (sympathetic resonance; the noise injector's crackle and hiss make good bows) and by a
    per-note STRIKE (a pluck burst or a mallet hit, louder and brighter with velocity).

    Models
      String    Karplus-Strong waveguide: one-pole damping (BRIGHTNESS), two first-order allpasses in the loop
                for stiffness dispersion (stretched partials, like a piano wire), tuned by the loop's exact
                phase delay at the fundamental so it stays in tune at every brightness and stiffness.
      Tube      the same waveguide with an inverting reflection and half the length: a closed bore with odd
                harmonics only (clarinet-like), with a soft saturating junction.
      Bell      modal: 7 two-pole resonators at church-bell partials (hum 0.5, prime, tierce 1.2, quint 1.5,
      Bar       nominal 2, 2.5, 3), free-free bar partials (1, 2.756, 5.404, 8.933, 13.34) or circular
      Membrane  membrane Bessel partials (1, 1.594, 2.136, 2.296, 2.653, 2.918, 3.156). STIFFNESS warps the
                ratios (exponent 0.97..1.15), BRIGHTNESS sets how much faster the upper modes die.
      Plate     12 modes of a thin rectangular plate (aspect 1.37, simply supported): f(m,n) ~ m^2 + (n/1.37)^2,
                including the near-degenerate (1,4)/(3,1) pair that makes sheet metal shimmer. POSITION moves the
                strike point across the surface; each mode is weighted by its shape there, |sin(m pi x) sin(n pi y)|.
      Bowl      a singing bowl / glass: Rayleigh's thin-ring bending modes, n(n^2-1)/sqrt(n^2+1) for n = 2..7
                (1, 2.83, 5.42, 8.77, 12.9, 17.7), each split into a doublet the way a real bowl's slight
                asymmetry splits it, so the pair beats. STIFFNESS sets the split (0.15..1.35 %), i.e. the beat rate.
      Gong      12 dense shell modes, plus the two things that make a gong a gong: the strike's energy blooms from
                the low modes into the upper ones after the hit (the low modes' level, squared, drives the upper
                modes with noise, so hard hits swell and soft ones stay pure), and the pitch sits slightly high
                while loud and settles as the ring decays (tension modulation).

    DECAY is the T60 while the key is held; DAMPER shortens it after release (0 = rings on, 1 = felt-damped
    in ~80 ms). POSITION is the pluck / strike point: a comb on the waveguide input, mode weights sin(πk·pos)
    for the modal models. INPUT is how hard the bus drives the resonators; each is normalised to unity gain at
    its own resonance, so the sustained level is independent of DECAY.

    It sits before RUIN: the tube can drive it, and the clean sub (which re-joins after RUIN) never passes
    through it, so a resonated patch keeps its sub. Real-time safe: every buffer is allocated in prepare(). */
class Resonator
{
public:
    enum Model { String, Tube, Bell, Bar, Membrane, Plate, Bowl, Gong };
    static constexpr int kSlots = 8;
    static constexpr int kMaxModes = 12;

    struct Params
    {
        bool on = false;
        int model = String;
        float tune = 0.0f, decay = 1.5f, bright = 0.55f, stiff = 0.15f, pos = 0.18f, damper = 0.5f;
        float input = 0.5f, strike = 0.5f, mix = 0.5f;
        float bend = 0.0f, glideMs = 0.0f;
    };

    void prepare (float sampleRate)
    {
        sr = sampleRate;
        maxDelay = (int) std::ceil (sr / kMinHz) + 8;
        for (auto& s : slots)
        {
            s.line.prepare (maxDelay);
            s.burst.assign ((size_t) maxDelay, 0.0f);
        }
        inHist.prepare (maxDelay);
        dcL.prepare (sr); dcR.prepare (sr);
        switchStep = 1.0f / (0.005f * sr);
        bloomCoef = 1.0f - std::exp (-1.0f / (0.01f * sr));
        reset();
    }

    void reseed (uint32_t seedVal) noexcept { rng.seed (seedVal ? seedVal : 0x51A7u); }

    void reset()
    {
        for (auto& s : slots) { clearState (s); s.note = -1; s.quietSamples = 0; s.held = s.sustained = s.active = s.pendingOff = false; s.pending = -1; }
        inHist.clear();
        dcL.reset(); dcR.reset();
        mixR.reset(); inputR.reset();
        switchGain = 1.0f;
        sustain = false; wasOn = true;
    }

    // keyboard (audio thread, from the MIDI handler)
    void noteOn (int note, float vel)
    {
        if (note < 0 || note >= 128) return;
        Slot* t = nullptr;
        for (auto& s : slots) if (s.active && s.note == note) { t = &s; break; }          // re-strike the same string
        if (! t) for (auto& s : slots) if (! s.active && s.pending < 0) { t = &s; break; }
        if (! t)
        {   // steal: prefer released, then the quietest, then the oldest
            float best = -1.0f;
            for (auto& s : slots)
            {
                const float score = (s.held ? 0.0f : 10.0f) + (1.0f - std::min (1.0f, s.env * 20.0f)) + (float) s.age * 1.0e-6f;
                if (score > best) { best = score; t = &s; }
            }
        }
        for (auto& s : slots) ++s.age;
        t->age = 0;

        if (t->active && t->note != note)
        {   // stolen while ringing at another pitch: 5 ms fade, then restart
            t->pending = note; t->pendingVel = vel; t->pendingOff = false;
            return;
        }
        start (*t, note, vel, ! t->active);
    }

    void noteOff (int note)
    {
        if (note < 0 || note >= 128) return;
        for (auto& s : slots)
        {
            if (s.pending == note) s.pendingOff = true;   // very short note: still sounds, released at once
            if (s.note == note && s.held)
            {
                if (sustain) s.sustained = true;
                else s.held = false;
            }
        }
    }

    void setSustain (bool on)
    {
        sustain = on;
        if (! on) for (auto& s : slots) if (s.sustained) { s.sustained = false; s.held = false; }
    }

    /** Mono / legato voice modes: slot 0 follows the voice. restrike = a new attack (not a legato step). */
    void monoNote (int note, float vel, bool restrike)
    {
        if (note < 0 || note >= 128) return;
        auto& s = slots[0];
        for (size_t i = 1; i < slots.size(); ++i) slots[i].held = false;
        if (! s.active) { start (s, note, vel, true); return; }
        s.note = note; s.held = true; s.sustained = false;
        if (restrike) { s.vel = vel; s.wantStrike = true; }
    }
    /** Fresh start (prepare, state load): adopt the model at once instead of fading over to it, so a note in the very
        first block (an offline bounce that starts on a hit) is struck rather than cleared by the switch. */
    void snapModel (int m) noexcept
    {
        if (m == model) return;
        model = m; switchGain = 1.0f;
        for (auto& s : slots) { clearState (s); s.coefDirty = true; }
    }

    void monoRelease() { for (auto& s : slots) s.held = s.sustained = false; }
    void allOff()      { for (auto& s : slots) { s.held = s.sustained = false; s.pending = -1; } }

    // audio
    /** In place on the stereo bus. Called every chunk; with p.on == false it only lets released strings go. */
    void process (float* l, float* r, int n, const Params& p)
    {
        if (! p.on)
        {
            // idle: no resonance is computed, but the input history stays current so switching back on is clean
            for (int i = 0; i < n; ++i) inHist.push (0.5f * (l[i] + r[i]));
            for (auto& s : slots) if (! s.held) { s.active = false; s.pending = -1; }
            mixR.snap (0.0f);
            wasOn = false;
            return;
        }
        if (! wasOn)
        {   // switched back on: held strings restart from silence (a frozen ring would resume out of phase)
            for (auto& s : slots) clearState (s);
            dcL.reset(); dcR.reset();
            wasOn = true;
        }
        mixR.to (p.mix, n);
        inputR.to (2.0f * p.input, n);

        // If the model changed while no resonator slot is currently ringing (e.g. a fresh preset switch
        // immediately followed by a note-on), adopt the new model before updateSlot() so the first strike
        // is not silenced by a 5 ms fade-and-clear. Live switches while ringing still crossfade smoothly.
        if (p.model != model)
        {
            bool anyRinging = false;
            for (const auto& s : slots)
                if (s.active && (s.env > 1.0e-5f || s.peak > 1.0e-5f)) { anyRinging = true; break; }
            if (! anyRinging)
            {
                model = p.model;
                switchGain = 1.0f;
                for (auto& s : slots)
                {
                    const bool keepStrike = s.wantStrike;
                    clearState (s);
                    s.wantStrike = keepStrike;
                }
            }
        }

        const bool waveguide = model == String || model == Tube;
        for (auto& s : slots) if (s.active) updateSlot (s, p, n);

        for (int i = 0; i < n; ++i)
        {
            // model switch: fade the wet path out, swap, fade back in
            if (p.model != model)
            {
                switchGain = std::max (0.0f, switchGain - switchStep);
                if (switchGain <= 0.0f)
                {
                    model = p.model;
                    for (auto& s : slots) { clearState (s); if (s.active) { s.coefDirty = true; } }
                    // coefficients for the new model are computed at the next chunk; stay silent until then,
                    // while keeping inHist current and completing per-chunk steal/activity bookkeeping
                    for (int j = i; j < n; ++j)
                    {
                        inHist.push (0.5f * (l[j] + r[j]));
                        const float m = mixR.next(); inputR.next();
                        l[j] -= m * l[j]; r[j] -= m * r[j];
                    }
                    break;
                }
            }
            else switchGain = std::min (1.0f, switchGain + switchStep);

            const float m = mixR.next(), ig = inputR.next();
            const float x = 0.5f * (l[i] + r[i]);
            inHist.push (x);

            float wl = 0.0f, wr = 0.0f;
            for (auto& s : slots)
            {
                if (! s.active) continue;
                float y;
                if (waveguide) y = tickWaveguide (s, ig, (float) i / (float) n);
                else           y = tickModal (s, ig, x);

                // steal fade
                if (s.pending >= 0)
                {
                    s.fade = std::max (0.0f, s.fade - switchStep);
                    y *= s.fade;
                }
                s.peak = std::max (s.peak, std::abs (y));
                wl += y * s.gl; wr += y * s.gr;
            }

            // DC guard (a lossless string resonates at 0 Hz too), then a gentle safety limiter on the sum
            wl = 2.0f * fastTanh (0.5f * dcL.process (wl)) * switchGain;
            wr = 2.0f * fastTanh (0.5f * dcR.process (wr)) * switchGain;
            l[i] += m * (wl - l[i]);
            r[i] += m * (wr - r[i]);
        }

        // per-chunk bookkeeping: activity, steals (sample-counted so 1-sample buffers behave identically to 64-sample chunks)
        const float envDecay = std::pow (0.7f, (float) n / 64.0f);
        const int quietLimit = std::max (64, (int) (0.02f * sr));
        for (int k = 0; k < kSlots; ++k)
        {
            auto& s = slots[(size_t) k];
            if (! s.active) continue;
            s.env = std::max (s.peak, s.env * envDecay);
            if (s.pending >= 0 && s.fade <= 0.0f)
            {
                const int note = s.pending; const float v = s.pendingVel;
                const bool off = s.pendingOff;
                s.pending = -1; s.pendingOff = false; s.fade = 1.0f;
                start (s, note, v, true);
                if (off) s.held = false;
                continue;
            }
            const bool ringing = s.peak > 1.0e-5f || s.burstPos < s.burstLen || s.strikePending;
            s.quietSamples = (ringing || s.held) ? 0 : s.quietSamples + n;
            if (s.quietSamples > quietLimit) s.active = false;
            s.peak = 0.0f;
        }
    }

    int model = String;   // current (after any switch fade)

    // mode ratios (shared with the editor's partial ruler)
    static constexpr float kBell[7]   { 0.5f, 1.0f, 1.2f, 1.5f, 2.0f, 2.5f, 3.0f };
    static constexpr float kBar[5]    { 1.0f, 2.756f, 5.404f, 8.933f, 13.34f };
    static constexpr float kMemb[7]   { 1.0f, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f };
    static constexpr float kPlate[12] { 1.0f, 2.043f, 2.957f, 3.781f, 4.0f, 5.738f, 6.214f, 6.219f, 7.262f, 8.171f, 9.0f, 9.342f };
    static constexpr int   kPlateM[12] { 1, 1, 2, 1, 2, 2, 1, 3, 3, 2, 3, 1 };
    static constexpr int   kPlateN[12] { 1, 2, 1, 3, 2, 3, 4, 1, 2, 4, 3, 5 };
    static constexpr float kRing[6]   { 1.0f, 2.828f, 5.423f, 8.771f, 12.866f, 17.709f };
    static constexpr float kGong[12]  { 1.0f, 1.52f, 1.97f, 2.43f, 2.91f, 3.37f, 3.94f, 4.41f, 5.02f, 5.63f, 6.29f, 7.01f };

private:
    static constexpr float kMinHz = 20.0f;
    // gong bloom: above a low-mode level of 0.05 (a hard strike; soft ones stay below it), the upper modes are driven
    // with noise up to 0.5 RMS-normalised (each mode's bloom amplitude ~0.17 x its position weight). Tuned in VesselLab "res":
    // a full-velocity hit's upper band peaks ~150-200 ms after the strike, a 0.3-velocity one stays as struck.
    static constexpr float kBloom = 0.5f, kBloomFloor = 0.05f;

    struct Slot
    {
        DelayLine line;
        std::vector<float> burst;
        int burstLen = 0, burstPos = 0;

        int note = -1, pending = -1, quietSamples = 0;
        float vel = 0.0f, pendingVel = 0.0f;
        bool held = false, sustained = false, active = false, wantStrike = false, strikePending = false, coefDirty = true;
        bool pendingOff = false;
        uint32_t age = 0;
        float env = 0.0f, peak = 0.0f, fade = 1.0f, gl = 1.0f, gr = 1.0f;

        // pitch
        float hz = 110.0f, hzTarget = 110.0f;

        // waveguide
        float d0 = 100.0f, d1 = 100.0f;           // read delay at the start / end of the chunk
        float g = 0.0f, inNorm = 0.0f, lpB = 0.0f, apA = 0.0f, combD = 10.0f, combNorm = 1.0f;
        float lp = 0.0f, ap1x = 0.0f, ap1y = 0.0f, ap2x = 0.0f, ap2y = 0.0f;
        float sign = 1.0f;

        // modal
        int modes = 0;
        float bloomK = 0.0f, bloomEnv = 0.0f;
        float nz[kMaxModes] {};
        float c1[kMaxModes] {}, c2[kMaxModes] {}, gin[kMaxModes] {}, sinT[kMaxModes] {}, amp[kMaxModes] {};
        float y1[kMaxModes] {}, y2[kMaxModes] {};
    };

    static void clearState (Slot& s)
    {
        s.line.clear();
        s.lp = s.ap1x = s.ap1y = s.ap2x = s.ap2y = 0.0f;
        std::fill (std::begin (s.y1), std::end (s.y1), 0.0f);
        std::fill (std::begin (s.y2), std::end (s.y2), 0.0f);
        s.bloomEnv = 0.0f;
        s.burstLen = s.burstPos = 0;
        s.env = s.peak = 0.0f;
        s.coefDirty = true;
    }

    void start (Slot& s, int note, float vel, bool fresh)
    {
        if (fresh) clearState (s);
        s.note = note; s.vel = vel; s.held = true; s.sustained = false; s.active = true; s.quietSamples = 0;
        s.wantStrike = true; s.coefDirty = true; s.fade = 1.0f;
        const int idx = (int) (&s - slots.data());
        const float pan = ((idx & 1) ? 1.0f : -1.0f) * (0.15f + 0.1f * (float) (idx >> 1));
        const float th = (pan + 1.0f) * kPi * 0.25f;
        s.gl = std::cos (th) * 1.41421356f; s.gr = std::sin (th) * 1.41421356f;
        s.hz = s.hzTarget = -1.0f;   // set on the next chunk (snaps, no glide from a fresh start)
    }

    static float foldUp (float hz) noexcept { if (! (hz > 0.0f)) return kMinHz; while (hz < kMinHz) hz *= 2.0f; return hz; }

    // phase delays (samples) at w rad/sample
    static float lpDelay (float b, float w) noexcept { return std::atan2 (b * std::sin (w), 1.0f - b * std::cos (w)) / w; }
    static float lpMag (float b, float w) noexcept { return (1.0f - b) / std::sqrt (1.0f - 2.0f * b * std::cos (w) + b * b); }
    static float apDelay (float a, float w) noexcept
    {
        const float num = std::atan2 (-std::sin (w), a + std::cos (w));
        const float den = std::atan2 (-a * std::sin (w), 1.0f + a * std::cos (w));
        float ph = num - den;
        while (ph > 0.0f) ph -= kTwoPi;   // phase lag is negative; unwrap into (−2π, 0]
        return -ph / w;
    }

    void updateSlot (Slot& s, const Params& p, int n)
    {
        // pitch: note + tune + bend, octave-folded above 20 Hz; mono glide
        const float target = foldUp (midiToHz ((float) s.note + p.tune + p.bend));
        s.hzTarget = target;
        if (s.hz <= 0.0f || p.glideMs <= 0.5f) s.hz = target;
        else s.hz = target + std::exp (-(float) n / (0.001f * p.glideMs * sr)) * (s.hz - target);

        const bool releasedNow = ! s.held;
        float t60 = std::max (0.02f, p.decay);
        if (releasedNow)
        {
            const float damped = std::min (t60, 0.08f);
            t60 = std::exp (lerp (std::log (t60), std::log (damped), clamp01 (p.damper)));
        }

        const float velBright = clamp01 (p.bright + (s.vel - 0.6f) * 0.5f);
        const float strikeAmp = p.strike * (0.25f + 0.75f * s.vel);

        if (model == String || model == Tube)
        {
            const bool tube = model == Tube;
            const float hz = std::min (s.hz, sr * (tube ? 0.06f : 0.12f));
            const float loop = tube ? sr / (2.0f * hz) : sr / hz;          // total loop delay in samples
            const float w = kTwoPi * hz / sr;                              // fundamental, rad/sample
            s.sign = tube ? -1.0f : 1.0f;
            s.lpB = 0.03f + (1.0f - p.bright) * 0.82f;
            float a = -0.72f * clamp01 (p.stiff);
            float filt = lpDelay (s.lpB, w) + 2.0f * apDelay (a, w);
            if (loop - filt < 4.0f) { a = 0.0f; filt = lpDelay (s.lpB, w) + 2.0f; }
            s.apA = a;
            const float d = std::max (4.0f, loop - filt);
            const float dMax = (float) maxDelay - 4.0f;
            s.d0 = s.coefDirty ? std::min (d, dMax) : s.d1;
            s.d1 = std::min (d, dMax);

            // Loop gain for the T60 at the fundamental, making up the damping filter's loss there.
            //  Tube: its resonances are the odd harmonics only, the lowest (strongest) being the fundamental, so the
            //        make-up is exact and stable at any brightness.
            //  String: a string also resonates at (and, through any phase lead, near) 0 Hz, where the damping
            //        filter has no loss, so the loop gain must stay below 1. High, dark strings therefore decay
            //        faster than DECAY asks, as real ones do (measured in VesselLab "res").
            const float g = std::pow (10.0f, -3.0f * loop / (sr * t60));
            const float mag = lpMag (s.lpB, w);
            s.g = tube ? std::min (1.5f, g / std::max (0.05f, mag)) : std::min (0.99995f, g / mag);
            s.inNorm = 1.0f - g;

            // pluck position comb on the input: x[n] − x[n − pos·L]
            s.combD = std::max (1.0f, clamp01 (p.pos) * loop);
            s.combNorm = 1.0f / std::max (0.5f, 2.0f * std::sin (kPi * clamp01 (p.pos)));

            if (s.wantStrike && strikeAmp > 1.0e-4f) makeBurst (s, (int) std::lround (loop), strikeAmp, velBright, s.combD, s.combNorm);
            s.wantStrike = false;
        }
        else
        {
            const float warpExp = 1.0f + 0.18f * (clamp01 (p.stiff) - 0.15f);
            const float split = 0.0015f + 0.012f * clamp01 (p.stiff);                    // bowl doublets
            const float tension = model == Gong ? 1.0f + 0.012f * std::min (1.0f, s.env * 4.0f) : 1.0f;
            const float px = 0.05f + 0.9f * clamp01 (p.pos), py = 0.21f + 0.62f * px;   // plate strike point

            int count = 0;
            float modeRatio[kMaxModes] {}, modeW[kMaxModes] {};
            auto add = [&] (float ratio, float w) { modeRatio[count] = ratio; modeW[count] = w; ++count; };
            auto lineW = [&] (int k) { return 0.25f + 0.75f * std::abs (std::sin (kPi * (float) (k + 1) * clamp01 (p.pos))); };
            switch (model)
            {
                case Bell:  for (int k = 0; k < 7; ++k) add (std::pow (kBell[k], warpExp), lineW (k)); break;
                case Bar:   for (int k = 0; k < 5; ++k) add (std::pow (kBar[k], warpExp), lineW (k)); break;
                case Plate:
                    for (int k = 0; k < 12; ++k)
                        add (std::pow (kPlate[k], warpExp),
                             0.15f + 0.85f * std::abs (std::sin (kPi * (float) kPlateM[k] * px) * std::sin (kPi * (float) kPlateN[k] * py)));
                    break;
                case Bowl:
                    for (int k = 0; k < 6; ++k)
                    {
                        // unequal halves (as struck off-axis): the beat swings ~19 dB instead of cancelling fully
                        add (kRing[k] * (1.0f - 0.5f * split), 0.8f * lineW (k));
                        add (kRing[k] * (1.0f + 0.5f * split), 0.6f * lineW (k));
                    }
                    break;
                case Gong:  for (int k = 0; k < 12; ++k) add (std::pow (kGong[k], warpExp), lineW (k)); break;
                default:    for (int k = 0; k < 7; ++k) add (std::pow (kMemb[k], warpExp), lineW (k)); break;
            }
            s.bloomK = model == Gong ? kBloom : 0.0f;

            float strikeNorm = 0.0f;
            float w8[kMaxModes] {};
            s.modes = count;
            for (int k = 0; k < count; ++k)
            {
                const float ratio = modeRatio[k];
                const float f = s.hz * ratio * tension;
                if (f < kMinHz || f > 0.45f * sr) { s.c1[k] = s.c2[k] = s.gin[k] = s.amp[k] = 0.0f; s.sinT[k] = 0.0f; continue; }
                const float th = kTwoPi * f / sr;
                const float dk = ratio < 0.9f ? 1.25f : 1.0f / (1.0f + (1.0f - p.bright) * std::max (0.0f, ratio - 1.0f) * 0.7f);   // the bell's hum rings longest
                const float rad = std::exp (-6.9077553f / (sr * t60 * dk));
                s.c1[k] = 2.0f * rad * std::cos (th);
                s.c2[k] = rad * rad;
                s.sinT[k] = std::sin (th);
                const float posW = modeW[k];
                s.gin[k] = 2.0f * (1.0f - rad) * s.sinT[k] * posW;
                // gong bloom input: scaled by the mode's white-noise RMS gain (AR(2): (1 + c2) / ((1 - c2)((1 + c2)^2 - c1^2))),
                // so the bloom level is a mode amplitude, whatever the decay
                if (model == Gong && k >= 3)
                {
                    const float c1 = s.c1[k], c2 = s.c2[k];
                    const float arGain = (1.0f + c2) / std::max (1.0e-12f, (1.0f - c2) * ((1.0f + c2) * (1.0f + c2) - c1 * c1));
                    s.nz[k] = posW / std::sqrt (std::max (1.0f, arGain));
                }
                else s.nz[k] = 0.0f;
                w8[k] = posW * std::pow (ratio, -(1.0f - velBright) * 1.5f);
                strikeNorm += w8[k] * w8[k];
            }
            if (s.wantStrike && strikeAmp > 1.0e-4f && strikeNorm > 0.0f)
            {
                const float A = strikeAmp * 0.3f / std::sqrt (strikeNorm);
                for (int k = 0; k < count; ++k) s.amp[k] = A * w8[k];
                s.strikePending = true;
            }
            s.wantStrike = false;
            if (s.strikePending)
            {   // y[n] = A r^n sin((n+1)θ): set y[n−1] = A sinθ with y[n−2] = 0, superposed on the ringing state
                for (int k = 0; k < count; ++k) s.y1[k] += s.amp[k] * s.sinT[k];
                s.strikePending = false;
            }
        }
        s.coefDirty = false;
    }

    void makeBurst (Slot& s, int len, float amp, float bright, float combD, float combNorm)
    {
        len = std::clamp (len, 4, (int) s.burst.size());
        const float c = 0.05f + (1.0f - bright) * 0.9f;   // noise lowpass: darker for soft hits
        float z = 0.0f, pk = 1.0e-6f;
        for (int i = 0; i < len; ++i)
        {
            z = (1.0f - c) * rng.bip() + c * z;
            s.burst[(size_t) i] = z;
            pk = std::max (pk, std::abs (z));
        }
        const int D = std::clamp ((int) std::lround (combD), 1, len - 1);
        for (int i = len - 1; i >= D; --i) s.burst[(size_t) i] -= s.burst[(size_t) (i - D)];
        const float g = amp * 0.3f * combNorm / pk;
        for (int i = 0; i < len; ++i) s.burst[(size_t) i] *= g;
        s.burstLen = len; s.burstPos = 0;
    }

    float tickWaveguide (Slot& s, float inGain, float t) noexcept
    {
        const float d = s.d0 + (s.d1 - s.d0) * t;
        const float y = s.line.readHermite (d);

        // damping, then dispersion
        s.lp = flushDenormal ((1.0f - s.lpB) * y + s.lpB * s.lp);
        float z = s.lp;
        const float a = s.apA;
        { const float o = flushDenormal (a * z + s.ap1x - a * s.ap1y); s.ap1x = z; s.ap1y = o; z = o; }
        { const float o = flushDenormal (a * z + s.ap2x - a * s.ap2y); s.ap2x = z; s.ap2y = o; z = o; }

        // excitation: the bus through the position comb, normalised to unity at resonance, plus the strike burst
        const float xin = inHist.readInt (1) - inHist.readLinear (1.0f + s.combD);
        float e = inGain * s.inNorm * s.combNorm * xin;
        if (s.burstPos < s.burstLen) e += s.burst[(size_t) s.burstPos++];

        float wv = e + s.sign * s.g * z;
        if (s.sign < 0.0f) wv = 0.8f * fastTanh (1.25f * wv);   // tube: soft reed junction
        s.line.push (flushDenormal (wv));
        return y;
    }

    float tickModal (Slot& s, float inGain, float x) noexcept
    {
        float sum = 0.0f, low = 0.0f;
        // gong bloom: feed-forward only (low modes -> upper modes), so it cannot run away
        // only above a hard-hit level, and saturating (a loud sympathetic input cannot run it away)
        float bloom = 0.0f;
        if (s.bloomK > 0.0f && s.bloomEnv > kBloomFloor)
        {
            const float t = fastTanh ((s.bloomEnv - kBloomFloor) * 25.0f);
            bloom = s.bloomK * t * t * rng.bip();
        }
        for (int k = 0; k < s.modes; ++k)
        {
            const float y = s.c1[k] * s.y1[k] - s.c2[k] * s.y2[k] + s.gin[k] * inGain * x + s.nz[k] * bloom;
            s.y2[k] = s.y1[k]; s.y1[k] = flushDenormal (y);
            sum += y;
            if (k < 3) low += y;
        }
        if (s.bloomK > 0.0f) s.bloomEnv = flushDenormal (s.bloomEnv + bloomCoef * (std::abs (low) - s.bloomEnv));
        return sum;
    }

    std::array<Slot, kSlots> slots;
    DelayLine inHist;
    DcBlocker dcL, dcR;
    Rng rng { 0x51A7u };
    Ramp mixR, inputR;
    float sr = 48000.0f, switchGain = 1.0f, switchStep = 1.0f / 240.0f, bloomCoef = 0.002f;
    int maxDelay = 2400;
    bool sustain = false, wasOn = true;
};
} // namespace vessel::dsp
