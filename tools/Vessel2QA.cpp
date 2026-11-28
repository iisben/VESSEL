// VESSEL II — Permanent Headless 8-Gate DSP QA Harness
// Zero GUI, zero OpenGL, zero OS audio-device dependencies.
// Compiles cleanly on macOS, Windows (MSVC), and Linux (GCC/Clang).
//
// Usage:
//   Vessel2QA [--baseline] [--strict] [--gates 1,2,3,4,5,6,7,8]
//             [--out-dir <dir>] [--compare-v1 <csv>]
//
// Locked DSP QA thresholds (never weakened):
//   Gate 1: Static/WT/VA aliasing < -80 dBc @ C6, < -75 dBc @ C7;
//           High-index FM (index 3.0 @ C5, 1:1 & 1:1.414) < -70 dBc (or >= 18 dB over naive 1x)
//   Gate 2: Bright saw harmonics extend to >= min(0.38*fs, 18000 Hz) @ 48 & 96 kHz;
//           Min-attack reaches 90% peak within attack window, zero DC step discontinuity
//   Gate 3: ADAA F1'==f (and F2'==F1) within 1e-5 + continuity across |dx|~eps;
//           Driven saturators @ 6/8/10 kHz (-6 dBFS): aliasing < -80 dBc (or >= 20 dB vs naive 1x), DC < -90 dBFS
//   Gate 4: Audio-rate 1 kHz cutoff mod (20 Hz-18 kHz) @ 95-100% res across 44.1/48/96/192 kHz: bounded, 0 NaN/Inf;
//           Self-osc C3-C6: ±5 ct SVF, ±15 ct 4-pole ladder; rate-invariant mag ±0.5 dB up to 12 kHz
//   Gate 5: Uniform 512 vs odd/prime blocks (0,1,7,13,64,137,2048) residual < -100 dBFS;
//           Rapid prepareToPlay rate switches (44.1->192->48->96->48); 0-sample PDC error (0 latency when bypassed)
//   Gate 6: Voice stealing dry 50ms 100Hz sine/LPF: max 2nd-diff <= 5x steady, DC < -90 dBFS;
//           10s idle tail < -120 dBFS, active voices == 0, idle CPU < 0.5% RT
//   Gate 7: Deterministic V1 factory preset snapshots (RMS, peak, centroid, sub-120 Hz stereo phase correlation)
//   Gate 8: Release (-O3) polyphonic stress CPU (median >=200 512-sample blocks @ 48 kHz): < 15.0% RT

#include "PluginProcessor.h"
#include "Presets.h"
#include "dsp/Tables.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace vessel;
namespace fs = std::filesystem;

namespace
{
constexpr double kPiD    = std::numbers::pi_v<double>;
constexpr double kTwoPiD = 2.0 * kPiD;

enum class GateStatus { Pass, Fail, Pending };

const char* statusStr (GateStatus s)
{
    switch (s)
    {
        case GateStatus::Pass:    return "PASS";
        case GateStatus::Fail:    return "FAIL";
        case GateStatus::Pending: return "PENDING";
    }
    return "UNKNOWN";
}

struct GateResult
{
    int id = 0;
    std::string name;
    GateStatus status = GateStatus::Pass;
    std::string summary;
    std::vector<std::string> details;
};

struct RenderBuf
{
    std::vector<float> l, r;
    int nonFiniteCount = 0;
};

// Optional QA deterministic PRNG hook (activated automatically in Stage 3B when VesselProcessor exposes it)
template <typename T>
concept HasQaSeed = requires (T& p, uint32_t s) { p.setQaDeterministicSeed (s); };

template <typename T>
void maybeSeedQa (T& p, uint32_t seed)
{
    if constexpr (HasQaSeed<T>)
        p.setQaDeterministicSeed (seed);
    else
        (void) p, (void) seed;
}

template <typename T>
constexpr bool kHasQaSeed = HasQaSeed<T>;

// Optional ADAA1/ADAA2 verification hook (activated automatically in Stage 3A when TubeDrive exposes adaaF1/adaaEval)
template <typename T>
concept HasAdaa1Api = requires (double x, double xPrev, double b, int mode)
{
    { T::adaaF0 (x, b, mode) } -> std::convertible_to<double>;
    { T::adaaF1 (x, b, mode) } -> std::convertible_to<double>;
    { T::adaa1Eval (x, xPrev, b, mode) } -> std::convertible_to<double>;
};

template <typename T>
concept HasAdaa2Api = requires (double x, double b, int mode)
{
    { T::adaaF2 (x, b, mode) } -> std::convertible_to<double>;
};

double linToDb (double x) { return 20.0 * std::log10 (std::max (std::abs (x), 1.0e-15)); }
double powToDb (double p) { return 10.0 * std::log10 (std::max (p, 1.0e-30)); }
double midiToHzD (double note) { return 440.0 * std::exp2 ((note - 69.0) / 12.0); }

void setP (VesselProcessor& p, int pid, float plain)
{
    auto* prm = p.param (pid);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
}

void setCleanDryPatch (VesselProcessor& p)
{
    for (int i = 0; i < NUM_PARAMS; ++i)
        p.param (i)->setValueNotifyingHost (p.param (i)->getDefaultValue());

    for (int pid : { sub_on, drone_on, noise_on, gran_on, tube_on, bias_on, crush_on, vhs_on,
                     flt_on, envf_on, unmask_on, form_on, res_on, z_on, conv_on, verb_on,
                     haas_on, ott_on, mono_on, duck_on, exc_on, clip_on, fm_on })
        setP (p, pid, 0.0f);

    setP (p, tape_mode, 0.0f);
    setP (p, wt_on, 1.0f);
    setP (p, wt_table, 0.0f);
    setP (p, wt_level, 0.9f);
    setP (p, wt_morph, 2.0f / 7.0f);   // built-in saw frame 2
    setP (p, wt_pw, 0.5f);
    setP (p, wt_pwm, 0.0f);
    setP (p, wt_drift, 0.0f);
    setP (p, wt_oct, 2.0f);            // 0 oct
    setP (p, wt_semi, 0.0f);
    setP (p, wt_fine, 0.0f);
    setP (p, voice_mode, 0.0f);        // Poly
    setP (p, glide, 0.0f);
    setP (p, vel_amp, 0.0f);
    setP (p, amp_a, 1.0f);
    setP (p, amp_d, 100.0f);
    setP (p, amp_s, 1.0f);
    setP (p, amp_r, 50.0f);
    setP (p, out_gain, 0.0f);
    setP (p, in_level, 0.0f);
}

std::unique_ptr<VesselProcessor> makeProcessor (double sr, int blockSize)
{
    auto p = std::make_unique<VesselProcessor>();
    p->setPlayConfigDetails (2, 2, sr, blockSize);
    p->prepareToPlay (sr, blockSize);
    return p;
}

struct MidiEvent
{
    int samplePos = 0;
    juce::MidiMessage msg;
};

RenderBuf renderEvents (VesselProcessor& p, double /*sr*/, int totalSamples,
                        const std::vector<int>& blockPattern,
                        const std::vector<MidiEvent>& events,
                        const std::vector<float>* extInL = nullptr,
                        const std::vector<float>* extInR = nullptr)
{
    RenderBuf out;
    out.l.assign ((size_t) totalSamples, 0.0f);
    out.r.assign ((size_t) totalSamples, 0.0f);

    int maxBlk = 1;
    for (int b : blockPattern) maxBlk = std::max (maxBlk, b);
    juce::AudioBuffer<float> buf (2, maxBlk);

    int pos = 0;
    size_t patIdx = 0;
    int zeroGuard = 0;
    while (pos < totalSamples)
    {
        const int req = blockPattern[patIdx % blockPattern.size()];
        ++patIdx;
        const int n = std::min (req, totalSamples - pos);
        if (n == 0)
        {
            juce::AudioBuffer<float> emptyBuf (2, 0);
            juce::MidiBuffer emptyMidi;
            p.processBlock (emptyBuf, emptyMidi);
            if (++zeroGuard > 100000) break;
            continue;
        }
        buf.setSize (2, n, false, false, true);
        if (extInL && extInR)
        {
            std::copy_n (extInL->data() + pos, n, buf.getWritePointer (0));
            std::copy_n (extInR->data() + pos, n, buf.getWritePointer (1));
        }
        else
        {
            buf.clear();
        }

        juce::MidiBuffer midi;
        for (const auto& ev : events)
            if (ev.samplePos >= pos && ev.samplePos < pos + n)
                midi.addEvent (ev.msg, ev.samplePos - pos);

        p.processBlock (buf, midi);
        const float* rl = buf.getReadPointer (0);
        const float* rr = buf.getReadPointer (1);
        for (int i = 0; i < n; ++i)
        {
            if (! std::isfinite (rl[i]) || ! std::isfinite (rr[i])) ++out.nonFiniteCount;
            out.l[(size_t) (pos + i)] = rl[i];
            out.r[(size_t) (pos + i)] = rr[i];
        }
        pos += n;
    }
    return out;
}

// 4-term Blackman-Harris FFT spectrum (magnitude squared per bin 0..N/2)
std::vector<double> bhSpectrumPow (const std::vector<float>& x, size_t start, int order)
{
    const int N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) N * 2, 0.0f);
    for (int i = 0; i < N; ++i)
    {
        const double t = (double) i / (double) (N - 1);
        const double w = 0.35875
                       - 0.48829 * std::cos (kTwoPiD * t)
                       + 0.14128 * std::cos (4.0 * kPiD * t)
                       - 0.01168 * std::cos (6.0 * kPiD * t);
        buf[(size_t) i] = (float) ((double) x[start + (size_t) i] * w);
    }
    fft.performFrequencyOnlyForwardTransform (buf.data());
    std::vector<double> powSpec ((size_t) (N / 2 + 1), 0.0);
    for (int k = 0; k <= N / 2; ++k)
    {
        const double m = (double) buf[(size_t) k];
        powSpec[(size_t) k] = m * m;
    }
    return powSpec;
}

struct AliasMetrics
{
    double aliasedDbc = 0.0;      // 10*log10(aliasedEnergy / harmonicEnergy)
    double worstSpurDbc = 0.0;    // 10*log10(maxAliasedBin / fundPower)
    double dcDbfs = -300.0;
};

// 4-term Blackman-Harris windowed DC estimator (exact gain 1.0 at 0 Hz, <-92 dB sidelobes so periodic tones don't leak into DC)
double windowedDcDbfs (const std::vector<float>& x, size_t start, size_t len)
{
    double num = 0.0, den = 0.0;
    const double inv = 1.0 / (double) std::max<size_t> (1, len - 1);
    for (size_t i = 0; i < len; ++i)
    {
        const double t = (double) i * inv;
        const double w = 0.35875
                       - 0.48829 * std::cos (kTwoPiD * t)
                       + 0.14128 * std::cos (4.0 * kPiD * t)
                       - 0.01168 * std::cos (6.0 * kPiD * t);
        num += w * (double) x[start + i];
        den += w;
    }
    return linToDb (num / std::max (den, 1.0e-30));
}

AliasMetrics measureHarmonicAliasing (const std::vector<float>& x, double sr, double f0, int order = 16)
{
    const int N = 1 << order;
    const size_t start = x.size() - (size_t) N;
    auto spec = bhSpectrumPow (x, start, order);
    const double binHz = sr / (double) N;
    const double nyq = 0.5 * sr;
    const double bandHi = std::min (20000.0, 0.45 * sr);   // audible passband below oversampler transition band

    double harmPow = 0.0, aliasPow = 0.0, maxSpur = 0.0, fundPow = 0.0;
    const int fundBin = (int) std::llround (f0 / binHz);
    for (int k = std::max (1, fundBin - 4); k <= std::min (N / 2 - 1, fundBin + 4); ++k)
        fundPow += spec[(size_t) k];

    for (int k = 5; k < N / 2 - 4; ++k)   // exclude DC lobe (0..4) and Nyquist edge
    {
        const double f = (double) k * binHz;
        if (f < 20.0 || f > bandHi) continue;
        const double h = std::round (f / f0);
        const double nearest = h * f0;
        const bool isHarm = (h >= 1.0 && nearest < nyq && std::abs (f - nearest) <= 4.0 * binHz);
        if (isHarm)
            harmPow += spec[(size_t) k];
        else
        {
            aliasPow += spec[(size_t) k];
            maxSpur = std::max (maxSpur, spec[(size_t) k]);
        }
    }

    AliasMetrics m;
    m.aliasedDbc = powToDb ((aliasPow + 1.0e-30) / (harmPow + 1.0e-30));
    m.worstSpurDbc = powToDb ((maxSpur + 1.0e-30) / (std::max (fundPow, harmPow) + 1.0e-30));
    m.dcDbfs = windowedDcDbfs (x, start, (size_t) N);
    return m;
}

AliasMetrics measureFmBesselAliasing (const std::vector<float>& x, double sr, double fc, double fm, int order = 16)
{
    const int N = 1 << order;
    const size_t start = x.size() - (size_t) N;
    auto spec = bhSpectrumPow (x, start, order);
    const double binHz = sr / (double) N;
    const double nyq = 0.5 * sr;
    const double bandHi = std::min (20000.0, 0.45 * sr);

    // Collect analytical non-aliased Bessel sidebands |fc ± k * fm| where |fc + k*fm| < nyq
    std::vector<double> validFreqs;
    for (int k = -32; k <= 32; ++k)
    {
        const double fSigned = fc + (double) k * fm;
        if (std::abs (fSigned) < nyq && std::abs (fSigned) > 1.0)
            validFreqs.push_back (std::abs (fSigned));
    }

    double sigPow = 0.0, aliasPow = 0.0, maxSpur = 0.0;
    for (int k = 5; k < N / 2 - 4; ++k)
    {
        const double f = (double) k * binHz;
        if (f < 20.0 || f > bandHi) continue;
        bool isValid = false;
        for (double vf : validFreqs)
        {
            if (std::abs (f - vf) <= 4.0 * binHz) { isValid = true; break; }
        }
        if (isValid) sigPow += spec[(size_t) k];
        else { aliasPow += spec[(size_t) k]; maxSpur = std::max (maxSpur, spec[(size_t) k]); }
    }

    AliasMetrics m;
    m.aliasedDbc = powToDb ((aliasPow + 1.0e-30) / (sigPow + 1.0e-30));
    m.worstSpurDbc = powToDb ((maxSpur + 1.0e-30) / (sigPow + 1.0e-30));
    m.dcDbfs = windowedDcDbfs (x, start, (size_t) N);
    return m;
}

// Parabolic FFT peak frequency estimator on steady tail
double estimatePitchHz (const std::vector<float>& x, double sr, int order = 16)
{
    const int N = 1 << order;
    const size_t start = x.size() - (size_t) N;
    auto spec = bhSpectrumPow (x, start, order);
    int bestK = 5;
    double bestP = 0.0;
    for (int k = 5; k < N / 2 - 2; ++k)
    {
        if (spec[(size_t) k] > bestP) { bestP = spec[(size_t) k]; bestK = k; }
    }
    const double l0 = std::log (std::max (spec[(size_t) (bestK - 1)], 1.0e-30));
    const double l1 = std::log (std::max (spec[(size_t) bestK],       1.0e-30));
    const double l2 = std::log (std::max (spec[(size_t) (bestK + 1)], 1.0e-30));
    const double denom = (l0 - 2.0 * l1 + l2);
    const double delta = (std::abs (denom) > 1.0e-12) ? 0.5 * (l0 - l2) / denom : 0.0;
    return ((double) bestK + std::clamp (delta, -0.5, 0.5)) * (sr / (double) N);
}

std::string fmt (double v, int prec = 2)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision (prec) << v;
    return oss.str();
}

//==============================================================================
// GATE 1 — Oscillator & FM Aliasing Floor (SFDR in dBc)
//==============================================================================
GateResult runGate1()
{
    GateResult res { 1, "Oscillator & FM Aliasing Floor", GateStatus::Pass, "", {} };
    constexpr double sr = 48000.0;
    const int total = (int) (1.6 * sr);   // > 65536 steady samples after settling

    struct OscCase { const char* name; int midi; float morph; float pw; double limitDbc; };
    const OscCase oscCases[] {
        { "Saw C4",     60, 2.0f / 7.0f, 0.50f, -80.0 },
        { "Saw C6",     84, 2.0f / 7.0f, 0.50f, -80.0 },
        { "Saw C7",     96, 2.0f / 7.0f, 0.50f, -75.0 },
        { "Square C6",  84, 3.0f / 7.0f, 0.50f, -80.0 },
        { "Square C7",  96, 3.0f / 7.0f, 0.50f, -75.0 },
        { "PWM-25 C6",  84, 2.0f / 7.0f, 0.25f, -80.0 },
        { "PWM-25 C7",  96, 2.0f / 7.0f, 0.25f, -75.0 },
    };

    double worstC6 = -300.0, worstC7 = -300.0;
    for (const auto& c : oscCases)
    {
        auto p = makeProcessor (sr, 512);
        setCleanDryPatch (*p);
        setP (*p, wt_morph, c.morph);
        setP (*p, wt_pw, c.pw);
        p->prepareToPlay (sr, 512);

        auto r = renderEvents (*p, sr, total, { 512 }, { { 0, juce::MidiMessage::noteOn (1, c.midi, 1.0f) } });
        const double f0 = midiToHzD (c.midi);
        const auto m = measureHarmonicAliasing (r.l, sr, f0);
        const bool ok = (r.nonFiniteCount == 0) && (m.aliasedDbc < c.limitDbc);
        if (! ok) res.status = GateStatus::Fail;
        if (c.midi == 84) worstC6 = std::max (worstC6, m.aliasedDbc);
        if (c.midi == 96) worstC7 = std::max (worstC7, m.aliasedDbc);
        res.details.push_back (std::string (c.name) + ": alias=" + fmt (m.aliasedDbc, 1) + " dBc (spur="
                               + fmt (m.worstSpurDbc, 1) + " dBc, lim<" + fmt (c.limitDbc, 0) + ") [" + (ok ? "OK" : "FAIL") + "]");
    }

    // High-index FM/PM at C5 (MIDI 72, 523.25 Hz), index = 3.0 rad (fm_index = 0.5 since 12*x^2 = 3.0)
    struct FmCase { const char* name; int ratioChoice; float fineCents; double actualRatio; };
    const FmCase fmCases[] {
        { "FM C5 1:1 idx=3.0",     3,    0.0f, 1.0 },
        { "FM C5 1:1.414 idx=3.0", 4, -100.0f, 1.5 * std::exp2 (-100.0 / 1200.0) },
    };

    double worstFm = -300.0;
    for (const auto& c : fmCases)
    {
        const double fc = midiToHzD (72);
        const double fm = fc * c.actualRatio;
        auto p = makeProcessor (sr, 512);
        setCleanDryPatch (*p);
        setP (*p, wt_morph, 0.0f);   // sine carrier for exact analytical Bessel sidebands
        setP (*p, fm_on, 1.0f);
        setP (*p, fm_ratio, (float) c.ratioChoice);
        setP (*p, fm_fine, c.fineCents);
        setP (*p, fm_index, 0.5f);   // beta = 12 * 0.5^2 = 3.0 rad
        setP (*p, fm_feedback, 0.0f);
        setP (*p, fm_env, 0.0f);
        setP (*p, fm_key, 0.0f);
        p->prepareToPlay (sr, 512);

        auto r = renderEvents (*p, sr, total, { 512 }, { { 0, juce::MidiMessage::noteOn (1, 72, 1.0f) } });
        const auto m = measureFmBesselAliasing (r.l, sr, fc, fm);

        // Naive 1x saw-carrier or high-index PM reference (same fc, fm, index=3.0 at 1x 48 kHz)
        std::vector<float> naive ((size_t) total, 0.0f);
        for (int i = 0; i < total; ++i)
        {
            const double t = (double) i / sr;
            naive[(size_t) i] = (float) std::sin (kTwoPiD * fc * t + 3.0 * std::sin (kTwoPiD * fm * t));
        }
        const auto mNaive = measureFmBesselAliasing (naive, sr, fc, fm);
        const double suppDb = mNaive.aliasedDbc - m.aliasedDbc;
        const bool ok = (r.nonFiniteCount == 0) && (m.aliasedDbc < -70.0 || suppDb >= 18.0);
        if (! ok) res.status = GateStatus::Fail;
        worstFm = std::max (worstFm, m.aliasedDbc);
        res.details.push_back (std::string (c.name) + ": alias=" + fmt (m.aliasedDbc, 1)
                               + " dBc (naive1x=" + fmt (mNaive.aliasedDbc, 1) + ", supp=" + fmt (suppDb, 1)
                               + " dB, lim<-70) [" + (ok ? "OK" : "FAIL") + "]");
    }

    res.summary = "worstC6=" + fmt (worstC6, 1) + " dBc, worstC7=" + fmt (worstC7, 1) + " dBc, worstFM=" + fmt (worstFm, 1) + " dBc";
    return res;
}

//==============================================================================
// GATE 2 — Bandwidth & Transient Preservation (Anti-Over-Damping)
//==============================================================================
GateResult runGate2()
{
    GateResult res { 2, "Bandwidth & Transient Preservation", GateStatus::Pass, "", {} };

    // Part A: Bright saw harmonics at C4 @ 48 kHz and 96 kHz
    double minMaxHz = 1.0e9;
    for (double sr : { 48000.0, 96000.0 })
    {
        const int order = 16;
        const int total = (1 << order) + (int) (0.25 * sr);
        auto p = makeProcessor (sr, 512);
        setCleanDryPatch (*p);
        setP (*p, wt_morph, 2.0f / 7.0f);   // bright saw
        p->prepareToPlay (sr, 512);

        auto r = renderEvents (*p, sr, total, { 512 }, { { 0, juce::MidiMessage::noteOn (1, 60, 1.0f) } });
        const int N = 1 << order;
        const size_t start = r.l.size() - (size_t) N;
        auto spec = bhSpectrumPow (r.l, start, order);
        const double binHz = sr / (double) N;
        const double f0 = midiToHzD (60);

        auto harmPower = [&] (int h)
        {
            const int c = (int) std::llround ((double) h * f0 / binHz);
            double sum = 0.0;
            for (int k = std::max (1, c - 3); k <= std::min (N / 2 - 1, c + 3); ++k) sum += spec[(size_t) k];
            return sum;
        };

        const double p1 = harmPower (1);
        const double targetHz = std::min (0.38 * sr, 18000.0);
        const int targetH = (int) std::floor (targetHz / f0);
        double highestValidHz = 0.0;
        double droopAtTargetDb = 0.0;
        for (int h = 1; h <= targetH + 2; ++h)
        {
            const double f = (double) h * f0;
            if (f >= 0.49 * sr) break;
            const double relDb = powToDb ((harmPower (h) + 1.0e-30) / (p1 + 1.0e-30));
            const double idealDb = -20.0 * std::log10 ((double) h);
            const double droopDb = idealDb - relDb;   // positive means attenuated vs ideal 1/h saw
            if (h == targetH) droopAtTargetDb = droopDb;
            if (droopDb <= 6.0) highestValidHz = f;
        }
        minMaxHz = std::min (minMaxHz, highestValidHz);
        const bool ok = (highestValidHz >= targetHz - f0) && (droopAtTargetDb <= 6.0);
        if (! ok) res.status = GateStatus::Fail;
        res.details.push_back ("Saw C4 @" + fmt (sr / 1000.0, 0) + "k: bw=" + fmt (highestValidHz, 0)
                               + " Hz (target>=" + fmt (targetHz, 0) + " Hz, droop=" + fmt (droopAtTargetDb, 2) + " dB) [" + (ok ? "OK" : "FAIL") + "]");
    }

    // Part B: Minimum-attack envelope rise time & zero single-sample DC step discontinuity
    constexpr double sr = 48000.0;
    dsp::Adsr env;
    env.setSampleRate (sr);
    juce::ADSR::Parameters ap { 0.0005f, 0.1f, 1.0f, 0.05f };   // 0.5 ms attack
    env.setParameters (ap);
    env.noteOn();
    int hit90 = -1;
    float maxEnvStep = 0.0f, prevE = 0.0f;
    for (int i = 0; i < 480; ++i)
    {
        const float e = env.getNextSample();
        maxEnvStep = std::max (maxEnvStep, std::abs (e - prevE));
        prevE = e;
        if (hit90 < 0 && e >= 0.90f) hit90 = i;
    }
    const double adsrRiseMs = (hit90 >= 0) ? (double) (hit90 + 1) * 1000.0 / sr : 999.0;
    const bool adsrOk = (hit90 >= 0 && adsrRiseMs <= 0.60 && maxEnvStep < 0.25f);

    // End-to-end minimum attack on C7 sine (period 0.48 ms) after 2048-sample idle settle
    auto p = makeProcessor (sr, 512);
    setCleanDryPatch (*p);
    setP (*p, wt_morph, 0.0f);   // sine
    setP (*p, amp_a, 0.5f);      // 0.5 ms min attack
    p->prepareToPlay (sr, 512);
    constexpr int noteAt = 2048;
    auto r = renderEvents (*p, sr, noteAt + 4800, { 512 }, { { noteAt, juce::MidiMessage::noteOn (1, 96, 1.0f) } });

    float steadyPeak = 0.0f, steadyMaxStep = 0.0f;
    for (int i = noteAt + 2400; i < noteAt + 4799; ++i)
    {
        steadyPeak = std::max (steadyPeak, std::abs (r.l[(size_t) i]));
        steadyMaxStep = std::max (steadyMaxStep, std::abs (r.l[(size_t) (i + 1)] - r.l[(size_t) i]));
    }
    int onsetIdx = -1, reach90Idx = -1;
    float attackMaxStep = 0.0f;
    for (int i = noteAt; i < noteAt + 2400; ++i)
    {
        const float a = std::abs (r.l[(size_t) i]);
        if (onsetIdx < 0 && a > 0.02f * steadyPeak) onsetIdx = i;
        if (onsetIdx >= 0 && reach90Idx < 0 && a >= 0.90f * steadyPeak) reach90Idx = i;
        attackMaxStep = std::max (attackMaxStep, std::abs (r.l[(size_t) i] - r.l[(size_t) (i - 1)]));
    }
    const double procRiseMs = (onsetIdx >= 0 && reach90Idx >= onsetIdx)
                                ? (double) (reach90Idx - onsetIdx) * 1000.0 / sr : 999.0;
    const double stepRatio = attackMaxStep / std::max (steadyMaxStep, 1.0e-9f);
    const bool procOk = (procRiseMs <= 1.2 && stepRatio <= 2.5);
    if (! adsrOk || ! procOk) res.status = GateStatus::Fail;

    res.details.push_back ("ADSR 0.5ms attack: 90% in " + fmt (adsrRiseMs, 2) + " ms, maxStep=" + fmt (maxEnvStep, 3) + " [" + (adsrOk ? "OK" : "FAIL") + "]");
    res.details.push_back ("Proc 0.5ms attack: 90% in " + fmt (procRiseMs, 2) + " ms, stepRatio=" + fmt (stepRatio, 2) + "x [" + (procOk ? "OK" : "FAIL") + "]");
    res.summary = "minBW=" + fmt (minMaxHz, 0) + " Hz, attack90=" + fmt (adsrRiseMs, 2) + "/" + fmt (procRiseMs, 2) + " ms";
    return res;
}

template <typename T>
void checkAdaaApi (GateResult& res, bool& anyFail, bool& anyPending)
{
    if constexpr (HasAdaa1Api<T>)
    {
        double maxDerivErr = 0.0, maxContErr = 0.0;
        const double h = 1.0e-6;
        for (int mode = 0; mode < 3; ++mode)
        {
            const double b = 0.15;
            for (int i = -500; i <= 500; ++i)
            {
                const double x = (double) i * 0.02;   // [-10, 10]
                const double fNum = (T::adaaF1 (x + h, b, mode) - T::adaaF1 (x - h, b, mode)) / (2.0 * h);
                const double fRef = T::adaaF0 (x, b, mode);
                maxDerivErr = std::max (maxDerivErr, std::abs (fNum - fRef));

                if constexpr (HasAdaa2Api<T>)
                {
                    const double f1Num = (T::adaaF2 (x + h, b, mode) - T::adaaF2 (x - h, b, mode)) / (2.0 * h);
                    const double f1Ref = T::adaaF1 (x, b, mode);
                    maxDerivErr = std::max (maxDerivErr, std::abs (f1Num - f1Ref));
                }
            }
            // Continuity across |dx| ~ eps
            for (double x0 : { -1.5, -0.2, 0.0, 0.3, 1.5 })
            {
                const double fAt = T::adaaF0 (x0, b, mode);
                for (double dx : { 1.0e-12, 1.0e-9, 1.0e-7, 1.0e-6, 1.0e-5, 1.0e-4 })
                {
                    const double y = T::adaa1Eval (x0 + 0.5 * dx, x0 - 0.5 * dx, b, mode);
                    maxContErr = std::max (maxContErr, std::abs (y - fAt));
                }
            }
        }
        const bool ok = (maxDerivErr < 1.0e-5 && maxContErr < 1.0e-3);
        if (! ok) anyFail = true;
        res.details.push_back ("ADAA F1'==f maxErr=" + fmt (maxDerivErr, 7) + ", contErr=" + fmt (maxContErr, 6) + " [" + (ok ? "OK" : "FAIL") + "]");
    }
    else
    {
        anyPending = true;
        res.details.push_back ("ADAA F1'/F2' numerical derivative check: PENDING (ADAA not yet implemented — Stage 3A)");
    }
}

//==============================================================================
// GATE 3 — Nonlinearity & ADAA Integrity
//==============================================================================
GateResult runGate3()
{
    GateResult res { 3, "Nonlinearity & ADAA Integrity", GateStatus::Pass, "", {} };
    bool anyFail = false, anyPending = false;

    // Part A: Analytical ADAA derivative & continuity check
    checkAdaaApi<dsp::TubeDrive> (res, anyFail, anyPending);

    // Part B: Driven saturators @ 6, 8, 10 kHz (-6 dBFS = 0.501187) at 48 kHz
    constexpr double sr = 48000.0;
    constexpr int order = 16;
    const int N = 1 << order;
    const int total = N + 8192;
    const double amp = std::pow (10.0, -6.0 / 20.0);

    double worstAliasDbc = -300.0, worstDcDbfs = -300.0;
    // Non-submultiple prime-offset frequencies near 6, 8, 10 kHz so folded aliases never collide onto harmonic bins at 48 kHz
    const double freqs[] { 6017.0, 8009.0, 9973.0 };

    // Test TubeDrive modes 0/1/2 inside RuinCore
    const char* tubeNames[] { "Tube Triode", "Tube Pentode", "Tube Starved" };
    for (int mode = 0; mode < 3; ++mode)
    {
        double modeWorstAlias = -300.0, modeMinSupp = 300.0, modeWorstDc = -300.0;
        for (double f0 : freqs)
        {
            dsp::RuinCore rc;
            rc.prepare ((float) sr, 512);
            dsp::RuinCore::Params p {};
            p.tube.on = true; p.tube.mode = mode; p.tube.drive = 0.75f; p.tube.bias = 0.15f; p.tube.mix = 1.0f; p.tube.outDb = 0.0f;
            p.bias.on = false;
            rc.snapBias (false);

            std::vector<float> y ((size_t) total), rCh ((size_t) total), naive ((size_t) total);
            const float g = std::exp2 (p.tube.drive * 6.643856f);
            for (int i = 0; i < total; ++i)
            {
                const float x = (float) (amp * std::sin (kTwoPiD * f0 * (double) i / sr));
                y[(size_t) i] = x; rCh[(size_t) i] = x;
                naive[(size_t) i] = dsp::TubeDrive::shape (x * g, p.tube.bias, mode);
            }
            for (int pos = 0; pos < total; pos += 512)
            {
                const int n = std::min (512, total - pos);
                rc.process (y.data() + pos, rCh.data() + pos, n, p);
            }
            const auto m = measureHarmonicAliasing (y, sr, f0, order);
            const auto mn = measureHarmonicAliasing (naive, sr, f0, order);
            const double supp = mn.aliasedDbc - m.aliasedDbc;
            modeWorstAlias = std::max (modeWorstAlias, m.aliasedDbc);
            modeMinSupp = std::min (modeMinSupp, supp);
            modeWorstDc = std::max (modeWorstDc, m.dcDbfs);
        }
        const bool ok = (modeWorstAlias < -80.0 || modeMinSupp >= 20.0) && (modeWorstDc < -90.0);
        if (! ok) anyFail = true;
        worstAliasDbc = std::max (worstAliasDbc, modeWorstAlias);
        worstDcDbfs = std::max (worstDcDbfs, modeWorstDc);
        res.details.push_back (std::string (tubeNames[mode]) + " (6/8/10k): alias=" + fmt (modeWorstAlias, 1)
                               + " dBc, minSupp=" + fmt (modeMinSupp, 1) + " dB, DC=" + fmt (modeWorstDc, 1)
                               + " dBFS [" + (ok ? "OK" : "FAIL") + "]");
    }

    // Test OutputStage Clipper (+12 dB drive into -0.3 dBFS ceiling)
    {
        double clipWorstAlias = -300.0, clipMinSupp = 300.0, clipWorstDc = -300.0;
        for (double f0 : freqs)
        {
            dsp::OutputStage os;
            os.prepare ((float) sr, 512);
            os.snap (false, true);
            dsp::OutputStage::Params p {};
            p.excOn = false; p.clipOn = true; p.ceilingDb = -0.3f; p.gainDb = 12.0f;

            std::vector<float> y ((size_t) total), rCh ((size_t) total), naive ((size_t) total);
            const float g = dsp::dbToGain (12.0f);
            const float c = dsp::dbToGain (-0.3f);
            for (int i = 0; i < total; ++i)
            {
                const float x = (float) (amp * std::sin (kTwoPiD * f0 * (double) i / sr));
                y[(size_t) i] = x; rCh[(size_t) i] = x;
                naive[(size_t) i] = std::clamp (x * g, -c, c);
            }
            for (int pos = 0; pos < total; pos += 512)
            {
                const int n = std::min (512, total - pos);
                os.process (y.data() + pos, rCh.data() + pos, n, p);
            }
            const auto m = measureHarmonicAliasing (y, sr, f0, order);
            const auto mn = measureHarmonicAliasing (naive, sr, f0, order);
            const double supp = mn.aliasedDbc - m.aliasedDbc;
            clipWorstAlias = std::max (clipWorstAlias, m.aliasedDbc);
            clipMinSupp = std::min (clipMinSupp, supp);
            clipWorstDc = std::max (clipWorstDc, m.dcDbfs);
        }
        const bool ok = (clipWorstAlias < -80.0 || clipMinSupp >= 20.0) && (clipWorstDc < -90.0);
        if (! ok) anyFail = true;
        worstAliasDbc = std::max (worstAliasDbc, clipWorstAlias);
        worstDcDbfs = std::max (worstDcDbfs, clipWorstDc);
        res.details.push_back ("OutputStage Clipper +12dB (6/8/10k): alias=" + fmt (clipWorstAlias, 1)
                               + " dBc, minSupp=" + fmt (clipMinSupp, 1) + " dB, DC=" + fmt (clipWorstDc, 1)
                               + " dBFS [" + (ok ? "OK" : "FAIL") + "]");
    }

    // Part C: Magnetic Hysteresis & Iron Transformer Core (C6 & 6 kHz aliasing < -75 dBc, DC < -90 dBFS,
    // LF->HF operating-point modulation, and 0% drive clean 0-latency bypass)
    {
        double magWorstAlias = -300.0, magWorstDc = -300.0;
        bool magFinite = true;
        for (double f0 : { midiToHzD (84), 6017.0 })
        {
            dsp::RuinCore rc;
            rc.prepare ((float) sr, 512);
            dsp::RuinCore::Params p {};
            p.tube.on = false;
            p.bias.on = true;
            p.bias.sat = 0.70f;
            p.bias.sag = 0.25f;
            p.bias.amount = 0.10f;
            p.bias.speedMs = 80.0f;
            rc.snap (false, true);

            std::vector<float> y ((size_t) total), rCh ((size_t) total);
            for (int i = 0; i < total; ++i)
            {
                const float x = (float) (amp * std::sin (kTwoPiD * f0 * (double) i / sr));
                y[(size_t) i] = x;
                rCh[(size_t) i] = x;
            }
            for (int pos = 0; pos < total; pos += 512)
            {
                const int n = std::min (512, total - pos);
                rc.process (y.data() + pos, rCh.data() + pos, n, p);
            }
            for (float v : y) if (! std::isfinite (v)) magFinite = false;
            const auto m = measureHarmonicAliasing (y, sr, f0, order);
            magWorstAlias = std::max (magWorstAlias, m.aliasedDbc);
            magWorstDc = std::max (magWorstDc, m.dcDbfs);
        }

        // Verify low-frequency energy dynamically modulates upper-harmonic saturation operating point
        double lfModGainDb = 0.0;
        {
            auto measureH2Db = [&] (bool withBass)
            {
                dsp::RuinCore rc;
                rc.prepare ((float) sr, 512);
                dsp::RuinCore::Params p {};
                p.tube.on = false;
                p.bias.on = true;
                p.bias.sat = 0.65f;
                p.bias.sag = 0.0f;
                p.bias.amount = 0.0f;
                rc.snap (false, true);
                std::vector<float> y ((size_t) total), rCh ((size_t) total);
                for (int i = 0; i < total; ++i)
                {
                    const double t = (double) i / sr;
                    double x = 0.18 * std::sin (kTwoPiD * 3000.0 * t);
                    if (withBass) x += 0.55 * std::sin (kTwoPiD * 80.0 * t);
                    y[(size_t) i] = rCh[(size_t) i] = (float) x;
                }
                for (int pos = 0; pos < total; pos += 512)
                    rc.process (y.data() + pos, rCh.data() + pos, std::min (512, total - pos), p);
                const auto sp = bhSpectrumPow (y, (size_t) (total - N), order);
                const double binHz = sr / (double) N;
                auto bandPow = [&] (double fHz)
                {
                    const int b = (int) std::llround (fHz / binHz);
                    double acc = 0.0;
                    for (int k = std::max (1, b - 3); k <= std::min (N / 2 - 1, b + 3); ++k) acc += sp[(size_t) k];
                    return acc;
                };
                return powToDb ((bandPow (6000.0) + 1.0e-30) / (bandPow (3000.0) + 1.0e-30));
            };
            lfModGainDb = measureH2Db (true) - measureH2Db (false);
        }

        // Verify 0% drive clean bypass with 0 added latency
        bool zeroLatBypOk = false;
        {
            auto p = makeProcessor (sr, 512);
            setCleanDryPatch (*p);
            setP (*p, wt_on, 0.0f);
            setP (*p, in_level, 1.0f);
            setP (*p, tube_on, 1.0f);
            setP (*p, tube_drive, 0.0f);
            setP (*p, tube_bias, 0.0f);
            setP (*p, tube_out, 0.0f);
            setP (*p, bias_on, 0.0f);
            setP (*p, clip_on, 0.0f);
            p->prepareToPlay (sr, 512);
            const int repLat = p->getLatencySamples();
            std::vector<float> imp (1024, 0.0f);
            imp[100] = 0.5f;
            auto r = renderEvents (*p, sr, 1024, { 512 }, {}, &imp, &imp);
            double maxDiff = 0.0;
            for (int i = 0; i < 1024; ++i)
                maxDiff = std::max (maxDiff, std::abs ((double) r.l[(size_t) i] - (double) imp[(size_t) i]));
            zeroLatBypOk = (repLat == 0) && (maxDiff < 1.0e-5) && (r.nonFiniteCount == 0);
        }

        const bool magOk = magFinite && (magWorstAlias < -75.0) && (magWorstDc < -90.0)
                        && (lfModGainDb >= 10.0) && zeroLatBypOk;
        if (! magOk) anyFail = true;
        worstAliasDbc = std::max (worstAliasDbc, magWorstAlias);
        worstDcDbfs = std::max (worstDcDbfs, magWorstDc);
        res.details.push_back ("Magnetic Hysteresis Core (C6/6k): alias=" + fmt (magWorstAlias, 1)
                               + " dBc (lim<-75), DC=" + fmt (magWorstDc, 1)
                               + " dBFS (lim<-90), LF->HF H2=" + fmt (lfModGainDb, 1)
                               + " dB, 0%-drv 0-lat=" + std::string (zeroLatBypOk ? "OK" : "FAIL")
                               + " [" + (magOk ? "OK" : "FAIL") + "]");
    }

    res.status = anyFail ? GateStatus::Fail : (anyPending ? GateStatus::Pending : GateStatus::Pass);
    res.summary = "worstAlias=" + fmt (worstAliasDbc, 1) + " dBc, worstDC=" + fmt (worstDcDbfs, 1) + " dBFS"
                + (anyPending ? " (ADAA PENDING)" : "");
    return res;
}

//==============================================================================
// GATE 4 — Filter Modulation Stability & Self-Oscillation Tracking
//==============================================================================
GateResult runGate4()
{
    GateResult res { 4, "Filter Mod Stability & Self-Osc Tracking", GateStatus::Pass, "", {} };

    // Part A: Audio-rate 1 kHz cutoff sweep (20 Hz - 18 kHz) at 95-100% resonance across 4 rates
    bool stabOk = true;
    double maxSvfPeak = 0.0, maxLadPeak = 0.0, maxSkPeak = 0.0;
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const int nSamp = (int) (0.5 * sr);
        const double hiCut = std::min (18000.0, 0.45 * sr);
        const double logLo = std::log2 (20.0), logHi = std::log2 (hiCut);

        for (float resFrac : { 0.95f, 1.00f })
        {
            dsp::Svf svf;
            svf.reset();
            dsp::LadderBank<1> lad;
            lad.setSampleRate (4.0 * sr);
            lad.resetLane (0);
            dsp::SallenKeyCore sk;
            sk.prepare (sr, true);
            const float ladOsr = (float) (4.0 * sr);   // matches voice 4x oversampling
            const float svfQ = 0.707f + resFrac * 25.0f;
            const float ladK = resFrac * 4.3f;
            const double skK = 2.02 + (double) (resFrac - 0.95f) * 5.2;   // 2.02 .. 2.28 (diode scream)

            for (int i = 0; i < nSamp; ++i)
            {
                const double t = (double) i / sr;
                const float lfo = (float) (0.5 + 0.5 * std::sin (kTwoPiD * 1000.0 * t));
                const float cut = (float) std::exp2 (logLo + (logHi - logLo) * (double) lfo);
                const float in = 0.25f * (float) std::sin (kTwoPiD * 220.0 * t);

                svf.setup (cut, svfQ, (float) sr);
                const float ys = svf.lowpass (in);
                if (! std::isfinite (ys) || std::abs (ys) > 100.0f) stabOk = false;
                maxSvfPeak = std::max (maxSvfPeak, (double) std::abs (ys));

                const float g = dsp::LadderBank<1>::coefG (cut, ladOsr);
                lad.setLane (0, g, ladK, 4.0f, 1, true);
                float lin[1] { in }, lout[1] { 0.0f };
                for (int s = 0; s < 4; ++s) lad.tick (lin, lout);
                if (! std::isfinite (lout[0]) || std::abs (lout[0]) > 20.0f) stabOk = false;
                maxLadPeak = std::max (maxLadPeak, (double) std::abs (lout[0]));

                sk.setup ((double) cut, skK, 2.0);
                const double ysk = sk.process2x ((double) in);
                if (! std::isfinite (ysk) || std::abs (ysk) > 20.0) stabOk = false;
                maxSkPeak = std::max (maxSkPeak, std::abs (ysk));
            }
        }
    }
    if (! stabOk) res.status = GateStatus::Fail;
    res.details.push_back ("Audio-rate 1kHz mod (44.1..192k, 95-100% res): svfPk=" + fmt (maxSvfPeak, 2)
                           + ", ladPk=" + fmt (maxLadPeak, 2) + ", skPk=" + fmt (maxSkPeak, 2)
                           + " [" + (stabOk ? "OK" : "FAIL") + "]");

    // Part B: Static self-oscillation pitch tracking C3-C6 (SVF ±5 ct, 4-pole ladder ±15 ct, Sallen-Key ±15 ct)
    constexpr double sr = 48000.0;
    const int notes[] { 48, 60, 72, 84 };   // C3, C4, C5, C6
    double maxSvfCents = 0.0, maxLadCents = 0.0, maxSkCents = 0.0;
    for (int midi : notes)
    {
        const double targetHz = midiToHzD (midi);
        const int order = 16;
        const int N = 1 << order;
        const int total = N + 24000;

        // SVF near-zero damping self-oscillation (k = 1e-4 -> Q = 10000)
        {
            dsp::Svf svf;
            svf.setup ((float) targetHz, 10000.0f, (float) sr);
            std::vector<float> y ((size_t) total, 0.0f);
            for (int i = 0; i < total; ++i)
                y[(size_t) i] = svf.bandpass (i == 0 ? 0.5f : 0.0f);
            const double measHz = estimatePitchHz (y, sr, order);
            const double cents = std::abs (1200.0 * std::log2 (std::max (measHz, 1.0) / targetHz));
            maxSvfCents = std::max (maxSvfCents, cents);
        }

        // 4-pole nonlinear LadderBank at 4x oversampled rate (192 kHz)
        {
            constexpr int os = 4;
            const double osr = sr * (double) os;
            dsp::LadderBank<1> lad;
            lad.setSampleRate (osr);
            lad.resetLane (0);
            const float g = dsp::LadderBank<1>::coefG ((float) targetHz, (float) osr);
            lad.setLane (0, g, 4.15f, 1.0f, 1, true);
            std::vector<float> y ((size_t) total, 0.0f);
            for (int i = 0; i < total; ++i)
            {
                float outS = 0.0f;
                for (int s = 0; s < os; ++s)
                {
                    float in[1] { (i == 0 && s == 0) ? 0.1f : 0.0f };
                    float o[1] { 0.0f };
                    lad.tick (in, o);
                    outS = o[0];
                }
                y[(size_t) i] = outS;
            }
            const double measHz = estimatePitchHz (y, sr, order);
            const double cents = std::abs (1200.0 * std::log2 (std::max (measHz, 1.0) / targetHz));
            maxLadCents = std::max (maxLadCents, cents);
        }

        // 12 dB/oct ZDF Sallen-Key Dual-Diode self-oscillation at 2x oversampled rate (96 kHz)
        {
            dsp::SallenKeyCore sk;
            sk.prepare (sr, true);
            sk.setup (targetHz, 2.08, 1.0);
            std::vector<float> y ((size_t) total, 0.0f);
            for (int i = 0; i < total; ++i)
                y[(size_t) i] = (float) sk.process2x (i == 0 ? 0.1 : 0.0);
            const double measHz = estimatePitchHz (y, sr, order);
            const double cents = std::abs (1200.0 * std::log2 (std::max (measHz, 1.0) / targetHz));
            maxSkCents = std::max (maxSkCents, cents);
        }
    }
    const bool svfPitchOk = (maxSvfCents <= 5.0);
    const bool ladPitchOk = (maxLadCents <= 15.0);
    const bool skPitchOk  = (maxSkCents  <= 15.0);
    if (! svfPitchOk || ! ladPitchOk || ! skPitchOk) res.status = GateStatus::Fail;
    res.details.push_back ("Self-osc C3-C6: SVF maxErr=" + fmt (maxSvfCents, 2) + " ct (lim<=5) [" + (svfPitchOk ? "OK" : "FAIL")
                           + "], Ladder maxErr=" + fmt (maxLadCents, 2) + " ct (lim<=15) [" + (ladPitchOk ? "OK" : "FAIL")
                           + "], SallenKey maxErr=" + fmt (maxSkCents, 2) + " ct (lim<=15) [" + (skPitchOk ? "OK" : "FAIL") + "]");

    // Part C: Rate-invariant frequency response (±0.5 dB up to 12 kHz) on 4x-oversampled Ladder & 1x SVF
    const double testFreqs[] { 200.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 12000.0 };
    const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
    auto measureLadderMagDb = [] (double rate, double fc, double fTest)
    {
        const double osr = 4.0 * rate;
        dsp::LadderBank<1> lad;
        lad.setSampleRate (osr);
        lad.resetLane (0);
        const float g = dsp::LadderBank<1>::coefG ((float) fc, (float) osr);
        lad.setLane (0, g, 1.2f, 1.0f, 1, true);
        const int settle = (int) (0.08 * osr);
        const int meas = (int) (0.05 * osr);
        double sumIn = 0.0, sumOut = 0.0;
        const double amp = 0.01;   // small signal linear regime
        for (int i = 0; i < settle + meas; ++i)
        {
            const float x = (float) (amp * std::sin (kTwoPiD * fTest * (double) i / osr));
            float in[1] { x }, o[1] { 0.0f };
            lad.tick (in, o);
            if (i >= settle) { sumIn += (double) x * x; sumOut += (double) o[0] * o[0]; }
        }
        return powToDb ((sumOut + 1.0e-30) / (sumIn + 1.0e-30));
    };

    double maxLadRateDiffDb = 0.0;
    for (double fTest : testFreqs)
    {
        const double refDb = measureLadderMagDb (192000.0, 2000.0, fTest);
        for (double r : rates)
            maxLadRateDiffDb = std::max (maxLadRateDiffDb, std::abs (measureLadderMagDb (r, 2000.0, fTest) - refDb));
    }
    const bool rateOk = (maxLadRateDiffDb <= 0.5);
    if (! rateOk) res.status = GateStatus::Fail;
    res.details.push_back ("Rate-invariant mag (44.1..192k, <=12kHz): Ladder4x maxDiff=" + fmt (maxLadRateDiffDb, 3)
                           + " dB (lim<=0.5) [" + (rateOk ? "OK" : "FAIL") + "]");

    // Part D: Driven Nonlinear Filter Aliasing (< -75 dBc @ C6 & 6 kHz) & DC Hygiene (< -90 dBFS)
    double worstFltAlias = -300.0, worstFltDc = -300.0;
    {
        const int totalA = (int) (1.6 * sr);
        // 1) Driven 4-Pole Coupled Transistor Ladder at C6 (1046.5 Hz) saw & ~6 kHz sine via VesselProcessor
        for (int testKind = 0; testKind < 2; ++testKind)
        {
            auto p = makeProcessor (sr, 512);
            setCleanDryPatch (*p);
            setP (*p, wt_level, 0.55f);   // calibrated preset nominal oscillator level
            setP (*p, wt_morph, testKind == 0 ? (2.0f / 7.0f) : 0.0f);   // saw @ C6 or sine @ ~6 kHz (MIDI 114 = 5919.9 Hz)
            setP (*p, flt_on, 1.0f);
            setP (*p, flt_route, 2.0f);
            setP (*p, flt_a_cut, testKind == 0 ? 6000.0f : 12000.0f);
            setP (*p, flt_a_res, 0.50f);
            setP (*p, flt_drive, testKind == 0 ? 0.60f : 0.55f);
            setP (*p, flt_env, 0.0f);
            setP (*p, flt_key, 0.0f);
            p->prepareToPlay (sr, 512);
            const int midi = testKind == 0 ? 84 : 114;
            auto r = renderEvents (*p, sr, totalA, { 512 }, { { 0, juce::MidiMessage::noteOn (1, midi, 1.0f) } });
            const auto am = measureHarmonicAliasing (r.l, sr, midiToHzD (midi));
            worstFltAlias = std::max (worstFltAlias, am.aliasedDbc);
            worstFltDc = std::max (worstFltDc, am.dcDbfs);
            if (r.nonFiniteCount > 0) stabOk = false;
        }
        // 2) Driven 12 dB/oct ZDF Sallen-Key Dual-Diode Core at C6 (1046.5 Hz) & 6000 Hz (2x OS + DcBlocker)
        for (double f0 : { midiToHzD (84), 6000.0 })
        {
            dsp::SallenKeyCore sk;
            dsp::DcBlocker dc;
            sk.prepare (sr, true);
            dc.prepare (sr, 5.0);
            sk.setup (std::min (14000.0, f0 * 1.45), 1.92, 1.8);
            std::vector<float> y ((size_t) totalA, 0.0f);
            for (int i = 0; i < totalA; ++i)
            {
                const double x = 0.65 * std::sin (kTwoPiD * f0 * (double) i / sr);
                y[(size_t) i] = (float) dc.process (sk.process2x (x));
                if (! std::isfinite (y[(size_t) i])) stabOk = false;
            }
            const auto am = measureHarmonicAliasing (y, sr, f0);
            worstFltAlias = std::max (worstFltAlias, am.aliasedDbc);
            worstFltDc = std::max (worstFltDc, am.dcDbfs);
        }
    }
    const bool fltAliasOk = (worstFltAlias < -75.0) && (worstFltDc < -90.0) && stabOk;
    if (! fltAliasOk) res.status = GateStatus::Fail;
    res.details.push_back ("Driven Nonlinear Filters (C6 & 6kHz): alias=" + fmt (worstFltAlias, 1)
                           + " dBc (lim<-75), DC=" + fmt (worstFltDc, 1) + " dBFS (lim<-90) [" + (fltAliasOk ? "OK" : "FAIL") + "]");

    // Part E: Vactrol Low-Pass Gate (fast attack, sluggish two-stage curved release, coupled cutoff & VCA,
    // sample-rate invariance across 44.1-192 kHz, clean idle tail, 0 added latency)
    bool lpgOk = true;
    double lpgRiseMs = 0.0, lpgCurveRatio = 0.0, lpgCentRatio = 0.0, lpgSrSpread = 0.0;
    {
        double refPeak = -1.0, refMid = -1.0;
        for (double rSr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            dsp::VactrolLpg vac;
            vac.strike (1.0, true);
            const double dt = 8.0 / rSr;
            const int nSteps = (int) std::ceil (6.0 / dt);
            double peak = 0.0, mid60 = 0.0;
            double tPeak = 0.0, t90 = -1.0, t80 = -1.0, t40 = -1.0, t25 = -1.0, t0625 = -1.0;
            std::vector<double> envCurve;
            envCurve.reserve ((size_t) std::min (nSteps, 60000));
            for (int s = 0; s < nSteps; ++s)
            {
                vac.advance (dt, 0.0, 0.50, 1.0);
                const double g = vac.conductance();
                if (! std::isfinite (g)) lpgOk = false;
                if (g > peak) { peak = g; tPeak = (double) (s + 1) * dt; }
                if (std::abs ((double) (s + 1) * dt - 0.060) < 0.6 * dt) mid60 = g;
                if (s < 60000) envCurve.push_back (g);
            }
            if (vac.conductance() > 1.0e-5 || vac.isActive()) lpgOk = false;
            for (size_t s = 0; s < envCurve.size(); ++s)
            {
                const double t = (double) (s + 1) * dt;
                const double g = envCurve[s];
                if (t90 < 0.0 && g >= 0.90 * peak) t90 = t;
                if (t > tPeak)
                {
                    if (t80 < 0.0 && g <= 0.80 * peak) t80 = t;
                    if (t40 < 0.0 && g <= 0.40 * peak) t40 = t;
                    if (t25 < 0.0 && g <= 0.25 * peak) t25 = t;
                    if (t0625 < 0.0 && g <= 0.0625 * peak) t0625 = t;
                }
            }
            const double tEarly = (t40 > t80 && t80 > 0.0) ? (t40 - t80) : 0.0;
            const double tLate  = (t0625 > t25 && t25 > 0.0) ? (t0625 - t25) : 0.0;
            const double ratio  = (tEarly > 1.0e-6) ? (tLate / (2.0 * tEarly)) : 0.0;
            if (std::abs (rSr - 48000.0) < 1.0)
            {
                lpgRiseMs = t90 * 1000.0;
                lpgCurveRatio = ratio;
            }
            if (refPeak < 0.0) { refPeak = peak; refMid = mid60; }
            else lpgSrSpread = std::max ({ lpgSrSpread, std::abs (peak - refPeak), std::abs (mid60 - refMid) });
            if (t90 < 0.0 || t90 * 1000.0 > 6.0 || ratio < 2.0) lpgOk = false;
        }

        // End-to-end VesselProcessor LPG Both mode: coupled filter darkening + VCA ring, 0 latency, DC < -90 dBFS
        auto p = makeProcessor (sr, 512);
        setCleanDryPatch (*p);
        setP (*p, wt_morph, 2.0f / 7.0f);   // bright saw
        setP (*p, flt_on, 1.0f);
        setP (*p, flt_route, 2.0f);
        setP (*p, flt_a_cut, 5000.0f);
        setP (*p, flt_a_res, 0.35f);
        setP (*p, flt_env, 0.0f);
        setP (*p, v2_lpg_mode, 1.0f);       // Both (coupled cutoff + VCA)
        setP (*p, v2_lpg_decay, 0.45f);
        setP (*p, v2_lpg_strike, 1.0f);
        p->prepareToPlay (sr, 512);
        if (p->getLatencySamples() != 0) lpgOk = false;
        auto r = renderEvents (*p, sr, (int) (0.6 * sr), { 512 }, {
            { 0, juce::MidiMessage::noteOn (1, 48, 0.95f) },
            { (int) (0.04 * sr), juce::MidiMessage::noteOff (1, 48) }
        });
        if (r.nonFiniteCount > 0) lpgOk = false;
        const double lpgDc = windowedDcDbfs (r.l, 0, r.l.size());
        if (lpgDc >= -90.0) lpgOk = false;
        auto centOf = [&] (size_t off, int ord)
        {
            const int N = 1 << ord;
            auto sp = bhSpectrumPow (r.l, off, ord);
            const double binHz = sr / (double) N;
            double num = 0.0, den = 0.0;
            for (int k = 1; k < N / 2; ++k) { const double m = std::sqrt (sp[(size_t) k]); num += ((double) k * binHz) * m; den += m; }
            return den > 1.0e-12 ? num / den : 0.0;
        };
        const double cEarly = centOf (64, 10);                  // ~1..22 ms bright transient
        const double cTail  = centOf ((size_t) (0.10 * sr), 12); // ~100..185 ms warm darkened ring
        lpgCentRatio = (cTail > 1.0) ? (cEarly / cTail) : 0.0;
        if (lpgCentRatio < 1.8 || lpgSrSpread > 0.025) lpgOk = false;
    }
    if (! lpgOk) res.status = GateStatus::Fail;
    res.details.push_back ("Vactrol LPG (44.1..192k): rise90=" + fmt (lpgRiseMs, 2) + " ms, 2-stageRatio=" + fmt (lpgCurveRatio, 2)
                           + "x, centDrop=" + fmt (lpgCentRatio, 2) + "x, srSpread=" + fmt (lpgSrSpread, 4) + " [" + (lpgOk ? "OK" : "FAIL") + "]");

    // Part F: Oversampled DC-Blocked Audio-Rate Feedback Path (44.1-192k stability, self-oscillation,
    // C6/6kHz aliasing < -75 dBc, DC < -90 dBFS, odd-block < -100 dBFS, 0 added latency)
    bool fbOk = true;
    double fbWorstAlias = -300.0, fbWorstDc = -300.0, fbMaxPk = 0.0, fbOddResDb = -300.0;
    {
        // 1) Multi-rate extreme stress & musical self-oscillation across 44.1, 48, 96, 192 kHz
        for (double rSr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const int nTot = (int) (0.70 * rSr);
            const size_t dcStart = (size_t) (0.25 * rSr);
            const size_t dcLen   = (size_t) (0.45 * rSr);
            for (float fbSign : { -1.0f, 1.0f })
            {
                auto p = makeProcessor (rSr, 512);
                setCleanDryPatch (*p);
                setP (*p, wt_level, 0.55f);
                setP (*p, flt_on, 1.0f);
                setP (*p, flt_route, 0.0f);   // Serial dual ladders
                setP (*p, flt_a_cut, 1200.0f);
                setP (*p, flt_a_res, 0.85f);
                setP (*p, flt_b_cut, 3600.0f);
                setP (*p, flt_b_res, 0.65f);
                setP (*p, flt_drive, 0.75f);
                setP (*p, flt_env, 0.0f);
                setP (*p, v2_fb_filter, fbSign);
                setP (*p, v2_fb_fm, 0.80f);
                p->prepareToPlay (rSr, 512);
                if (p->getLatencySamples() != 0) fbOk = false;
                auto r = renderEvents (*p, rSr, nTot, { 512 }, { { 0, juce::MidiMessage::noteOn (1, 52, 0.9f) } });
                if (r.nonFiniteCount > 0) fbOk = false;
                for (float v : r.l) fbMaxPk = std::max (fbMaxPk, (double) std::abs (v));
                const double dc = windowedDcDbfs (r.l, dcStart, dcLen);
                fbWorstDc = std::max (fbWorstDc, dc);
                if (fbMaxPk > 6.0 || dc >= -90.0) fbOk = false;
            }
            // Self-oscillation stability via inverted post-filter feedback at moderate resonance (res=0.65)
            {
                auto p = makeProcessor (rSr, 512);
                setCleanDryPatch (*p);
                setP (*p, wt_level, 0.03f);
                setP (*p, flt_on, 1.0f);
                setP (*p, flt_route, 2.0f);
                setP (*p, flt_a_cut, 1000.0f);
                setP (*p, flt_a_res, 0.65f);
                setP (*p, flt_drive, 0.35f);
                setP (*p, flt_env, 0.0f);
                setP (*p, v2_fb_filter, -0.95f);
                p->prepareToPlay (rSr, 512);
                auto r = renderEvents (*p, rSr, nTot, { 512 }, { { 0, juce::MidiMessage::noteOn (1, 60, 0.8f) } });
                if (r.nonFiniteCount > 0) fbOk = false;
                double sq = 0.0;
                const size_t st = (size_t) (nTot / 2);
                for (size_t i = st; i < r.l.size(); ++i) sq += (double) r.l[i] * r.l[i];
                const double rmsDb = linToDb (std::sqrt (sq / (double) (r.l.size() - st)));
                if (rmsDb < -38.0 || rmsDb > 6.0) fbOk = false;
            }
        }

        // 2) Driven Audio-Rate Feedback Aliasing (< -75 dBc) & DC (< -90 dBFS) at C6 (1046.5 Hz) & ~6 kHz (MIDI 114)
        const int totalFb = (int) (1.6 * sr);
        struct FbAliasCase { int midi; float morph; float cut; float fbFlt; float fbFm; };
        const FbAliasCase fbCases[] {
            { 84,  2.0f / 7.0f,  6000.0f, -0.55f, 0.00f },   // Filter FB C6 saw
            { 114, 0.0f,        12000.0f, -0.55f, 0.00f },   // Filter FB ~6kHz sine
            { 84,  2.0f / 7.0f,  6000.0f,  0.30f, 0.45f },   // Filter+FM FB C6 saw
            { 114, 0.0f,        12000.0f,  0.30f, 0.45f },   // Filter+FM FB ~6kHz sine
        };
        for (const auto& c : fbCases)
        {
            auto p = makeProcessor (sr, 512);
            setCleanDryPatch (*p);
            setP (*p, wt_level, 0.55f);
            setP (*p, wt_morph, c.morph);
            setP (*p, flt_on, 1.0f);
            setP (*p, flt_route, 2.0f);
            setP (*p, flt_a_cut, c.cut);
            setP (*p, flt_a_res, 0.40f);
            setP (*p, flt_drive, 0.40f);
            setP (*p, flt_env, 0.0f);
            setP (*p, flt_key, 0.0f);
            setP (*p, v2_fb_filter, c.fbFlt);
            setP (*p, v2_fb_fm, c.fbFm);
            p->prepareToPlay (sr, 512);
            auto r = renderEvents (*p, sr, totalFb, { 512 }, { { 0, juce::MidiMessage::noteOn (1, c.midi, 1.0f) } });
            if (r.nonFiniteCount > 0) fbOk = false;
            const auto am = measureHarmonicAliasing (r.l, sr, midiToHzD (c.midi));
            fbWorstAlias = std::max (fbWorstAlias, am.aliasedDbc);
            fbWorstDc = std::max (fbWorstDc, am.dcDbfs);
        }
        if (fbWorstAlias >= -75.0 || fbWorstDc >= -90.0) fbOk = false;

        // 3) Odd-block invariance with BOTH Vactrol LPG + Audio-Rate Filter/FM Feedback active
        auto setupLpgFb = [] (VesselProcessor& p)
        {
            setCleanDryPatch (p);
            setP (p, flt_on, 1.0f);
            setP (p, flt_route, 0.0f);
            setP (p, flt_a_cut, 2600.0f);
            setP (p, flt_a_res, 0.45f);
            setP (p, flt_drive, 0.45f);
            setP (p, v2_lpg_mode, 1.0f);
            setP (p, v2_lpg_decay, 0.50f);
            setP (p, v2_lpg_strike, 0.85f);
            setP (p, v2_fb_filter, -0.50f);
            setP (p, v2_fb_fm, 0.40f);
            maybeSeedQa (p, 0x5EED4242u);
        };
        const int nOdd = (int) (0.5 * sr);
        const std::vector<MidiEvent> evOdd {
            { 0, juce::MidiMessage::noteOn (1, 48, 0.9f) },
            { 4000, juce::MidiMessage::noteOn (1, 55, 0.8f) },
            { 14000, juce::MidiMessage::noteOff (1, 48) },
        };
        auto pR = makeProcessor (sr, 512); setupLpgFb (*pR); pR->prepareToPlay (sr, 512); maybeSeedQa (*pR, 0x5EED4242u);
        auto rR = renderEvents (*pR, sr, nOdd, { 512 }, evOdd);
        auto pO = makeProcessor (sr, 2048); setupLpgFb (*pO); pO->prepareToPlay (sr, 2048); maybeSeedQa (*pO, 0x5EED4242u);
        auto rO = renderEvents (*pO, sr, nOdd, { 0, 1, 7, 13, 64, 137, 2048 }, evOdd);
        double dSq = 0.0;
        for (int i = 0; i < nOdd; ++i)
        {
            const double dl = (double) rR.l[(size_t) i] - (double) rO.l[(size_t) i];
            const double dr = (double) rR.r[(size_t) i] - (double) rO.r[(size_t) i];
            dSq += 0.5 * (dl * dl + dr * dr);
        }
        fbOddResDb = linToDb (std::sqrt (dSq / (double) nOdd));
        if (rO.nonFiniteCount > 0 || fbOddResDb >= -100.0) fbOk = false;
    }
    if (! fbOk) res.status = GateStatus::Fail;
    res.details.push_back ("Audio-Rate Feedback (44.1..192k, C6/6k): pk=" + fmt (fbMaxPk, 2) + ", alias=" + fmt (fbWorstAlias, 1)
                           + " dBc (lim<-75), DC=" + fmt (fbWorstDc, 1) + " dBFS (lim<-90), oddRes=" + fmt (fbOddResDb, 1)
                           + " dBFS [" + (fbOk ? "OK" : "FAIL") + "]");

    // Part G: Polyphonic Power-Rail Sag (20-80 ms RC across 44.1-192 kHz, headroom + micro-pitch (-1..-3 ct)
    // + filter bias chord glue, C6/6kHz aliasing < -75 dBc, DC < -90 dBFS, 0 added latency, odd-block < -100 dBFS)
    bool railOk = true;
    double railWorstAlias = -300.0, railWorstDc = -300.0, railRcErrMs = 0.0;
    double railChordDb = 0.0, railPitchCt = 0.0, railCentDrop = 1.0, railOddResDb = -300.0;
    {
        // 1) Multi-rate RC time constant invariance (20 ms & 80 ms) across 44.1, 48, 96, 192 kHz
        for (double rSr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const double osr = 4.0 * rSr;
            for (double targetMs : { 20.0, 80.0 })
            {
                dsp::PowerRailSag rs;
                rs.prepare (osr);
                double droopEff = 0.0;
                // Charge rail to steady power, then measure 1/e RC recovery time constant
                std::vector<float> bL (32, 0.5f), bR (32, 0.5f), sB (32, 0.4f), pg (32, 0.5f);
                for (int k = 0; k < (int) std::ceil (0.4 * osr / 32.0); ++k)
                {
                    std::fill (bL.begin(), bL.end(), 0.5f);
                    std::fill (bR.begin(), bR.end(), 0.5f);
                    std::fill (sB.begin(), sB.end(), 0.4f);
                    rs.beginControlBlock (1.0, targetMs, 32, droopEff);
                    rs.processBus (bL.data(), bR.data(), sB.data(), pg.data(), 32);
                }
                const double p0 = rs.railPow;
                if (! std::isfinite (p0) || p0 <= 0.1) railOk = false;
                const double targetPow = p0 * std::exp (-1.0);
                int recSamples = 0;
                const int maxSamples = (int) (0.25 * osr);
                std::fill (bL.begin(), bL.end(), 0.0f);
                std::fill (bR.begin(), bR.end(), 0.0f);
                std::fill (sB.begin(), sB.end(), 0.0f);
                while (recSamples < maxSamples && rs.railPow > targetPow)
                {
                    rs.beginControlBlock (1.0, targetMs, 32, droopEff);
                    rs.processBus (bL.data(), bR.data(), sB.data(), pg.data(), 32);
                    recSamples += 32;
                }
                const double measMs = (double) recSamples * 1000.0 / osr;
                railRcErrMs = std::max (railRcErrMs, std::abs (measMs - targetMs));
                if (std::abs (measMs - targetMs) > 2.5) railOk = false;
            }
        }

        // 2) Chord vs solo headroom glue & filter bias darkening at 48 kHz
        auto renderRailChord = [&] (float sag, bool withFilter, bool loudChord)
        {
            auto p = makeProcessor (sr, 512);
            setCleanDryPatch (*p);
            setP (*p, sub_on, 1.0f);
            setP (*p, sub1_level, 0.8f);
            setP (*p, sub2_level, 0.45f);
            setP (*p, wt_level, 0.65f);
            setP (*p, wt_morph, 2.0f / 7.0f);
            setP (*p, vel_amp, 0.9f);
            setP (*p, flt_on, withFilter ? 1.0f : 0.0f);
            setP (*p, flt_route, 2.0f);
            setP (*p, flt_a_cut, 1400.0f);
            setP (*p, flt_a_res, 0.45f);
            setP (*p, flt_env, 0.0f);
            setP (*p, v2_rail_sag, sag);
            setP (*p, v2_rail_time, 45.0f);
            p->prepareToPlay (sr, 512);
            if (p->getLatencySamples() != 0) railOk = false;
            std::vector<MidiEvent> ev;
            if (loudChord)
            {
                for (int n : { 36, 40, 43, 48, 52, 55 })
                    ev.push_back ({ 0, juce::MidiMessage::noteOn (1, n, 0.95f) });
            }
            else
            {
                ev.push_back ({ 0, juce::MidiMessage::noteOn (1, 48, 0.25f) });
            }
            return renderEvents (*p, sr, (int) (0.75 * sr), { 512 }, ev);
        };
        auto rmsTailDb = [&] (const RenderBuf& rb)
        {
            double sq = 0.0;
            const size_t st = (size_t) (0.25 * sr);
            for (size_t i = st; i < rb.l.size(); ++i)
                sq += 0.5 * ((double) rb.l[i] * rb.l[i] + (double) rb.r[i] * rb.r[i]);
            return linToDb (std::sqrt (sq / (double) (rb.l.size() - st)));
        };
        auto centTail = [&] (const RenderBuf& rb)
        {
            constexpr int ord = 14;
            const int N = 1 << ord;
            auto sp = bhSpectrumPow (rb.l, rb.l.size() - (size_t) N, ord);
            const double binHz = sr / (double) N;
            double num = 0.0, den = 0.0;
            for (int k = 1; k < N / 2; ++k) { const double m = std::sqrt (sp[(size_t) k]); num += ((double) k * binHz) * m; den += m; }
            return den > 1.0e-12 ? num / den : 0.0;
        };

        auto rChordDry = renderRailChord (0.0f, true, true);
        auto rChordSag = renderRailChord (1.0f, true, true);
        auto rSoloDry  = renderRailChord (0.0f, true, false);
        auto rSoloSag  = renderRailChord (1.0f, true, false);
        if (rChordSag.nonFiniteCount > 0 || rSoloSag.nonFiniteCount > 0) railOk = false;
        railChordDb = rmsTailDb (rChordDry) - rmsTailDb (rChordSag);
        const double soloDropDb = rmsTailDb (rSoloDry) - rmsTailDb (rSoloSag);
        const double cDry = centTail (rChordDry), cSag = centTail (rChordSag);
        railCentDrop = (cDry > 1.0) ? (cSag / cDry) : 1.0;
        if (railChordDb < 0.45 || railChordDb > 2.6 || railChordDb < soloDropDb + 0.35 || railCentDrop > 0.975)
            railOk = false;

        // 3) Micro-pitch droop (-1..-3 cents) on an isolated B4 sine voice inside a loud low chord
        auto measureChordPitchHz = [&] (float sag)
        {
            auto p = makeProcessor (sr, 512);
            setCleanDryPatch (*p);
            setP (*p, sub_on, 1.0f);
            setP (*p, sub1_oct, 0.0f);
            setP (*p, sub1_level, 0.85f);
            setP (*p, sub2_level, 0.0f);
            setP (*p, sub1_shape, 0.0f);
            setP (*p, wt_morph, 0.0f);   // pure sine
            setP (*p, wt_level, 0.70f);
            setP (*p, flt_on, 0.0f);
            setP (*p, v2_rail_sag, sag);
            setP (*p, v2_rail_time, 45.0f);
            p->prepareToPlay (sr, 512);
            std::vector<MidiEvent> ev;
            for (int n : { 36, 40, 43, 48, 71 })   // low chord + isolated B4 (493.88 Hz)
                ev.push_back ({ 0, juce::MidiMessage::noteOn (1, n, 0.95f) });
            const int nTot = (int) (1.7 * sr);
            auto r = renderEvents (*p, sr, nTot, { 512 }, ev);
            if (r.nonFiniteCount > 0) railOk = false;
            constexpr int ord = 16;
            const int N = 1 << ord;
            auto sp = bhSpectrumPow (r.l, r.l.size() - (size_t) N, ord);
            const double binHz = sr / (double) N;
            const int k0 = std::max (1, (int) std::floor (470.0 / binHz));
            const int k1 = std::min (N / 2 - 2, (int) std::ceil (520.0 / binHz));
            int bestK = k0;
            double bestP = -1.0;
            for (int k = k0; k <= k1; ++k) if (sp[(size_t) k] > bestP) { bestP = sp[(size_t) k]; bestK = k; }
            const double pL = std::log (std::max (sp[(size_t) (bestK - 1)], 1.0e-30));
            const double pC = std::log (std::max (sp[(size_t) bestK],       1.0e-30));
            const double pR = std::log (std::max (sp[(size_t) (bestK + 1)], 1.0e-30));
            const double den = (pL - 2.0 * pC + pR);
            const double del = std::abs (den) > 1.0e-12 ? 0.5 * (pL - pR) / den : 0.0;
            return ((double) bestK + std::clamp (del, -0.5, 0.5)) * binHz;
        };
        const double fDry = measureChordPitchHz (0.0f);
        const double fSag = measureChordPitchHz (1.0f);
        railPitchCt = 1200.0 * std::log2 (std::max (fSag, 1.0) / std::max (fDry, 1.0));
        if (railPitchCt > -0.7 || railPitchCt < -3.3) railOk = false;

        // 4) Aliasing (< -75 dBc) & DC (< -90 dBFS) at C6 saw & ~6 kHz sine with rail sag active
        const int totalR = (int) (1.6 * sr);
        for (int kind = 0; kind < 2; ++kind)
        {
            auto p = makeProcessor (sr, 512);
            setCleanDryPatch (*p);
            setP (*p, wt_level, 0.65f);
            setP (*p, wt_morph, kind == 0 ? (2.0f / 7.0f) : 0.0f);
            setP (*p, flt_on, 1.0f);
            setP (*p, flt_route, 2.0f);
            setP (*p, flt_a_cut, kind == 0 ? 6000.0f : 12000.0f);
            setP (*p, flt_a_res, 0.45f);
            setP (*p, flt_drive, 0.45f);
            setP (*p, flt_env, 0.0f);
            setP (*p, v2_rail_sag, 1.0f);
            setP (*p, v2_rail_time, 35.0f);
            p->prepareToPlay (sr, 512);
            const int midi = kind == 0 ? 84 : 114;
            auto r = renderEvents (*p, sr, totalR, { 512 }, { { 0, juce::MidiMessage::noteOn (1, midi, 1.0f) } });
            if (r.nonFiniteCount > 0) railOk = false;
            const double fNom = midiToHzD (midi);
            const double fAct = estimatePitchHz (r.l, sr);
            if (std::abs (1200.0 * std::log2 (std::max (fAct, 1.0) / fNom)) > 3.5) railOk = false;
            const auto am = measureHarmonicAliasing (r.l, sr, fAct);
            railWorstAlias = std::max (railWorstAlias, am.aliasedDbc);
            railWorstDc = std::max (railWorstDc, am.dcDbfs);
        }
        if (railWorstAlias >= -75.0 || railWorstDc >= -90.0) railOk = false;

        // 5) Odd-block invariance with Power-Rail Sag active
        auto setupRailOdd = [] (VesselProcessor& p)
        {
            setCleanDryPatch (p);
            setP (p, sub_on, 1.0f);
            setP (p, flt_on, 1.0f);
            setP (p, flt_a_cut, 1600.0f);
            setP (p, v2_rail_sag, 1.0f);
            setP (p, v2_rail_time, 30.0f);
            maybeSeedQa (p, 0x5A610001u);
        };
        const int nOdd = (int) (0.5 * sr);
        const std::vector<MidiEvent> evOdd {
            { 0,     juce::MidiMessage::noteOn (1, 40, 0.9f) },
            { 0,     juce::MidiMessage::noteOn (1, 47, 0.9f) },
            { 4000,  juce::MidiMessage::noteOn (1, 52, 0.9f) },
            { 14000, juce::MidiMessage::noteOff (1, 40) },
            { 14000, juce::MidiMessage::noteOff (1, 47) },
        };
        auto pR = makeProcessor (sr, 512); setupRailOdd (*pR); pR->prepareToPlay (sr, 512); maybeSeedQa (*pR, 0x5A610001u);
        auto rR = renderEvents (*pR, sr, nOdd, { 512 }, evOdd);
        auto pO = makeProcessor (sr, 2048); setupRailOdd (*pO); pO->prepareToPlay (sr, 2048); maybeSeedQa (*pO, 0x5A610001u);
        auto rO = renderEvents (*pO, sr, nOdd, { 0, 1, 7, 13, 64, 137, 2048 }, evOdd);
        double dSq = 0.0;
        for (int i = 0; i < nOdd; ++i)
        {
            const double dl = (double) rR.l[(size_t) i] - (double) rO.l[(size_t) i];
            const double dr = (double) rR.r[(size_t) i] - (double) rO.r[(size_t) i];
            dSq += 0.5 * (dl * dl + dr * dr);
        }
        railOddResDb = linToDb (std::sqrt (dSq / (double) nOdd));
        if (rO.nonFiniteCount > 0 || railOddResDb >= -100.0) railOk = false;
    }
    if (! railOk) res.status = GateStatus::Fail;
    res.details.push_back ("Poly Power-Rail Sag (20..80ms, 44.1..192k): rcErr=" + fmt (railRcErrMs, 2) + "ms, sag="
                           + fmt (railChordDb, 2) + "dB, pitch=" + fmt (railPitchCt, 2) + "ct, centMul="
                           + fmt (railCentDrop, 3) + ", alias=" + fmt (railWorstAlias, 1) + "dBc, DC="
                           + fmt (railWorstDc, 1) + "dBFS, oddRes=" + fmt (railOddResDb, 1) + "dBFS [" + (railOk ? "OK" : "FAIL") + "]");

    // Part H: Voice/Bus 4-to-8-Stage 64-bit TPT All-Pass Phase Dispersion (0 dB gain change across 0..Nyquist,
    // real bass transient dispersion, sub-120 Hz mono correlation, 44.1..192 kHz, C6/6kHz aliasing < -75 dBc,
    // DC < -90 dBFS, 0 added latency, odd-block < -100 dBFS)
    bool dispOk = true;
    double dispMaxMagErrDb = 0.0, dispBassPkRatio = 1.0, dispBassDelayMs = 0.0, dispSub120Corr = 1.0;
    double dispWorstAlias = -300.0, dispWorstDc = -300.0, dispOddResDb = -300.0;
    {
        // 1) Exact zero-gain-change & subterranean bass transient dispersion across 4, 6, 8 stages and 44.1..192 kHz
        for (double rSr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            double prevPeak = 1.0;
            for (int stg : { 4, 6, 8 })
            {
                constexpr int ord = 14;
                const int N = 1 << ord;   // 16384 samples (full impulse decay < -150 dBFS)
                std::vector<float> l ((size_t) N, 0.0f), r ((size_t) N, 0.0f);
                l[0] = r[0] = 1.0f;
                dsp::PhaseDisperser pd;
                pd.prepare (rSr);
                dsp::PhaseDisperser::Params prm { true, stg, 180.0f, 0.75f };
                for (int pos = 0; pos < N; pos += 64)
                    pd.process (l.data() + pos, r.data() + pos, std::min (64, N - pos), prm, true, 64);

                double eng = 0.0, pk = 0.0;
                for (int i = 0; i < N; ++i)
                {
                    if (! std::isfinite (l[(size_t) i]) || ! std::isfinite (r[(size_t) i])) dispOk = false;
                    eng += (double) l[(size_t) i] * (double) l[(size_t) i];
                    pk = std::max (pk, (double) std::abs (l[(size_t) i]));
                }
                if (std::abs (eng - 1.0) > 1.0e-3) dispOk = false;
                if (std::abs (rSr - 48000.0) < 1.0)
                {
                    if (pk >= prevPeak) dispOk = false;
                    prevPeak = pk;
                }

                // Exact unwindowed DFT magnitude of impulse response = |H(e^{jw})|
                juce::dsp::FFT fft (ord);
                std::vector<float> fb ((size_t) N * 2, 0.0f);
                std::copy_n (l.data(), N, fb.data());
                fft.performFrequencyOnlyForwardTransform (fb.data());
                const double binHz = rSr / (double) N;
                const double hiHz = std::min (18000.0, 0.45 * rSr);
                for (int k = 1; k < N / 2; ++k)
                {
                    const double f = (double) k * binHz;
                    if (f < 30.0 || f > hiHz) continue;
                    const double errDb = std::abs (linToDb ((double) fb[(size_t) k]));
                    dispMaxMagErrDb = std::max (dispMaxMagErrDb, errDb);
                }
            }

            // Subterranean bass transient (< 400 Hz) dispersion check at 48 kHz: verifies dramatic peak spread & group delay
            if (std::abs (rSr - 48000.0) < 1.0)
            {
                const int N = 8192;
                std::vector<float> dry ((size_t) N, 0.0f), wetL ((size_t) N, 0.0f), wetR ((size_t) N, 0.0f);
                dsp::Svf lp1, lp2;
                lp1.setup (350.0, 0.70710678, rSr);
                lp2.setup (350.0, 0.70710678, rSr);
                double dryPk = 0.0, dryEng = 0.0, dryT = 0.0;
                for (int i = 0; i < N; ++i)
                {
                    const float x = lp2.lowpass (lp1.lowpass (i == 0 ? 1.0f : 0.0f));
                    dry[(size_t) i] = wetL[(size_t) i] = wetR[(size_t) i] = x;
                    dryPk = std::max (dryPk, (double) std::abs (x));
                    dryEng += (double) x * x;
                    dryT += (double) i * ((double) x * x);
                }
                dsp::PhaseDisperser pdB;
                pdB.prepare (rSr);
                dsp::PhaseDisperser::Params prm { true, 8, 180.0f, 0.75f };
                for (int pos = 0; pos < N; pos += 64)
                    pdB.process (wetL.data() + pos, wetR.data() + pos, std::min (64, N - pos), prm, true, 64);
                double wetPk = 0.0, wetEng = 0.0, wetT = 0.0;
                for (int i = 0; i < N; ++i)
                {
                    wetPk = std::max (wetPk, (double) std::abs (wetL[(size_t) i]));
                    wetEng += (double) wetL[(size_t) i] * wetL[(size_t) i];
                    wetT += (double) i * ((double) wetL[(size_t) i] * wetL[(size_t) i]);
                }
                dispBassPkRatio = wetPk / std::max (dryPk, 1.0e-12);
                dispBassDelayMs = ((wetT / std::max (wetEng, 1.0e-30)) - (dryT / std::max (dryEng, 1.0e-30))) * 1000.0 / rSr;
                if (std::abs (wetEng - dryEng) / std::max (dryEng, 1.0e-30) > 0.005 || dispBassPkRatio > 0.60 || dispBassDelayMs < 8.0)
                    dispOk = false;
            }

            // Fast modulation stability across 44.1..192 kHz (1 kHz freq & pinch sweep)
            dsp::PhaseDisperser pdMod;
            pdMod.prepare (rSr);
            const int nMod = (int) (0.25 * rSr);
            for (int pos = 0; pos < nMod; pos += 64)
            {
                const int m = std::min (64, nMod - pos);
                float bl[64], br[64];
                for (int i = 0; i < m; ++i)
                {
                    const double t = (double) (pos + i) / rSr;
                    bl[i] = br[i] = 0.5f * (float) std::sin (kTwoPiD * 110.0 * t);
                }
                const double u = 0.5 + 0.5 * std::sin (kTwoPiD * 1000.0 * (double) pos / rSr);
                dsp::PhaseDisperser::Params mp { true, 8, (float) (40.0 + 1960.0 * u), (float) u };
                pdMod.process (bl, br, m, mp, true, 64);
                for (int i = 0; i < m; ++i)
                    if (! std::isfinite (bl[i]) || ! std::isfinite (br[i]) || std::abs (bl[i]) > 10.0f) dispOk = false;
            }
        }
        if (dispMaxMagErrDb > 0.02) dispOk = false;

        // 2) Sub-120 Hz mono correlation on subterranean bass chord (even with mono_on = 0!)
        {
            auto p = makeProcessor (sr, 512);
            setCleanDryPatch (*p);
            setP (*p, sub_on, 1.0f);
            setP (*p, sub1_level, 0.85f);
            setP (*p, sub2_level, 0.50f);
            setP (*p, wt_level, 0.60f);
            setP (*p, wt_drift, 0.25f);
            setP (*p, flt_on, 1.0f);
            setP (*p, flt_a_cut, 900.0f);
            setP (*p, mono_on, 0.0f);
            setP (*p, v2_disp_stages, 5.0f);   // 8 stages
            setP (*p, v2_disp_freq, 140.0f);
            setP (*p, v2_disp_pinch, 0.75f);
            p->prepareToPlay (sr, 512);
            if (p->getLatencySamples() != 0) dispOk = false;
            const int totalD = (int) (1.0 * sr);
            auto r = renderEvents (*p, sr, totalD, { 512 }, {
                { 0, juce::MidiMessage::noteOn (1, 36, 0.95f) },
                { 0, juce::MidiMessage::noteOn (1, 43, 0.85f) },
            });
            if (r.nonFiniteCount > 0) dispOk = false;
            dsp::Svf lpL1, lpL2, lpR1, lpR2;
            lpL1.setup (120.0, 0.70710678, sr); lpL2.setup (120.0, 0.70710678, sr);
            lpR1.setup (120.0, 0.70710678, sr); lpR2.setup (120.0, 0.70710678, sr);
            double sLL = 0.0, sRR = 0.0, sLR = 0.0;
            for (int i = 0; i < totalD; ++i)
            {
                const double sl = (double) lpL2.lowpass (lpL1.lowpass (r.l[(size_t) i]));
                const double srS = (double) lpR2.lowpass (lpR1.lowpass (r.r[(size_t) i]));
                sLL += sl * sl; sRR += srS * srS; sLR += sl * srS;
            }
            dispSub120Corr = sLR / std::sqrt (sLL * sRR + 1.0e-30);
            if (dispSub120Corr < 0.995) dispOk = false;
        }

        // 3) Aliasing (< -75 dBc) & DC (< -90 dBFS) at C6 saw & ~6 kHz sine with 8-stage dispersion active
        const int totalD = (int) (1.6 * sr);
        for (int kind = 0; kind < 2; ++kind)
        {
            auto p = makeProcessor (sr, 512);
            setCleanDryPatch (*p);
            setP (*p, wt_level, 0.65f);
            setP (*p, wt_morph, kind == 0 ? (2.0f / 7.0f) : 0.0f);
            setP (*p, v2_disp_stages, 5.0f);   // 8 stages
            setP (*p, v2_disp_freq, 220.0f);
            setP (*p, v2_disp_pinch, 0.80f);
            p->prepareToPlay (sr, 512);
            if (p->getLatencySamples() != 0) dispOk = false;
            const int midi = kind == 0 ? 84 : 114;
            auto r = renderEvents (*p, sr, totalD, { 512 }, { { 0, juce::MidiMessage::noteOn (1, midi, 1.0f) } });
            if (r.nonFiniteCount > 0) dispOk = false;
            const auto am = measureHarmonicAliasing (r.l, sr, midiToHzD (midi));
            dispWorstAlias = std::max (dispWorstAlias, am.aliasedDbc);
            dispWorstDc = std::max (dispWorstDc, am.dcDbfs);
        }
        if (dispWorstAlias >= -75.0 || dispWorstDc >= -90.0) dispOk = false;

        // 4) Odd-block invariance with Power-Rail Sag + 8-Stage Dispersion + LFO modulating Disperse Freq
        auto setupDispOdd = [] (VesselProcessor& p)
        {
            setCleanDryPatch (p);
            setP (p, sub_on, 1.0f);
            setP (p, flt_on, 1.0f);
            setP (p, flt_a_cut, 1800.0f);
            setP (p, v2_rail_sag, 0.85f);
            setP (p, v2_rail_time, 45.0f);
            setP (p, v2_disp_stages, 5.0f);
            setP (p, v2_disp_freq, 180.0f);
            setP (p, v2_disp_pinch, 0.70f);
            setP (p, lfo1_shape, 0.0f);
            setP (p, lfo1_rate, 3.0f);
            setP (p, lfo1_dest1, (float) destIndexOf (v2_disp_freq));
            setP (p, lfo1_amt1, 0.30f);
            maybeSeedQa (p, 0xD1590001u);
        };
        const int nOdd = (int) (0.5 * sr);
        const std::vector<MidiEvent> evOdd {
            { 0,     juce::MidiMessage::noteOn (1, 36, 0.9f) },
            { 4000,  juce::MidiMessage::noteOn (1, 48, 0.85f) },
            { 14000, juce::MidiMessage::noteOff (1, 36) },
        };
        auto pR = makeProcessor (sr, 512); setupDispOdd (*pR); pR->prepareToPlay (sr, 512); maybeSeedQa (*pR, 0xD1590001u);
        auto rR = renderEvents (*pR, sr, nOdd, { 512 }, evOdd);
        auto pO = makeProcessor (sr, 2048); setupDispOdd (*pO); pO->prepareToPlay (sr, 2048); maybeSeedQa (*pO, 0xD1590001u);
        auto rO = renderEvents (*pO, sr, nOdd, { 0, 1, 7, 13, 64, 137, 2048 }, evOdd);
        double dSq = 0.0;
        for (int i = 0; i < nOdd; ++i)
        {
            const double dl = (double) rR.l[(size_t) i] - (double) rO.l[(size_t) i];
            const double dr = (double) rR.r[(size_t) i] - (double) rO.r[(size_t) i];
            dSq += 0.5 * (dl * dl + dr * dr);
        }
        dispOddResDb = linToDb (std::sqrt (dSq / (double) nOdd));
        if (rO.nonFiniteCount > 0 || dispOddResDb >= -100.0) dispOk = false;
    }
    if (! dispOk) res.status = GateStatus::Fail;
    res.details.push_back ("All-Pass Phase Dispersion (4..8 stg, 44.1..192k): magErr=" + fmt (dispMaxMagErrDb, 4)
                           + "dB, bassPk=" + fmt (dispBassPkRatio, 2) + "x, bassDelay=" + fmt (dispBassDelayMs, 1)
                           + "ms, sub120Corr=" + fmt (dispSub120Corr, 4) + ", alias=" + fmt (dispWorstAlias, 1)
                           + "dBc, DC=" + fmt (dispWorstDc, 1) + "dBFS, oddRes=" + fmt (dispOddResDb, 1)
                           + "dBFS [" + (dispOk ? "OK" : "FAIL") + "]");

    const double worstAllAlias = std::max ({ worstFltAlias, fbWorstAlias, railWorstAlias, dispWorstAlias });
    const double worstAllDc    = std::max ({ worstFltDc, fbWorstDc, railWorstDc, dispWorstDc });
    res.summary = "modStab=" + std::string (stabOk ? "OK" : "FAIL")
                + ", SVF=" + fmt (maxSvfCents, 2) + "ct, Lad=" + fmt (maxLadCents, 2)
                + "ct, SK=" + fmt (maxSkCents, 2) + "ct, rateDiff=" + fmt (maxLadRateDiffDb, 3)
                + "dB, nlAlias=" + fmt (worstAllAlias, 1) + "dBc, DC=" + fmt (worstAllDc, 1) + "dBFS";
    return res;
}

//==============================================================================
// GATE 5 — Odd-Block, Rate-Switch & PDC Invariance
//==============================================================================
GateResult runGate5()
{
    GateResult res { 5, "Odd-Block, Rate-Switch & PDC Invariance", GateStatus::Pass, "", {} };
    constexpr double sr = 48000.0;

    auto setupMultiPatch = [] (VesselProcessor& p)
    {
        setCleanDryPatch (p);
        setP (p, sub_on, 1.0f);
        setP (p, flt_on, 1.0f);
        setP (p, flt_a_cut, 1400.0f);
        setP (p, flt_a_res, 0.45f);
        setP (p, tube_on, 1.0f);
        setP (p, tube_drive, 0.35f);
        setP (p, lfo1_shape, 0.0f);   // Sine
        setP (p, lfo1_rate, 3.5f);
        setP (p, lfo1_dest1, (float) destIndexOf (flt_a_cut));
        setP (p, lfo1_amt1, 0.35f);
        setP (p, mono_on, 1.0f);
        setP (p, clip_on, 1.0f);
        maybeSeedQa (p, 0x5EED2027u);
    };

    // Part A: Uniform 512 vs odd/prime blocks {0, 1, 7, 13, 64, 137, 2048, 3, 29, 113}
    const int total = (int) (1.0 * sr);
    const std::vector<MidiEvent> events {
        { 0,     juce::MidiMessage::noteOn (1, 48, 0.9f) },
        { 12000, juce::MidiMessage::noteOn (1, 55, 0.8f) },
        { 30000, juce::MidiMessage::noteOff (1, 48) },
        { 36000, juce::MidiMessage::noteOff (1, 55) },
    };

    auto pRef = makeProcessor (sr, 512);
    setupMultiPatch (*pRef);
    pRef->prepareToPlay (sr, 512);
    maybeSeedQa (*pRef, 0x5EED2027u);
    auto rRef = renderEvents (*pRef, sr, total, { 512 }, events);

    auto pOdd = makeProcessor (sr, 2048);
    setupMultiPatch (*pOdd);
    pOdd->prepareToPlay (sr, 2048);
    maybeSeedQa (*pOdd, 0x5EED2027u);
    auto rOdd = renderEvents (*pOdd, sr, total, { 0, 1, 7, 13, 64, 137, 2048, 3, 29, 113 }, events);

    double diffSq = 0.0;
    float maxDiff = 0.0f;
    for (int i = 0; i < total; ++i)
    {
        const float dl = rRef.l[(size_t) i] - rOdd.l[(size_t) i];
        const float dr = rRef.r[(size_t) i] - rOdd.r[(size_t) i];
        diffSq += 0.5 * ((double) dl * dl + (double) dr * dr);
        maxDiff = std::max ({ maxDiff, std::abs (dl), std::abs (dr) });
    }
    const double oddResDb = linToDb (std::sqrt (diffSq / (double) total));
    const bool oddOk = (rOdd.nonFiniteCount == 0) && (oddResDb < -100.0);
    if (! oddOk) res.status = GateStatus::Fail;
    res.details.push_back ("Odd-block residual: rms=" + fmt (oddResDb, 1) + " dBFS, max=" + fmt (linToDb (maxDiff), 1)
                           + " dBFS (lim<-100, qaSeed=" + (kHasQaSeed<VesselProcessor> ? "YES" : "NO") + ") [" + (oddOk ? "OK" : "FAIL") + "]");

    // Part B: Rapid prepareToPlay rate switches 44.1k -> 192k -> 48k -> 96k -> 48k
    auto pSw = makeProcessor (44100.0, 256);
    setupMultiPatch (*pSw);
    bool swFinite = true;
    for (auto [r, b] : std::array<std::pair<double, int>, 4> { { { 44100.0, 64 }, { 192000.0, 2048 }, { 48000.0, 137 }, { 96000.0, 512 } } })
    {
        pSw->prepareToPlay (r, b);
        maybeSeedQa (*pSw, 0x5EED2027u);
        auto tmp = renderEvents (*pSw, r, 1024, { b }, { { 0, juce::MidiMessage::noteOn (1, 60, 0.8f) } });
        if (tmp.nonFiniteCount > 0) swFinite = false;
    }
    pSw->prepareToPlay (sr, 512);
    maybeSeedQa (*pSw, 0x5EED2027u);
    auto rSw = renderEvents (*pSw, sr, total, { 512 }, events);
    double swSq = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const float dl = rRef.l[(size_t) i] - rSw.l[(size_t) i];
        const float dr = rRef.r[(size_t) i] - rSw.r[(size_t) i];
        swSq += 0.5 * ((double) dl * dl + (double) dr * dr);
    }
    const double swResDb = linToDb (std::sqrt (swSq / (double) total));
    const bool swOk = swFinite && (rSw.nonFiniteCount == 0) && (swResDb < -100.0);
    if (! swOk) res.status = GateStatus::Fail;
    res.details.push_back ("Rate-switch reset (44.1->192->48->96->48k): residual=" + fmt (swResDb, 1) + " dBFS [" + (swOk ? "OK" : "FAIL") + "]");

    // Part C: Exact PDC latency & 0-latency bypass via audio-input impulse
    auto measureImpulseLatency = [&] (bool activeOs) -> std::pair<int, int>
    {
        auto p = makeProcessor (sr, 512);
        setCleanDryPatch (*p);
        setP (*p, wt_on, 0.0f);
        setP (*p, in_level, 1.0f);
        setP (*p, tube_on, activeOs ? 1.0f : 0.0f);
        setP (*p, tube_drive, 0.0f);
        setP (*p, tube_bias, 0.0f);
        setP (*p, clip_on, activeOs ? 1.0f : 0.0f);
        p->prepareToPlay (sr, 512);
        const int reported = p->getLatencySamples();

        const int len = 2048;
        std::vector<float> imp ((size_t) len, 0.0f);
        imp[100] = 0.5f;
        auto r = renderEvents (*p, sr, len, { 512 }, {}, &imp, &imp);
        int peakIdx = 0;
        float peakVal = 0.0f;
        for (int i = 0; i < len; ++i)
            if (std::abs (r.l[(size_t) i]) > peakVal) { peakVal = std::abs (r.l[(size_t) i]); peakIdx = i; }
        return { reported, peakIdx - 100 };
    };

    const auto [repAct, measAct] = measureImpulseLatency (true);
    const auto [repByp, measByp] = measureImpulseLatency (false);
    const bool pdcActOk = (std::abs (repAct - measAct) == 0);
    const bool pdcBypOk = (repByp == 0 && measByp == 0);
    if (! pdcActOk || ! pdcBypOk) res.status = GateStatus::Fail;
    res.details.push_back ("PDC active (tube+clip): reported=" + std::to_string (repAct) + ", measured=" + std::to_string (measAct) + " [" + (pdcActOk ? "OK" : "FAIL") + "]");
    res.details.push_back ("PDC bypass (dry): reported=" + std::to_string (repByp) + ", measured=" + std::to_string (measByp) + " (lim==0) [" + (pdcBypOk ? "OK" : "FAIL") + "]");

    res.summary = "oddRes=" + fmt (oddResDb, 1) + "dBFS, swRes=" + fmt (swResDb, 1) + "dBFS, pdcAct="
                + std::to_string (repAct) + "/" + std::to_string (measAct) + ", pdcByp=" + std::to_string (repByp) + "/" + std::to_string (measByp);
    return res;
}

//==============================================================================
// GATE 6 — Voice Stealing, DC Hygiene & Idle-Tail Denormals
//==============================================================================
GateResult runGate6 (bool ciVmMode)
{
    GateResult res { 6, "Voice Stealing, DC Hygiene & Idle Tail", GateStatus::Pass, "", {} };
    constexpr double sr = 48000.0;

    auto setupDrySineLpf = [] (VesselProcessor& p)
    {
        setCleanDryPatch (p);
        setP (p, wt_morph, 0.0f);   // pure sine
        setP (p, flt_on, 1.0f);
        setP (p, flt_route, 2.0f);  // A only
        setP (p, flt_a_cut, 1200.0f);
        setP (p, flt_a_res, 0.1f);
        setP (p, flt_drive, 0.0f);
        setP (p, flt_env, 0.0f);
        setP (p, amp_a, 3.0f);
        setP (p, amp_d, 50.0f);
        setP (p, amp_s, 1.0f);
        setP (p, amp_r, 50.0f);
    };

    auto maxSecondDiff = [] (const std::vector<float>& x, size_t from, size_t to)
    {
        float m = 0.0f;
        for (size_t i = from + 2; i < to; ++i)
            m = std::max (m, std::abs (x[i] - 2.0f * x[i - 1] + x[i - 2]));
        return m;
    };

    // Steady-state reference curvature on ~100 Hz sine (MIDI 43 = 98.0 Hz)
    auto pRef = makeProcessor (sr, 512);
    setupDrySineLpf (*pRef);
    pRef->prepareToPlay (sr, 512);
    auto rRef = renderEvents (*pRef, sr, (int) (0.6 * sr), { 512 }, { { 0, juce::MidiMessage::noteOn (1, 43, 0.8f) } });
    const float steadyCurv = maxSecondDiff (rRef.l, (size_t) (0.2 * sr), rRef.l.size());

    // Rapid polyphonic voice stealing: 28 notes spaced 15 ms apart across MIDI 41..48 (forces repeated stealing on 8 or 16 voices)
    auto p = makeProcessor (sr, 512);
    setupDrySineLpf (*p);
    p->prepareToPlay (sr, 512);
    std::vector<MidiEvent> stealEv;
    const int stepSamp = (int) (0.015 * sr);
    for (int k = 0; k < 28; ++k)
    {
        const int note = 41 + (k % 8);
        stealEv.push_back ({ k * stepSamp, juce::MidiMessage::noteOn (1, note, 0.85f) });
        stealEv.push_back ({ k * stepSamp + (int) (0.25 * sr), juce::MidiMessage::noteOff (1, note) });
    }
    const int activeEnd = 28 * stepSamp + (int) (0.35 * sr);
    auto rSteal = renderEvents (*p, sr, activeEnd, { 512 }, stealEv);

    const float stealCurv = maxSecondDiff (rSteal.l, 0, rSteal.l.size());
    const double curvRatio = (double) stealCurv / std::max ((double) steadyCurv, 1.0e-12);
    const double dcDbfs = windowedDcDbfs (rSteal.l, 0, rSteal.l.size());

    const bool stealOk = (rSteal.nonFiniteCount == 0) && (curvRatio <= 5.0) && (dcDbfs < -90.0);

    // Part B: 10 seconds idle tail post-note-off (all notes off)
    {
        juce::MidiBuffer offMidi;
        for (int n = 41; n <= 48; ++n) offMidi.addEvent (juce::MidiMessage::noteOff (1, n), 0);
        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();
        p->processBlock (buf, offMidi);
    }

    const int idleBlocks = (int) std::ceil (10.0 * sr / 512.0);
    const int tailCheckStart = idleBlocks - (int) std::ceil (0.5 * sr / 512.0);
    const int cpuCheckStart  = idleBlocks - 100;
    float tailPeak = 0.0f;
    std::vector<double> idleBlockUs;
    idleBlockUs.reserve (128);
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer emptyMidi;

    for (int b = 0; b < idleBlocks; ++b)
    {
        buf.clear();
        const auto t0 = std::chrono::steady_clock::now();
        p->processBlock (buf, emptyMidi);
        const auto t1 = std::chrono::steady_clock::now();
        if (b >= cpuCheckStart)
            idleBlockUs.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());
        if (b >= tailCheckStart)
        {
            for (int c = 0; c < 2; ++c)
            {
                const float* ch = buf.getReadPointer (c);
                for (int i = 0; i < 512; ++i) tailPeak = std::max (tailPeak, std::abs (ch[i]));
            }
        }
    }
    std::sort (idleBlockUs.begin(), idleBlockUs.end());
    const double medIdleUs = idleBlockUs[idleBlockUs.size() / 2];
    const double idleCpuPct = medIdleUs / (512.0 / sr * 1.0e6) * 100.0;
    const double tailDb = linToDb (tailPeak);
    const int activeVoices = p->activeVoiceCount();
    const bool idleCpuOk = ciVmMode || (idleCpuPct < 0.5);
    const bool idleOk = (tailDb < -120.0) && (activeVoices == 0) && idleCpuOk;

    if (! stealOk || ! idleOk) res.status = GateStatus::Fail;
    res.details.push_back ("Voice steal 2nd-diff ratio=" + fmt (curvRatio, 2) + "x (lim<=5.0), DC=" + fmt (dcDbfs, 1)
                           + " dBFS (lim<-90) [" + (stealOk ? "OK" : "FAIL") + "]");
    res.details.push_back ("10s idle tail: peak=" + fmt (tailDb, 1) + " dBFS (lim<-120), voices="
                           + std::to_string (activeVoices) + " (lim==0), idleCPU=" + fmt (idleCpuPct, 3)
                           + "% RT (" + std::string (ciVmMode ? "CI-VM info" : "lim<0.5%") + ") [" + (idleOk ? "OK" : "FAIL") + "]");
    res.summary = "stealRatio=" + fmt (curvRatio, 2) + "x, DC=" + fmt (dcDbfs, 1) + "dBFS, tail=" + fmt (tailDb, 1)
                + "dBFS, voices=" + std::to_string (activeVoices) + ", idleCPU=" + fmt (idleCpuPct, 3) + "%";
    return res;
}

//==============================================================================
// GATE 7 — V1 Factory Preset & Stereo-Bass Snapshot
//==============================================================================
struct PresetMetrics
{
    std::string name, category;
    double rmsDb = -300.0, peakDb = -300.0, centroidHz = 0.0, sub120Corr = 1.0;
    int nonFinite = 0;
};

PresetMetrics measurePresetSnapshot (const FactoryPreset& preset, double sr)
{
    PresetMetrics m;
    m.name = preset.name.toStdString();
    m.category = preset.category.toStdString();

    auto p = makeProcessor (sr, 512);
    p->applyPreset (preset);
    p->prepareToPlay (sr, 512);
    maybeSeedQa (*p, 0x5EED2027u);

    const int total = (int) (1.5 * sr);
    const std::vector<MidiEvent> ev {
        { 0,                 juce::MidiMessage::noteOn (1, 48, 0.85f) },   // C3
        { 0,                 juce::MidiMessage::noteOn (1, 60, 0.75f) },   // C4
        { (int) (1.0 * sr),  juce::MidiMessage::noteOff (1, 48) },
        { (int) (1.0 * sr),  juce::MidiMessage::noteOff (1, 60) },
    };
    auto r = renderEvents (*p, sr, total, { 512 }, ev);
    m.nonFinite = r.nonFiniteCount;

    double sq = 0.0;
    float pk = 0.0f;
    for (int i = 0; i < total; ++i)
    {
        sq += 0.5 * ((double) r.l[(size_t) i] * r.l[(size_t) i] + (double) r.r[(size_t) i] * r.r[(size_t) i]);
        pk = std::max ({ pk, std::abs (r.l[(size_t) i]), std::abs (r.r[(size_t) i]) });
    }
    m.rmsDb = linToDb (std::sqrt (sq / (double) total));
    m.peakDb = linToDb (pk);

    // Spectral centroid on active middle window (N = 16384 starting at 0.25 s)
    constexpr int order = 14;
    const int N = 1 << order;
    std::vector<float> mono ((size_t) total);
    for (int i = 0; i < total; ++i) mono[(size_t) i] = 0.5f * (r.l[(size_t) i] + r.r[(size_t) i]);
    auto spec = bhSpectrumPow (mono, (size_t) (0.25 * sr), order);
    const double binHz = sr / (double) N;
    double num = 0.0, den = 0.0;
    for (int k = 1; k < N / 2; ++k)
    {
        const double mag = std::sqrt (spec[(size_t) k]);
        num += ((double) k * binHz) * mag;
        den += mag;
    }
    m.centroidHz = (den > 1.0e-12) ? (num / den) : 0.0;

    // Sub-120 Hz stereo phase correlation (LR4 = cascaded 2nd-order Butterworth SVF lowpass at 120 Hz)
    dsp::Svf lpL1, lpL2, lpR1, lpR2;
    lpL1.setup (120.0f, 0.70710678f, (float) sr);
    lpL2.setup (120.0f, 0.70710678f, (float) sr);
    lpR1.setup (120.0f, 0.70710678f, (float) sr);
    lpR2.setup (120.0f, 0.70710678f, (float) sr);
    double sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const double sl = (double) lpL2.lowpass (lpL1.lowpass (r.l[(size_t) i]));
        const double srS = (double) lpR2.lowpass (lpR1.lowpass (r.r[(size_t) i]));
        sumLL += sl * sl; sumRR += srS * srS; sumLR += sl * srS;
    }
    const double subRms = std::sqrt (0.5 * (sumLL + sumRR) / (double) total);
    m.sub120Corr = (subRms < 1.0e-4) ? 1.0 : (sumLR / std::sqrt (sumLL * sumRR + 1.0e-30));
    return m;
}

GateResult runGate7 (const fs::path& outDir, const fs::path& compareCsv)
{
    GateResult res { 7, "V1 Factory Preset & Stereo-Bass Snapshot", GateStatus::Pass, "", {} };
    constexpr double sr = 48000.0;

    const auto& bank = PresetBank::factory().all();
    // V1 factory presets only (V2 flagship presets are appended after them and verified by Vessel2PresetQA),
    // so the locked V1 snapshot selection stays identical to the frozen Stage 3B baseline.
    const size_t v1Count = bank.size() - std::min (bank.size(), (size_t) PresetBank::v2Flagship().size());
    // Pick up to 16 diverse presets: first of each category, then evenly spaced across the bank
    std::vector<size_t> selected;
    std::set<std::string> seenCat;
    for (size_t i = 0; i < v1Count; ++i)
    {
        const auto cat = bank[i].category.toStdString();
        if (seenCat.insert (cat).second) selected.push_back (i);
    }
    for (size_t i = 0; i < v1Count && selected.size() < 16; i += std::max<size_t> (1, v1Count / 16))
        if (std::find (selected.begin(), selected.end(), i) == selected.end()) selected.push_back (i);

    std::map<std::string, double> refRms;
    if (! compareCsv.empty() && fs::exists (compareCsv))
    {
        std::ifstream in (compareCsv);
        std::string line;
        std::getline (in, line);   // header
        while (std::getline (in, line))
        {
            std::stringstream ss (line);
            std::string name, cat, rmsS;
            if (std::getline (ss, name, ',') && std::getline (ss, cat, ',') && std::getline (ss, rmsS, ','))
                refRms[name] = std::stod (rmsS);
        }
    }

    fs::create_directories (outDir);
    std::ofstream csv (outDir / "v1_preset_snapshot.csv");
    csv << "name,category,rms_dbfs,peak_dbfs,centroid_hz,sub120_corr,non_finite\n";

    double minCorr = 1.0, maxRegressDb = 0.0;
    int finiteFails = 0;
    for (size_t idx : selected)
    {
        const auto m = measurePresetSnapshot (bank[idx], sr);
        if (m.nonFinite > 0) ++finiteFails;
        minCorr = std::min (minCorr, m.sub120Corr);
        csv << m.name << "," << m.category << "," << fmt (m.rmsDb, 3) << "," << fmt (m.peakDb, 3) << ","
            << fmt (m.centroidHz, 1) << "," << fmt (m.sub120Corr, 4) << "," << m.nonFinite << "\n";

        std::string regInfo;
        if (auto it = refRms.find (m.name); it != refRms.end())
        {
            const double diff = std::abs (m.rmsDb - it->second);
            maxRegressDb = std::max (maxRegressDb, diff);
            regInfo = ", dRMS=" + fmt (diff, 3) + "dB";
            if (diff > 0.05) res.status = GateStatus::Fail;
        }
        res.details.push_back (m.name + " [" + m.category + "]: RMS=" + fmt (m.rmsDb, 1) + " dBFS, pk="
                               + fmt (m.peakDb, 1) + " dBFS, cent=" + fmt (m.centroidHz, 0) + " Hz, sub120Corr="
                               + fmt (m.sub120Corr, 3) + regInfo);
    }

    if (finiteFails > 0) res.status = GateStatus::Fail;
    res.summary = "rendered=" + std::to_string (selected.size()) + "/" + std::to_string (v1Count)
                + ", nonFinite=" + std::to_string (finiteFails) + ", minSub120Corr=" + fmt (minCorr, 3)
                + (refRms.empty() ? "" : ", maxRegress=" + fmt (maxRegressDb, 3) + "dB");
    return res;
}

//==============================================================================
// GATE 8 — Release-Build Polyphonic CPU Budget
//==============================================================================
GateResult runGate8 (bool ciVmMode)
{
    GateResult res { 8, "Release-Build Polyphonic CPU Budget", GateStatus::Pass, "", {} };
    constexpr double sr = 48000.0;
    constexpr int blk = 512;
    const double blockBudgetUs = (double) blk / sr * 1.0e6;   // 10666.67 us

    auto p = makeProcessor (sr, blk);
    setCleanDryPatch (*p);
    // Heavy polyphonic flagship stress patch
    setP (*p, sub_on, 1.0f);
    setP (*p, wt_on, 1.0f);
    setP (*p, wt_morph, 0.45f);
    setP (*p, wt_pwm, 0.35f);
    setP (*p, wt_drift, 0.25f);
    setP (*p, fm_on, 1.0f);
    setP (*p, fm_index, 0.45f);
    setP (*p, flt_on, 1.0f);
    setP (*p, flt_route, 0.0f);   // Serial dual ladders
    setP (*p, flt_a_cut, 1600.0f);
    setP (*p, flt_a_res, 0.45f);
    setP (*p, flt_b_cut, 4200.0f);
    setP (*p, flt_b_res, 0.35f);
    setP (*p, flt_drive, 0.4f);
    setP (*p, tube_on, 1.0f);
    setP (*p, tube_drive, 0.45f);
    setP (*p, bias_on, 1.0f);
    setP (*p, verb_on, 1.0f);
    setP (*p, ott_on, 1.0f);
    setP (*p, mono_on, 1.0f);
    setP (*p, clip_on, 1.0f);
    setP (*p, amp_a, 5.0f);
    setP (*p, amp_s, 1.0f);
    setP (*p, amp_r, 2000.0f);
    p->prepareToPlay (sr, blk);

    // Trigger 16 distinct notes (saturates kVoices=8 in V1 and kVoices=16 in Stage 3B)
    juce::MidiBuffer chordMidi;
    for (int i = 0; i < 16; ++i)
        chordMidi.addEvent (juce::MidiMessage::noteOn (1, 36 + i * 2, 0.85f), 0);

    juce::AudioBuffer<float> buf (2, blk);
    buf.clear();
    p->processBlock (buf, chordMidi);

    juce::MidiBuffer emptyMidi;
    for (int w = 0; w < 32; ++w)
    {
        buf.clear();
        p->processBlock (buf, emptyMidi);
    }

    const int activeVoices = p->activeVoiceCount();
    constexpr int kBlocks = 240;
    std::vector<double> us;
    us.reserve (kBlocks);
    for (int b = 0; b < kBlocks; ++b)
    {
        buf.clear();
        const auto t0 = std::chrono::steady_clock::now();
        p->processBlock (buf, emptyMidi);
        const auto t1 = std::chrono::steady_clock::now();
        us.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());
    }
    std::sort (us.begin(), us.end());
    const double medUs = us[us.size() / 2];
    const double p95Us = us[(size_t) (0.95 * (double) us.size())];
    const double rtPct = medUs / blockBudgetUs * 100.0;
    const double p95Pct = p95Us / blockBudgetUs * 100.0;

    const bool ok = (activeVoices == dsp::VoiceManager::kVoices) && (ciVmMode || (rtPct < 15.0));
    if (! ok) res.status = GateStatus::Fail;

    // Also measure 16-voice Release CPU delta with Vactrol LPG + Audio-Rate Feedback active (lim < +2.5% RT)
    double lpgFbRtPct = 0.0, cpuDeltaPct = 0.0;
    {
        auto pFb = makeProcessor (sr, blk);
        setCleanDryPatch (*pFb);
        setP (*pFb, sub_on, 1.0f);
        setP (*pFb, wt_on, 1.0f);
        setP (*pFb, wt_morph, 0.45f);
        setP (*pFb, wt_pwm, 0.35f);
        setP (*pFb, wt_drift, 0.25f);
        setP (*pFb, fm_on, 1.0f);
        setP (*pFb, fm_index, 0.45f);
        setP (*pFb, flt_on, 1.0f);
        setP (*pFb, flt_route, 0.0f);
        setP (*pFb, flt_a_cut, 1600.0f);
        setP (*pFb, flt_a_res, 0.45f);
        setP (*pFb, flt_b_cut, 4200.0f);
        setP (*pFb, flt_b_res, 0.35f);
        setP (*pFb, flt_drive, 0.4f);
        setP (*pFb, v2_lpg_mode, 1.0f);
        setP (*pFb, v2_lpg_decay, 0.60f);
        setP (*pFb, v2_lpg_strike, 0.35f);
        setP (*pFb, v2_fb_filter, -0.55f);
        setP (*pFb, v2_fb_fm, 0.35f);
        setP (*pFb, tube_on, 1.0f);
        setP (*pFb, tube_drive, 0.45f);
        setP (*pFb, bias_on, 1.0f);
        setP (*pFb, verb_on, 1.0f);
        setP (*pFb, ott_on, 1.0f);
        setP (*pFb, mono_on, 1.0f);
        setP (*pFb, clip_on, 1.0f);
        setP (*pFb, amp_a, 5.0f);
        setP (*pFb, amp_s, 1.0f);
        setP (*pFb, amp_r, 2000.0f);
        pFb->prepareToPlay (sr, blk);

        buf.clear();
        pFb->processBlock (buf, chordMidi);
        for (int w = 0; w < 32; ++w) { buf.clear(); pFb->processBlock (buf, emptyMidi); }
        std::vector<double> usFb;
        usFb.reserve (kBlocks);
        for (int b = 0; b < kBlocks; ++b)
        {
            buf.clear();
            const auto t0 = std::chrono::steady_clock::now();
            pFb->processBlock (buf, emptyMidi);
            const auto t1 = std::chrono::steady_clock::now();
            usFb.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());
        }
        std::sort (usFb.begin(), usFb.end());
        lpgFbRtPct = usFb[usFb.size() / 2] / blockBudgetUs * 100.0;
        cpuDeltaPct = lpgFbRtPct - rtPct;
        const bool deltaOk = ciVmMode || (cpuDeltaPct < 2.5);
        if (! deltaOk) res.status = GateStatus::Fail;
        res.details.push_back ("LPG+Feedback 16v stress: median=" + fmt (usFb[usFb.size() / 2], 1) + " us ("
                               + fmt (lpgFbRtPct, 2) + "% RT, delta=" + (cpuDeltaPct >= 0.0 ? "+" : "")
                               + fmt (cpuDeltaPct, 2) + "% RT, lim<+2.5%) [" + (deltaOk ? "OK" : "FAIL") + "]");
    }
    // Also measure 16-voice Release CPU delta with Power-Rail Sag + 8-Stage Phase Dispersion active (lim < +2.5% RT)
    double railDispRtPct = 0.0, railDispDeltaPct = 0.0;
    {
        auto pRd = makeProcessor (sr, blk);
        setCleanDryPatch (*pRd);
        setP (*pRd, sub_on, 1.0f);
        setP (*pRd, wt_on, 1.0f);
        setP (*pRd, wt_morph, 0.45f);
        setP (*pRd, wt_pwm, 0.35f);
        setP (*pRd, wt_drift, 0.25f);
        setP (*pRd, fm_on, 1.0f);
        setP (*pRd, fm_index, 0.45f);
        setP (*pRd, flt_on, 1.0f);
        setP (*pRd, flt_route, 0.0f);
        setP (*pRd, flt_a_cut, 1600.0f);
        setP (*pRd, flt_a_res, 0.45f);
        setP (*pRd, flt_b_cut, 4200.0f);
        setP (*pRd, flt_b_res, 0.35f);
        setP (*pRd, flt_drive, 0.4f);
        setP (*pRd, v2_rail_sag, 1.0f);
        setP (*pRd, v2_rail_time, 45.0f);
        setP (*pRd, v2_disp_stages, 5.0f);   // 8 stages
        setP (*pRd, v2_disp_freq, 180.0f);
        setP (*pRd, v2_disp_pinch, 0.75f);
        setP (*pRd, tube_on, 1.0f);
        setP (*pRd, tube_drive, 0.45f);
        setP (*pRd, bias_on, 1.0f);
        setP (*pRd, verb_on, 1.0f);
        setP (*pRd, ott_on, 1.0f);
        setP (*pRd, mono_on, 1.0f);
        setP (*pRd, clip_on, 1.0f);
        setP (*pRd, amp_a, 5.0f);
        setP (*pRd, amp_s, 1.0f);
        setP (*pRd, amp_r, 2000.0f);
        pRd->prepareToPlay (sr, blk);

        buf.clear();
        pRd->processBlock (buf, chordMidi);
        for (int w = 0; w < 32; ++w) { buf.clear(); pRd->processBlock (buf, emptyMidi); }
        std::vector<double> usRd;
        usRd.reserve (kBlocks);
        for (int b = 0; b < kBlocks; ++b)
        {
            buf.clear();
            const auto t0 = std::chrono::steady_clock::now();
            pRd->processBlock (buf, emptyMidi);
            const auto t1 = std::chrono::steady_clock::now();
            usRd.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());
        }
        std::sort (usRd.begin(), usRd.end());
        railDispRtPct = usRd[usRd.size() / 2] / blockBudgetUs * 100.0;
        railDispDeltaPct = railDispRtPct - rtPct;
        const bool rdOk = ciVmMode || (railDispDeltaPct < 2.5);
        if (! rdOk) res.status = GateStatus::Fail;
        res.details.push_back ("RailSag+Disp8 16v stress: median=" + fmt (usRd[usRd.size() / 2], 1) + " us ("
                               + fmt (railDispRtPct, 2) + "% RT, delta=" + (railDispDeltaPct >= 0.0 ? "+" : "")
                               + fmt (railDispDeltaPct, 2) + "% RT, lim<+2.5%) [" + (rdOk ? "OK" : "FAIL") + "]");
    }
#ifndef NDEBUG
    res.details.push_back ("WARNING: Built without NDEBUG (not Release -O3)");
#endif
    res.details.push_back ("Poly stress (" + std::to_string (activeVoices) + "/" + std::to_string (dsp::VoiceManager::kVoices)
                           + " voices, 240x512 @48k): median=" + fmt (medUs, 1) + " us (" + fmt (rtPct, 2)
                           + "% RT, " + std::string (ciVmMode ? "CI-VM info" : "lim<15.0%") + "), p95=" + fmt (p95Us, 1)
                           + " us (" + fmt (p95Pct, 2) + "% RT) [" + (ok ? "OK" : "FAIL") + "]");
    res.summary = std::to_string (activeVoices) + "v median=" + fmt (medUs, 1) + "us (" + fmt (rtPct, 2)
                + "% RT), rail+disp=" + fmt (railDispRtPct, 2) + "% (d=" + (railDispDeltaPct >= 0.0 ? "+" : "")
                + fmt (railDispDeltaPct, 2) + "%), lpg+fb=" + fmt (lpgFbRtPct, 2) + "% (d="
                + (cpuDeltaPct >= 0.0 ? "+" : "") + fmt (cpuDeltaPct, 2) + "%), p95=" + fmt (p95Pct, 2) + "% RT";
    return res;
}

//==============================================================================
// STAGE 4 — Control-Plane QA (Macros, XY, MPE/MIDI, CC Learn, A/B, Undo, V1->V2)
//==============================================================================
GateResult runStage4ControlPlaneQA (const fs::path& outDir)
{
    GateResult res { 9, "Stage 4 Control-Plane QA", GateStatus::Pass, "", {} };
    constexpr double sr = 48000.0;
    auto rmsOf = [] (const std::vector<float>& x, size_t from = 0)
    {
        double sq = 0.0;
        const size_t n = x.size() > from ? (x.size() - from) : 0;
        for (size_t i = from; i < x.size(); ++i) sq += (double) x[i] * x[i];
        return n > 0 ? linToDb (std::sqrt (sq / (double) n)) : -300.0;
    };
    auto centroidOf = [&] (const std::vector<float>& x, size_t offset)
    {
        constexpr int order = 13;
        const int N = 1 << order;
        auto spec = bhSpectrumPow (x, offset, order);
        const double binHz = sr / (double) N;
        double num = 0.0, den = 0.0;
        for (int k = 1; k < N / 2; ++k)
        {
            const double mag = std::sqrt (spec[(size_t) k]);
            num += ((double) k * binHz) * mag;
            den += mag;
        }
        return den > 1.0e-12 ? num / den : 0.0;
    };
    auto peakFreqNear = [&] (const std::vector<float>& x, size_t offset, double loHz, double hiHz)
    {
        constexpr int order = 14;
        const int N = 1 << order;
        auto spec = bhSpectrumPow (x, offset, order);
        const double binHz = sr / (double) N;
        const int k0 = std::max (1, (int) std::floor (loHz / binHz));
        const int k1 = std::min (N / 2 - 2, (int) std::ceil (hiHz / binHz));
        int bestK = k0;
        double bestP = -1.0;
        for (int k = k0; k <= k1; ++k)
            if (spec[(size_t) k] > bestP) { bestP = spec[(size_t) k]; bestK = k; }
        // Parabolic interpolation on log power
        const double pL = std::log (std::max (spec[(size_t) (bestK - 1)], 1.0e-30));
        const double pC = std::log (std::max (spec[(size_t) bestK], 1.0e-30));
        const double pR = std::log (std::max (spec[(size_t) (bestK + 1)], 1.0e-30));
        const double denom = (pL - 2.0 * pC + pR);
        const double delta = std::abs (denom) > 1.0e-12 ? 0.5 * (pL - pR) / denom : 0.0;
        return ((double) bestK + std::clamp (delta, -0.5, 0.5)) * binHz;
    };

    int checksPass = 0, checksFail = 0;
    auto record = [&] (const std::string& name, bool ok, const std::string& info)
    {
        if (ok) ++checksPass;
        else { ++checksFail; res.status = GateStatus::Fail; }
        res.details.push_back (name + ": " + info + " [" + (ok ? "OK" : "FAIL") + "]");
    };

    // 1) 4 Hero Performance Macros (labels + multi-dest bipolar V1 & V2 targets)
    {
        auto p = makeProcessor (sr, 512);
        setCleanDryPatch (*p);
        setP (*p, flt_on, 1.0f);
        setP (*p, flt_a_cut, 600.0f);
        setP (*p, flt_a_res, 0.1f);
        setP (*p, flt_env, 0.0f);
        setP (*p, amp_a, 1.0f);
        setP (*p, amp_s, 1.0f);
        p->setMacroName (0, "WEIGHT");
        p->setMacroName (1, "FRACTURE");
        p->setMacroName (2, "DRIFT");
        p->setMacroName (3, "ABYSS");
        const bool labelsOk = p->heroMacroLabel (0) == "WEIGHT" && p->heroMacroLabel (1) == "FRACTURE"
                           && p->heroMacroLabel (2) == "DRIFT"  && p->heroMacroLabel (3) == "ABYSS";
        setP (*p, mac1_dest1, (float) destIndexOf (flt_a_cut));
        setP (*p, mac1_amt1,  0.75f);
        setP (*p, mac1_dest2, (float) destIndexOf (v2_tex_mix));
        setP (*p, mac1_amt2,  0.50f);
        setP (*p, mac2_dest1, (float) destIndexOf (flt_a_cut));
        setP (*p, mac2_amt1, -0.45f); // bipolar negative
        setP (*p, mac3_dest1, (float) destIndexOf (v2_organic_amt));
        setP (*p, mac3_amt1,  0.80f);
        setP (*p, mac4_dest1, (float) destIndexOf (verb_mix));
        setP (*p, mac4_amt1,  0.70f);
        setP (*p, verb_on, 1.0f);
        setP (*p, verb_mix, 0.0f);

        const std::vector<MidiEvent> ev { { 0, juce::MidiMessage::noteOn (1, 48, 0.9f) } };
        p->prepareToPlay (sr, 512);
        maybeSeedQa (*p, 0x4001u);
        auto rBase = renderEvents (*p, sr, 24000, { 512 }, ev);
        const double cBase = centroidOf (rBase.l, 8000);

        setP (*p, macro1, 1.0f);
        p->prepareToPlay (sr, 512);
        maybeSeedQa (*p, 0x4001u);
        auto rM1 = renderEvents (*p, sr, 24000, { 512 }, ev);
        const double cM1 = centroidOf (rM1.l, 8000);

        setP (*p, macro1, 0.0f);
        setP (*p, macro2, 1.0f);
        p->prepareToPlay (sr, 512);
        maybeSeedQa (*p, 0x4001u);
        auto rM2 = renderEvents (*p, sr, 24000, { 512 }, ev);
        const double cM2 = centroidOf (rM2.l, 8000);

        const bool ok = labelsOk && (cM1 > cBase * 1.8) && (cM2 < cBase * 0.85)
                     && rM1.nonFiniteCount == 0 && rM2.nonFiniteCount == 0;
        record ("Hero Macros (1-4)", ok,
                "baseCent=" + fmt (cBase, 0) + "Hz, M1(+cut/+tex)=" + fmt (cM1, 0) + "Hz, M2(-cut)=" + fmt (cM2, 0) + "Hz");
    }

    // 2) XY Sculptor Backend (neutral center bit-exact, bipolar X/Y, odd-block invariant)
    {
        auto p = makeProcessor (sr, 512);
        setCleanDryPatch (*p);
        setP (*p, flt_on, 1.0f);
        setP (*p, flt_a_cut, 900.0f);
        setP (*p, flt_env, 0.0f);
        setP (*p, amp_a, 1.0f);
        setP (*p, amp_s, 1.0f);
        const std::vector<MidiEvent> ev { { 0, juce::MidiMessage::noteOn (1, 48, 0.9f) } };

        p->prepareToPlay (sr, 512);
        maybeSeedQa (*p, 0x4002u);
        auto rNoRoute = renderEvents (*p, sr, 24000, { 512 }, ev);

        setP (*p, v2_xy_x_dest1, (float) destIndexOf (flt_a_cut));
        setP (*p, v2_xy_x_amt1, 0.65f);
        setP (*p, v2_xy_y_dest1, (float) destIndexOf (v2_tex_mix));
        setP (*p, v2_xy_y_amt1, 0.55f);
        p->setXyPosition (0.5f, 0.5f);
        p->prepareToPlay (sr, 512);
        maybeSeedQa (*p, 0x4002u);
        auto rCenter = renderEvents (*p, sr, 24000, { 512 }, ev);

        float maxCenterDiff = 0.0f;
        for (size_t i = 0; i < rNoRoute.l.size(); ++i)
            maxCenterDiff = std::max (maxCenterDiff, std::abs (rNoRoute.l[i] - rCenter.l[i]));

        p->setXyPosition (1.0f, 1.0f);
        p->prepareToPlay (sr, 512);
        maybeSeedQa (*p, 0x4002u);
        auto rCorner = renderEvents (*p, sr, 24000, { 512 }, ev);

        p->prepareToPlay (sr, 512);
        maybeSeedQa (*p, 0x4002u);
        auto rCornerOdd = renderEvents (*p, sr, 24000, { 1, 7, 13, 64, 137, 512 }, ev);
        float maxOddDiff = 0.0f;
        for (size_t i = 0; i < rCorner.l.size(); ++i)
            maxOddDiff = std::max (maxOddDiff, std::abs (rCorner.l[i] - rCornerOdd.l[i]));

        const double cCenter = centroidOf (rCenter.l, 8000);
        const double cCorner = centroidOf (rCorner.l, 8000);
        const bool ok = (linToDb (maxCenterDiff) < -120.0) && (cCorner > cCenter * 1.8)
                     && (linToDb (maxOddDiff) < -100.0);
        record ("XY Sculptor", ok,
                "centerDiff=" + fmt (linToDb (maxCenterDiff), 1) + "dBFS, cent " + fmt (cCenter, 0)
                + "->" + fmt (cCorner, 0) + "Hz, oddRes=" + fmt (linToDb (maxOddDiff), 1) + "dBFS");
    }

    // 3) MPE Per-Note Pitch Bend (±48 st), Slide (CC74), Poly Aftertouch, Breath (CC2), Velocity Curves
    {
        // 3a: MPE Per-Note Pitch Bend (+12 st on Ch 2 ONLY while Ch 3 stays stationary)
        auto p = makeProcessor (sr, 512);
        setCleanDryPatch (*p);
        setP (*p, wt_on, 0.0f);
        setP (*p, sub_on, 1.0f);
        setP (*p, sub1_oct, 0.0f);     // 0 oct = pure fundamental sine
        setP (*p, sub1_level, 0.85f);
        setP (*p, sub2_level, 0.0f);
        setP (*p, sub1_shape, 0.0f);
        setP (*p, flt_on, 0.0f);
        setP (*p, amp_a, 2.0f);
        setP (*p, amp_s, 1.0f);
        setP (*p, v2_mpe_on, 1.0f);
        setP (*p, v2_mpe_bend, 48.0f);
        p->prepareToPlay (sr, 512);

        // Ch 2 plays C4 (60 = 261.63 Hz) bent +12 st -> C5 (523.25 Hz); Ch 3 plays G4 (67 = 392.00 Hz) unbent
        const int pbPlus12 = 8192 + (int) std::lround (8192.0 * (12.0 / 48.0)); // 10240
        const std::vector<MidiEvent> evMpe {
            { 0, juce::MidiMessage::noteOn (2, 60, 0.85f) },
            { 0, juce::MidiMessage::noteOn (3, 67, 0.85f) },
            { 64, juce::MidiMessage::pitchWheel (2, pbPlus12) },
        };
        auto rMpe = renderEvents (*p, sr, 32768, { 512 }, evMpe);
        const double fCh3 = peakFreqNear (rMpe.l, 12000, 350.0, 430.0); // G4 ~ 392.00 Hz
        const double fCh2 = peakFreqNear (rMpe.l, 12000, 480.0, 560.0); // C5 ~ 523.25 Hz
        const double ctCh3 = std::abs (1200.0 * std::log2 (fCh3 / 391.9954));
        const double ctCh2 = std::abs (1200.0 * std::log2 (fCh2 / 523.2511));
        const bool mpeBendOk = (ctCh3 < 15.0) && (ctCh2 < 20.0);

        // 3b: MPE Slide (CC74) & Poly Aftertouch per-note isolation + Breath (CC2)
        auto p2 = makeProcessor (sr, 512);
        setCleanDryPatch (*p2);
        setP (*p2, flt_on, 1.0f);
        setP (*p2, flt_a_cut, 220.0f);
        setP (*p2, flt_env, 0.0f);
        setP (*p2, amp_a, 2.0f);
        setP (*p2, amp_s, 1.0f);
        setP (*p2, v2_mpe_on, 1.0f);
        setP (*p2, v2_slide_dest1, (float) destIndexOf (flt_a_cut));
        setP (*p2, v2_slide_amt1, 0.85f);
        setP (*p2, at_dest1, (float) destIndexOf (flt_a_cut));
        setP (*p2, at_amt1, 0.85f);
        setP (*p2, v2_breath_dest1, (float) destIndexOf (flt_a_cut));
        setP (*p2, v2_breath_amt1, 0.85f);

        p2->prepareToPlay (sr, 512);
        auto rDark = renderEvents (*p2, sr, 24000, { 512 }, { { 0, juce::MidiMessage::noteOn (2, 48, 0.85f) } });
        // Send CC74 ONLY on Ch 3 while note is on Ch 2 -> must stay dark (per-channel isolation!)
        p2->prepareToPlay (sr, 512);
        auto rOtherChSlide = renderEvents (*p2, sr, 24000, { 512 }, {
            { 0, juce::MidiMessage::noteOn (2, 48, 0.85f) },
            { 64, juce::MidiMessage::controllerEvent (3, 74, 127) }
        });
        // Send CC74 on Ch 2 -> must open filter!
        p2->prepareToPlay (sr, 512);
        auto rSameChSlide = renderEvents (*p2, sr, 24000, { 512 }, {
            { 0, juce::MidiMessage::noteOn (2, 48, 0.85f) },
            { 64, juce::MidiMessage::controllerEvent (2, 74, 127) }
        });
        // Polyphonic key pressure on note 48 vs note 60 (note 60 pressure must NOT open note 48!)
        p2->prepareToPlay (sr, 512);
        auto rPolyOther = renderEvents (*p2, sr, 24000, { 512 }, {
            { 0, juce::MidiMessage::noteOn (2, 48, 0.85f) },
            { 64, juce::MidiMessage::aftertouchChange (2, 60, 127) }
        });
        p2->prepareToPlay (sr, 512);
        auto rPolySame = renderEvents (*p2, sr, 24000, { 512 }, {
            { 0, juce::MidiMessage::noteOn (2, 48, 0.85f) },
            { 64, juce::MidiMessage::aftertouchChange (2, 48, 127) }
        });
        // Breath CC2 opens filter
        p2->prepareToPlay (sr, 512);
        auto rBreath = renderEvents (*p2, sr, 24000, { 512 }, {
            { 0, juce::MidiMessage::noteOn (2, 48, 0.85f) },
            { 64, juce::MidiMessage::controllerEvent (1, 2, 127) }
        });

        const double cDark = centroidOf (rDark.l, 8000);
        const double cOtherSl = centroidOf (rOtherChSlide.l, 8000);
        const double cSameSl = centroidOf (rSameChSlide.l, 8000);
        const double cPolySame = centroidOf (rPolySame.l, 8000);
        const double cBreath = centroidOf (rBreath.l, 8000);
        const bool mpeExprOk = (std::abs (cOtherSl - cDark) < 25.0)
                            && (cSameSl > cDark * 2.5)
                            && (cPolySame > cDark * 2.5)
                            && (cBreath > cDark * 2.5);
        juce::ignoreUnused (rPolyOther);

        // 3c: Selectable Velocity Curves (Hard < Linear < Soft < Fixed at vel=0.5)
        auto renderCurve = [&] (int curve)
        {
            auto pv = makeProcessor (sr, 512);
            setCleanDryPatch (*pv);
            setP (*pv, flt_on, 0.0f);
            setP (*pv, vel_amp, 1.0f);
            setP (*pv, v2_vel_curve, (float) curve);
            pv->prepareToPlay (sr, 512);
            auto r = renderEvents (*pv, sr, 12000, { 512 }, { { 0, juce::MidiMessage::noteOn (1, 48, 0.5f) } });
            return rmsOf (r.l, 2000);
        };
        const double rmsLin = renderCurve (0);
        const double rmsSoft = renderCurve (1);
        const double rmsHard = renderCurve (2);
        const double rmsFixed = renderCurve (4);
        const bool velCurveOk = (rmsHard < rmsLin - 2.0) && (rmsLin < rmsSoft - 1.0) && (rmsSoft < rmsFixed - 1.0);

        record ("MPE & Expression", mpeBendOk && mpeExprOk && velCurveOk,
                "MPEbend Ch2=" + fmt (fCh2, 1) + "Hz(" + fmt (ctCh2, 1) + "ct) Ch3=" + fmt (fCh3, 1) + "Hz(" + fmt (ctCh3, 1)
                + "ct), Slide/AT/Breath=" + fmt (cSameSl, 0) + "/" + fmt (cPolySame, 0) + "/" + fmt (cBreath, 0)
                + "Hz vs dark " + fmt (cDark, 0) + "Hz, Vel(H/L/S/F)=" + fmt (rmsHard, 1) + "/" + fmt (rmsLin, 1)
                + "/" + fmt (rmsSoft, 1) + "/" + fmt (rmsFixed, 1) + "dB");
    }

    // 4) Thread-Safe MIDI CC Learn (bind on audio thread, drive param, persist & reload, clear)
    {
        const auto tmpDir = juce::File (fs::absolute (outDir / "stage4_tmp").string());
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();

        auto p = makeProcessor (sr, 512);
        p->midiLearn.setStorageDirectory (tmpDir);
        setCleanDryPatch (*p);
        setP (*p, flt_on, 1.0f);
        setP (*p, flt_a_cut, 220.0f);
        setP (*p, flt_env, 0.0f);
        setP (*p, amp_a, 2.0f);
        setP (*p, amp_s, 1.0f);
        p->prepareToPlay (sr, 512);
        auto rBefore = renderEvents (*p, sr, 20000, { 512 }, { { 0, juce::MidiMessage::noteOn (1, 48, 0.85f) } });

        p->midiLearn.beginLearn (flt_a_cut);
        p->prepareToPlay (sr, 512);
        auto rAfter = renderEvents (*p, sr, 20000, { 512 }, {
            { 0, juce::MidiMessage::noteOn (1, 48, 0.85f) },
            { 64, juce::MidiMessage::controllerEvent (1, 21, 127) }
        });
        const bool learnedOk = (p->midiLearn.ccForPid (flt_a_cut) == 21) && (p->midiLearn.currentlyLearningPid() == -1);
        const bool savedOk = p->midiLearn.saveToDisk();

        MidiLearnMap reloaded;
        reloaded.setStorageDirectory (tmpDir);
        const bool loadedOk = reloaded.loadFromDisk() && (reloaded.ccForPid (flt_a_cut) == 21);
        reloaded.clearForPid (flt_a_cut);
        const bool clearedOk = (reloaded.ccForPid (flt_a_cut) == -1);

        const double cBefore = centroidOf (rBefore.l, 8000);
        const double cAfter = centroidOf (rAfter.l, 8000);
        const bool ok = learnedOk && savedOk && loadedOk && clearedOk && (cAfter > cBefore * 2.5);
        record ("MIDI CC Learn", ok,
                "CC21->flt_a_cut cent " + fmt (cBefore, 0) + "->" + fmt (cAfter, 0) + "Hz, persist=" + (loadedOk ? "OK" : "FAIL"));
    }

    // 5) A/B State Compare + Copy A->B (click-free mid-note transition)
    {
        auto p = makeProcessor (sr, 512);
        setCleanDryPatch (*p);
        setP (*p, flt_on, 1.0f);
        setP (*p, flt_a_cut, 260.0f);
        setP (*p, flt_env, 0.0f);
        setP (*p, amp_a, 2.0f);
        setP (*p, amp_s, 1.0f);
        p->copyAToB();
        p->setActiveABSlot (1);
        setP (*p, flt_a_cut, 12000.0f); // Slot B = bright, Slot A = dark
        p->prepareToPlay (sr, 512);

        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 48, 0.85f), 0);
        p->processBlock (buf, m);
        m.clear();

        std::vector<float> audioB, audioTrans, audioA;
        for (int b = 0; b < 24; ++b)
        {
            buf.clear();
            p->processBlock (buf, m);
            const float* ch = buf.getReadPointer (0);
            audioB.insert (audioB.end(), ch, ch + 512);
        }
        // Switch B -> A mid-note (click-free!)
        p->setActiveABSlot (0);
        for (int b = 0; b < 32; ++b)
        {
            buf.clear();
            p->processBlock (buf, m);
            const float* ch = buf.getReadPointer (0);
            if (b < 4) audioTrans.insert (audioTrans.end(), ch, ch + 512);
            else       audioA.insert (audioA.end(), ch, ch + 512);
        }
        const double cB = centroidOf (audioB, 2048);
        const double cA = centroidOf (audioA, 2048);
        auto max2ndDiff = [] (const std::vector<float>& x, size_t from, size_t to)
        {
            float mx = 0.0f;
            for (size_t i = std::max<size_t> (2, from); i < std::min (to, x.size()); ++i)
                mx = std::max (mx, std::abs (x[i] - 2.0f * x[i - 1] + x[i - 2]));
            return mx;
        };
        const float steadyStep = max2ndDiff (audioB, 2048, audioB.size());
        const float transStep = max2ndDiff (audioTrans, 0, audioTrans.size());
        const bool clickFree = (transStep <= std::max (steadyStep * 3.0f, 0.08f));
        const bool abValOk = (p->activeABSlot() == 0) && (std::abs (p->param (flt_a_cut)->convertFrom0to1 (p->param (flt_a_cut)->getValue()) - 260.0f) < 5.0f);
        const bool ok = abValOk && clickFree && (cB > cA * 2.5);
        record ("A/B Compare + Copy", ok,
                "B=" + fmt (cB, 0) + "Hz -> A=" + fmt (cA, 0) + "Hz, trans2ndDiff=" + fmt (transStep, 4) + " vs steady " + fmt (steadyStep, 4));
    }

    // 6) Undo / Redo Manager
    {
        auto p = makeProcessor (sr, 512);
        auto getCut = [&] { return p->param (flt_a_cut)->convertFrom0to1 (p->param (flt_a_cut)->getValue()); };
        p->clearUndoHistory();
        p->beginUndoTransaction ("Cut 500");
        setP (*p, flt_a_cut, 500.0f);
        p->flushUndoState();
        p->beginUndoTransaction ("Cut 4000");
        setP (*p, flt_a_cut, 4000.0f);
        p->flushUndoState();
        p->beginUndoTransaction ("Cut 9000");
        setP (*p, flt_a_cut, 9000.0f);
        p->flushUndoState();

        const bool u1 = p->canUndo() && p->undo();
        const float afterU1 = getCut();
        const bool u2 = p->canUndo() && p->undo();
        const float afterU2 = getCut();
        const bool r1 = p->canRedo() && p->redo();
        const float afterR1 = getCut();
        const bool r2 = p->canRedo() && p->redo();
        const float afterR2 = getCut();
        const bool ok = u1 && u2 && r1 && r2
                     && std::abs (afterU1 - 4000.0f) < 5.0f
                     && std::abs (afterU2 - 500.0f) < 5.0f
                     && std::abs (afterR1 - 4000.0f) < 5.0f
                     && std::abs (afterR2 - 9000.0f) < 5.0f;
        record ("Undo / Redo", ok,
                "9000 -> undo(" + fmt (afterU1, 0) + ") -> undo(" + fmt (afterU2, 0)
                + ") -> redo(" + fmt (afterR1, 0) + ") -> redo(" + fmt (afterR2, 0) + ")");
    }

    // 7) Versioned Preset Manager (version="2") & Lossless V1 -> V2 Migration (±0.05 dB RMS)
    {
        const auto tmpDir = juce::File (fs::absolute (outDir / "stage4_tmp").string());
        auto p = makeProcessor (sr, 512);
        p->presetManager.setStorageDirectory (tmpDir);
        setCleanDryPatch (*p);
        setP (*p, flt_a_cut, 1234.0f);
        setP (*p, v2_organic_amt, 0.42f);
        setP (*p, v2_tex_mix, 0.25f);
        p->setMacroName (0, "MONOLITH");
        p->setXyLabels ("GRIT", "VOID");

        auto captured = p->captureCurrentPreset ("Monolith Test", "VESSEL QA", "Bass", "Dark|Sub|MPE", "Stage 4 roundtrip preset", true);
        juce::File written;
        const bool saved = p->presetManager.saveUserPreset (captured, &written);
        const auto hits = p->presetManager.search ("mpe", "Bass", true);
        bool roundtripOk = saved && written.existsAsFile() && ! hits.empty();
        if (roundtripOk)
        {
            const auto& loaded = p->presetManager.allPresets()[(size_t) hits[0]];
            roundtripOk = (loaded.schemaVersion == 2) && loaded.name == "Monolith Test" && loaded.author == "VESSEL QA"
                       && loaded.favorite && loaded.xyLabelX == "GRIT" && loaded.xyLabelY == "VOID"
                       && loaded.macroNames[0] == "MONOLITH";
        }

        // Lossless V1 -> V2 state migration across 4 diverse factory presets
        const auto& bank = PresetBank::factory().all();
        const std::vector<size_t> testIdx { 0, std::min<size_t> (12, bank.size() - 1),
                                            std::min<size_t> (35, bank.size() - 1), std::min<size_t> (65, bank.size() - 1) };
        double maxMigDiffDb = 0.0;
        bool v2ResetOk = true;
        for (size_t idx : testIdx)
        {
            const auto& pr = bank[idx];
            const auto refM = measurePresetSnapshot (pr, sr);

            // Serialize state, then strip version + all v2_* params to simulate a legacy V1 DAW session
            auto pSrc = makeProcessor (sr, 512);
            pSrc->applyPreset (pr);
            juce::MemoryBlock stateBin;
            pSrc->getStateInformation (stateBin);
            auto xml = juce::AudioProcessor::getXmlFromBinary (stateBin.getData(), (int) stateBin.getSize());
            xml->removeAttribute ("version");
            std::vector<juce::XmlElement*> toRemove;
            for (auto* ch : xml->getChildWithTagNameIterator ("PARAM"))
                if (ch->getStringAttribute ("id").startsWith ("v2_"))
                    toRemove.push_back (ch);
            for (auto* ch : toRemove) xml->removeChildElement (ch, true);
            juce::MemoryBlock v1Bin;
            juce::AudioProcessor::copyXmlToBinary (*xml, v1Bin);

            // Dirty a fresh V2 processor with extreme V2 values, then load the stripped V1 binary state
            auto pDst = makeProcessor (sr, 512);
            setP (*pDst, v2_organic_amt, 0.95f);
            setP (*pDst, v2_organic_floor, 0.85f);
            setP (*pDst, v2_tex_mix, 0.90f);
            setP (*pDst, v2_xy_x, 1.0f);
            setP (*pDst, v2_xy_y, 0.0f);
            setP (*pDst, v2_mpe_on, 1.0f);
            setP (*pDst, v2_pb_range, 24.0f);
            setP (*pDst, v2_vel_curve, 2.0f);
            pDst->setStateInformation (v1Bin.getData(), (int) v1Bin.getSize());

            if (pDst->param (v2_organic_amt)->getValue() > 1.0e-5f
                || pDst->param (v2_tex_mix)->getValue() > 1.0e-5f
                || std::abs (pDst->param (v2_xy_x)->getValue() - 0.5f) > 1.0e-5f
                || pDst->param (v2_mpe_on)->getValue() > 0.5f)
                v2ResetOk = false;

            pDst->prepareToPlay (sr, 512);
            maybeSeedQa (*pDst, 0x5EED2027u);
            const int total = (int) (1.5 * sr);
            auto rMig = renderEvents (*pDst, sr, total, { 512 }, {
                { 0, juce::MidiMessage::noteOn (1, 48, 0.85f) },
                { 0, juce::MidiMessage::noteOn (1, 60, 0.75f) },
                { (int) (1.0 * sr), juce::MidiMessage::noteOff (1, 48) },
                { (int) (1.0 * sr), juce::MidiMessage::noteOff (1, 60) }
            });
            double sq = 0.0;
            for (int i = 0; i < total; ++i)
                sq += 0.5 * ((double) rMig.l[(size_t) i] * rMig.l[(size_t) i] + (double) rMig.r[(size_t) i] * rMig.r[(size_t) i]);
            const double migRms = linToDb (std::sqrt (sq / (double) total));
            maxMigDiffDb = std::max (maxMigDiffDb, std::abs (migRms - refM.rmsDb));
        }

        tmpDir.deleteRecursively();
        const bool ok = roundtripOk && v2ResetOk && (maxMigDiffDb <= 0.05);
        record ("V2 Preset & V1 Migration", ok,
                "V2 roundtrip=" + std::string (roundtripOk ? "OK" : "FAIL")
                + ", V1->V2 reset=" + std::string (v2ResetOk ? "OK" : "FAIL")
                + ", maxRMSdiff=" + fmt (maxMigDiffDb, 4) + "dB (lim<=0.05)");
    }

    res.summary = "checks PASS=" + std::to_string (checksPass) + " FAIL=" + std::to_string (checksFail);
    return res;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    bool baselineMode = false;
    bool runStage4 = false;
    bool ciVmMode = false;
    std::set<int> enabledGates { 1, 2, 3, 4, 5, 6, 7, 8 };
    fs::path outDir = "build/vessel2_qa";
    fs::path compareV1;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--baseline") baselineMode = true;
        else if (arg == "--strict") baselineMode = false;
        else if (arg == "--stage4-qa") runStage4 = true;
        else if (arg == "--ci-vm") ciVmMode = true;
        else if (arg == "--out-dir" && i + 1 < argc) outDir = argv[++i];
        else if (arg == "--compare-v1" && i + 1 < argc) compareV1 = argv[++i];
        else if (arg == "--gates" && i + 1 < argc)
        {
            enabledGates.clear();
            std::stringstream ss (argv[++i]);
            std::string tok;
            while (std::getline (ss, tok, ','))
                if (! tok.empty()) enabledGates.insert (std::stoi (tok));
        }
    }

#ifdef VESSEL_TABLES_DIR
    vessel::dsp::setTablesFolder (juce::File (VESSEL_TABLES_DIR));
#endif

    std::vector<GateResult> results;
    if (enabledGates.count (1)) results.push_back (runGate1());
    if (enabledGates.count (2)) results.push_back (runGate2());
    if (enabledGates.count (3)) results.push_back (runGate3());
    if (enabledGates.count (4)) results.push_back (runGate4());
    if (enabledGates.count (5)) results.push_back (runGate5());
    if (enabledGates.count (6)) results.push_back (runGate6 (ciVmMode));
    if (enabledGates.count (7)) results.push_back (runGate7 (outDir, compareV1));
    if (enabledGates.count (8)) results.push_back (runGate8 (ciVmMode));
    if (runStage4) results.push_back (runStage4ControlPlaneQA (outDir));

    fs::create_directories (outDir);
    std::ofstream report (outDir / "qa_report.txt");

    auto emit = [&] (const std::string& line) { std::cout << line << "\n"; report << line << "\n"; };

    emit ("=== VESSEL II — Locked 8-Gate Headless DSP QA Report ===");
    int nPass = 0, nFail = 0, nPending = 0;
    for (const auto& g : results)
    {
        if (g.status == GateStatus::Pass) ++nPass;
        else if (g.status == GateStatus::Fail) ++nFail;
        else ++nPending;
        emit ("Gate " + std::to_string (g.id) + " [" + statusStr (g.status) + "] " + g.name + " — " + g.summary);
        for (const auto& d : g.details)
            emit ("    " + d);
    }
    emit ("--------------------------------------------------------");
    emit ("TOTAL: PASS=" + std::to_string (nPass) + " FAIL=" + std::to_string (nFail) + " PENDING=" + std::to_string (nPending)
          + (baselineMode ? " (baseline capture mode)" : " (strict mode)"));

    if (baselineMode) return 0;
    return (nFail == 0 && nPending == 0) ? 0 : 1;
}
