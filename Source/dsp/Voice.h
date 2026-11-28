#pragma once

#include "Common.h"
#include "Wavetable.h"

namespace vessel::dsp
{
//==============================================================================
/** Analog RC exponential ADSR envelope generator with VCA control-voltage
   smoothing.
    - Attack: smooth RC charging curve toward small overshoot (1.008) so 0.5 ms
   attacks reach 90% well inside the window with zero single-sample step
   discontinuities, and 3 ms attacks have smooth curvature.
    - Decay & Release: true exponential RC curves (-60 dB in configured time)
   that preserve smooth release trajectory even when parameters refresh
   mid-release on zero-sustain patches.
    - VCA CV buffer: subtle 1-pole analog CV smoothing removes 1st-derivative
   corners at attack/release edges. */
class Adsr
{
public:
    using Parameters = juce::ADSR::Parameters;

    Adsr() noexcept { recalculateRates(); }

    void setSampleRate(double sr) noexcept
    {
        sr = std::max(1.0, sr);
        if (std::abs(sr - sampleRate) < 1.0e-6) return;
        sampleRate = sr;
        recalculateRates();
    }

    void setParameters(const Parameters& p) noexcept
    {
        if (paramsValid && std::abs(p.attack - parameters.attack) <= 1.0e-9f &&
            std::abs(p.decay - parameters.decay) <= 1.0e-9f && std::abs(p.sustain - parameters.sustain) <= 1.0e-9f &&
            std::abs(p.release - parameters.release) <= 1.0e-9f)
            return;
        parameters = p;
        paramsValid = true;
        recalculateRates();
    }

    bool isActive() const noexcept { return state != State::idle || smoothOut > 1.0e-6f; }
    float currentLevel() const noexcept { return smoothOut; }
    void reset() noexcept
    {
        level = 0.0f;
        smoothOut = 0.0f;
        state = State::idle;
    }

    void noteOn() noexcept
    {
        if (parameters.attack > 0.0f)
            state = State::attack;
        else if (parameters.decay > 0.0f)
        {
            level = 1.0f;
            state = State::decay;
        }
        else
        {
            level = parameters.sustain;
            state = State::sustain;
        }
    }

    void noteOff() noexcept
    {
        if (state == State::idle) return;
        if (parameters.release <= 0.0f)
        {
            reset();
            return;
        }
        releaseFrom = std::max(level, 0.05f);
        relTarget = -kRelEps * releaseFrom;
        state = State::release;
    }

    float getNextSample() noexcept
    {
        switch (state)
        {
        case State::idle:
            level = 0.0f;
            break;
        case State::attack:
            level = kAttTarget + attCoef * (level - kAttTarget);
            if (level >= 1.0f)
            {
                level = 1.0f;
                next();
            }
            break;
        case State::decay:
            level = decTarget + decCoef * (level - decTarget);
            if (level <= parameters.sustain)
            {
                level = parameters.sustain;
                next();
            }
            break;
        case State::sustain:
            level = parameters.sustain;
            break;
        case State::release:
            level = relTarget + relCoef * (level - relTarget);
            if (level <= 0.0f)
            {
                level = 0.0f;
                state = State::idle;
            }
            break;
        }
        smoothOut = level + cvCoef * (smoothOut - level);
        if (state == State::idle && smoothOut < 1.0e-6f) smoothOut = 0.0f;
        return smoothOut;
    }

private:
    enum class State
    {
        idle,
        attack,
        decay,
        sustain,
        release
    };

    static constexpr float kAttTarget = 1.008f;
    static constexpr float kAttRate = 4.8362819f; // -ln(0.008 / 1.008)
    static constexpr float kDecEps = 0.001f;
    static constexpr float kRelEps = 0.001f;
    static constexpr float kExpRate = 6.9087553f; // ln(1001)

    void recalculateRates() noexcept
    {
        attCoef = parameters.attack > 0.0f
                      ? (float)std::exp(-(double)kAttRate / ((double)parameters.attack * sampleRate))
                      : 0.0f;
        decCoef = parameters.decay > 0.0f ? (float)std::exp(-(double)kExpRate / ((double)parameters.decay * sampleRate))
                                          : 0.0f;
        relCoef = parameters.release > 0.0f
                      ? (float)std::exp(-(double)kExpRate / ((double)parameters.release * sampleRate))
                      : 0.0f;

        const double minSeg =
            std::min(std::max(0.0002, (double)parameters.attack), std::max(0.0002, (double)parameters.release));
        const double cvTau = std::clamp(0.25 * minSeg, 0.00008, 0.00075);
        cvCoef = (float)std::exp(-1.0 / (cvTau * sampleRate));

        decTarget = parameters.sustain - kDecEps * std::max(0.05f, 1.0f - parameters.sustain);
        if (state == State::release) relTarget = -kRelEps * std::max(releaseFrom, 0.05f);

        if ((state == State::attack && parameters.attack <= 0.0f) ||
            (state == State::decay && (parameters.decay <= 0.0f || level <= parameters.sustain)) ||
            (state == State::release && parameters.release <= 0.0f))
            next();
    }

    void next() noexcept
    {
        if (state == State::attack)
            state = parameters.decay > 0.0f ? State::decay : State::sustain;
        else if (state == State::decay)
            state = State::sustain;
        else if (state == State::release)
        {
            level = 0.0f;
            state = State::idle;
        }
    }

    State state = State::idle;
    Parameters parameters;
    double sampleRate = 44100.0;
    float level = 0.0f, smoothOut = 0.0f, releaseFrom = 0.0f;
    float attCoef = 0.0f, decCoef = 0.0f, relCoef = 0.0f, cvCoef = 0.0f;
    float decTarget = 0.0f, relTarget = 0.0f;
    bool paramsValid = false;
};

//==============================================================================
/** 64-bit nonlinear CdS photoresistor (VTL5C3 / Buchla 292 style) Vactrol
   Low-Pass Gate model.
    - Fast photo-carrier attack (~1.0 ms shallow, ~4.0 ms deep trap) with zero
      step discontinuities.
    - Sluggish two-stage nonlinear curved release:
        1) Stage 1 (shallow traps): snappy 14..110 ms percussive transient drop.
        2) Stage 2 (deep traps): persistent 60..360 ms base decay slowed further
           as conductance drops (dG/dt ~ -(c0 + c1*G)*G), creating organic
           woody/liquid IDM plucks that still decay cleanly below -120 dBFS.
    - Physically coupled filter cutoff ((G + G_dark)/(1 + G_dark), ~7 octaves)
      and VCA photoresistor voltage-divider gain (G*(1 + k)/(G + k), harmonics
      darken before fundamental fades).
    - Sample-rate & block-size invariant across 44.1-192 kHz. */
struct VactrolLpg
{
    static constexpr double kDarkCond = 0.008;     // ~125:1 CdS dark/light ratio (~7 oct)
    static constexpr double kVcaKnee = 0.22;       // photoresistor VCA divider curve knee
    static constexpr double kPingHoldS = 0.0032;   // 3.2 ms LED strike pulse hold
    static constexpr double kPingTauS = 0.0018;    // 1.8 ms LED turn-off fall
    static constexpr double kAttFastS = 0.0010;    // 1.0 ms fast shallow-trap rise
    static constexpr double kAttSlowS = 0.0040;    // 4.0 ms deep-trap charging rise
    static constexpr double kSmoothTauS = 0.00045; // 0.45 ms C1 corner-rounding smoother

    double pingPeak = 0.0;
    double pingTimerS = 0.0;
    double pingLevel = 0.0;
    double vFast = 0.0;
    double vSlow = 0.0;
    double vOut = 0.0;

    double lastH = -1.0;
    double aPing = 0.9, aAttFast = 0.15, aAttSlow = 0.04, aSmooth = 0.3;

    void reset() noexcept
    {
        pingPeak = pingTimerS = pingLevel = 0.0;
        vFast = vSlow = vOut = 0.0;
    }

    /** Excite the internal LED on note-on (from current state for natural
     * overlapping strikes when not fresh). */
    void strike(double velScale, bool freshVoice) noexcept
    {
        if (freshVoice) reset();
        pingPeak = std::clamp(velScale, 0.05, 1.0);
        pingTimerS = kPingHoldS;
        pingLevel = pingPeak;
    }

    bool isActive() const noexcept
    {
        return pingTimerS > 0.0 || pingLevel > 1.0e-6 || vFast > 1.0e-6 || vSlow > 1.0e-6 || vOut > 1.0e-6;
    }

    /** Advance photoresistor state by exact duration dtSec (100% sample-rate
     * invariant across 44.1-192 kHz). */
    void advance(double dtSec, double gateEnv, double decay01, double strike01) noexcept
    {
        const double d = std::clamp(decay01, 0.0, 1.0);
        const double st = std::clamp(strike01, 0.0, 1.0);
        const double d2 = d * d;
        const double tauFastDec = 0.014 + 0.096 * d2; // 14..110 ms initial knock
        const double tauSlowDec = 0.060 + 0.300 * d2; // 60..360 ms deep-trap tail
        const double wSlow = 0.24 + 0.30 * d;         // 24%..54% deep-trap weight

        const int nSub = (dtSec > 0.00022) ? std::max(2, (int)std::ceil(dtSec / 0.00020)) : 1;
        const double h = dtSec / (double)nSub;
        if (std::abs(h - lastH) > 1.0e-12)
        {
            lastH = h;
            aPing = std::exp(-h / kPingTauS);
            aAttFast = 1.0 - std::exp(-h / kAttFastS);
            aAttSlow = 1.0 - std::exp(-h / kAttSlowS);
            aSmooth = 1.0 - std::exp(-h / kSmoothTauS);
        }
        const double invTauF = h / tauFastDec;
        const double invTauS = h / tauSlowDec;

        for (int s = 0; s < nSub; ++s)
        {
            if (pingTimerS > 0.0)
            {
                pingTimerS = std::max(0.0, pingTimerS - h);
                pingLevel = pingPeak;
            }
            else
            {
                pingLevel = flushDenormal(pingLevel * aPing);
            }

            const double u = std::clamp(std::max(pingLevel * st, gateEnv * (1.0 - st)), 0.0, 1.0);

            // Stage 1 (shallow traps): fast attack, nonlinear conductance-dependent drop
            if (u > vFast)
            {
                vFast += aAttFast * (u - vFast);
            }
            else
            {
                const double decF = std::exp(-invTauF * (0.35 + 0.65 * vFast));
                vFast = u + decF * (vFast - u);
            }

            // Stage 2 (deep traps): slower charging, sluggish concave tail
            const double deepTarget = std::max(u, 0.85 * vFast);
            if (deepTarget > vSlow)
            {
                vSlow += aAttSlow * (deepTarget - vSlow);
            }
            else
            {
                const double decS = std::exp(-invTauS * (0.42 + 0.58 * vSlow));
                vSlow = u + decS * (vSlow - u);
            }

            vFast = flushDenormal(vFast);
            vSlow = flushDenormal(vSlow);
            const double gRaw = std::min(1.0, (1.0 - wSlow) * vFast + wSlow * vSlow * 1.15);
            vOut = flushDenormal(vOut + aSmooth * (gRaw - vOut));
        }

        if (pingTimerS <= 0.0 && pingLevel < 1.0e-6 && gateEnv < 1.0e-6 && vFast < 1.0e-6 && vSlow < 1.0e-6 &&
            vOut < 1.0e-6)
        {
            vFast = vSlow = vOut = pingLevel = 0.0;
        }
    }

    double conductance() const noexcept { return std::clamp(vOut, 0.0, 1.0); }

    /** Physical CdS filter cutoff scaling: f_c(G) / f_max = (G + G_dark) / (1 + G_dark). */
    double filterCutoffMul() const noexcept
    {
        const double g = conductance();
        return (g + kDarkCond) / (1.0 + kDarkCond);
    }

    /** Physical CdS VCA voltage-divider curve: fuller during attack, closes gently after filter. */
    double vcaGain() const noexcept
    {
        const double g = conductance();
        if (g <= 1.0e-7) return 0.0;
        return g * (1.0 + kVcaKnee) / (g + kVcaKnee);
    }
};

//==============================================================================
/** Lane-parallel bank of 64-bit double coupled 4-pole nonlinear ZDF transistor
   ladders (24 dB/oct, running at 4x voice oversampling):
    - Deterministic per-stage +/-0.5% cutoff & gain component mismatch
   (zero-mean sum / unity product per lane to preserve pitch tracking and gain
   staging).
    - Asymmetric differential pair clipping at input + coupled mid-ladder BJT
      stage (exact phi(0)=0, phi'(0)=1 -> subtle even + odd harmonics).
    - Shared I_BIAS current-starvation sag (bass-weighted + broadband energy
      smoothly compresses cutoff and resonance under heavy drive/bass).
    - O(1) strictly bounded ZDF Newton-Raphson solver (max 3 iterations;
      provably F'(y3) >= 1.0 everywhere). Structure-of-arrays for portable
      NEON / SSE2 / AVX2 auto-vectorization. */
template <int L> struct LadderBank
{
    // Asymmetric input differential pair constants (bIn = +0.075, phi(0)=0,
    // phi'(0)=1)
    static constexpr double kBIn = 0.075;
    static constexpr double kTIn0 = 0.07485965928379538;   // tanh(0.075)
    static constexpr double kInvSIn0 = 1.0056355427585296; // 1 / (1 - kTIn0^2)
    static constexpr double kPhiMin = (-1.0 - kTIn0) * kInvSIn0;
    static constexpr double kPhiMax = (1.0 - kTIn0) * kInvSIn0;

    // Coupled mid-ladder BJT pair constants (C2 cubic Hermite, zero-division,
    // zero >3rd-harmonic aliasing; betaM = 0.30, bM = -0.05, exact psi(0)=0,
    // psi'(0)=1, globally monotonic psi'(y) >= 0)
    static constexpr double kBetaM = 0.30;
    static constexpr double kInvBetaM = 1.0 / kBetaM;
    static constexpr double kBM = -0.05;
    static constexpr double kCubicC = 4.0 / 27.0;
    static constexpr double kDerivC = 4.0 / 9.0;
    static constexpr double kHM0 = kBM - kCubicC * kBM * kBM * kBM;
    static constexpr double kInvDHM0 = 1.0 / (1.0 - kDerivC * kBM * kBM);
    static constexpr double kInvBetaDHM0 = kInvBetaM * kInvDHM0;
    static constexpr double kPsiMin = kInvBetaM * (-1.0 - kHM0) * kInvDHM0;
    static constexpr double kPsiMax = kInvBetaM * (1.0 - kHM0) * kInvDHM0;

    // Per-stage +/-0.5% component tolerances (exact zero-sum cutoff, exact
    // zero-sum gain -> unity product)
    static constexpr double kDg0 = 0.0046, kDg1 = -0.0038, kDg2 = 0.0028, kDg3 = -0.0036;
    static constexpr double kDa0 = 0.0038, kDa1 = -0.0042, kDa2 = 0.0040, kDa3 = -0.0036;

    alignas(32) double s0[L]{}, s1[L]{}, s2[L]{}, s3[L]{};
    alignas(32) double sBass[L]{}, sBias[L]{}, drvSag[L]{};
    alignas(32) double G[L]{}, dG[L]{}, k[L]{}, dk[L]{}, drv[L]{}, ddrv[L]{}, isd[L]{}, disd[L]{};
    alignas(32) double tol[L]{};
    double cBassEnv = 0.005873205660390293; // 1 - exp(-2*pi*180 / 192000)
    double cSag = 0.0002083116375176354;    // 1 - exp(-1 / (0.025 * 192000))

    LadderBank() noexcept
    {
        // Deterministic per-lane component tolerance scale in [-1, +1] (never zero)
        for (int l = 0; l < L; ++l)
        {
            uint32_t h = (uint32_t)(l + 1) * 2654435761u ^ 0xA3C59AC3u;
            h ^= h << 13;
            h ^= h >> 17;
            h ^= h << 5;
            const double u = (double)(h >> 8) * (1.0 / 16777216.0); // [0, 1)
            const double mag = 0.55 + 0.45 * u;
            tol[l] = (l & 1) ? -mag : mag;
            drvSag[l] = 1.0;
        }
    }

    void setSampleRate(double osr) noexcept
    {
        const double safeOsr = std::max(osr, 1.0);
        cBassEnv = 1.0 - std::exp(-kTwoPiD * 180.0 / safeOsr);
        cSag = 1.0 - std::exp(-1.0 / (0.025 * safeOsr));
    }

    static double coefGD(double hz, double osr) noexcept
    {
        const double safeOsr = std::max(osr, 1.0);
        hz = std::clamp(hz, 10.0, 0.485 * safeOsr);
        const double g = std::tan(kPiD * hz / safeOsr);
        return g / (1.0 + g);
    }

    static float coefG(float hz, float osr) noexcept { return (float)coefGD((double)hz, (double)osr); }

    void resetLane(int l) noexcept
    {
        s0[l] = s1[l] = s2[l] = s3[l] = 0.0;
        sBass[l] = sBias[l] = 0.0;
    }

    /** Apply shared I_BIAS current-starvation sag from lane's energy state (called
     * on fixed control ticks). */
    inline void applyBiasSag(int l, double& Gt, double& kt) const noexcept
    {
        const double uSag = sBias[l] < 3.6 ? (sBias[l] * (1.0 / 3.6)) : 1.0;
        const double omU = 1.0 - uSag;
        const double sag = 0.85 * (1.0 - omU * omU * omU);
        Gt *= (1.0 - 0.12 * sag);
        kt *= (1.0 - 0.08 * sag);
    }

    void setLane(int l, double Gt, double kt, double driveT, int m, bool snap) noexcept
    {
        const double isdT = 1.0 / std::sqrt(std::max(driveT, 1.0e-6));
        drvSag[l] = 0.5 + 0.5 * driveT * isdT;
        if (snap || (std::abs(Gt - G[l]) <= 0.0006 * std::max(G[l], 1.0e-5) && std::abs(kt - k[l]) <= 0.002 &&
                     std::abs(driveT - drv[l]) <= 0.0005 * std::max(driveT, 1.0)))
        {
            G[l] = Gt;
            k[l] = kt;
            drv[l] = driveT;
            isd[l] = isdT;
            dG[l] = dk[l] = ddrv[l] = disd[l] = 0.0;
            return;
        }
        const double inv = 1.0 / (double)std::max(1, m);
        dG[l] = (Gt - G[l]) * inv;
        dk[l] = (kt - k[l]) * inv;
        ddrv[l] = (driveT - drv[l]) * inv;
        disd[l] = (isdT - isd[l]) * inv;
    }
    void holdLane(int l) noexcept { dG[l] = dk[l] = ddrv[l] = disd[l] = 0.0; }

    /** Single-sample tick across all L lanes (O(1) bounded 3-step NR solver). */
    inline void tick(const float* __restrict in, float* __restrict out) noexcept
    {
        for (int l = 0; l < L; ++l)
        {
            const double g = (G[l] += dG[l]);
            const double kk = (k[l] += dk[l]);
            const double d = (drv[l] += ddrv[l]);
            const double id = (isd[l] += disd[l]);
            const double did = d * id;
            const double xin = (double)in[l];
            const double tl = tol[l];

            // Shared I_BIAS current-starvation sag (zero-division C2 saturator)
            sBass[l] += cBassEnv * (xin - sBass[l]);
            const double eInst = (1.4 * sBass[l] * sBass[l] + 0.35 * xin * xin) * (0.5 + 0.5 * did);
            sBias[l] += cSag * (eInst - sBias[l]);
            const double uSag = sBias[l] < 3.6 ? (sBias[l] * (1.0 / 3.6)) : 1.0;
            const double omU = 1.0 - uSag;
            const double sag = 0.85 * (1.0 - omU * omU * omU);

            const double gEff = g * (1.0 - 0.12 * sag);
            const double kEff = kk * (1.0 - 0.08 * sag);
            const double gStep = gEff * (1.0 - gEff) * tl;
            const double g0 = gEff + gStep * kDg0;
            const double g1 = gEff + gStep * kDg1;
            const double g2 = gEff + gStep * kDg2;
            const double g3 = gEff + gStep * kDg3;

            const double a0 = 1.0 + kDa0 * tl, a1 = 1.0 + kDa1 * tl;
            const double a2 = 1.0 + kDa2 * tl, a3 = 1.0 + kDa3 * tl;
            const double p0 = a0 * g0, p1 = a1 * g1, p2 = a2 * g2, p3 = a3 * g3;

            const double G01 = p1 * p0;
            const double S01 = p1 * (1.0 - g0) * s0[l] + (1.0 - g1) * s1[l];
            const double G23 = p3 * p2;
            const double S23 = p3 * (1.0 - g2) * s2[l] + (1.0 - g3) * s3[l];
            const double G4 = G23 * G01;

            const double x = xin * (1.0 + 0.55 * kEff);
            const double G4did = G4 * did;
            const double G4idk = G4did * kEff;
            double y3 = (G4did * x + G23 * S01 + S23) / (1.0 + G4idk);

            // O(1) strictly bounded 3-step Newton-Raphson (F'(y3) >= 1.0 everywhere)
            for (int it = 0; it < 3; ++it)
            {
                const double thIn = fastTanhLadder(d * (x - kEff * y3) + kBIn);
                const double phi = (thIn - kTIn0) * kInvSIn0;
                const double dphi = (1.0 - thIn * thIn) * kInvSIn0;
                const double y1Try = G01 * (id * phi) + S01;
                const double wm = std::clamp(kBetaM * y1Try + kBM, -1.5, 1.5);
                const double wm2 = wm * wm;
                const double m1Try = (wm * (1.0 - kCubicC * wm2) - kHM0) * kInvBetaDHM0;
                const double dpsi = (1.0 - kDerivC * wm2) * kInvDHM0;
                const double res = y3 - (G23 * m1Try + S23);
                const double jac = 1.0 + G4idk * dpsi * dphi;
                y3 -= res / jac;
            }

            const double thF = fastTanhLadder(d * (x - kEff * y3) + kBIn);
            const double u = id * (thF - kTIn0) * kInvSIn0;

            const double y0 = (a0 * u - s0[l]) * g0 + s0[l];
            s0[l] = flushDenormal(2.0 * y0 - s0[l]);

            const double y1 = (a1 * y0 - s1[l]) * g1 + s1[l];
            s1[l] = flushDenormal(2.0 * y1 - s1[l]);

            const double wmF = std::clamp(kBetaM * y1 + kBM, -1.5, 1.5);
            const double m1 = (wmF * (1.0 - kCubicC * wmF * wmF) - kHM0) * kInvBetaDHM0;

            const double y2 = (a2 * m1 - s2[l]) * g2 + s2[l];
            s2[l] = flushDenormal(2.0 * y2 - s2[l]);

            const double y3Out = (a3 * y2 - s3[l]) * g3 + s3[l];
            s3[l] = flushDenormal(2.0 * y3Out - s3[l]);

            sBass[l] = flushDenormal(sBass[l]);
            sBias[l] = flushDenormal(sBias[l]);
            out[l] = (float)y3Out;
        }
    }

    /** Portable SoA SIMD block processing across 8-lane groups (4 independent
     * NEON/SSE2 double chains interleave across the TPT feedback latency,
     * saturating all FP execution pipes with zero spills). */
    template <int Stride>
    void processSoA(const float (&inBuf)[L][Stride], float (&outBuf)[L][Stride], int m, uint32_t activeMask) noexcept
    {
        static_assert(L % 8 == 0, "LadderBank::processSoA requires L to be a multiple of 8");
        const double cB = cBassEnv;
        const double cS = cSag;

        for (int base = 0; base < L; base += 8)
        {
            if (((activeMask >> base) & 0xFFu) == 0u) continue;

            // Isolated per-sample I_BIAS energy tracker (0 vector register pressure
            // in main ZDF loop, 100% odd-block invariant).
            {
                alignas(32) double lb[8], le[8], lds[8];
                for (int j = 0; j < 8; ++j)
                {
                    const int l = base + j;
                    lb[j] = sBass[l];
                    le[j] = sBias[l];
                    lds[j] = drvSag[l];
                }
                for (int i = 0; i < m; ++i)
                {
                    alignas(32) double x8[8];
                    for (int j = 0; j < 8; ++j) x8[j] = (double)inBuf[base + j][i];
#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
                    for (int j = 0; j < 8; ++j)
                    {
                        const double xr = x8[j];
                        lb[j] += cB * (xr - lb[j]);
                        le[j] += cS * ((1.4 * lb[j] * lb[j] + 0.35 * xr * xr) * lds[j] - le[j]);
                    }
                }
                for (int j = 0; j < 8; ++j)
                {
                    const int l = base + j;
                    sBass[l] = flushDenormal(lb[j]);
                    sBias[l] = flushDenormal(le[j]);
                }
            }

            alignas(32) double ls0[8], ls1[8], ls2[8], ls3[8];
            alignas(32) double lG[8], ldG[8], lk[8], ldk[8], ld[8], ldd[8], lid[8], ldid[8], lTol[8];
            double maxG = 0.0;
            double maxDelta = 0.0;
            for (int j = 0; j < 8; ++j)
            {
                const int l = base + j;
                ls0[j] = s0[l];
                ls1[j] = s1[l];
                ls2[j] = s2[l];
                ls3[j] = s3[l];
                lG[j] = G[l];
                ldG[j] = dG[l];
                lk[j] = k[l];
                ldk[j] = dk[l];
                ld[j] = drv[l];
                ldd[j] = ddrv[l];
                lid[j] = isd[l];
                ldid[j] = disd[l];
                lTol[j] = tol[l];
                maxG = std::max({maxG, lG[j], lG[j] + ldG[j] * (double)m});
                maxDelta =
                    std::max({maxDelta, std::abs(ldG[j]), std::abs(ldk[j]), std::abs(ldd[j]), std::abs(ldid[j])});
            }

            const bool useFastInv = (maxG < 0.24);
            const bool coeffsConst = (maxDelta == 0.0);

            if (useFastInv && coeffsConst)
            {
                // Steady-state hoisted fast path: all mismatched stage coeffs hoisted
                // outside sample loop, 1 division total per sample (thIn), zero spills.
                alignas(32) double cg0[8], cg1[8], cg2[8], cg3[8];
                alignas(32) double cA1[8], cA2[8], cA3[8], cIdA0[8];
                alignas(32) double cS0[8], cS1[8], cS2[8], cS3[8];
                alignas(32) double cG01[8], cG23[8], cG4did[8], cG4idk[8], cInvPiv[8];
                alignas(32) double cBass[8], cD[8], cK[8], cDK[8];
                for (int j = 0; j < 8; ++j)
                {
                    const double g = lG[j], kk = lk[j], d = ld[j], id = lid[j], tl = lTol[j];
                    const double gStep = g * (1.0 - g) * tl;
                    const double g0 = g + gStep * kDg0;
                    const double g1 = g + gStep * kDg1;
                    const double g2 = g + gStep * kDg2;
                    const double g3 = g + gStep * kDg3;
                    const double a0 = 1.0 + kDa0 * tl;
                    const double a1 = 1.0 + kDa1 * tl;
                    const double a2 = 1.0 + kDa2 * tl;
                    const double a3 = 1.0 + kDa3 * tl;
                    const double p0 = a0 * g0, p1 = a1 * g1, p2 = a2 * g2, p3 = a3 * g3;
                    const double G01 = p1 * p0, G23 = p3 * p2, G4 = G23 * G01;
                    const double G4did = G4 * (d * id);
                    const double G4idk = G4did * kk;
                    cg0[j] = g0;
                    cg1[j] = g1;
                    cg2[j] = g2;
                    cg3[j] = g3;
                    cA1[j] = a1;
                    cA2[j] = a2;
                    cA3[j] = a3;
                    cIdA0[j] = id * a0;
                    cS0[j] = p1 * (1.0 - g0);
                    cS1[j] = 1.0 - g1;
                    cS2[j] = p3 * (1.0 - g2);
                    cS3[j] = 1.0 - g3;
                    cG01[j] = G01 * id;
                    cG23[j] = G23;
                    cG4did[j] = G4did;
                    cG4idk[j] = G4idk;
                    cInvPiv[j] = 1.0 - G4idk * (1.0 - G4idk * (1.0 - G4idk));
                    cBass[j] = 1.0 + 0.55 * kk;
                    cD[j] = d;
                    cK[j] = kk;
                    cDK[j] = d * kk;
                }

                for (int i = 0; i < m; ++i)
                {
                    alignas(32) double xin[8];
                    for (int j = 0; j < 8; ++j) xin[j] = (double)inBuf[base + j][i];
                    alignas(32) double yout[8];

#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
                    for (int j = 0; j < 8; ++j)
                    {
                        const double S01 = cS0[j] * ls0[j] + cS1[j] * ls1[j];
                        const double S23 = cS2[j] * ls2[j] + cS3[j] * ls3[j];
                        const double x = xin[j] * cBass[j];
                        const double y3 = (cG4did[j] * x + cG23[j] * S01 + S23) * cInvPiv[j];

                        const double wIn = cD[j] * (x - cK[j] * y3) + kBIn;
                        const double ac = wIn < -6.0 ? -6.0 : (wIn > 6.0 ? 6.0 : wIn);
                        const double a2 = ac * ac;
                        const double thIn =
                            (ac * (34459425.0 + a2 * (4729725.0 + a2 * (135135.0 + a2 * (990.0 + a2))))) /
                            (34459425.0 + a2 * (16216200.0 + a2 * (945945.0 + a2 * (13860.0 + 45.0 * a2))));
                        const double phi = (thIn - kTIn0) * kInvSIn0;
                        const double dphi = (1.0 - thIn * thIn) * kInvSIn0;

                        const double y1_0 = cG01[j] * phi + S01;
                        const double wMRaw = kBetaM * y1_0 + kBM;
                        const double wm = wMRaw < -1.5 ? -1.5 : (wMRaw > 1.5 ? 1.5 : wMRaw);
                        const double wm2 = wm * wm;
                        const double m1_0 = (wm * (1.0 - kCubicC * wm2) - kHM0) * kInvBetaDHM0;
                        const double dpsi = (1.0 - kDerivC * wm2) * kInvDHM0;

                        const double uNr = cG4idk[j] * dpsi * dphi;
                        const double invNr = 1.0 - uNr * (1.0 - uNr * (1.0 - uNr));
                        const double step = (y3 - (cG23[j] * m1_0 + S23)) * invNr;

                        const double rawPhi = phi + dphi * (cDK[j] * step);
                        const double clampedPhi = rawPhi < kPhiMin ? kPhiMin : (rawPhi > kPhiMax ? kPhiMax : rawPhi);
                        const double a0u = cIdA0[j] * clampedPhi;

                        double v = (a0u - ls0[j]) * cg0[j];
                        const double y0 = v + ls0[j];
                        ls0[j] = y0 + v;

                        v = (cA1[j] * y0 - ls1[j]) * cg1[j];
                        const double y1 = v + ls1[j];
                        ls1[j] = y1 + v;

                        const double rawM1 = m1_0 + dpsi * (y1 - y1_0);
                        const double m1 = rawM1 < kPsiMin ? kPsiMin : (rawM1 > kPsiMax ? kPsiMax : rawM1);

                        v = (cA2[j] * m1 - ls2[j]) * cg2[j];
                        const double y2 = v + ls2[j];
                        ls2[j] = y2 + v;

                        v = (cA3[j] * y2 - ls3[j]) * cg3[j];
                        const double y3Out = v + ls3[j];
                        ls3[j] = y3Out + v;
                        yout[j] = y3Out;
                    }

                    for (int j = 0; j < 8; ++j) outBuf[base + j][i] = (float)yout[j];
                }
            }
            else if (useFastInv)
            {
                alignas(32) double cA0[8], cA1[8], cA2[8], cA3[8];
                for (int j = 0; j < 8; ++j)
                {
                    const double tl = lTol[j];
                    cA0[j] = 1.0 + kDa0 * tl;
                    cA1[j] = 1.0 + kDa1 * tl;
                    cA2[j] = 1.0 + kDa2 * tl;
                    cA3[j] = 1.0 + kDa3 * tl;
                }
                for (int i = 0; i < m; ++i)
                {
                    alignas(32) double xin[8];
                    for (int j = 0; j < 8; ++j) xin[j] = (double)inBuf[base + j][i];
                    alignas(32) double yout[8];

#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
                    for (int j = 0; j < 8; ++j)
                    {
                        const double g = (lG[j] += ldG[j]);
                        const double kk = (lk[j] += ldk[j]);
                        const double d = (ld[j] += ldd[j]);
                        const double id = (lid[j] += ldid[j]);

                        const double gStep = g * (1.0 - g) * lTol[j];
                        const double g0 = g + gStep * kDg0;
                        const double g1 = g + gStep * kDg1;
                        const double g2 = g + gStep * kDg2;
                        const double g3 = g + gStep * kDg3;

                        const double p0 = cA0[j] * g0, p1 = cA1[j] * g1;
                        const double p2 = cA2[j] * g2, p3 = cA3[j] * g3;
                        const double G01 = p1 * p0, G23 = p3 * p2, G4 = G23 * G01;
                        const double S01 = p1 * (1.0 - g0) * ls0[j] + (1.0 - g1) * ls1[j];
                        const double S23 = p3 * (1.0 - g2) * ls2[j] + (1.0 - g3) * ls3[j];

                        const double x = xin[j] * (1.0 + 0.55 * kk);
                        const double G4did = G4 * (d * id);
                        const double G4idk = G4did * kk;
                        const double invPiv = 1.0 - G4idk * (1.0 - G4idk * (1.0 - G4idk));
                        const double y3 = (G4did * x + G23 * S01 + S23) * invPiv;

                        const double wIn = d * (x - kk * y3) + kBIn;
                        const double ac = wIn < -6.0 ? -6.0 : (wIn > 6.0 ? 6.0 : wIn);
                        const double a2 = ac * ac;
                        const double thIn =
                            (ac * (34459425.0 + a2 * (4729725.0 + a2 * (135135.0 + a2 * (990.0 + a2))))) /
                            (34459425.0 + a2 * (16216200.0 + a2 * (945945.0 + a2 * (13860.0 + 45.0 * a2))));
                        const double phi = (thIn - kTIn0) * kInvSIn0;
                        const double dphi = (1.0 - thIn * thIn) * kInvSIn0;

                        const double y1_0 = G01 * (id * phi) + S01;
                        const double wMRaw = kBetaM * y1_0 + kBM;
                        const double wm = wMRaw < -1.5 ? -1.5 : (wMRaw > 1.5 ? 1.5 : wMRaw);
                        const double wm2 = wm * wm;
                        const double m1_0 = (wm * (1.0 - kCubicC * wm2) - kHM0) * kInvBetaDHM0;
                        const double dpsi = (1.0 - kDerivC * wm2) * kInvDHM0;

                        const double uNr = G4idk * dpsi * dphi;
                        const double invNr = 1.0 - uNr * (1.0 - uNr * (1.0 - uNr));
                        const double step = (y3 - (G23 * m1_0 + S23)) * invNr;

                        const double rawPhi = phi + dphi * (d * kk * step);
                        const double clampedPhi = rawPhi < kPhiMin ? kPhiMin : (rawPhi > kPhiMax ? kPhiMax : rawPhi);
                        const double a0u = (cA0[j] * id) * clampedPhi;

                        double v = (a0u - ls0[j]) * g0;
                        const double y0 = v + ls0[j];
                        ls0[j] = y0 + v;

                        v = (cA1[j] * y0 - ls1[j]) * g1;
                        const double y1 = v + ls1[j];
                        ls1[j] = y1 + v;

                        const double rawM1 = m1_0 + dpsi * (y1 - y1_0);
                        const double m1 = rawM1 < kPsiMin ? kPsiMin : (rawM1 > kPsiMax ? kPsiMax : rawM1);

                        v = (cA2[j] * m1 - ls2[j]) * g2;
                        const double y2 = v + ls2[j];
                        ls2[j] = y2 + v;

                        v = (cA3[j] * y2 - ls3[j]) * g3;
                        const double y3Out = v + ls3[j];
                        ls3[j] = y3Out + v;
                        yout[j] = y3Out;
                    }

                    for (int j = 0; j < 8; ++j) outBuf[base + j][i] = (float)yout[j];
                }
            }
            else
            {
                // High-cutoff exact path (G >= 0.24): strictly bounded 3-step NR with
                // exact Jacobian division
                alignas(32) double cA0[8], cA1[8], cA2[8], cA3[8];
                for (int j = 0; j < 8; ++j)
                {
                    const double tl = lTol[j];
                    cA0[j] = 1.0 + kDa0 * tl;
                    cA1[j] = 1.0 + kDa1 * tl;
                    cA2[j] = 1.0 + kDa2 * tl;
                    cA3[j] = 1.0 + kDa3 * tl;
                }
                for (int i = 0; i < m; ++i)
                {
                    alignas(32) double xin[8];
                    for (int j = 0; j < 8; ++j) xin[j] = (double)inBuf[base + j][i];
                    alignas(32) double yout[8];

#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
                    for (int j = 0; j < 8; ++j)
                    {
                        const double g = (lG[j] += ldG[j]);
                        const double kk = (lk[j] += ldk[j]);
                        const double d = (ld[j] += ldd[j]);
                        const double id = (lid[j] += ldid[j]);

                        const double gStep = g * (1.0 - g) * lTol[j];
                        const double g0 = g + gStep * kDg0;
                        const double g1 = g + gStep * kDg1;
                        const double g2 = g + gStep * kDg2;
                        const double g3 = g + gStep * kDg3;

                        const double p0 = cA0[j] * g0, p1 = cA1[j] * g1;
                        const double p2 = cA2[j] * g2, p3 = cA3[j] * g3;
                        const double G01 = p1 * p0, G23 = p3 * p2, G4 = G23 * G01;
                        const double S01 = p1 * (1.0 - g0) * ls0[j] + (1.0 - g1) * ls1[j];
                        const double S23 = p3 * (1.0 - g2) * ls2[j] + (1.0 - g3) * ls3[j];

                        const double x = xin[j] * (1.0 + 0.55 * kk);
                        const double G4did = G4 * (d * id);
                        const double G4idk = G4did * kk;
                        double y3 = (G4did * x + G23 * S01 + S23) / (1.0 + G4idk);

                        for (int it = 0; it < 3; ++it)
                        {
                            const double thIn = fastTanhLadder(d * (x - kk * y3) + kBIn);
                            const double phi = (thIn - kTIn0) * kInvSIn0;
                            const double dphi = (1.0 - thIn * thIn) * kInvSIn0;
                            const double y1Try = G01 * (id * phi) + S01;
                            const double wm = std::clamp(kBetaM * y1Try + kBM, -1.5, 1.5);
                            const double wm2 = wm * wm;
                            const double m1Try = (wm * (1.0 - kCubicC * wm2) - kHM0) * kInvBetaDHM0;
                            const double dpsi = (1.0 - kDerivC * wm2) * kInvDHM0;
                            const double res = y3 - (G23 * m1Try + S23);
                            const double jac = 1.0 + G4idk * dpsi * dphi;
                            y3 -= res / jac;
                        }

                        const double thF = fastTanhLadder(d * (x - kk * y3) + kBIn);
                        const double a0u = (cA0[j] * id) * (thF - kTIn0) * kInvSIn0;

                        double v = (a0u - ls0[j]) * g0;
                        const double y0 = v + ls0[j];
                        ls0[j] = y0 + v;

                        v = (cA1[j] * y0 - ls1[j]) * g1;
                        const double y1 = v + ls1[j];
                        ls1[j] = y1 + v;

                        const double wmF = std::clamp(kBetaM * y1 + kBM, -1.5, 1.5);
                        const double m1 = (wmF * (1.0 - kCubicC * wmF * wmF) - kHM0) * kInvBetaDHM0;

                        v = (cA2[j] * m1 - ls2[j]) * g2;
                        const double y2 = v + ls2[j];
                        ls2[j] = y2 + v;

                        v = (cA3[j] * y2 - ls3[j]) * g3;
                        const double y3Out = v + ls3[j];
                        ls3[j] = y3Out + v;
                        yout[j] = y3Out;
                    }

                    for (int j = 0; j < 8; ++j) outBuf[base + j][i] = (float)yout[j];
                }
            }

            for (int j = 0; j < 8; ++j)
            {
                const int l = base + j;
                s0[l] = flushDenormal(ls0[j]);
                s1[l] = flushDenormal(ls1[j]);
                s2[l] = flushDenormal(ls2[j]);
                s3[l] = flushDenormal(ls3[j]);
                G[l] = lG[j];
                k[l] = lk[j];
                drv[l] = ld[j];
                isd[l] = lid[j];
            }
        }
    }

    /** 8-lane SIMD register cache for sample-by-sample closed-loop audio-rate
     * feedback across 8 voices simultaneously (hoisted steady-state fast path +
     * isolated bias tracker -> zero vector spills, zero scalar fallback). */
    struct GroupState8
    {
        alignas(32) double ls0[8]{}, ls1[8]{}, ls2[8]{}, ls3[8]{};
        alignas(32) double lG[8]{}, ldG[8]{}, lk[8]{}, ldk[8]{}, ld[8]{}, ldd[8]{}, lid[8]{}, ldid[8]{}, lTol[8]{};
        alignas(32) double cA0[8]{}, cA1[8]{}, cA2[8]{}, cA3[8]{};
        // Steady-state hoisted constants (used when useFastInv && coeffsConst)
        alignas(32) double cg0[8]{}, cg1[8]{}, cg2[8]{}, cg3[8]{};
        alignas(32) double cIdA0[8]{}, cS0[8]{}, cS1[8]{}, cS2[8]{}, cS3[8]{};
        alignas(32) double cG01[8]{}, cG23[8]{}, cG4did[8]{}, cG4idk[8]{}, cInvPiv[8]{};
        alignas(32) double cBass[8]{}, cD[8]{}, cK[8]{}, cDK[8]{};
        bool useFastInv = true;
        bool coeffsConst = false;

        void load(const LadderBank<L>& bank, int base, int m) noexcept
        {
            double maxG = 0.0;
            double maxDelta = 0.0;
            for (int j = 0; j < 8; ++j)
            {
                const int l = base + j;
                ls0[j] = bank.s0[l];
                ls1[j] = bank.s1[l];
                ls2[j] = bank.s2[l];
                ls3[j] = bank.s3[l];
                lG[j] = bank.G[l];
                ldG[j] = bank.dG[l];
                lk[j] = bank.k[l];
                ldk[j] = bank.dk[l];
                ld[j] = bank.drv[l];
                ldd[j] = bank.ddrv[l];
                lid[j] = bank.isd[l];
                ldid[j] = bank.disd[l];
                lTol[j] = bank.tol[l];
                cA0[j] = 1.0 + kDa0 * lTol[j];
                cA1[j] = 1.0 + kDa1 * lTol[j];
                cA2[j] = 1.0 + kDa2 * lTol[j];
                cA3[j] = 1.0 + kDa3 * lTol[j];
                maxG = std::max({maxG, lG[j], lG[j] + ldG[j] * (double)m});
                maxDelta =
                    std::max({maxDelta, std::abs(ldG[j]), std::abs(ldk[j]), std::abs(ldd[j]), std::abs(ldid[j])});
            }
            useFastInv = (maxG < 0.24);
            coeffsConst = (maxDelta == 0.0);
            if (useFastInv && coeffsConst)
            {
                for (int j = 0; j < 8; ++j)
                {
                    const double g = lG[j], kk = lk[j], d = ld[j], id = lid[j], tl = lTol[j];
                    const double gStep = g * (1.0 - g) * tl;
                    const double g0 = g + gStep * kDg0;
                    const double g1 = g + gStep * kDg1;
                    const double g2 = g + gStep * kDg2;
                    const double g3 = g + gStep * kDg3;
                    const double p0 = cA0[j] * g0, p1 = cA1[j] * g1;
                    const double p2 = cA2[j] * g2, p3 = cA3[j] * g3;
                    const double G01 = p1 * p0, G23 = p3 * p2, G4 = G23 * G01;
                    const double G4did = G4 * (d * id);
                    const double G4idk = G4did * kk;
                    cg0[j] = g0;
                    cg1[j] = g1;
                    cg2[j] = g2;
                    cg3[j] = g3;
                    cIdA0[j] = id * cA0[j];
                    cS0[j] = p1 * (1.0 - g0);
                    cS1[j] = 1.0 - g1;
                    cS2[j] = p3 * (1.0 - g2);
                    cS3[j] = 1.0 - g3;
                    cG01[j] = G01 * id;
                    cG23[j] = G23;
                    cG4did[j] = G4did;
                    cG4idk[j] = G4idk;
                    cInvPiv[j] = 1.0 - G4idk * (1.0 - G4idk * (1.0 - G4idk));
                    cBass[j] = 1.0 + 0.55 * kk;
                    cD[j] = d;
                    cK[j] = kk;
                    cDK[j] = d * kk;
                }
            }
        }

        void resetLane(int j) noexcept { ls0[j] = ls1[j] = ls2[j] = ls3[j] = 0.0; }

        void store(LadderBank<L>& bank, int base) const noexcept
        {
            for (int j = 0; j < 8; ++j)
            {
                const int l = base + j;
                bank.s0[l] = flushDenormal(ls0[j]);
                bank.s1[l] = flushDenormal(ls1[j]);
                bank.s2[l] = flushDenormal(ls2[j]);
                bank.s3[l] = flushDenormal(ls3[j]);
                bank.G[l] = lG[j];
                bank.k[l] = lk[j];
                bank.drv[l] = ld[j];
                bank.isd[l] = lid[j];
            }
        }

        inline void stepFastConst(const double* __restrict xin, double* __restrict yout) noexcept
        {
#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
            for (int j = 0; j < 8; ++j)
            {
                const double S01 = cS0[j] * ls0[j] + cS1[j] * ls1[j];
                const double S23 = cS2[j] * ls2[j] + cS3[j] * ls3[j];
                const double x = xin[j] * cBass[j];
                const double y3 = (cG4did[j] * x + cG23[j] * S01 + S23) * cInvPiv[j];

                const double wIn = cD[j] * (x - cK[j] * y3) + kBIn;
                const double ac = wIn < -6.0 ? -6.0 : (wIn > 6.0 ? 6.0 : wIn);
                const double a2 = ac * ac;
                const double thIn = (ac * (34459425.0 + a2 * (4729725.0 + a2 * (135135.0 + a2 * (990.0 + a2))))) /
                                    (34459425.0 + a2 * (16216200.0 + a2 * (945945.0 + a2 * (13860.0 + 45.0 * a2))));
                const double phi = (thIn - kTIn0) * kInvSIn0;
                const double dphi = (1.0 - thIn * thIn) * kInvSIn0;

                const double y1_0 = cG01[j] * phi + S01;
                const double wMRaw = kBetaM * y1_0 + kBM;
                const double wm = wMRaw < -1.5 ? -1.5 : (wMRaw > 1.5 ? 1.5 : wMRaw);
                const double wm2 = wm * wm;
                const double m1_0 = (wm * (1.0 - kCubicC * wm2) - kHM0) * kInvBetaDHM0;
                const double dpsi = (1.0 - kDerivC * wm2) * kInvDHM0;

                const double uNr = cG4idk[j] * dpsi * dphi;
                const double invNr = 1.0 - uNr * (1.0 - uNr * (1.0 - uNr));
                const double step = (y3 - (cG23[j] * m1_0 + S23)) * invNr;

                const double rawPhi = phi + dphi * (cDK[j] * step);
                const double clampedPhi = rawPhi < kPhiMin ? kPhiMin : (rawPhi > kPhiMax ? kPhiMax : rawPhi);
                const double a0u = cIdA0[j] * clampedPhi;

                double v = (a0u - ls0[j]) * cg0[j];
                const double y0 = v + ls0[j];
                ls0[j] = y0 + v;

                v = (cA1[j] * y0 - ls1[j]) * cg1[j];
                const double y1 = v + ls1[j];
                ls1[j] = y1 + v;

                const double rawM1 = m1_0 + dpsi * (y1 - y1_0);
                const double m1 = rawM1 < kPsiMin ? kPsiMin : (rawM1 > kPsiMax ? kPsiMax : rawM1);

                v = (cA2[j] * m1 - ls2[j]) * cg2[j];
                const double y2 = v + ls2[j];
                ls2[j] = y2 + v;

                v = (cA3[j] * y2 - ls3[j]) * cg3[j];
                const double y3Out = v + ls3[j];
                ls3[j] = y3Out + v;
                yout[j] = y3Out;
            }
        }

        inline void step(const double* __restrict xin, double* __restrict yout) noexcept
        {
            if (useFastInv && coeffsConst)
            {
                stepFastConst(xin, yout);
            }
            else if (useFastInv)
            {
#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
                for (int j = 0; j < 8; ++j)
                {
                    const double g = (lG[j] += ldG[j]);
                    const double kk = (lk[j] += ldk[j]);
                    const double d = (ld[j] += ldd[j]);
                    const double id = (lid[j] += ldid[j]);

                    const double gStep = g * (1.0 - g) * lTol[j];
                    const double g0 = g + gStep * kDg0;
                    const double g1 = g + gStep * kDg1;
                    const double g2 = g + gStep * kDg2;
                    const double g3 = g + gStep * kDg3;

                    const double p0 = cA0[j] * g0, p1 = cA1[j] * g1;
                    const double p2 = cA2[j] * g2, p3 = cA3[j] * g3;
                    const double G01 = p1 * p0, G23 = p3 * p2, G4 = G23 * G01;
                    const double S01 = p1 * (1.0 - g0) * ls0[j] + (1.0 - g1) * ls1[j];
                    const double S23 = p3 * (1.0 - g2) * ls2[j] + (1.0 - g3) * ls3[j];

                    const double x = xin[j] * (1.0 + 0.55 * kk);
                    const double G4did = G4 * (d * id);
                    const double G4idk = G4did * kk;
                    const double invPiv = 1.0 - G4idk * (1.0 - G4idk * (1.0 - G4idk));
                    const double y3 = (G4did * x + G23 * S01 + S23) * invPiv;

                    const double wIn = d * (x - kk * y3) + kBIn;
                    const double ac = wIn < -6.0 ? -6.0 : (wIn > 6.0 ? 6.0 : wIn);
                    const double a2 = ac * ac;
                    const double thIn = (ac * (34459425.0 + a2 * (4729725.0 + a2 * (135135.0 + a2 * (990.0 + a2))))) /
                                        (34459425.0 + a2 * (16216200.0 + a2 * (945945.0 + a2 * (13860.0 + 45.0 * a2))));
                    const double phi = (thIn - kTIn0) * kInvSIn0;
                    const double dphi = (1.0 - thIn * thIn) * kInvSIn0;

                    const double y1_0 = G01 * (id * phi) + S01;
                    const double wMRaw = kBetaM * y1_0 + kBM;
                    const double wm = wMRaw < -1.5 ? -1.5 : (wMRaw > 1.5 ? 1.5 : wMRaw);
                    const double wm2 = wm * wm;
                    const double m1_0 = (wm * (1.0 - kCubicC * wm2) - kHM0) * kInvBetaDHM0;
                    const double dpsi = (1.0 - kDerivC * wm2) * kInvDHM0;

                    const double uNr = G4idk * dpsi * dphi;
                    const double invNr = 1.0 - uNr * (1.0 - uNr * (1.0 - uNr));
                    const double step = (y3 - (G23 * m1_0 + S23)) * invNr;

                    const double rawPhi = phi + dphi * (d * kk * step);
                    const double clampedPhi = rawPhi < kPhiMin ? kPhiMin : (rawPhi > kPhiMax ? kPhiMax : rawPhi);
                    const double a0u = (cA0[j] * id) * clampedPhi;

                    double v = (a0u - ls0[j]) * g0;
                    const double y0 = v + ls0[j];
                    ls0[j] = y0 + v;

                    v = (cA1[j] * y0 - ls1[j]) * g1;
                    const double y1 = v + ls1[j];
                    ls1[j] = y1 + v;

                    const double rawM1 = m1_0 + dpsi * (y1 - y1_0);
                    const double m1 = rawM1 < kPsiMin ? kPsiMin : (rawM1 > kPsiMax ? kPsiMax : rawM1);

                    v = (cA2[j] * m1 - ls2[j]) * g2;
                    const double y2 = v + ls2[j];
                    ls2[j] = y2 + v;

                    v = (cA3[j] * y2 - ls3[j]) * g3;
                    const double y3Out = v + ls3[j];
                    ls3[j] = y3Out + v;
                    yout[j] = y3Out;
                }
            }
            else
            {
#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
                for (int j = 0; j < 8; ++j)
                {
                    const double g = (lG[j] += ldG[j]);
                    const double kk = (lk[j] += ldk[j]);
                    const double d = (ld[j] += ldd[j]);
                    const double id = (lid[j] += ldid[j]);

                    const double gStep = g * (1.0 - g) * lTol[j];
                    const double g0 = g + gStep * kDg0;
                    const double g1 = g + gStep * kDg1;
                    const double g2 = g + gStep * kDg2;
                    const double g3 = g + gStep * kDg3;

                    const double p0 = cA0[j] * g0, p1 = cA1[j] * g1;
                    const double p2 = cA2[j] * g2, p3 = cA3[j] * g3;
                    const double G01 = p1 * p0, G23 = p3 * p2, G4 = G23 * G01;
                    const double S01 = p1 * (1.0 - g0) * ls0[j] + (1.0 - g1) * ls1[j];
                    const double S23 = p3 * (1.0 - g2) * ls2[j] + (1.0 - g3) * ls3[j];

                    const double x = xin[j] * (1.0 + 0.55 * kk);
                    const double G4did = G4 * (d * id);
                    const double G4idk = G4did * kk;
                    double y3 = (G4did * x + G23 * S01 + S23) / (1.0 + G4idk);

                    for (int it = 0; it < 3; ++it)
                    {
                        const double thIn = fastTanhLadder(d * (x - kk * y3) + kBIn);
                        const double phi = (thIn - kTIn0) * kInvSIn0;
                        const double dphi = (1.0 - thIn * thIn) * kInvSIn0;
                        const double y1Try = G01 * (id * phi) + S01;
                        const double wm = std::clamp(kBetaM * y1Try + kBM, -1.5, 1.5);
                        const double wm2 = wm * wm;
                        const double m1Try = (wm * (1.0 - kCubicC * wm2) - kHM0) * kInvBetaDHM0;
                        const double dpsi = (1.0 - kDerivC * wm2) * kInvDHM0;
                        const double res = y3 - (G23 * m1Try + S23);
                        const double jac = 1.0 + G4idk * dpsi * dphi;
                        y3 -= res / jac;
                    }

                    const double thF = fastTanhLadder(d * (x - kk * y3) + kBIn);
                    const double a0u = (cA0[j] * id) * (thF - kTIn0) * kInvSIn0;

                    double v = (a0u - ls0[j]) * g0;
                    const double y0 = v + ls0[j];
                    ls0[j] = y0 + v;

                    v = (cA1[j] * y0 - ls1[j]) * g1;
                    const double y1 = v + ls1[j];
                    ls1[j] = y1 + v;

                    const double wmF = std::clamp(kBetaM * y1 + kBM, -1.5, 1.5);
                    const double m1 = (wmF * (1.0 - kCubicC * wmF * wmF) - kHM0) * kInvBetaDHM0;

                    v = (cA2[j] * m1 - ls2[j]) * g2;
                    const double y2 = v + ls2[j];
                    ls2[j] = y2 + v;

                    v = (cA3[j] * y2 - ls3[j]) * g3;
                    const double y3Out = v + ls3[j];
                    ls3[j] = y3Out + v;
                    yout[j] = y3Out;
                }
            }
        }
    };

    /** Isolated per-group I_BIAS energy tracker for feedback slices (0 vector
     * register pressure inside the ZDF feedback loop). */
    void updateBiasGroup8(int base, const double (*inSeq)[8], int m) noexcept
    {
        alignas(32) double lb[8], le[8], lds[8];
        for (int j = 0; j < 8; ++j)
        {
            const int l = base + j;
            lb[j] = sBass[l];
            le[j] = sBias[l];
            lds[j] = drvSag[l];
        }
        const double cB = cBassEnv, cS = cSag;
        for (int i = 0; i < m; ++i)
        {
#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
            for (int j = 0; j < 8; ++j)
            {
                const double xr = inSeq[i][j];
                lb[j] += cB * (xr - lb[j]);
                le[j] += cS * ((1.4 * lb[j] * lb[j] + 0.35 * xr * xr) * lds[j] - le[j]);
            }
        }
        for (int j = 0; j < 8; ++j)
        {
            const int l = base + j;
            sBass[l] = flushDenormal(lb[j]);
            sBias[l] = flushDenormal(le[j]);
        }
    }
};

//==============================================================================
/** 64-bit SoA oversampled audio-rate feedback conditioning bank (runs inside
   the existing 4x voice oversampling loop; zero nested oversampling, zero added
   latency):
    - 64-bit ~22 Hz DC blocker (exact 0 at DC, unity at Nyquist) prevents DC
      latch-up even under heavy asymmetric filter/drive self-oscillation.
    - Sample-rate-invariant 12 kHz analog return lowpass keeps feedback phase,
      harmonic warmth, and aliasing floor consistent across 44.1-192 kHz.
    - Zero-division odd-symmetric C2 cubic soft-saturation limiter strictly
      bounds return energy in [-kFbLim, +kFbLim] with zero DC injection and
      zero >3rd-order aliasing harmonics. */
template <int L> struct FeedbackBank
{
    static constexpr double kFbLim = 0.85;
    static constexpr double kInvFbLim = 1.0 / kFbLim;
    static constexpr double kCubicC = 4.0 / 27.0;

    alignas(32) double dcX1[L]{}, dcY1[L]{}, lpZ[L]{}, fbSat[L]{};
    alignas(32) double fbG[L]{}, dFbG[L]{};
    double dcR = 0.99928, dcG = 0.99964, lpA = 0.3247;

    void setSampleRate(double osr) noexcept
    {
        const double safeOsr = std::max(osr, 1.0);
        dcR = std::exp(-kTwoPiD * 22.0 / safeOsr);
        dcG = 0.5 * (1.0 + dcR);
        const double fcLp = std::min(12000.0, 0.42 * safeOsr);
        lpA = 1.0 - std::exp(-kTwoPiD * fcLp / safeOsr);
    }

    void resetLane(int l) noexcept { dcX1[l] = dcY1[l] = lpZ[l] = fbSat[l] = 0.0; }

    void setLane(int l, double targetFbG, int m, bool snap) noexcept
    {
        if (snap || std::abs(targetFbG - fbG[l]) <= 1.0e-6)
        {
            fbG[l] = targetFbG;
            dFbG[l] = 0.0;
            return;
        }
        dFbG[l] = (targetFbG - fbG[l]) / (double)std::max(1, m);
    }

    void holdLane(int l) noexcept { dFbG[l] = 0.0; }
};

//==============================================================================
/** 64-bit shared virtual analog power-supply rail droop tracker (20-80 ms RC
   reservoir time constant, running inside the existing 4x voice oversampling
   loop with zero nested oversampling and zero added latency):
    - Tracks uncompensated polyphonic bus current demand (stereo body + heavier
      sub draw) through an asymmetric 64-bit RC reservoir capacitor model.
    - Soft linear-regulator knee stays stiff on quiet solo notes and smoothly
      sags on loud polyphonic chords, gluing voices together with coupled
      headroom compression (-0..2.1 dB), micro-pitch droop (-1..3 cents),
      and filter OTA bias modulation (-0..0.24 oct cutoff, -6% res, +12% drive).
    - Sample-rate & odd-block invariant across 44.1-192 kHz; zero-overhead exact
      bypass when sag == 0. */
struct PowerRailSag
{
    static constexpr double kKnee = 0.46;        // soft regulator current knee
    static constexpr double kMaxGainDrop = 0.22; // max headroom sag (~ -2.16 dB)
    static constexpr double kMaxPitchSt = 0.026; // max shared droop = -2.6 ct (per-voice +/-15% -> -2.2..-3.0 ct)
    static constexpr double kMaxCutOct = 0.24;   // max OTA bias cutoff darkening (-0.24 oct)
    static constexpr double kMaxResDrop = 0.06;  // subtle resonance relaxation under rail droop
    static constexpr double kMaxDriveAdd = 0.12; // subtle input-stage compression warmth

    double osr = 192000.0;
    double lastTimeMs = -1.0;
    double aAtt = 0.00016, aRel = 0.000116, invTauRelOs = 0.000116;
    double railPow = 0.0;
    double droopCur = 0.0;
    double gainCur = 1.0, dGain = 0.0;
    bool active = false;

    void prepare(double oversampledRate) noexcept
    {
        osr = std::max(oversampledRate, 1.0);
        lastTimeMs = -1.0;
        setTimeMs(45.0);
        reset();
    }

    void reset() noexcept
    {
        railPow = 0.0;
        droopCur = 0.0;
        gainCur = 1.0;
        dGain = 0.0;
        active = false;
    }

    void setTimeMs(double timeMs) noexcept
    {
        timeMs = std::clamp(timeMs, 20.0, 80.0);
        if (std::abs(timeMs - lastTimeMs) <= 1.0e-4) return;
        lastTimeMs = timeMs;
        const double tRel = timeMs * 0.001;
        const double tAtt = 0.72 * tRel;
        invTauRelOs = 1.0 / (tRel * osr);
        aAtt = 1.0 - std::exp(-1.0 / (tAtt * osr));
        aRel = 1.0 - std::exp(-invTauRelOs);
    }

    void beginControlBlock(double sagAmt, double timeMs, int ctrlSpanOs, double& outDroopEff) noexcept
    {
        sagAmt = std::clamp(sagAmt, 0.0, 1.0);
        if (sagAmt <= 1.0e-5 && !active)
        {
            droopCur = 0.0;
            gainCur = 1.0;
            dGain = 0.0;
            outDroopEff = 0.0;
            return;
        }
        setTimeMs(timeMs);
        railPow = flushDenormal(railPow);
        if (railPow < 1.0e-12) railPow = 0.0;
        const double iRms = std::sqrt(std::max(0.0, railPow));
        const double u = iRms / kKnee;
        const double u2 = u * u;
        droopCur = u2 / (1.0 + u2);
        const double dEff = sagAmt * droopCur;
        const double gainTarget = 1.0 - kMaxGainDrop * dEff;
        const double invSpan = 1.0 / (double)std::max(1, ctrlSpanOs);
        dGain = (gainTarget - gainCur) * invSpan;
        if (sagAmt <= 1.0e-5 && railPow == 0.0 && std::abs(gainCur - 1.0) < 1.0e-6)
        {
            reset();
            outDroopEff = 0.0;
            return;
        }
        active = true;
        outDroopEff = dEff;
    }

    void processBus(float* __restrict bodyL, float* __restrict bodyR, float* __restrict sub, const float* __restrict pg,
                    int n2) noexcept
    {
        if (!active) return;
        double p = railPow;
        double g = gainCur;
        const double dg = dGain;
        const double att = aAtt;
        const double rel = aRel;
        for (int i = 0; i < n2; ++i)
        {
            g += dg;
            const float gf = (float)g;
            const float bl = bodyL[i] * gf;
            const float br = bodyR[i] * gf;
            const float sb = sub[i] * gf;
            bodyL[i] = bl;
            bodyR[i] = br;
            sub[i] = sb;

            const double pgD = std::max(0.125, (double)pg[i]);
            const double invPgSq = 1.0 / (pgD * pgD);
            const double pBus =
                (0.5 * ((double)bl * (double)bl + (double)br * (double)br) + 1.4 * (double)sb * (double)sb) * invPgSq;
            const double c = (pBus > p) ? att : rel;
            p += c * (pBus - p);
        }
        railPow = flushDenormal(p);
        gainCur = g;
    }

    void decaySilent(int nOs) noexcept
    {
        if (!active || nOs <= 0) return;
        const double alpha = std::exp(-(double)nOs * invTauRelOs);
        railPow = flushDenormal(railPow * alpha);
        gainCur = 1.0 + alpha * (gainCur - 1.0);
        dGain = 0.0;
        if (railPow < 1.0e-12 && std::abs(gainCur - 1.0) < 1.0e-6) reset();
    }
};

struct VoiceParams
{
    // sub (1)
    bool subOn = true;
    int sub1Oct = 1, sub2Oct = 2; // octaves below the played note
    float sub1Level = 0.8f, sub2Level = 0.45f, sub1Shape = 0, sub2Shape = 0.25f, subWeight = 0.3f;
    // wavetable (5)
    bool wtOn = true;
    float wtLevel = 0.55f, morph = 0.35f, pw = 0.5f, pwm = 0.3f, pwmRate = 0.35f, wtTune = 0.0f, drift = 0.25f;
    // ladders (11)
    bool fltOn = true;
    int route = 0;
    float cutA = 1800, resA = 0.35f, cutB = 5000, resB = 0.2f, fDrive = 0.25f, envAmt = 0.35f, keyTrack = 0.3f;
    juce::ADSR::Parameters amp, filt;
    float glideMs = 0.0f, bend = 0.0f, velAmp = 0.65f;
    // FM operator (27)
    bool fmOn = false;
    float fmRatio = 1.0f, fmFine = 0.0f, fmIndex = 0.3f, fmFb = 0.0f, fmEnv = 0.0f, fmDecay = 0.4f, fmKey = 0.0f;
    // Organic voice drift & warmth (0% default)
    float organicAmt = 0.0f;
    // V2 Vactrol LPG & Audio-Rate Feedback (Off / 0% default)
    int lpgMode = 0;
    float lpgDecay = 0.45f, lpgStrike = 0.65f;
    float fbFilter = 0.0f, fbFm = 0.0f;
    // V2 Polyphonic Power-Rail Sag (0% default)
    float railSag = 0.0f, railTimeMs = 45.0f;
};

//==============================================================================
/** One voice of the oversampled engine: sub stack, wavetable oscillator, RC
   envelopes, 4.0 ms raised-cosine voice-steal de-click, and per-voice
   deterministic 1/f organic drift. */
class Voice
{
public:
    void prepare(float baseSampleRate, float osFactor, uint32_t seedVal)
    {
        sr = baseSampleRate;
        osr = osFactor * baseSampleRate;
        baseSeed = seedVal ? seedVal : 101u;
        ampEnv.setSampleRate(osr);
        filtEnv.setSampleRate(osr);
        oscDc.prepare((double)osr, 5.0);
        stealTotal = std::max(32,
                              (int)std::lround(0.0040f * osr)); // 4.0 ms raised-cosine de-click ramp
        fmGateAlpha = 1.0f - std::exp(-32.0f / (0.015f * osr));
        lastFmDecay = -1.0f;
        lastGlideMs = -1.0f;
        reseed(baseSeed);
        reset();
    }

    void reseed(uint32_t seedVal) noexcept
    {
        baseSeed = seedVal ? seedVal : 101u;
        drift.prepare(osr, 0.27f, baseSeed);
        organicDrift.prepare(osr, baseSeed ^ 0x4F5247u);
        rng.seed(baseSeed * 7919u + 13u);
        pwmPhase = rng.uni();
        cutTol = rng.bip() * 0.035f; // deterministic ±3.5% analog filter component tolerance
        // Deterministic per-voice PSU trace impedance sensitivity [0.85..1.15] (0 rng draws)
        uint32_t rh = baseSeed * 2654435761u ^ 0x5A91C3E7u;
        rh ^= rh << 13;
        rh ^= rh >> 17;
        rh ^= rh << 5;
        railPitchSens = 0.85f + 0.30f * ((float)(rh >> 8) * (1.0f / 16777216.0f));
    }

    void reset()
    {
        ampEnv.reset();
        filtEnv.reset();
        vactrol.reset();
        oscDc.reset();
        active = false;
        gateHeld = false;
        note = -1;
        lastEnv = 0.0f;
        fresh = true;
        stealPending = false;
        pendingOff = false;
        stealPos = 0;
        pendingNote = -1;
        pendingVel = 0.0f;
        subPhase = 0.0;
        wt.phase = 0.0;
        fmPhase = 0.0;
        fmModPrev = 0.0;
        fbPmPrev = 0.0;
        fmY1 = fmY2 = fmEnvLevel = fmGate = 0.0f;
        lpgMode = 0;
        vactrolStrikePending = false;
        curFbFmLive = false;
        lastRailPitchSt = 0.0f;
        for (auto* r :
             {&sub1L,   &sub2L,     &sub1S, &sub2S,    &weight, &wtL,       &morph,      &pwBase,  &pwmDepth,
              &cutA,    &cutB,      &resA,  &resB,     &fDrive, &envAmt,    &keyTrack,   &velGain, &bend,
              &wtTuneR, &spreadPan, &fmIdx, &fmEnvAmt, &fmFbR,  &lpgDecayR, &lpgStrikeR, &fbFmR,   &vactrolVca})
            r->reset();
    }

    void start(int midiNote, float vel, bool retrigger, bool snapPitch)
    {
        const bool wasSilent = !active || lastEnv < 1.0e-4f;
        if (!wasSilent && retrigger)
        {
            // Active voice stolen or retriggered: 4.0 ms raised-cosine de-click
            // fade-out before clean restart
            pendingNote = midiNote;
            pendingVel = vel;
            pendingOff = false;
            pendingSnap = snapPitch;
            if (!stealPending)
            {
                stealPending = true;
                stealPos = 0;
            }
            age = 0;
            return;
        }

        stealPending = false;
        pendingOff = false;
        stealPos = 0;
        note = midiNote;
        velocity = vel;
        targetNote = (float)midiNote;
        if (snapPitch || wasSilent) currentNote = targetNote;

        if (wasSilent)
        {
            subPhase = 0.0; // phase-aligned: sub stack and wavetable start together
                            // at a zero crossing
            wt.phase = 0.0;
            oscDc.reset();
            pan = rng.uni() - 0.5f;
            fresh = true; // controls snap to this note's values instead of ramping
                          // from the last note
            ladderSnap = true;
        }
        if (retrigger || wasSilent)
        {
            ampEnv.noteOn();
            filtEnv.noteOn();
            fmEnvLevel = 1.0f;
            vactrolStrikePending = true;
        }
        if (wasSilent)
        {
            fmPhase = 0.0;
            fmModPrev = 0.0;
            fbPmPrev = 0.0;
            fmY1 = fmY2 = 0.0f;
        }
        active = true;
        age = 0;
    }

    void glideTo(int midiNote)
    {
        note = midiNote;
        targetNote = (float)midiNote;
    }
    void setTable(const WavetableBank* b) noexcept { wt.bank = b; }
    void release()
    {
        if (stealPending)
        {
            pendingOff = true;
            return;
        }
        ampEnv.noteOff();
        filtEnv.noteOff();
    }
    bool isActive() const noexcept { return active; }
    bool isFresh() const noexcept { return fresh; }
    int getNote() const noexcept { return stealPending ? pendingNote : note; }
    float getVelocity() const noexcept { return stealPending ? pendingVel : velocity; }
    float envLevel() const noexcept { return lastEnv; }

    bool gateHeld = false;
    uint32_t age = 0;
    bool ladderSnap = false;

    /** Fixed-cadence control update; nCtrlOs = oversampled samples in one control
     * tick (kSub = 32). */
    void control(const VoiceParams& p, int nCtrlOs)
    {
        ampEnv.setParameters(p.amp);
        filtEnv.setParameters(p.filt);

        auto set = [this, nCtrlOs](Ramp& r, float v)
        {
            if (fresh)
                r.snap(v);
            else
                r.to(v, nCtrlOs);
        };
        set(sub1L, p.subOn ? p.sub1Level : 0.0f);
        set(sub2L, p.subOn ? p.sub2Level : 0.0f);
        set(sub1S, p.sub1Shape);
        set(sub2S, p.sub2Shape);
        set(weight, p.subWeight);
        set(wtL, p.wtOn ? p.wtLevel : 0.0f);
        set(morph, p.morph);
        set(pwBase, p.pw);
        set(pwmDepth, p.pwm);
        set(cutA, fastLog2(std::max(10.0f, p.cutA)));
        set(cutB, fastLog2(std::max(10.0f, p.cutB)));
        set(resA, p.resA);
        set(resB, p.resB);
        set(fDrive, p.fDrive);
        set(envAmt, p.envAmt);
        set(keyTrack, p.keyTrack);
        set(velGain, 1.0f - p.velAmp + p.velAmp * velocity);
        set(bend, p.bend);
        set(wtTuneR, p.wtTune);
        set(spreadPan, p.drift);
        set(fbFmR, p.fbFm);

        lpgMode = p.lpgMode;
        if (lpgMode != 0)
        {
            set(lpgDecayR, p.lpgDecay);
            set(lpgStrikeR, p.lpgStrike);
            if (vactrolStrikePending)
            {
                const double velScale = (double)(1.0f - p.velAmp + p.velAmp * velocity);
                vactrol.strike(velScale, fresh);
                if (fresh) vactrolVca.snap(0.0f);
                vactrolStrikePending = false;
            }
        }
        else
        {
            vactrolStrikePending = false;
            vactrolVca.snap(ampEnv.currentLevel());
        }

        const float gateTarget = p.fmOn ? 1.0f : 0.0f;
        if (fresh)
            fmGate = gateTarget;
        else if (std::abs(gateTarget - fmGate) > 1.0e-5f)
            fmGate += (gateTarget - fmGate) * fmGateAlpha;
        if (!p.fmOn && fmGate < 1.0e-3f) fmGate = 0.0f;
        set(fmIdx, p.fmIndex * fmGate);
        set(fmEnvAmt, p.fmEnv * fmGate);
        set(fmFbR, p.fmFb);
        fmOn = fmGate > 0.0f;
        fmRatio = p.fmRatio * semis(p.fmFine * 0.01f);
        if (std::abs(p.fmDecay - lastFmDecay) > 1.0e-6f)
        {
            lastFmDecay = p.fmDecay;
            fmEnvMul = std::exp(-1.0f / (std::max(0.005f, p.fmDecay) * osr));
        }
        fmKey = p.fmKey;

        if (std::abs(p.glideMs - lastGlideMs) > 1.0e-5f)
        {
            lastGlideMs = p.glideMs;
            glideCoef = p.glideMs <= 0.5f ? 0.0f : std::exp(-1.0f / (0.001f * p.glideMs * osr));
        }
        pwmRate = p.pwmRate;
        driftAmt = p.drift;
        organicAmt = p.organicAmt;
        velAmpLast = p.velAmp;
        wtTune = p.wtTune;
        s1Mul = 1 << (3 - p.sub1Oct);
        s2Mul = 1 << (3 - p.sub2Oct);
        fresh = false;
    }

    /** Executed strictly on fixed control-tick boundaries (mCtrl = kSub = 32
     * oversampled samples). */
    void beginControlBlock(int mCtrl, float railDroopEff, float& cA, float& rA, float& cB, float& rB,
                           float& drive) noexcept
    {
        glideM = (glideCoef > 0.0f) ? std::pow(glideCoef, (float)mCtrl) : 0.0f;
        currentNote = targetNote + glideM * (currentNote - targetNote);
        bend.skip(mCtrl);
        wtTuneR.skip(mCtrl);
        noteBent = currentNote + bend.cur;
        const float hz = midiToHz(noteBent);

        // V1 smooth drift + V2 per-voice 1/f pink organic pitch micro-drift
        // (sub-120 Hz bass-locked) + shared polyphonic power-rail micro-pitch droop
        curDrift = drift.next(mCtrl) * driftAmt;
        const float orgWalk = organicAmt > 1.0e-5f ? organicDrift.next(mCtrl) : 0.0f;
        const float subLock = std::min(1.0f, (hz / 120.0f) * (hz / 120.0f));
        const float orgPitchSt = orgWalk * organicAmt * 0.12f * subLock;
        const bool railActive = railDroopEff > 1.0e-6f;
        lastRailPitchSt = railActive ? -(float)PowerRailSag::kMaxPitchSt * railDroopEff * railPitchSens : 0.0f;
        const float railSubMul = railActive ? semis(lastRailPitchSt) : 1.0f;

        const float oscHz = hz * semis(wtTuneR.cur + curDrift * 0.18f + orgPitchSt + lastRailPitchSt);
        curDt = std::clamp((double)oscHz / (double)osr, 1.0e-7, 0.48);
        curDSub = std::clamp((hz * railSubMul) / (8.0f * osr), 0.0f, 0.48f);

        const float pwmStart = pwmLfo;
        pwmPhase += pwmRate * (float)mCtrl / osr;
        pwmPhase -= std::floor(pwmPhase);
        if (!(pwmPhase >= 0.0f && pwmPhase < 1.0f)) pwmPhase = 0.0f;
        pwmLfo = fastSin01(pwmPhase);
        curPwmV = pwmStart;
        curPwmInc = (pwmLfo - pwmStart) / (float)std::max(1, mCtrl);

        curFmLive = fmOn && (fmIdx.cur > 0.0f || fmIdx.inc != 0.0f || (fmEnvAmt.cur > 0.0f && fmEnvLevel > 1.0e-4f));
        curFmDt = std::min(0.45, curDt * (double)fmRatio);
        curFmKeyGain = (double)std::clamp(fastExp2(-fmKey * (noteBent - 48.0f) * (1.0f / 12.0f)), 0.25f, 2.0f);
        curFbFmLive = (fbFmR.cur > 1.0e-5f || fbFmR.inc != 0.0f || std::abs(fbPmPrev) > 1.0e-7);
        curFbFmKeyGain = (double)std::clamp(fastExp2(-0.045f * std::max(0.0f, noteBent - 84.0f)), 0.45f, 1.0f);

        constexpr double kMaxIndex = 12.0 / kTwoPiD;
        float fmSpread = 1.0f;
        if (curFmLive)
        {
            const float xEnd = fmIdx.cur + fmIdx.inc * (float)mCtrl;
            const float xPeak =
                std::min(1.25f, std::max(fmIdx.cur, xEnd) +
                                    std::max(fmEnvAmt.cur, fmEnvAmt.cur + fmEnvAmt.inc * (float)mCtrl) * fmEnvLevel);
            const float fbPeak = std::max(fmFbR.cur, fmFbR.cur + fmFbR.inc * (float)mCtrl);
            fmSpread = 1.0f + (float)(kTwoPiD * kMaxIndex * (double)(xPeak * xPeak) * curFmKeyGain * (double)fmRatio *
                                      (1.0 + 1.5 * (double)fbPeak));
        }
        if (curFbFmLive)
        {
            const float fbFmPeak = clamp01(std::max(fbFmR.cur, fbFmR.cur + fbFmR.inc * (float)mCtrl));
            fmSpread += (float)(kTwoPiD * kFbFmMaxCycles * (double)(fbFmPeak * fbFmPeak) * curFbFmKeyGain * 1.5);
        }

        const float pwMid = std::clamp(pwBase.cur + pwmDepth.cur * 0.44f * pwmLfo + curDrift * 0.03f, 0.03f, 0.97f);
        WavetableOsc::mipBlend(oscHz * fmSpread * std::max(0.5f / pwMid, 0.5f / (1.0f - pwMid)), sr, curMip, curWLower);

        for (auto* r : {&cutA, &cutB, &resA, &resB, &fDrive, &envAmt, &keyTrack}) r->skip(mCtrl);
        const float fenv = filtEnv.currentLevel();
        const float orgCutOct = organicAmt > 1.0e-5f ? organicAmt * (cutTol + 0.14f * orgWalk) : 0.0f;
        const float railCutOct = railActive ? -(float)PowerRailSag::kMaxCutOct * railDroopEff : 0.0f;
        const float octs =
            envAmt.cur * 7.0f * fenv + keyTrack.cur * (noteBent - 60.0f) * (1.0f / 12.0f) + orgCutOct + railCutOct;
        const float hi = 0.485f * sr;
        cA = std::clamp(fastExp2(cutA.cur + octs), 10.0f, hi);
        cB = std::clamp(fastExp2(cutB.cur + octs), 10.0f, hi);
        if (lpgMode != 0)
        {
            lpgDecayR.skip(mCtrl);
            lpgStrikeR.skip(mCtrl);
            vactrol.advance((double)mCtrl / (double)osr, (double)ampEnv.currentLevel(), (double)lpgDecayR.cur,
                            (double)lpgStrikeR.cur);
            if (lpgMode == 1 || lpgMode == 2)
            {
                const float lpgMul = (float)vactrol.filterCutoffMul();
                cA = std::clamp(cA * lpgMul, 18.0f, hi);
                cB = std::clamp(cB * lpgMul, 18.0f, hi);
            }
            if (lpgMode == 1 || lpgMode == 3)
            {
                vactrolVca.to((float)vactrol.vcaGain(), mCtrl);
            }
            else
            {
                vactrolVca.snap(ampEnv.currentLevel());
            }
        }
        const float railResMul = railActive ? (1.0f - (float)PowerRailSag::kMaxResDrop * railDroopEff) : 1.0f;
        rA = clamp01(resA.cur) * railResMul;
        rB = clamp01(resB.cur) * railResMul;
        const float dr = clamp01(fDrive.cur);
        const float railDrvMul = railActive ? (1.0f + (float)PowerRailSag::kMaxDriveAdd * railDroopEff) : 1.0f;
        drive = (1.0f + 6.0f * dr * dr) * railDrvMul;
    }

    bool isFbFmActive() const noexcept { return curFbFmLive; }
    bool isLpgFilterActive() const noexcept { return lpgMode == 1 || lpgMode == 2; }

    /** Compact pre-computed wavetable parameters for closed-loop post-filter ->
     * oscillator TZFM feedback (all envelopes, sub, FM op, and ramps stay
     * register-promoted inside renderPreFbFm; only wt.phase + oscDc run per
     * sample inside the 8-lane feedback loop). */
    struct FbFmSlice
    {
        bool wtLive = false, morphConst = true, fbFmLive = false;
        int stealAt = -1;
        double dtAfterSteal = 0.001;
        WavetableBank::ResolvedSlice resolvedWt{};
        float pwBuf[32]{}, morphBuf[32]{}, wtGainBuf[32]{};
        double tzfmBase[32]{}, fbDepthBuf[32]{};
    };

    /** Register-promoted pre-pass for voices inside an active fbFm closed loop:
     * computes sub, amp/LPG VCA gain, FM operator, PWM, and feedback depth for
     * all m samples while keeping all Voice state in registers. */
    void renderPreFbFm(int m, FbFmSlice& fs, float* __restrict gainOut, float* __restrict sub) noexcept
    {
        constexpr double kMaxIndex = 12.0 / kTwoPiD;
        const bool subLive = sub1L.cur > 0.0f || sub2L.cur > 0.0f || sub1L.inc != 0.0f || sub2L.inc != 0.0f;
        fs.wtLive = wtL.cur > 0.0f || wtL.inc != 0.0f;
        const bool fmLive = curFmLive;
        fs.fbFmLive = curFbFmLive;
        fs.stealAt = -1;
        fs.dtAfterSteal = curDt;
        const bool useVactrolVca = (lpgMode == 1 || lpgMode == 3);

        if (!fmLive)
        {
            fmIdx.skip(m);
            fmEnvAmt.skip(m);
            fmFbR.skip(m);
            if (fmOn && fmEnvLevel > 1.0e-6f) fmEnvLevel *= std::pow(fmEnvMul, (float)m);
        }
        if (!fs.fbFmLive)
        {
            fbFmR.skip(m);
            fbPmPrev = 0.0;
        }

        const bool weightConst = (weight.inc == 0.0f);
        const float wNConst = 1.0f + 3.0f * weight.cur;
        const float invTanhWConst = 1.0f / fastTanh(wNConst);
        fs.morphConst = (morph.inc == 0.0f);
        if (fs.wtLive && fs.morphConst) fs.resolvedWt = wt.bank->resolveSlice(morph.cur, curMip, curWLower);

        for (int i = 0; i < m; ++i)
        {
            float stealW = 1.0f;
            if (stealPending)
            {
                const float u = (float)stealPos / (float)stealTotal;
                stealW = 0.5f * (1.0f + fastCos01(0.5f * u));
                if (++stealPos >= stealTotal)
                {
                    stealPending = false;
                    note = pendingNote;
                    velocity = pendingVel;
                    targetNote = currentNote = (float)pendingNote;
                    noteBent = currentNote + bend.cur;
                    const float hzNew = midiToHz(noteBent);
                    const float oscHzNew = hzNew * semis(wtTuneR.cur + curDrift * 0.18f + lastRailPitchSt);
                    const float railSubNew = lastRailPitchSt != 0.0f ? semis(lastRailPitchSt) : 1.0f;
                    curDt = std::clamp((double)oscHzNew / (double)osr, 1.0e-7, 0.48);
                    curDSub = std::clamp((hzNew * railSubNew) / (8.0f * osr), 0.0f, 0.48f);
                    curFmDt = std::min(0.45, curDt * (double)fmRatio);
                    fs.stealAt = i;
                    fs.dtAfterSteal = curDt;
                    subPhase = 0.0;
                    fmPhase = 0.0;
                    fmModPrev = 0.0;
                    fmY1 = fmY2 = 0.0f;
                    ampEnv.reset();
                    filtEnv.reset();
                    ampEnv.noteOn();
                    filtEnv.noteOn();
                    if (lpgMode != 0)
                    {
                        vactrol.strike((double)(1.0f - velAmpLast + velAmpLast * velocity), true);
                        vactrolVca.snap(0.0f);
                    }
                    fmEnvLevel = 1.0f;
                    velGain.snap(1.0f - velAmpLast + velAmpLast * velocity);
                    if (pendingOff)
                    {
                        ampEnv.noteOff();
                        filtEnv.noteOff();
                        pendingOff = false;
                    }
                    stealW = 0.0f;
                }
            }

            const float env = ampEnv.getNextSample();
            (void)filtEnv.getNextSample();
            const float vcaEnv = useVactrolVca ? vactrolVca.next() : env;
            lastEnv = vcaEnv;
            const float vg = velGain.next();

            const float a = sub1L.next(), b = sub2L.next(), sh1 = sub1S.next(), sh2 = sub2S.next(), w = weight.next();
            if (subLive)
            {
                const float ph1f = (float)(subPhase * (double)s1Mul);
                const float ph2f = (float)(subPhase * (double)s2Mul);
                const float s =
                    subShapeFast(ph1f - (float)(int)ph1f, sh1) * a + subShapeFast(ph2f - (float)(int)ph2f, sh2) * b;
                const float wN = weightConst ? wNConst : (1.0f + 3.0f * w);
                const float invT = weightConst ? invTanhWConst : (1.0f / fastTanh(wN));
                sub[i] += fastTanh(s * wN) * invT * 0.36f * vcaEnv * vg * (stealW * stealW);
            }
            subPhase += (double)curDSub;
            if (subPhase >= 1.0) subPhase -= 1.0;

            const float lvl = wtL.next(), mo = morph.next();
            curPwmV += curPwmInc;
            fs.pwBuf[i] = pwBase.next() + pwmDepth.next() * 0.44f * curPwmV + curDrift * 0.03f;
            fs.morphBuf[i] = mo;
            fs.wtGainBuf[i] = lvl * stealW;
            double tzfm = 0.0;
            if (fmLive)
            {
                const double x = (double)std::min(1.25f, fmIdx.next() + fmEnvAmt.next() * fmEnvLevel);
                fmEnvLevel *= fmEnvMul;
                const float ph = (float)(fmPhase + 0.11 * (double)fmFbR.next() * (double)(fmY1 + fmY2));
                const double y = (double)fastSin01(ph);
                fmY2 = fmY1;
                fmY1 = (float)y;
                const double dMod = y - fmModPrev;
                fmModPrev = y;
                fmPhase += curFmDt;
                if (fmPhase >= 1.0) fmPhase -= 1.0;
                tzfm = kMaxIndex * x * x * curFmKeyGain * dMod;
            }
            fs.tzfmBase[i] = tzfm;
            if (fs.fbFmLive)
            {
                const float fbm = clamp01(fbFmR.next());
                fs.fbDepthBuf[i] = kFbFmMaxCycles * (double)(fbm * fbm) * curFbFmKeyGain;
            }
            else
            {
                fs.fbDepthBuf[i] = 0.0;
            }
            gainOut[i] = vcaEnv * vg * 0.5f * stealW;
        }
    }

    /** Ultra-light 1-sample wavetable + DC-block step inside closed-loop fbFm. */
    inline float stepOscFbFm(const FbFmSlice& fs, int i, double extFbSat, bool& stoleNow) noexcept
    {
        stoleNow = (i == fs.stealAt);
        if (stoleNow)
        {
            wt.phase = 0.0;
            oscDc.reset();
            fbPmPrev = 0.0;
        }
        if (!fs.wtLive) return 0.0f;
        double tzfm = fs.tzfmBase[i];
        if (fs.fbFmLive)
        {
            const double pmTarget = fs.fbDepthBuf[i] * extFbSat;
            tzfm += (pmTarget - fbPmPrev);
            fbPmPrev = pmTarget;
        }
        const double dtNow = (fs.stealAt >= 0 && i >= fs.stealAt) ? fs.dtAfterSteal : curDt;
        const float rawOsc = fs.morphConst ? wt.processResolved(dtNow, fs.resolvedWt, fs.pwBuf[i], tzfm)
                                           : wt.process(dtNow, curMip, curWLower, fs.morphBuf[i], fs.pwBuf[i], tzfm);
        return oscDc.process(rawOsc) * fs.wtGainBuf[i];
    }

    /** Renders m oversampled samples into contiguous SoA buffers oscOut[m],
     * gainOut[m], and sums into sub[m] (zero-overhead baseline path when
     * fbFm==0). */
    void renderSamples(int m, float* __restrict oscOut, float* __restrict gainOut, float* __restrict sub) noexcept
    {
        constexpr double kMaxIndex = 12.0 / kTwoPiD;
        const bool subLive = sub1L.cur > 0.0f || sub2L.cur > 0.0f || sub1L.inc != 0.0f || sub2L.inc != 0.0f;
        const bool wtLive = wtL.cur > 0.0f || wtL.inc != 0.0f;
        const bool fmLive = curFmLive;
        const bool useVactrolVca = (lpgMode == 1 || lpgMode == 3);

        if (!fmLive)
        {
            fmIdx.skip(m);
            fmEnvAmt.skip(m);
            fmFbR.skip(m);
            if (fmOn && fmEnvLevel > 1.0e-6f) fmEnvLevel *= std::pow(fmEnvMul, (float)m);
        }
        fbFmR.skip(m);
        fbPmPrev = 0.0;

        const bool weightConst = (weight.inc == 0.0f);
        const float wNConst = 1.0f + 3.0f * weight.cur;
        const float invTanhWConst = 1.0f / fastTanh(wNConst);
        const bool morphConst = (morph.inc == 0.0f);
        const auto resolvedWt = (wtLive && morphConst) ? wt.bank->resolveSlice(morph.cur, curMip, curWLower)
                                                       : WavetableBank::ResolvedSlice{};

        for (int i = 0; i < m; ++i)
        {
            float stealW = 1.0f;
            if (stealPending)
            {
                const float u = (float)stealPos / (float)stealTotal;
                stealW = 0.5f * (1.0f + fastCos01(0.5f * u)); // C1 raised-cosine 1 -> 0
                if (++stealPos >= stealTotal)
                {
                    stealPending = false;
                    note = pendingNote;
                    velocity = pendingVel;
                    targetNote = currentNote = (float)pendingNote;
                    noteBent = currentNote + bend.cur;
                    const float hzNew = midiToHz(noteBent);
                    const float oscHzNew = hzNew * semis(wtTuneR.cur + curDrift * 0.18f + lastRailPitchSt);
                    const float railSubNew = lastRailPitchSt != 0.0f ? semis(lastRailPitchSt) : 1.0f;
                    curDt = std::clamp((double)oscHzNew / (double)osr, 1.0e-7, 0.48);
                    curDSub = std::clamp((hzNew * railSubNew) / (8.0f * osr), 0.0f, 0.48f);
                    curFmDt = std::min(0.45, curDt * (double)fmRatio);
                    subPhase = 0.0;
                    wt.phase = 0.0;
                    fmPhase = 0.0;
                    fmModPrev = 0.0;
                    fmY1 = fmY2 = 0.0f;
                    oscDc.reset();
                    ampEnv.reset();
                    filtEnv.reset();
                    ampEnv.noteOn();
                    filtEnv.noteOn();
                    if (lpgMode != 0)
                    {
                        vactrol.strike((double)(1.0f - velAmpLast + velAmpLast * velocity), true);
                        vactrolVca.snap(0.0f);
                    }
                    fmEnvLevel = 1.0f;
                    velGain.snap(1.0f - velAmpLast + velAmpLast * velocity);
                    if (pendingOff)
                    {
                        ampEnv.noteOff();
                        filtEnv.noteOff();
                        pendingOff = false;
                    }
                    stealW = 0.0f;
                }
            }

            const float env = ampEnv.getNextSample();
            (void)filtEnv.getNextSample();
            const float vcaEnv = useVactrolVca ? vactrolVca.next() : env;
            lastEnv = vcaEnv;
            const float vg = velGain.next();

            const float a = sub1L.next(), b = sub2L.next(), sh1 = sub1S.next(), sh2 = sub2S.next(), w = weight.next();
            if (subLive)
            {
                const float ph1f = (float)(subPhase * (double)s1Mul);
                const float ph2f = (float)(subPhase * (double)s2Mul);
                const float s =
                    subShapeFast(ph1f - (float)(int)ph1f, sh1) * a + subShapeFast(ph2f - (float)(int)ph2f, sh2) * b;
                const float wN = weightConst ? wNConst : (1.0f + 3.0f * w);
                const float invT = weightConst ? invTanhWConst : (1.0f / fastTanh(wN));
                sub[i] += fastTanh(s * wN) * invT * 0.36f * vcaEnv * vg * (stealW * stealW);
            }
            subPhase += (double)curDSub;
            if (subPhase >= 1.0) subPhase -= 1.0;

            const float lvl = wtL.next(), mo = morph.next();
            curPwmV += curPwmInc;
            const float pw = pwBase.next() + pwmDepth.next() * 0.44f * curPwmV + curDrift * 0.03f;
            double tzfm = 0.0;
            if (fmLive)
            {
                const double x = (double)std::min(1.25f, fmIdx.next() + fmEnvAmt.next() * fmEnvLevel);
                fmEnvLevel *= fmEnvMul;
                const float ph = (float)(fmPhase + 0.11 * (double)fmFbR.next() * (double)(fmY1 + fmY2));
                const double y = (double)fastSin01(ph);
                fmY2 = fmY1;
                fmY1 = (float)y;
                const double dMod = y - fmModPrev;
                fmModPrev = y;
                fmPhase += curFmDt;
                if (fmPhase >= 1.0) fmPhase -= 1.0;
                tzfm = kMaxIndex * x * x * curFmKeyGain * dMod;
            }
            const float rawOsc = wtLive ? (morphConst ? wt.processResolved(curDt, resolvedWt, pw, tzfm)
                                                      : wt.process(curDt, curMip, curWLower, mo, pw, tzfm))
                                        : 0.0f;
            oscOut[i] = wtLive ? oscDc.process(rawOsc) * lvl * stealW : 0.0f;
            gainOut[i] = vcaEnv * vg * 0.5f * stealW;
        }
    }

    void pans(float& l, float& r) const noexcept
    {
        const float p = pan * spreadPan.cur;
        l = std::sqrt(0.5f - 0.5f * p);
        r = std::sqrt(0.5f + 0.5f * p);
    }

    void advanceAge(uint32_t baseSamples) noexcept
    {
        age += baseSamples;
        const bool lpgHolding = (lpgMode == 1 || lpgMode == 3) && vactrol.isActive();
        if (!ampEnv.isActive() && !lpgHolding && !stealPending)
        {
            active = false;
            note = -1;
            lastEnv = 0.0f;
            fbPmPrev = 0.0;
        }
    }

private:
    static constexpr double kFbFmMaxCycles = 2.4 / kTwoPiD;
    static inline float subShapeFast(float ph, float shape) noexcept
    {
        const float s = fastSin01Unit(ph);
        if (shape <= 1.0e-4f) return s;
        const float tri = ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
        if (shape <= 0.5f) return lerp(s, tri, shape * 2.0f);
        return lerp(tri, fastTanh(4.0f * s), shape * 2.0f - 1.0f);
    }

    float sr = 48000.0f, osr = 192000.0f;
    uint32_t baseSeed = 101u;
    Adsr ampEnv, filtEnv;
    VactrolLpg vactrol;
    WavetableOsc wt;
    DcBlocker oscDc;
    Drift drift;
    PinkDrift organicDrift;
    Rng rng;
    double subPhase = 0.0, curDt = 0.001, curFmDt = 0.001, curFmKeyGain = 1.0;
    double fbPmPrev = 0.0, curFbFmKeyGain = 1.0;
    float pwmPhase = 0.0f, pwmLfo = 0.0f, curPwmV = 0.0f, curPwmInc = 0.0f, pan = 0.0f, cutTol = 0.0f;
    float currentNote = 60.0f, targetNote = 60.0f, noteBent = 60.0f, velocity = 1.0f, lastEnv = 0.0f;
    float glideCoef = 0.0f, glideM = 0.0f, pwmRate = 0.35f, driftAmt = 0.25f, organicAmt = 0.0f, wtTune = 0.0f;
    float curDrift = 0.0f, curDSub = 0.0f, curWLower = 0.0f, velAmpLast = 0.65f;
    float railPitchSens = 1.0f, lastRailPitchSt = 0.0f;
    int curMip = 0, s1Mul = 4, s2Mul = 2, lpgMode = 0;
    bool curFmLive = false, curFbFmLive = false, vactrolStrikePending = false;
    Ramp sub1L, sub2L, sub1S, sub2S, weight, wtL, morph, pwBase, pwmDepth;
    Ramp cutA, cutB, resA, resB, fDrive, envAmt, keyTrack, velGain, bend, wtTuneR, spreadPan;
    Ramp fmIdx, fmEnvAmt, fmFbR;
    Ramp lpgDecayR, lpgStrikeR, fbFmR, vactrolVca;
    bool fmOn = false;
    float fmGate = 0.0f, fmGateAlpha = 0.01f, lastFmDecay = -1.0f, lastGlideMs = -1.0f;
    double fmPhase = 0.0, fmModPrev = 0.0;
    float fmY1 = 0.0f, fmY2 = 0.0f, fmEnvLevel = 0.0f, fmEnvMul = 0.999f, fmRatio = 1.0f, fmKey = 0.0f;
    int note = -1, pendingNote = -1, stealPos = 0, stealTotal = 768;
    float pendingVel = 0.0f;
    bool active = false, fresh = true, stealPending = false, pendingOff = false, pendingSnap = false;
};

//==============================================================================
/** 16-voice poly / mono / legato voice manager with portable SoA SIMD
   dual-ladder filtering, fixed-cadence odd-block-invariant modulation, smooth
   polyphonic headroom scaling, and raised-cosine de-click voice stealing. */
class VoiceManager
{
public:
    static constexpr int kVoices = 16;
    static constexpr int kOsLog2 = 2; // voices run at 4x the host rate
    static constexpr int kOs = 1 << kOsLog2;
    static constexpr int kControlStep = 8;          // fixed base-rate modulation cadence (8 samples = 6 kHz @ 48k)
    static constexpr int kSub = kControlStep * kOs; // 32 oversampled samples per control tick

    void prepare(float baseSr)
    {
        osr = (float)kOs * baseSr;
        ladA.setSampleRate((double)osr);
        ladB.setSampleRate((double)osr);
        fbBank.setSampleRate((double)osr);
        railSag.prepare((double)osr);
        lastRailDroop = 0.0;
        for (int i = 0; i < kVoices; ++i)
        {
            voices[(size_t)i].prepare(baseSr, (float)kOs, (uint32_t)(i * 7717 + 101));
            lastCutA[i] = lastCutB[i] = -1.0f;
            cachedGA[i] = cachedGB[i] = 0.0;
        }
        fltFade.prepare(osr, 10.0f);
        fltFade.snap(true);
        polySmooth.prepare(osr, 12.0f);
        polySmooth.snap(1.0f);
        allOff();
    }

    void reseed(uint32_t seedVal) noexcept
    {
        for (int i = 0; i < kVoices; ++i) voices[(size_t)i].reseed(seedVal ^ (uint32_t)(i * 7717 + 101));
        railSag.reset();
        lastRailDroop = 0.0;
    }

    void allOff()
    {
        for (int v = 0; v < kVoices; ++v)
        {
            voices[(size_t)v].reset();
            ladA.resetLane(v);
            ladB.resetLane(v);
            fbBank.resetLane(v);
            fbBank.setLane(v, 0.0, 1, true);
            lastCutA[v] = lastCutB[v] = -1.0f;
        }
        fbWasActive = false;
        railSag.reset();
        lastRailDroop = 0.0;
        polySmooth.snap(1.0f);
        stackSize = 0;
        sustain = false;
        std::fill(std::begin(sustained), std::end(sustained), false);
    }

    void tickIdle(int nOs) noexcept
    {
        railSag.decaySilent(nOs);
        lastRailDroop = 0.0;
    }

    void setTable(const WavetableBank* b) noexcept
    {
        for (auto& v : voices) v.setTable(b);
    }
    void snapFilter(bool fltOn) noexcept { fltFade.snap(fltOn); }

    void setMode(int m) noexcept
    {
        if (m != mode)
        {
            for (auto& v : voices) v.release();
            mode = m;
        }
    }

    void noteOn(int note, float vel)
    {
        if (note < 0 || note >= 128) return;
        pushNote(note);
        sustained[note] = false;
        noteVel[note] = vel;

        if (mode == 0)
        {
            Voice* target = nullptr;
            for (auto& v : voices)
                if (v.isActive() && v.getNote() == note)
                {
                    target = &v;
                    break;
                }
            if (!target)
                for (auto& v : voices)
                    if (!v.isActive())
                    {
                        target = &v;
                        break;
                    }
            if (!target)
            { // steal: prefer released voices, then the oldest
                uint32_t best = 0;
                for (auto& v : voices)
                {
                    const uint32_t score = v.age + (v.gateHeld ? 0u : 100000000u);
                    if (!target || score > best)
                    {
                        target = &v;
                        best = score;
                    }
                }
            }
            target->start(note, vel, true, false);
            target->gateHeld = true;
        }
        else
        {
            auto& v = voices[0];
            const bool legato = mode == 2 && stackSize > 1 && v.isActive();
            v.start(note, vel, !legato, false);
            v.gateHeld = true;
        }
    }

    void noteOff(int note)
    {
        if (note < 0 || note >= 128) return;
        if (sustain)
        {
            sustained[note] = true;
            return;
        }
        releaseNote(note);
    }

    void setSustain(bool on)
    {
        sustain = on;
        if (!on)
            for (int n = 0; n < 128; ++n)
                if (sustained[n])
                {
                    sustained[n] = false;
                    releaseNote(n);
                }
    }

    void renderSlice(float* bodyL, float* bodyR, float* sub, int n2, const std::array<VoiceParams, kVoices>& vp,
                     bool fltOn, int route, bool isCtrlTick, int ctrlSpanOs = kSub)
    {
        if (isCtrlTick)
        {
            railSag.beginControlBlock((double)vp[0].railSag, (double)vp[0].railTimeMs, ctrlSpanOs, lastRailDroop);
        }
        uint32_t activeMask = 0u;
        float envSum = 0.0f;
        bool anyLpgFilter = false;
        bool anyFbFilter = false;
        bool anyFbFm = false;
        for (int v = 0; v < kVoices; ++v)
        {
            auto& voice = voices[(size_t)v];
            if (!voice.isActive())
            {
                if (isCtrlTick)
                {
                    ladA.holdLane(v);
                    ladB.holdLane(v);
                    fbBank.holdLane(v);
                }
                continue;
            }
            activeMask |= (1u << v);
            envSum += voice.envLevel();

            if (isCtrlTick || voice.isFresh())
            {
                const bool wasFresh = voice.isFresh();
                voice.control(vp[(size_t)v], ctrlSpanOs);
                float cA, rA, cB, rB, drive;
                voice.beginControlBlock(ctrlSpanOs, (float)lastRailDroop, cA, rA, cB, rB, drive);
                const bool snap = voice.ladderSnap || wasFresh;
                if (snap)
                {
                    ladA.resetLane(v);
                    ladB.resetLane(v);
                    fbBank.resetLane(v);
                    voice.ladderSnap = false;
                    lastCutA[v] = lastCutB[v] = -1.0f;
                }
                if (std::abs(cA - lastCutA[v]) > 1.0e-3f)
                {
                    lastCutA[v] = cA;
                    cachedGA[v] = LadderBank<kVoices>::coefGD((double)cA, (double)osr);
                }
                if (std::abs(cB - lastCutB[v]) > 1.0e-3f)
                {
                    lastCutB[v] = cB;
                    cachedGB[v] = LadderBank<kVoices>::coefGD((double)cB, (double)osr);
                }
                double gA = cachedGA[v], kA = 4.3 * (double)rA;
                double gB = cachedGB[v], kB = 4.3 * (double)rB;
                ladA.applyBiasSag(v, gA, kA);
                ladB.applyBiasSag(v, gB, kB);
                ladA.setLane(v, gA, kA, (double)drive, ctrlSpanOs, snap);
                ladB.setLane(v, gB, kB, (double)drive, ctrlSpanOs, snap);
                const float fbRaw = std::clamp(vp[(size_t)v].fbFilter, -1.0f, 1.0f);
                const double targetFbG = (fbRaw >= 0.0f) ? (1.25 * (double)fbRaw) : (1.35 * (double)fbRaw);
                fbBank.setLane(v, targetFbG, ctrlSpanOs, snap);
            }

            if (voice.isLpgFilterActive()) anyLpgFilter = true;
            if (std::abs(fbBank.fbG[v]) > 1.0e-6 || std::abs(fbBank.dFbG[v]) > 1.0e-6) anyFbFilter = true;
            if (voice.isFbFmActive()) anyFbFm = true;
        }
        const bool fltEffOn = fltOn || anyLpgFilter;
        if (activeMask == 0u)
        {
            fltFade.snap(fltEffOn);
            polySmooth.snap(1.0f);
            railSag.decaySilent(n2);
            lastRailDroop = 0.0;
            return;
        }

        // Smooth polyphonic headroom compensation: unity (1.0) for 1 voice,
        // 1/sqrt(N) for chords (prevents bus clipping)
        if (isCtrlTick) polySmooth.set(1.0f / std::sqrt(std::max(1.0f, envSum)));

        alignas(32) float osc[kVoices][kSub];
        alignas(32) float gain[kVoices][kSub];
        alignas(32) float outA[kVoices][kSub];
        alignas(32) float outB[kVoices][kSub];
        float panL[kVoices]{}, panR[kVoices]{};

        const bool runFilters = fltEffOn || !fltFade.settled(fltEffOn);
        const int effRoute = fltOn ? route : 2;
        float (*filtered)[kSub] = osc;

        if (!anyFbFilter && !anyFbFm)
        {
            if (fbWasActive)
            {
                for (int v = 0; v < kVoices; ++v) fbBank.resetLane(v);
                fbWasActive = false;
            }
            for (int v = 0; v < kVoices; ++v)
            {
                if ((activeMask & (1u << v)) == 0u) continue;
                auto& voice = voices[(size_t)v];
                voice.renderSamples(n2, osc[v], gain[v], sub);
                voice.pans(panL[v], panR[v]);
            }

            if (runFilters)
            {
                switch (effRoute)
                {
                case 0:
                    ladA.processSoA(osc, outA, n2, activeMask);
                    ladB.processSoA(outA, outB, n2, activeMask);
                    filtered = outB;
                    break;
                case 1:
                    ladA.processSoA(osc, outA, n2, activeMask);
                    ladB.processSoA(osc, outB, n2, activeMask);
                    for (int v = 0; v < kVoices; ++v)
                    {
                        if ((activeMask & (1u << v)) == 0u) continue;
                        for (int i = 0; i < n2; ++i) outA[v][i] = 0.5f * (outA[v][i] + outB[v][i]);
                    }
                    filtered = outA;
                    break;
                default:
                    ladA.processSoA(osc, outA, n2, activeMask);
                    filtered = outA;
                    break;
                }
            }
        }
        else
        {
            // Oversampled 64-bit DC-blocked, soft-saturated Audio-Rate Feedback Path
            // (8-lane auto-vectorized SIMD closed loop inside existing 4x voice OS)
            fbWasActive = true;
            std::array<Voice::FbFmSlice, kVoices> fbFmSlices{};
            if (!anyFbFm)
            {
                for (int v = 0; v < kVoices; ++v)
                {
                    if ((activeMask & (1u << v)) == 0u) continue;
                    auto& voice = voices[(size_t)v];
                    voice.renderSamples(n2, osc[v], gain[v], sub);
                    voice.pans(panL[v], panR[v]);
                }
            }
            else
            {
                for (int v = 0; v < kVoices; ++v)
                {
                    if ((activeMask & (1u << v)) == 0u) continue;
                    auto& voice = voices[(size_t)v];
                    voice.renderPreFbFm(n2, fbFmSlices[(size_t)v], gain[v], sub);
                    voice.pans(panL[v], panR[v]);
                }
            }

            const double dcG = fbBank.dcG, dcR = fbBank.dcR, lpA = fbBank.lpA;
            constexpr double kFbLim = FeedbackBank<kVoices>::kFbLim;
            constexpr double kInvFbLim = FeedbackBank<kVoices>::kInvFbLim;
            constexpr double kFbCubic = FeedbackBank<kVoices>::kCubicC;
            const bool useB = runFilters && (effRoute != 2);

            for (int base = 0; base < kVoices; base += 8)
            {
                const uint32_t grpMask = (activeMask >> base) & 0xFFu;
                if (grpMask == 0u) continue;

                typename LadderBank<kVoices>::GroupState8 gA, gB;
                if (runFilters)
                {
                    gA.load(ladA, base, ctrlSpanOs);
                    if (useB) gB.load(ladB, base, ctrlSpanOs);
                }

                alignas(32) double lDcX1[8], lDcY1[8], lLpZ[8], lFbSat[8], lFbG[8], ldFbG[8];
                for (int j = 0; j < 8; ++j)
                {
                    const int l = base + j;
                    lDcX1[j] = fbBank.dcX1[l];
                    lDcY1[j] = fbBank.dcY1[l];
                    lLpZ[j] = fbBank.lpZ[l];
                    lFbSat[j] = fbBank.fbSat[l];
                    lFbG[j] = fbBank.fbG[l];
                    ldFbG[j] = fbBank.dFbG[l];
                }

                alignas(32) double xinASeq[kSub][8];
                alignas(32) double xinBSeq[kSub][8];

                for (int i = 0; i < n2; ++i)
                {
                    alignas(32) double osc8[8]{};
                    if (anyFbFm)
                    {
                        for (int j = 0; j < 8; ++j)
                        {
                            if ((grpMask & (1u << j)) == 0u) continue;
                            const int v = base + j;
                            bool stole = false;
                            const float oS = voices[(size_t)v].stepOscFbFm(fbFmSlices[(size_t)v], i, lFbSat[j], stole);
                            if (stole)
                            {
                                if (runFilters)
                                {
                                    gA.resetLane(j);
                                    if (useB) gB.resetLane(j);
                                }
                                lDcX1[j] = lDcY1[j] = lLpZ[j] = lFbSat[j] = 0.0;
                            }
                            osc[v][i] = oS;
                            osc8[j] = (double)oS;
                        }
                    }
                    else
                    {
                        for (int j = 0; j < 8; ++j)
                            if ((grpMask & (1u << j)) != 0u) osc8[j] = (double)osc[base + j][i];
                    }

                    alignas(32) double xinA[8];
#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
                    for (int j = 0; j < 8; ++j)
                    {
                        lFbG[j] += ldFbG[j];
                        const double xa = osc8[j] + lFbG[j] * lFbSat[j];
                        xinA[j] = xa;
                        xinASeq[i][j] = xa;
                    }

                    alignas(32) double yout8[8];
                    if (runFilters)
                    {
                        if (effRoute == 0)
                        {
                            alignas(32) double mid8[8];
                            gA.step(xinA, mid8);
                            for (int j = 0; j < 8; ++j) xinBSeq[i][j] = mid8[j];
                            gB.step(mid8, yout8);
                        }
                        else if (effRoute == 1)
                        {
                            alignas(32) double yA8[8], yB8[8];
                            gA.step(xinA, yA8);
                            gB.step(xinA, yB8);
                            for (int j = 0; j < 8; ++j)
                            {
                                xinBSeq[i][j] = xinA[j];
                                yout8[j] = 0.5 * (yA8[j] + yB8[j]);
                            }
                        }
                        else
                        {
                            gA.step(xinA, yout8);
                        }
                    }
                    else
                    {
                        for (int j = 0; j < 8; ++j) yout8[j] = osc8[j];
                    }

#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
                    for (int j = 0; j < 8; ++j)
                    {
                        const double y = yout8[j];
                        const double dc = dcG * (y - lDcX1[j]) + dcR * lDcY1[j];
                        lDcX1[j] = y;
                        lDcY1[j] = dc;
                        lLpZ[j] += lpA * (dc - lLpZ[j]);
                        const double u = lLpZ[j] * kInvFbLim;
                        const double uc = u < -1.5 ? -1.5 : (u > 1.5 ? 1.5 : u);
                        lFbSat[j] = kFbLim * uc * (1.0 - kFbCubic * uc * uc);
                    }

                    for (int j = 0; j < 8; ++j) outB[base + j][i] = (float)yout8[j];
                }

                if (runFilters)
                {
                    gA.store(ladA, base);
                    ladA.updateBiasGroup8(base, xinASeq, n2);
                    if (useB)
                    {
                        gB.store(ladB, base);
                        ladB.updateBiasGroup8(base, xinBSeq, n2);
                    }
                }
                for (int j = 0; j < 8; ++j)
                {
                    const int l = base + j;
                    fbBank.dcX1[l] = flushDenormal(lDcX1[j]);
                    fbBank.dcY1[l] = flushDenormal(lDcY1[j]);
                    fbBank.lpZ[l] = flushDenormal(lLpZ[j]);
                    fbBank.fbSat[l] = flushDenormal(lFbSat[j]);
                    fbBank.fbG[l] = lFbG[j];
                }
            }
            filtered = runFilters ? outB : osc;
        }

        if (runFilters)
        {
            if (fltFade.settled(fltEffOn))
            {
                if (!fltEffOn) filtered = osc;
            }
            else
            {
                float fg[kSub];
                for (int i = 0; i < n2; ++i) fg[i] = fltFade.next(fltEffOn);
                for (int v = 0; v < kVoices; ++v)
                {
                    if ((activeMask & (1u << v)) == 0u) continue;
                    for (int i = 0; i < n2; ++i) filtered[v][i] = osc[v][i] + fg[i] * (filtered[v][i] - osc[v][i]);
                }
            }
        }

        // Accumulate active voices into stereo oversampled bus with smooth
        // polyphonic headroom scaling
        float pg[kSub];
        for (int i = 0; i < n2; ++i) pg[i] = polySmooth.next();

        for (int v = 0; v < kVoices; ++v)
        {
            if ((activeMask & (1u << v)) == 0u) continue;
            const float pl = panL[v], pr = panR[v];
            const float* __restrict yv = filtered[v];
            const float* __restrict gv = gain[v];
            for (int i = 0; i < n2; ++i)
            {
                const float s = yv[i] * gv[i] * pg[i];
                bodyL[i] += s * pl;
                bodyR[i] += s * pr;
            }
        }
        for (int i = 0; i < n2; ++i) sub[i] *= pg[i];

        // Shared virtual analog power-rail droop (exact per-sample RC integration + headroom glue)
        railSag.processBus(bodyL, bodyR, sub, pg, n2);

        const uint32_t baseSamples = (uint32_t)std::max(1, n2 / kOs);
        for (int v = 0; v < kVoices; ++v)
            if ((activeMask & (1u << v)) != 0u) voices[(size_t)v].advanceAge(baseSamples);
    }

    /** Backwards-compatible wrapper for any standalone caller rendering arbitrary
     * n2. */
    void render(float* bodyL, float* bodyR, float* sub, int n2, const std::array<VoiceParams, kVoices>& vp, bool fltOn,
                int route)
    {
        for (int start = 0; start < n2; start += kSub)
        {
            const int m = std::min(kSub, n2 - start);
            renderSlice(bodyL + start, bodyR + start, sub + start, m, vp, fltOn, route, true, m);
        }
    }

    bool anyHeld() const noexcept { return stackSize > 0; }
    int lowestHeld() const noexcept
    {
        int lo = 128;
        for (int i = 0; i < stackSize; ++i) lo = std::min(lo, stack[i]);
        return lo == 128 ? -1 : lo;
    }
    int lastHeld() const noexcept { return stackSize > 0 ? stack[stackSize - 1] : -1; }
    bool anyActive() const noexcept
    {
        for (auto& v : voices)
            if (v.isActive()) return true;
        return false;
    }
    bool voiceActive(int v) const noexcept { return voices[(size_t)v].isActive(); }
    float voiceVelocity(int v) const noexcept { return voices[(size_t)v].getVelocity(); }

private:
    void pushNote(int note)
    {
        removeFromStack(note);
        if (stackSize < 128) stack[stackSize++] = note;
    }
    void removeFromStack(int note)
    {
        int w = 0;
        for (int i = 0; i < stackSize; ++i)
            if (stack[i] != note) stack[w++] = stack[i];
        stackSize = w;
    }
    void releaseNote(int note)
    {
        removeFromStack(note);
        if (mode == 0)
        {
            for (auto& v : voices)
                if (v.isActive() && v.getNote() == note && v.gateHeld)
                {
                    v.release();
                    v.gateHeld = false;
                }
        }
        else
        {
            auto& v = voices[0];
            if (stackSize > 0)
            {
                const int back = stack[stackSize - 1];
                if (v.getNote() != back)
                {
                    if (mode == 2)
                        v.glideTo(back);
                    else
                        v.start(back, noteVel[back], true, false);
                }
            }
            else if (v.gateHeld)
            {
                v.release();
                v.gateHeld = false;
            }
        }
    }

    std::array<Voice, kVoices> voices;
    LadderBank<kVoices> ladA, ladB;
    FeedbackBank<kVoices> fbBank;
    PowerRailSag railSag;
    double lastRailDroop = 0.0;
    bool fbWasActive = false;
    float lastCutA[kVoices]{}, lastCutB[kVoices]{};
    double cachedGA[kVoices]{}, cachedGB[kVoices]{};
    BypassFade fltFade;
    Smooth polySmooth;
    float osr = 192000.0f;
    int stack[128]{};
    float noteVel[128]{};
    int stackSize = 0;
    int mode = 0;
    bool sustain = false;
    bool sustained[128]{};
};
} // namespace vessel::dsp
