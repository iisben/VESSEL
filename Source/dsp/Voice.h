#pragma once

#include "Common.h"
#include "Wavetable.h"

namespace vessel::dsp {
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
class Adsr {
public:
  using Parameters = juce::ADSR::Parameters;

  Adsr() noexcept { recalculateRates(); }

  void setSampleRate(double sr) noexcept {
    sr = std::max(1.0, sr);
    if (std::abs(sr - sampleRate) < 1.0e-6)
      return;
    sampleRate = sr;
    recalculateRates();
  }

  void setParameters(const Parameters &p) noexcept {
    if (paramsValid && std::abs(p.attack - parameters.attack) <= 1.0e-9f &&
        std::abs(p.decay - parameters.decay) <= 1.0e-9f &&
        std::abs(p.sustain - parameters.sustain) <= 1.0e-9f &&
        std::abs(p.release - parameters.release) <= 1.0e-9f)
      return;
    parameters = p;
    paramsValid = true;
    recalculateRates();
  }

  bool isActive() const noexcept {
    return state != State::idle || smoothOut > 1.0e-6f;
  }
  float currentLevel() const noexcept { return smoothOut; }
  void reset() noexcept {
    level = 0.0f;
    smoothOut = 0.0f;
    state = State::idle;
  }

  void noteOn() noexcept {
    if (parameters.attack > 0.0f)
      state = State::attack;
    else if (parameters.decay > 0.0f) {
      level = 1.0f;
      state = State::decay;
    } else {
      level = parameters.sustain;
      state = State::sustain;
    }
  }

  void noteOff() noexcept {
    if (state == State::idle)
      return;
    if (parameters.release <= 0.0f) {
      reset();
      return;
    }
    releaseFrom = std::max(level, 0.05f);
    relTarget = -kRelEps * releaseFrom;
    state = State::release;
  }

  float getNextSample() noexcept {
    switch (state) {
    case State::idle:
      level = 0.0f;
      break;
    case State::attack:
      level = kAttTarget + attCoef * (level - kAttTarget);
      if (level >= 1.0f) {
        level = 1.0f;
        next();
      }
      break;
    case State::decay:
      level = decTarget + decCoef * (level - decTarget);
      if (level <= parameters.sustain) {
        level = parameters.sustain;
        next();
      }
      break;
    case State::sustain:
      level = parameters.sustain;
      break;
    case State::release:
      level = relTarget + relCoef * (level - relTarget);
      if (level <= 0.0f) {
        level = 0.0f;
        state = State::idle;
      }
      break;
    }
    smoothOut = level + cvCoef * (smoothOut - level);
    if (state == State::idle && smoothOut < 1.0e-6f)
      smoothOut = 0.0f;
    return smoothOut;
  }

private:
  enum class State { idle, attack, decay, sustain, release };

  static constexpr float kAttTarget = 1.008f;
  static constexpr float kAttRate = 4.8362819f; // -ln(0.008 / 1.008)
  static constexpr float kDecEps = 0.001f;
  static constexpr float kRelEps = 0.001f;
  static constexpr float kExpRate = 6.9087553f; // ln(1001)

  void recalculateRates() noexcept {
    attCoef = parameters.attack > 0.0f
                  ? (float)std::exp(-(double)kAttRate /
                                    ((double)parameters.attack * sampleRate))
                  : 0.0f;
    decCoef = parameters.decay > 0.0f
                  ? (float)std::exp(-(double)kExpRate /
                                    ((double)parameters.decay * sampleRate))
                  : 0.0f;
    relCoef = parameters.release > 0.0f
                  ? (float)std::exp(-(double)kExpRate /
                                    ((double)parameters.release * sampleRate))
                  : 0.0f;

    const double minSeg =
        std::min(std::max(0.0002, (double)parameters.attack),
                 std::max(0.0002, (double)parameters.release));
    const double cvTau = std::clamp(0.25 * minSeg, 0.00008, 0.00075);
    cvCoef = (float)std::exp(-1.0 / (cvTau * sampleRate));

    decTarget = parameters.sustain -
                kDecEps * std::max(0.05f, 1.0f - parameters.sustain);
    if (state == State::release)
      relTarget = -kRelEps * std::max(releaseFrom, 0.05f);

    if ((state == State::attack && parameters.attack <= 0.0f) ||
        (state == State::decay &&
         (parameters.decay <= 0.0f || level <= parameters.sustain)) ||
        (state == State::release && parameters.release <= 0.0f))
      next();
  }

  void next() noexcept {
    if (state == State::attack)
      state = parameters.decay > 0.0f ? State::decay : State::sustain;
    else if (state == State::decay)
      state = State::sustain;
    else if (state == State::release) {
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
/** Lane-parallel bank of 64-bit double 24 dB/oct nonlinear zero-delay-feedback
   transistor ladders. Structure-of-arrays: every operation is identical across
   lanes with zero data-dependent branches, enabling portable NEON / SSE2 / AVX2
   auto-vectorization. processSoA() register-tiles 4 lanes across the entire
   sub-block and skips idle 4-lane groups. */
template <int L> struct LadderBank {
  alignas(32) double s0[L]{}, s1[L]{}, s2[L]{}, s3[L]{};
  alignas(32) double G[L]{}, dG[L]{}, k[L]{}, dk[L]{}, drv[L]{}, ddrv[L]{},
      isd[L]{}, disd[L]{};

  static double coefGD(double hz, double osr) noexcept {
    const double safeOsr = std::max(osr, 1.0);
    hz = std::clamp(hz, 10.0, 0.485 * safeOsr);
    const double g = std::tan(kPiD * hz / safeOsr);
    return g / (1.0 + g);
  }

  static float coefG(float hz, float osr) noexcept {
    return (float)coefGD((double)hz, (double)osr);
  }

  void resetLane(int l) noexcept { s0[l] = s1[l] = s2[l] = s3[l] = 0.0; }

  void setLane(int l, double Gt, double kt, double driveT, int m,
               bool snap) noexcept {
    const double isdT = 1.0 / std::sqrt(std::max(driveT, 1.0e-6));
    if (snap) {
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

  /** Single-sample tick across all L lanes (used by standalone QA / 1-lane
   * verification). */
  inline void tick(const float *__restrict in, float *__restrict out) noexcept {
    for (int l = 0; l < L; ++l) {
      const double g = (G[l] += dG[l]);
      const double kk = (k[l] += dk[l]);
      const double d = (drv[l] += ddrv[l]);
      const double id = (isd[l] += disd[l]);

      const double g2 = g * g, g3 = g2 * g, g4 = g3 * g;
      const double oneMinusG = 1.0 - g;
      const double S =
          (g3 * s0[l] + g2 * s1[l] + g * s2[l] + s3[l]) * oneMinusG;
      const double x = (double)in[l] * (1.0 + 0.55 * kk);

      const double g4id = g4 * id;
      const double g4idk = g4id * d * kk;
      double y4 = (g4 * (d * id) * x + S) / (1.0 + g4idk);

      double th = fastTanhLadder(d * (x - kk * y4));
      double sech2 = 1.0 - th * th;
      double step = (y4 - S - g4id * th) / (1.0 + g4idk * sech2);
      y4 -= step;

      th = fastTanhLadder(d * (x - kk * y4));
      sech2 = 1.0 - th * th;
      step = (y4 - S - g4id * th) / (1.0 + g4idk * sech2);
      y4 -= step;

      const double u = id * std::clamp(th + sech2 * (d * kk * step), -1.0, 1.0);

      double v = (u - s0[l]) * g;
      double y = v + s0[l];
      s0[l] = y + v;
      double st = y;
      v = (st - s1[l]) * g;
      y = v + s1[l];
      s1[l] = y + v;
      st = y;
      v = (st - s2[l]) * g;
      y = v + s2[l];
      s2[l] = y + v;
      st = y;
      v = (st - s3[l]) * g;
      y = v + s3[l];
      s3[l] = y + v;
      out[l] = (float)y;
    }
  }

  /** Portable SoA SIMD block processing across 8-lane groups (4 independent NEON/SSE2 double chains
   * interleave across the TPT feedback latency, saturating all FP execution pipes with zero spills). */
  template <int Stride>
  void processSoA(const float (&inBuf)[L][Stride], float (&outBuf)[L][Stride],
                  int m, uint32_t activeMask) noexcept {
    static_assert(L % 8 == 0,
                  "LadderBank::processSoA requires L to be a multiple of 8");
    for (int base = 0; base < L; base += 8) {
      if (((activeMask >> base) & 0xFFu) == 0u)
        continue;

      alignas(32) double ls0[8], ls1[8], ls2[8], ls3[8];
      alignas(32) double lG[8], ldG[8], lk[8], ldk[8], ld[8], ldd[8], lid[8],
          ldid[8];
      double maxG = 0.0;
      double maxDelta = 0.0;
      for (int j = 0; j < 8; ++j) {
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
        maxG = std::max({maxG, lG[j], lG[j] + ldG[j] * (double)m});
        maxDelta = std::max({maxDelta, std::abs(ldG[j]), std::abs(ldk[j]),
                             std::abs(ldd[j]), std::abs(ldid[j])});
      }

      const bool useFastInv = (maxG < 0.24);
      const bool coeffsConst = (maxDelta == 0.0);

      if (useFastInv && coeffsConst) {
        // Steady-state fast path: hoist all filter coefficients outside the m-sample loop (zero ramp math per sample!)
        alignas(32) double cg[8], cg2[8], cg3[8], cOmG[8], cBass[8], cG4id[8],
            cG4idk[8], cG4did[8], cInvPiv[8], cD[8], cK[8], cId[8], cDK[8];
        for (int j = 0; j < 8; ++j) {
          const double g = lG[j], kk = lk[j], d = ld[j], id = lid[j];
          const double g2 = g * g, g3 = g2 * g, g4 = g3 * g;
          const double g4id = g4 * id;
          const double g4idk = g4id * d * kk;
          cg[j] = g;
          cg2[j] = g2;
          cg3[j] = g3;
          cOmG[j] = 1.0 - g;
          cBass[j] = 1.0 + 0.55 * kk;
          cG4id[j] = g4id;
          cG4idk[j] = g4idk;
          cG4did[j] = g4 * (d * id);
          cInvPiv[j] = 1.0 - g4idk * (1.0 - g4idk * (1.0 - g4idk));
          cD[j] = d;
          cK[j] = kk;
          cId[j] = id;
          cDK[j] = d * kk;
        }

        for (int i = 0; i < m; ++i) {
          alignas(32) double xin[8];
          for (int j = 0; j < 8; ++j)
            xin[j] = (double)inBuf[base + j][i];
          alignas(32) double yout[8];

#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
          for (int j = 0; j < 8; ++j) {
            const double g = cg[j];
            const double S =
                (cg3[j] * ls0[j] + cg2[j] * ls1[j] + g * ls2[j] + ls3[j]) *
                cOmG[j];
            const double x = xin[j] * cBass[j];
            const double y4 = (cG4did[j] * x + S) * cInvPiv[j];

            const double arg = cD[j] * (x - cK[j] * y4);
            const double ac = arg < -6.0 ? -6.0 : (arg > 6.0 ? 6.0 : arg);
            const double a2 = ac * ac;
            const double th =
                (ac * (34459425.0 +
                       a2 * (4729725.0 +
                             a2 * (135135.0 + a2 * (990.0 + a2))))) /
                (34459425.0 +
                 a2 * (16216200.0 +
                       a2 * (945945.0 + a2 * (13860.0 + 45.0 * a2))));
            const double sech2 = 1.0 - th * th;
            const double uNr = cG4idk[j] * sech2;
            const double invNr = 1.0 - uNr * (1.0 - uNr * (1.0 - uNr));
            const double step = (y4 - S - cG4id[j] * th) * invNr;
            const double rawU = th + sech2 * (cDK[j] * step);
            const double u =
                cId[j] * (rawU < -1.0 ? -1.0 : (rawU > 1.0 ? 1.0 : rawU));

            double v = (u - ls0[j]) * g;
            double y = v + ls0[j];
            ls0[j] = y + v;
            double st = y;
            v = (st - ls1[j]) * g;
            y = v + ls1[j];
            ls1[j] = y + v;
            st = y;
            v = (st - ls2[j]) * g;
            y = v + ls2[j];
            ls2[j] = y + v;
            st = y;
            v = (st - ls3[j]) * g;
            y = v + ls3[j];
            ls3[j] = y + v;
            yout[j] = y;
          }

          for (int j = 0; j < 8; ++j)
            outBuf[base + j][i] = (float)yout[j];
        }
      } else if (useFastInv) {
        for (int i = 0; i < m; ++i) {
          alignas(32) double xin[8];
          for (int j = 0; j < 8; ++j)
            xin[j] = (double)inBuf[base + j][i];
          alignas(32) double yout[8];

#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
          for (int j = 0; j < 8; ++j) {
            const double g = (lG[j] += ldG[j]);
            const double kk = (lk[j] += ldk[j]);
            const double d = (ld[j] += ldd[j]);
            const double id = (lid[j] += ldid[j]);

            const double g2 = g * g, g3 = g2 * g, g4 = g3 * g;
            const double S =
                (g3 * ls0[j] + g2 * ls1[j] + g * ls2[j] + ls3[j]) * (1.0 - g);
            const double x = xin[j] * (1.0 + 0.55 * kk);

            const double g4id = g4 * id;
            const double g4idk = g4id * d * kk;
            const double invPiv = 1.0 - g4idk * (1.0 - g4idk * (1.0 - g4idk));
            const double y4 = (g4 * (d * id) * x + S) * invPiv;

            const double arg = d * (x - kk * y4);
            const double ac = arg < -6.0 ? -6.0 : (arg > 6.0 ? 6.0 : arg);
            const double a2 = ac * ac;
            const double th =
                (ac * (34459425.0 +
                       a2 * (4729725.0 +
                             a2 * (135135.0 + a2 * (990.0 + a2))))) /
                (34459425.0 +
                 a2 * (16216200.0 +
                       a2 * (945945.0 + a2 * (13860.0 + 45.0 * a2))));
            const double sech2 = 1.0 - th * th;
            const double uNr = g4idk * sech2;
            const double invNr = 1.0 - uNr * (1.0 - uNr * (1.0 - uNr));
            const double step = (y4 - S - g4id * th) * invNr;
            const double rawU = th + sech2 * (d * kk * step);
            const double u =
                id * (rawU < -1.0 ? -1.0 : (rawU > 1.0 ? 1.0 : rawU));

            double v = (u - ls0[j]) * g;
            double y = v + ls0[j];
            ls0[j] = y + v;
            double st = y;
            v = (st - ls1[j]) * g;
            y = v + ls1[j];
            ls1[j] = y + v;
            st = y;
            v = (st - ls2[j]) * g;
            y = v + ls2[j];
            ls2[j] = y + v;
            st = y;
            v = (st - ls3[j]) * g;
            y = v + ls3[j];
            ls3[j] = y + v;
            yout[j] = y;
          }

          for (int j = 0; j < 8; ++j)
            outBuf[base + j][i] = (float)yout[j];
        }
      } else {
        for (int i = 0; i < m; ++i) {
          alignas(32) double xin[8];
          for (int j = 0; j < 8; ++j)
            xin[j] = (double)inBuf[base + j][i];
          alignas(32) double yout[8];

#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
          for (int j = 0; j < 8; ++j) {
            const double g = (lG[j] += ldG[j]);
            const double kk = (lk[j] += ldk[j]);
            const double d = (ld[j] += ldd[j]);
            const double id = (lid[j] += ldid[j]);

            const double g2 = g * g, g3 = g2 * g, g4 = g3 * g;
            const double S =
                (g3 * ls0[j] + g2 * ls1[j] + g * ls2[j] + ls3[j]) * (1.0 - g);
            const double x = xin[j] * (1.0 + 0.55 * kk);

            const double g4id = g4 * id;
            const double g4idk = g4id * d * kk;
            const double y4 = (g4 * (d * id) * x + S) / (1.0 + g4idk);

            const double th = fastTanhLadder(d * (x - kk * y4));
            const double sech2 = 1.0 - th * th;
            const double step = (y4 - S - g4id * th) / (1.0 + g4idk * sech2);
            const double rawU = th + sech2 * (d * kk * step);
            const double u =
                id * (rawU < -1.0 ? -1.0 : (rawU > 1.0 ? 1.0 : rawU));

            double v = (u - ls0[j]) * g;
            double y = v + ls0[j];
            ls0[j] = y + v;
            double st = y;
            v = (st - ls1[j]) * g;
            y = v + ls1[j];
            ls1[j] = y + v;
            st = y;
            v = (st - ls2[j]) * g;
            y = v + ls2[j];
            ls2[j] = y + v;
            st = y;
            v = (st - ls3[j]) * g;
            y = v + ls3[j];
            ls3[j] = y + v;
            yout[j] = y;
          }

          for (int j = 0; j < 8; ++j)
            outBuf[base + j][i] = (float)yout[j];
        }
      }

      for (int j = 0; j < 8; ++j) {
        const int l = base + j;
        s0[l] = ls0[j];
        s1[l] = ls1[j];
        s2[l] = ls2[j];
        s3[l] = ls3[j];
        G[l] = lG[j];
        k[l] = lk[j];
        drv[l] = ld[j];
        isd[l] = lid[j];
      }
    }
  }
};

struct VoiceParams {
  // sub (1)
  bool subOn = true;
  int sub1Oct = 1, sub2Oct = 2; // octaves below the played note
  float sub1Level = 0.8f, sub2Level = 0.45f, sub1Shape = 0, sub2Shape = 0.25f,
        subWeight = 0.3f;
  // wavetable (5)
  bool wtOn = true;
  float wtLevel = 0.55f, morph = 0.35f, pw = 0.5f, pwm = 0.3f, pwmRate = 0.35f,
        wtTune = 0.0f, drift = 0.25f;
  // ladders (11)
  bool fltOn = true;
  int route = 0;
  float cutA = 1800, resA = 0.35f, cutB = 5000, resB = 0.2f, fDrive = 0.25f,
        envAmt = 0.35f, keyTrack = 0.3f;
  juce::ADSR::Parameters amp, filt;
  float glideMs = 0.0f, bend = 0.0f, velAmp = 0.65f;
  // FM operator (27)
  bool fmOn = false;
  float fmRatio = 1.0f, fmFine = 0.0f, fmIndex = 0.3f, fmFb = 0.0f,
        fmEnv = 0.0f, fmDecay = 0.4f, fmKey = 0.0f;
  // V2 Dark Analog Organic Layer (0% default)
  float organicAmt = 0.0f;
};

//==============================================================================
/** One voice of the oversampled engine: sub stack, wavetable oscillator, RC
   envelopes, 4.0 ms raised-cosine voice-steal de-click, and per-voice
   deterministic 1/f organic drift. */
class Voice {
public:
  void prepare(float baseSampleRate, float osFactor, uint32_t seedVal) {
    sr = baseSampleRate;
    osr = osFactor * baseSampleRate;
    baseSeed = seedVal ? seedVal : 101u;
    ampEnv.setSampleRate(osr);
    filtEnv.setSampleRate(osr);
    oscDc.prepare((double)osr, 5.0);
    stealTotal = std::max(
        32,
        (int)std::lround(0.0040f * osr)); // 4.0 ms raised-cosine de-click ramp
    fmGateAlpha = 1.0f - std::exp(-32.0f / (0.015f * osr));
    lastFmDecay = -1.0f;
    lastGlideMs = -1.0f;
    reseed(baseSeed);
    reset();
  }

  void reseed(uint32_t seedVal) noexcept {
    baseSeed = seedVal ? seedVal : 101u;
    drift.prepare(osr, 0.27f, baseSeed);
    organicDrift.prepare(osr, baseSeed ^ 0x4F5247u);
    rng.seed(baseSeed * 7919u + 13u);
    pwmPhase = rng.uni();
    cutTol = rng.bip() *
             0.035f; // deterministic ±3.5% analog filter component tolerance
  }

  void reset() {
    ampEnv.reset();
    filtEnv.reset();
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
    fmY1 = fmY2 = fmEnvLevel = fmGate = 0.0f;
    for (auto *r : {&sub1L,   &sub2L,     &sub1S,    &sub2S,    &weight,  &wtL,
                    &morph,   &pwBase,    &pwmDepth, &cutA,     &cutB,    &resA,
                    &resB,    &fDrive,    &envAmt,   &keyTrack, &velGain, &bend,
                    &wtTuneR, &spreadPan, &fmIdx,    &fmEnvAmt, &fmFbR})
      r->reset();
  }

  void start(int midiNote, float vel, bool retrigger, bool snapPitch) {
    const bool wasSilent = !active || lastEnv < 1.0e-4f;
    if (!wasSilent && retrigger) {
      // Active voice stolen or retriggered: 4.0 ms raised-cosine de-click
      // fade-out before clean restart
      pendingNote = midiNote;
      pendingVel = vel;
      pendingOff = false;
      pendingSnap = snapPitch;
      if (!stealPending) {
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
    if (snapPitch || wasSilent)
      currentNote = targetNote;

    if (wasSilent) {
      subPhase = 0.0; // phase-aligned: sub stack and wavetable start together
                      // at a zero crossing
      wt.phase = 0.0;
      oscDc.reset();
      pan = rng.uni() - 0.5f;
      fresh = true; // controls snap to this note's values instead of ramping
                    // from the last note
      ladderSnap = true;
    }
    if (retrigger || wasSilent) {
      ampEnv.noteOn();
      filtEnv.noteOn();
      fmEnvLevel = 1.0f;
    }
    if (wasSilent) {
      fmPhase = 0.0;
      fmModPrev = 0.0;
      fmY1 = fmY2 = 0.0f;
    }
    active = true;
    age = 0;
  }

  void glideTo(int midiNote) {
    note = midiNote;
    targetNote = (float)midiNote;
  }
  void setTable(const WavetableBank *b) noexcept { wt.bank = b; }
  void release() {
    if (stealPending) {
      pendingOff = true;
      return;
    }
    ampEnv.noteOff();
    filtEnv.noteOff();
  }
  bool isActive() const noexcept { return active; }
  bool isFresh() const noexcept { return fresh; }
  int getNote() const noexcept { return stealPending ? pendingNote : note; }
  float getVelocity() const noexcept {
    return stealPending ? pendingVel : velocity;
  }
  float envLevel() const noexcept { return lastEnv; }

  bool gateHeld = false;
  uint32_t age = 0;
  bool ladderSnap = false;

  /** Fixed-cadence control update; nCtrlOs = oversampled samples in one control
   * tick (kSub = 32). */
  void control(const VoiceParams &p, int nCtrlOs) {
    ampEnv.setParameters(p.amp);
    filtEnv.setParameters(p.filt);

    auto set = [this, nCtrlOs](Ramp &r, float v) {
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

    const float gateTarget = p.fmOn ? 1.0f : 0.0f;
    if (fresh)
      fmGate = gateTarget;
    else if (std::abs(gateTarget - fmGate) > 1.0e-5f)
      fmGate += (gateTarget - fmGate) * fmGateAlpha;
    if (!p.fmOn && fmGate < 1.0e-3f)
      fmGate = 0.0f;
    set(fmIdx, p.fmIndex * fmGate);
    set(fmEnvAmt, p.fmEnv * fmGate);
    set(fmFbR, p.fmFb);
    fmOn = fmGate > 0.0f;
    fmRatio = p.fmRatio * semis(p.fmFine * 0.01f);
    if (std::abs(p.fmDecay - lastFmDecay) > 1.0e-6f) {
      lastFmDecay = p.fmDecay;
      fmEnvMul = std::exp(-1.0f / (std::max(0.005f, p.fmDecay) * osr));
    }
    fmKey = p.fmKey;

    if (std::abs(p.glideMs - lastGlideMs) > 1.0e-5f) {
      lastGlideMs = p.glideMs;
      glideCoef = p.glideMs <= 0.5f
                      ? 0.0f
                      : std::exp(-1.0f / (0.001f * p.glideMs * osr));
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
  void beginControlBlock(int mCtrl, float &cA, float &rA, float &cB, float &rB,
                         float &drive) noexcept {
    glideM = (glideCoef > 0.0f) ? std::pow(glideCoef, (float)mCtrl) : 0.0f;
    currentNote = targetNote + glideM * (currentNote - targetNote);
    bend.skip(mCtrl);
    wtTuneR.skip(mCtrl);
    noteBent = currentNote + bend.cur;
    const float hz = midiToHz(noteBent);

    // V1 smooth drift + V2 per-voice 1/f pink organic pitch micro-drift
    // (sub-120 Hz bass-locked)
    curDrift = drift.next(mCtrl) * driftAmt;
    const float orgWalk =
        organicAmt > 1.0e-5f ? organicDrift.next(mCtrl) : 0.0f;
    const float subLock = std::min(1.0f, (hz / 120.0f) * (hz / 120.0f));
    const float orgPitchSt = orgWalk * organicAmt * 0.12f * subLock;

    const float oscHz = hz * semis(wtTuneR.cur + curDrift * 0.18f + orgPitchSt);
    curDt = std::clamp((double)oscHz / (double)osr, 1.0e-7, 0.48);
    curDSub = std::clamp(hz / (8.0f * osr), 0.0f, 0.48f);

    const float pwmStart = pwmLfo;
    pwmPhase += pwmRate * (float)mCtrl / osr;
    pwmPhase -= std::floor(pwmPhase);
    if (!(pwmPhase >= 0.0f && pwmPhase < 1.0f))
      pwmPhase = 0.0f;
    pwmLfo = fastSin01(pwmPhase);
    curPwmV = pwmStart;
    curPwmInc = (pwmLfo - pwmStart) / (float)std::max(1, mCtrl);

    curFmLive = fmOn && (fmIdx.cur > 0.0f || fmIdx.inc != 0.0f ||
                         (fmEnvAmt.cur > 0.0f && fmEnvLevel > 1.0e-4f));
    curFmDt = std::min(0.45, curDt * (double)fmRatio);
    curFmKeyGain = (double)std::clamp(
        fastExp2(-fmKey * (noteBent - 48.0f) * (1.0f / 12.0f)), 0.25f, 2.0f);

    constexpr double kMaxIndex = 12.0 / kTwoPiD;
    float fmSpread = 1.0f;
    if (curFmLive) {
      const float xEnd = fmIdx.cur + fmIdx.inc * (float)mCtrl;
      const float xPeak = std::min(
          1.25f, std::max(fmIdx.cur, xEnd) +
                     std::max(fmEnvAmt.cur,
                              fmEnvAmt.cur + fmEnvAmt.inc * (float)mCtrl) *
                         fmEnvLevel);
      const float fbPeak =
          std::max(fmFbR.cur, fmFbR.cur + fmFbR.inc * (float)mCtrl);
      fmSpread = 1.0f + (float)(kTwoPiD * kMaxIndex * (double)(xPeak * xPeak) *
                                curFmKeyGain * (double)fmRatio *
                                (1.0 + 1.5 * (double)fbPeak));
    }

    const float pwMid = std::clamp(pwBase.cur + pwmDepth.cur * 0.44f * pwmLfo +
                                       curDrift * 0.03f,
                                   0.03f, 0.97f);
    WavetableOsc::mipBlend(oscHz * fmSpread *
                               std::max(0.5f / pwMid, 0.5f / (1.0f - pwMid)),
                           sr, curMip, curWLower);

    for (auto *r : {&cutA, &cutB, &resA, &resB, &fDrive, &envAmt, &keyTrack})
      r->skip(mCtrl);
    const float fenv = filtEnv.currentLevel();
    const float orgCutOct =
        organicAmt > 1.0e-5f ? organicAmt * (cutTol + 0.14f * orgWalk) : 0.0f;
    const float octs = envAmt.cur * 7.0f * fenv +
                       keyTrack.cur * (noteBent - 60.0f) * (1.0f / 12.0f) +
                       orgCutOct;
    const float hi = 0.485f * sr;
    cA = std::clamp(fastExp2(cutA.cur + octs), 10.0f, hi);
    cB = std::clamp(fastExp2(cutB.cur + octs), 10.0f, hi);
    rA = clamp01(resA.cur);
    rB = clamp01(resB.cur);
    const float dr = clamp01(fDrive.cur);
    drive = 1.0f + 6.0f * dr * dr;
  }

  /** Renders m oversampled samples into contiguous SoA buffers oscOut[m],
   * gainOut[m], and sums into sub[m]. */
  void renderSamples(int m, float *__restrict oscOut, float *__restrict gainOut,
                     float *__restrict sub) noexcept {
    constexpr double kMaxIndex = 12.0 / kTwoPiD;
    const bool subLive = sub1L.cur > 0.0f || sub2L.cur > 0.0f ||
                         sub1L.inc != 0.0f || sub2L.inc != 0.0f;
    const bool wtLive = wtL.cur > 0.0f || wtL.inc != 0.0f;
    const bool fmLive = curFmLive;

    if (!fmLive) {
      fmIdx.skip(m);
      fmEnvAmt.skip(m);
      fmFbR.skip(m);
      if (fmOn && fmEnvLevel > 1.0e-6f)
        fmEnvLevel *= std::pow(fmEnvMul, (float)m);
    }

    const bool weightConst = (weight.inc == 0.0f);
    const float wNConst = 1.0f + 3.0f * weight.cur;
    const float invTanhWConst = 1.0f / fastTanh(wNConst);
    const bool morphConst = (morph.inc == 0.0f);
    const auto resolvedWt =
        (wtLive && morphConst)
            ? wt.bank->resolveSlice(morph.cur, curMip, curWLower)
            : WavetableBank::ResolvedSlice{};

    for (int i = 0; i < m; ++i) {
      float stealW = 1.0f;
      if (stealPending) {
        const float u = (float)stealPos / (float)stealTotal;
        stealW = 0.5f * (1.0f + fastCos01(0.5f * u)); // C1 raised-cosine 1 -> 0
        if (++stealPos >= stealTotal) {
          stealPending = false;
          note = pendingNote;
          velocity = pendingVel;
          targetNote = currentNote = (float)pendingNote;
          noteBent = currentNote + bend.cur;
          const float hzNew = midiToHz(noteBent);
          const float oscHzNew = hzNew * semis(wtTuneR.cur + curDrift * 0.18f);
          curDt = std::clamp((double)oscHzNew / (double)osr, 1.0e-7, 0.48);
          curDSub = std::clamp(hzNew / (8.0f * osr), 0.0f, 0.48f);
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
          fmEnvLevel = 1.0f;
          velGain.snap(1.0f - velAmpLast + velAmpLast * velocity);
          if (pendingOff) {
            ampEnv.noteOff();
            filtEnv.noteOff();
            pendingOff = false;
          }
          stealW = 0.0f;
        }
      }

      const float env = ampEnv.getNextSample();
      (void)filtEnv.getNextSample();
      lastEnv = env;
      const float vg = velGain.next();

      const float a = sub1L.next(), b = sub2L.next(), sh1 = sub1S.next(),
                  sh2 = sub2S.next(), w = weight.next();
      if (subLive) {
        const float ph1f = (float)(subPhase * (double)s1Mul);
        const float ph2f = (float)(subPhase * (double)s2Mul);
        const float s = subShapeFast(ph1f - (float)(int)ph1f, sh1) * a +
                        subShapeFast(ph2f - (float)(int)ph2f, sh2) * b;
        const float wN = weightConst ? wNConst : (1.0f + 3.0f * w);
        const float invT = weightConst ? invTanhWConst : (1.0f / fastTanh(wN));
        sub[i] +=
            fastTanh(s * wN) * invT * 0.36f * env * vg * (stealW * stealW);
      }
      subPhase += (double)curDSub;
      if (subPhase >= 1.0)
        subPhase -= 1.0;

      const float lvl = wtL.next(), mo = morph.next();
      curPwmV += curPwmInc;
      const float pw =
          pwBase.next() + pwmDepth.next() * 0.44f * curPwmV + curDrift * 0.03f;
      double tzfm = 0.0;
      if (fmLive) {
        const double x = (double)std::min(
            1.25f, fmIdx.next() + fmEnvAmt.next() * fmEnvLevel);
        fmEnvLevel *= fmEnvMul;
        const float ph =
            (float)(fmPhase + 0.11 * (double)fmFbR.next() *
                                  (double)(fmY1 + fmY2));
        const double y = (double)fastSin01(ph);
        fmY2 = fmY1;
        fmY1 = (float)y;
        const double dMod = y - fmModPrev;
        fmModPrev = y;
        fmPhase += curFmDt;
        if (fmPhase >= 1.0)
          fmPhase -= 1.0;
        tzfm = kMaxIndex * x * x * curFmKeyGain * dMod;
      }
      const float rawOsc =
          wtLive ? (morphConst
                        ? wt.processResolved(curDt, resolvedWt, pw, tzfm)
                        : wt.process(curDt, curMip, curWLower, mo, pw, tzfm))
                 : 0.0f;
      oscOut[i] = wtLive ? oscDc.process(rawOsc) * lvl * stealW : 0.0f;
      gainOut[i] = env * vg * 0.5f * stealW;
    }
  }

  void pans(float &l, float &r) const noexcept {
    const float p = pan * spreadPan.cur;
    l = std::sqrt(0.5f - 0.5f * p);
    r = std::sqrt(0.5f + 0.5f * p);
  }

  void advanceAge(uint32_t baseSamples) noexcept {
    age += baseSamples;
    if (!ampEnv.isActive() && !stealPending) {
      active = false;
      note = -1;
      lastEnv = 0.0f;
    }
  }

private:
  static inline float subShapeFast(float ph, float shape) noexcept {
    const float s = fastSin01Unit(ph);
    if (shape <= 1.0e-4f)
      return s;
    const float tri = ph < 0.25f
                          ? 4.0f * ph
                          : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
    if (shape <= 0.5f)
      return lerp(s, tri, shape * 2.0f);
    return lerp(tri, fastTanh(4.0f * s), shape * 2.0f - 1.0f);
  }

  float sr = 48000.0f, osr = 192000.0f;
  uint32_t baseSeed = 101u;
  Adsr ampEnv, filtEnv;
  WavetableOsc wt;
  DcBlocker oscDc;
  Drift drift;
  PinkDrift organicDrift;
  Rng rng;
  double subPhase = 0.0, curDt = 0.001, curFmDt = 0.001, curFmKeyGain = 1.0;
  float pwmPhase = 0.0f, pwmLfo = 0.0f, curPwmV = 0.0f, curPwmInc = 0.0f,
        pan = 0.0f, cutTol = 0.0f;
  float currentNote = 60.0f, targetNote = 60.0f, noteBent = 60.0f,
        velocity = 1.0f, lastEnv = 0.0f;
  float glideCoef = 0.0f, glideM = 0.0f, pwmRate = 0.35f, driftAmt = 0.25f,
        organicAmt = 0.0f, wtTune = 0.0f;
  float curDrift = 0.0f, curDSub = 0.0f, curWLower = 0.0f, velAmpLast = 0.65f;
  int curMip = 0, s1Mul = 4, s2Mul = 2;
  bool curFmLive = false;
  Ramp sub1L, sub2L, sub1S, sub2S, weight, wtL, morph, pwBase, pwmDepth;
  Ramp cutA, cutB, resA, resB, fDrive, envAmt, keyTrack, velGain, bend, wtTuneR,
      spreadPan;
  Ramp fmIdx, fmEnvAmt, fmFbR;
  bool fmOn = false;
  float fmGate = 0.0f, fmGateAlpha = 0.01f, lastFmDecay = -1.0f,
        lastGlideMs = -1.0f;
  double fmPhase = 0.0, fmModPrev = 0.0;
  float fmY1 = 0.0f, fmY2 = 0.0f, fmEnvLevel = 0.0f, fmEnvMul = 0.999f,
        fmRatio = 1.0f, fmKey = 0.0f;
  int note = -1, pendingNote = -1, stealPos = 0, stealTotal = 768;
  float pendingVel = 0.0f;
  bool active = false, fresh = true, stealPending = false, pendingOff = false,
       pendingSnap = false;
};

//==============================================================================
/** 16-voice poly / mono / legato voice manager with portable SoA SIMD
   dual-ladder filtering, fixed-cadence odd-block-invariant modulation, smooth
   polyphonic headroom scaling, and raised-cosine de-click voice stealing. */
class VoiceManager {
public:
  static constexpr int kVoices = 16;
  static constexpr int kOsLog2 = 2; // voices run at 4x the host rate
  static constexpr int kOs = 1 << kOsLog2;
  static constexpr int kControlStep =
      8; // fixed base-rate modulation cadence (8 samples = 6 kHz @ 48k)
  static constexpr int kSub =
      kControlStep * kOs; // 32 oversampled samples per control tick

  void prepare(float baseSr) {
    osr = (float)kOs * baseSr;
    for (int i = 0; i < kVoices; ++i) {
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

  void reseed(uint32_t seedVal) noexcept {
    for (int i = 0; i < kVoices; ++i)
      voices[(size_t)i].reseed(seedVal ^ (uint32_t)(i * 7717 + 101));
  }

  void allOff() {
    for (int v = 0; v < kVoices; ++v) {
      voices[(size_t)v].reset();
      ladA.resetLane(v);
      ladB.resetLane(v);
      lastCutA[v] = lastCutB[v] = -1.0f;
    }
    polySmooth.snap(1.0f);
    stackSize = 0;
    sustain = false;
    std::fill(std::begin(sustained), std::end(sustained), false);
  }

  void setTable(const WavetableBank *b) noexcept {
    for (auto &v : voices)
      v.setTable(b);
  }
  void snapFilter(bool fltOn) noexcept { fltFade.snap(fltOn); }

  void setMode(int m) noexcept {
    if (m != mode) {
      for (auto &v : voices)
        v.release();
      mode = m;
    }
  }

  void noteOn(int note, float vel) {
    if (note < 0 || note >= 128)
      return;
    pushNote(note);
    sustained[note] = false;
    noteVel[note] = vel;

    if (mode == 0) {
      Voice *target = nullptr;
      for (auto &v : voices)
        if (v.isActive() && v.getNote() == note) {
          target = &v;
          break;
        }
      if (!target)
        for (auto &v : voices)
          if (!v.isActive()) {
            target = &v;
            break;
          }
      if (!target) { // steal: prefer released voices, then the oldest
        uint32_t best = 0;
        for (auto &v : voices) {
          const uint32_t score = v.age + (v.gateHeld ? 0u : 100000000u);
          if (!target || score > best) {
            target = &v;
            best = score;
          }
        }
      }
      target->start(note, vel, true, false);
      target->gateHeld = true;
    } else {
      auto &v = voices[0];
      const bool legato = mode == 2 && stackSize > 1 && v.isActive();
      v.start(note, vel, !legato, false);
      v.gateHeld = true;
    }
  }

  void noteOff(int note) {
    if (note < 0 || note >= 128)
      return;
    if (sustain) {
      sustained[note] = true;
      return;
    }
    releaseNote(note);
  }

  void setSustain(bool on) {
    sustain = on;
    if (!on)
      for (int n = 0; n < 128; ++n)
        if (sustained[n]) {
          sustained[n] = false;
          releaseNote(n);
        }
  }

  void renderSlice(float *bodyL, float *bodyR, float *sub, int n2,
                   const std::array<VoiceParams, kVoices> &vp, bool fltOn,
                   int route, bool isCtrlTick, int ctrlSpanOs = kSub) {
    uint32_t activeMask = 0u;
    float envSum = 0.0f;
    for (int v = 0; v < kVoices; ++v) {
      auto &voice = voices[(size_t)v];
      if (!voice.isActive()) {
        if (isCtrlTick) {
          ladA.holdLane(v);
          ladB.holdLane(v);
        }
        continue;
      }
      activeMask |= (1u << v);
      envSum += voice.envLevel();

      if (isCtrlTick || voice.isFresh()) {
        const bool wasFresh = voice.isFresh();
        voice.control(vp[(size_t)v], ctrlSpanOs);
        float cA, rA, cB, rB, drive;
        voice.beginControlBlock(ctrlSpanOs, cA, rA, cB, rB, drive);
        const bool snap = voice.ladderSnap || wasFresh;
        if (snap) {
          ladA.resetLane(v);
          ladB.resetLane(v);
          voice.ladderSnap = false;
          lastCutA[v] = lastCutB[v] = -1.0f;
        }
        if (std::abs(cA - lastCutA[v]) > 1.0e-3f) {
          lastCutA[v] = cA;
          cachedGA[v] = LadderBank<kVoices>::coefGD((double)cA, (double)osr);
        }
        if (std::abs(cB - lastCutB[v]) > 1.0e-3f) {
          lastCutB[v] = cB;
          cachedGB[v] = LadderBank<kVoices>::coefGD((double)cB, (double)osr);
        }
        ladA.setLane(v, cachedGA[v], 4.3 * (double)rA, (double)drive,
                     ctrlSpanOs, snap);
        ladB.setLane(v, cachedGB[v], 4.3 * (double)rB, (double)drive,
                     ctrlSpanOs, snap);
      }
    }
    if (activeMask == 0u) {
      fltFade.snap(fltOn);
      polySmooth.snap(1.0f);
      return;
    }

    // Smooth polyphonic headroom compensation: unity (1.0) for 1 voice,
    // 1/sqrt(N) for chords (prevents bus clipping)
    if (isCtrlTick)
      polySmooth.set(1.0f / std::sqrt(std::max(1.0f, envSum)));

    alignas(32) float osc[kVoices][kSub];
    alignas(32) float gain[kVoices][kSub];
    alignas(32) float outA[kVoices][kSub];
    alignas(32) float outB[kVoices][kSub];
    float panL[kVoices]{}, panR[kVoices]{};

    for (int v = 0; v < kVoices; ++v) {
      if ((activeMask & (1u << v)) == 0u)
        continue;
      auto &voice = voices[(size_t)v];
      voice.renderSamples(n2, osc[v], gain[v], sub);
      voice.pans(panL[v], panR[v]);
    }

    const bool runFilters = fltOn || !fltFade.settled(fltOn);
    float (*filtered)[kSub] = osc;
    if (runFilters) {
      switch (route) {
      case 0:
        ladA.processSoA(osc, outA, n2, activeMask);
        ladB.processSoA(outA, outB, n2, activeMask);
        filtered = outB;
        break;
      case 1:
        ladA.processSoA(osc, outA, n2, activeMask);
        ladB.processSoA(osc, outB, n2, activeMask);
        for (int v = 0; v < kVoices; ++v) {
          if ((activeMask & (1u << v)) == 0u)
            continue;
          for (int i = 0; i < n2; ++i)
            outA[v][i] = 0.5f * (outA[v][i] + outB[v][i]);
        }
        filtered = outA;
        break;
      default:
        ladA.processSoA(osc, outA, n2, activeMask);
        filtered = outA;
        break;
      }

      if (fltFade.settled(fltOn)) {
        if (!fltOn)
          filtered = osc;
      } else {
        float fg[kSub];
        for (int i = 0; i < n2; ++i)
          fg[i] = fltFade.next(fltOn);
        for (int v = 0; v < kVoices; ++v) {
          if ((activeMask & (1u << v)) == 0u)
            continue;
          for (int i = 0; i < n2; ++i)
            filtered[v][i] = osc[v][i] + fg[i] * (filtered[v][i] - osc[v][i]);
        }
      }
    }

    // Accumulate active voices into stereo oversampled bus with smooth
    // polyphonic headroom scaling
    float pg[kSub];
    for (int i = 0; i < n2; ++i)
      pg[i] = polySmooth.next();

    for (int v = 0; v < kVoices; ++v) {
      if ((activeMask & (1u << v)) == 0u)
        continue;
      const float pl = panL[v], pr = panR[v];
      const float *__restrict yv = filtered[v];
      const float *__restrict gv = gain[v];
      for (int i = 0; i < n2; ++i) {
        const float s = yv[i] * gv[i] * pg[i];
        bodyL[i] += s * pl;
        bodyR[i] += s * pr;
      }
    }
    for (int i = 0; i < n2; ++i)
      sub[i] *= pg[i];

    const uint32_t baseSamples = (uint32_t)std::max(1, n2 / kOs);
    for (int v = 0; v < kVoices; ++v)
      if ((activeMask & (1u << v)) != 0u)
        voices[(size_t)v].advanceAge(baseSamples);
  }

  /** Backwards-compatible wrapper for any standalone caller rendering arbitrary
   * n2. */
  void render(float *bodyL, float *bodyR, float *sub, int n2,
              const std::array<VoiceParams, kVoices> &vp, bool fltOn,
              int route) {
    for (int start = 0; start < n2; start += kSub) {
      const int m = std::min(kSub, n2 - start);
      renderSlice(bodyL + start, bodyR + start, sub + start, m, vp, fltOn,
                  route, true, m);
    }
  }

  bool anyHeld() const noexcept { return stackSize > 0; }
  int lowestHeld() const noexcept {
    int lo = 128;
    for (int i = 0; i < stackSize; ++i)
      lo = std::min(lo, stack[i]);
    return lo == 128 ? -1 : lo;
  }
  int lastHeld() const noexcept {
    return stackSize > 0 ? stack[stackSize - 1] : -1;
  }
  bool anyActive() const noexcept {
    for (auto &v : voices)
      if (v.isActive())
        return true;
    return false;
  }
  bool voiceActive(int v) const noexcept {
    return voices[(size_t)v].isActive();
  }
  float voiceVelocity(int v) const noexcept {
    return voices[(size_t)v].getVelocity();
  }

private:
  void pushNote(int note) {
    removeFromStack(note);
    if (stackSize < 128)
      stack[stackSize++] = note;
  }
  void removeFromStack(int note) {
    int w = 0;
    for (int i = 0; i < stackSize; ++i)
      if (stack[i] != note)
        stack[w++] = stack[i];
    stackSize = w;
  }
  void releaseNote(int note) {
    removeFromStack(note);
    if (mode == 0) {
      for (auto &v : voices)
        if (v.isActive() && v.getNote() == note && v.gateHeld) {
          v.release();
          v.gateHeld = false;
        }
    } else {
      auto &v = voices[0];
      if (stackSize > 0) {
        const int back = stack[stackSize - 1];
        if (v.getNote() != back) {
          if (mode == 2)
            v.glideTo(back);
          else
            v.start(back, noteVel[back], true, false);
        }
      } else if (v.gateHeld) {
        v.release();
        v.gateHeld = false;
      }
    }
  }

  std::array<Voice, kVoices> voices;
  LadderBank<kVoices> ladA, ladB;
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
