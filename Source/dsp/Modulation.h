#pragma once

#include "Common.h"
#include <atomic>

namespace vessel::dsp
{
/** (20) Warped LFO. Eleven shapes (incl. a randomised "glitch" step sequence, a smooth random "wander", a
    sidechain-style "pump" and a user-drawn table), phase warp (exponent 0.25..4 on the phase ramp), free-running
    in Hz or locked to the host's beat grid.

    Movement controls: trigger mode (free / retrigger on note-on / one-shot envelope), start-phase offset,
    fade-in after the trigger, output smoothing (slew) and unipolar output. */
class WarpedLfo
{
public:
    enum Shape { Sine, Triangle, Saw, Square, SampleHold, Glitch, Drawn, RampDown, Wander, Pump, Stairs, kNumShapes };
    enum Mode { Free, Retrig, OneShot };
    static constexpr int kPoints = 32;
    using Table = std::array<std::atomic<float>, kPoints>;

    struct Params
    {
        float rateHz = 1.0f, warp = 0.0f;
        int shape = Sine;
        bool synced = false;
        double beatsPerCycle = 1.0;
        int mode = Free;
        float phaseOffset = 0.0f;   // 0..1 of a cycle
        float fadeSec = 0.0f;       // fade-in after a trigger
        float smooth = 0.0f;        // 0 = off, 1 = ~0.5 s slew
        bool unipolar = false;      // 0..1 instead of −1..1
    };

    explicit WarpedLfo (uint32_t seed = 1) : baseSeed (seed ? seed : 1u), rng (baseSeed) { regenerateGlitch(); wanderA.store (rng.bip()); wanderB.store (rng.bip()); }

    void reseed (uint32_t seed) noexcept
    {
        baseSeed = seed ? seed : 1u;
        rng.seed (baseSeed);
        reset();
    }

    void reset()
    {
        rng.seed (baseSeed);
        phase = 0.0; fadePos = 0.0f; done = false; smoothed = 0.0f; smoothPrimed = false;
        held.store (rng.bip()); wanderA.store (rng.bip()); wanderB.store (rng.bip()); regenerateGlitch();
    }

    /** Note-on. Retrig and One-shot restart the cycle; the fade-in restarts on any retrigger, and in Free mode
        only when the note arrives from silence (so legato playing keeps the movement going). */
    void trigger (int mode, bool fromSilence) noexcept
    {
        if (mode == Retrig || mode == OneShot)
        {
            phase = 0.0; done = false; wrapped();
            fadePos = 0.0f;
        }
        else if (fromSilence) fadePos = 0.0f;
    }

    /** Advance by n samples and return the value at the end of the chunk; the engine ramps linearly from the
        previous chunk's value, so the modulation is piecewise-linear, never stepped. Bipolar −1..1, or 0..1. */
    float advance (int n, float sr, const Params& p, const Table& drawn, bool hostPlaying, double ppq)
    {
        double inc = (double) p.rateHz / (double) sr;
        if (p.synced)
        {
            inc = 1.0 / (p.beatsPerCycle * 60.0 / std::max (1.0, bpm) * (double) sr);
            if (hostPlaying && p.mode == Free)   // retriggered LFOs keep the tempo rate but restart on the note
            {
                const double target = std::fmod (ppq / p.beatsPerCycle, 1.0);
                if (target < phase - 0.5) wrapped();   // follow the transport through a wrap
                phase = target < 0.0 ? target + 1.0 : target;
            }
        }
        if (p.mode != OneShot) done = false;

        double end = phase + inc * (double) n;
        if (p.mode == OneShot && (done || end >= 1.0)) { end = 0.99999; done = true; }

        const float ph = wrapPhase ((float) (end - std::floor (end)) + p.phaseOffset);
        float v = evaluate (ph, p.warp, p.shape, drawn);
        if (p.unipolar) v = 0.5f * (v + 1.0f);

        // fade-in (smoothstep), counted from the last trigger
        if (p.fadeSec > 0.0f)
        {
            fadePos = std::min (fadePos + (float) n / sr, 1.0e6f);
            const float t = std::min (1.0f, fadePos / p.fadeSec);
            v *= t * t * (3.0f - 2.0f * t);
        }

        // output slew: one-pole per chunk, tau 2 ms .. 0.5 s; off at zero so hard shapes stay hard
        if (p.smooth > 0.001f && smoothPrimed)
        {
            const float tau = 0.002f + p.smooth * p.smooth * 0.5f;
            const float a = std::exp (-(float) n / (tau * sr));
            v = v + a * (smoothed - v);
        }
        smoothed = v; smoothPrimed = true;

        if (done) phase = 0.99999;
        else
        {
            phase = end;
            while (phase >= 1.0) { phase -= 1.0; wrapped(); }
        }
        shownPhase = ph;
        out = v;
        return v;
    }

    void setBpm (double b) noexcept { bpm = b; }
    float value() const noexcept { return out; }
    /** Phase actually being read (offset included), for the UI needle. */
    float currentPhase() const noexcept { return shownPhase; }

    static float wrapPhase (float x) noexcept { return x - std::floor (x); }

    /** Shape preview for the UI (no state change except the random shapes use their current random state). */
    float evaluate (float p, float warp, int shape, const Table& drawn) const noexcept
    {
        const float e = std::exp2 (-2.0f * warp);   // warp −1..1 → exponent 4..0.25
        const float w = std::pow (std::clamp (p, 0.0f, 0.99999f), e);
        switch (shape)
        {
            case Sine:       return std::sin (kTwoPi * w);
            case Triangle:   return w < 0.5f ? 4.0f * w - 1.0f : 3.0f - 4.0f * w;
            case Saw:        return 2.0f * w - 1.0f;
            case RampDown:   return 1.0f - 2.0f * w;
            case Square:     return w < 0.5f ? 1.0f : -1.0f;
            case SampleHold: return held.load (std::memory_order_relaxed);
            case Wander:
            {
                const float t = w * w * (3.0f - 2.0f * w);
                return lerp (wanderA.load (std::memory_order_relaxed), wanderB.load (std::memory_order_relaxed), t);
            }
            case Pump:       return 1.0f - 2.0f * std::exp (-5.0f * w);   // ducked on the beat, swelling back
            case Stairs:     return -1.0f + (2.0f / 3.0f) * (float) std::min (3, (int) (w * 4.0f));
            case Glitch:
            {
                const float x = w * (float) kSteps;
                const int i = std::min (kSteps - 1, (int) x);
                const float frac = x - (float) i;
                const float st = steps[(size_t) i].load (std::memory_order_relaxed);
                if (stutter[(size_t) i].load (std::memory_order_relaxed)) return (std::fmod (frac * 4.0f, 1.0f) < 0.5f) ? st : -st;
                return st;
            }
            case Drawn:
            default:
            {
                const float x = w * (float) kPoints;
                const int i = (int) x % kPoints;
                const int j = (i + 1) % kPoints;
                const float f = x - std::floor (x);
                return lerp (drawn[(size_t) i].load (std::memory_order_relaxed), drawn[(size_t) j].load (std::memory_order_relaxed), f);
            }
        }
    }

private:
    static constexpr int kSteps = 8;

    void wrapped()
    {
        held.store (rng.bip(), std::memory_order_relaxed);
        wanderA.store (wanderB.load (std::memory_order_relaxed), std::memory_order_relaxed);
        wanderB.store (rng.bip(), std::memory_order_relaxed);
        regenerateGlitch();
    }

    void regenerateGlitch()
    {
        float prev = 0.0f;
        for (int i = 0; i < kSteps; ++i)
        {
            const float r = rng.uni();
            float v;
            if (r < 0.25f)      v = prev;                 // repeat
            else if (r < 0.38f) v = 0.0f;                 // drop-out
            else                v = rng.bip();
            v = std::copysign (std::pow (std::abs (v), 0.6f), v);   // favour extremes
            steps[(size_t) i].store (v, std::memory_order_relaxed);   // the UI preview reads these concurrently
            stutter[(size_t) i].store (rng.uni() < 0.18f, std::memory_order_relaxed);
            prev = v;
        }
    }

    uint32_t baseSeed = 1u;
    Rng rng;
    double phase = 0.0, bpm = 120.0;
    float out = 0.0f, shownPhase = 0.0f, fadePos = 0.0f, smoothed = 0.0f;
    bool done = false, smoothPrimed = false;
    std::atomic<float> held { 0.0f }, wanderA { 0.0f }, wanderB { 0.0f };
    std::array<std::atomic<float>, kSteps> steps {};
    std::array<std::atomic<bool>, kSteps> stutter {};
};
} // namespace vessel::dsp
