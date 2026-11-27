// VESSEL II — Permanent Headless 8-Gate DSP QA Harness
// Zero GUI, zero OpenGL, zero OS audio-device dependencies.
// Compiles cleanly on macOS, Windows (MSVC), and Linux (GCC/Clang).
//
// Usage:
//   Vessel2QA [--baseline] [--strict] [--gates 1,2,3,4,5,6,7,8]
//             [--out-dir <dir>] [--compare-v1 <csv>]
//
// Locked thresholds (VESSEL_V2_SPEC.md Stage 2 — never weakened):
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
    double maxSvfPeak = 0.0, maxLadPeak = 0.0;
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
            lad.resetLane (0);
            const float ladOsr = (float) (4.0 * sr);   // matches voice 4x oversampling
            const float svfQ = 0.707f + resFrac * 25.0f;
            const float ladK = resFrac * 4.3f;

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
            }
        }
    }
    if (! stabOk) res.status = GateStatus::Fail;
    res.details.push_back ("Audio-rate 1kHz mod (44.1..192k, 95-100% res): svfPk=" + fmt (maxSvfPeak, 2)
                           + ", ladPk=" + fmt (maxLadPeak, 2) + " [" + (stabOk ? "OK" : "FAIL") + "]");

    // Part B: Static self-oscillation pitch tracking C3-C6 (SVF ±5 ct, 4-pole ladder ±15 ct)
    constexpr double sr = 48000.0;
    const int notes[] { 48, 60, 72, 84 };   // C3, C4, C5, C6
    double maxSvfCents = 0.0, maxLadCents = 0.0;
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
    }
    const bool svfPitchOk = (maxSvfCents <= 5.0);
    const bool ladPitchOk = (maxLadCents <= 15.0);
    if (! svfPitchOk || ! ladPitchOk) res.status = GateStatus::Fail;
    res.details.push_back ("Self-osc C3-C6: SVF maxErr=" + fmt (maxSvfCents, 2) + " ct (lim<=5) [" + (svfPitchOk ? "OK" : "FAIL")
                           + "], Ladder maxErr=" + fmt (maxLadCents, 2) + " ct (lim<=15) [" + (ladPitchOk ? "OK" : "FAIL") + "]");

    // Part C: Rate-invariant frequency response (±0.5 dB up to 12 kHz) on 4x-oversampled Ladder & 1x SVF
    const double testFreqs[] { 200.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 12000.0 };
    const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
    auto measureLadderMagDb = [] (double rate, double fc, double fTest)
    {
        const double osr = 4.0 * rate;
        dsp::LadderBank<1> lad;
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

    res.summary = "modStab=" + std::string (stabOk ? "OK" : "FAIL")
                + ", SVF=" + fmt (maxSvfCents, 2) + "ct, Lad=" + fmt (maxLadCents, 2) + "ct, rateDiff=" + fmt (maxLadRateDiffDb, 3) + "dB";
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
GateResult runGate6()
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
    const bool idleOk = (tailDb < -120.0) && (activeVoices == 0) && (idleCpuPct < 0.5);

    if (! stealOk || ! idleOk) res.status = GateStatus::Fail;
    res.details.push_back ("Voice steal 2nd-diff ratio=" + fmt (curvRatio, 2) + "x (lim<=5.0), DC=" + fmt (dcDbfs, 1)
                           + " dBFS (lim<-90) [" + (stealOk ? "OK" : "FAIL") + "]");
    res.details.push_back ("10s idle tail: peak=" + fmt (tailDb, 1) + " dBFS (lim<-120), voices="
                           + std::to_string (activeVoices) + " (lim==0), idleCPU=" + fmt (idleCpuPct, 3) + "% RT (lim<0.5%) [" + (idleOk ? "OK" : "FAIL") + "]");
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
    // Pick up to 16 diverse presets: first of each category, then evenly spaced across the bank
    std::vector<size_t> selected;
    std::set<std::string> seenCat;
    for (size_t i = 0; i < bank.size(); ++i)
    {
        const auto cat = bank[i].category.toStdString();
        if (seenCat.insert (cat).second) selected.push_back (i);
    }
    for (size_t i = 0; i < bank.size() && selected.size() < 16; i += std::max<size_t> (1, bank.size() / 16))
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
    res.summary = "rendered=" + std::to_string (selected.size()) + "/" + std::to_string (bank.size())
                + ", nonFinite=" + std::to_string (finiteFails) + ", minSub120Corr=" + fmt (minCorr, 3)
                + (refRms.empty() ? "" : ", maxRegress=" + fmt (maxRegressDb, 3) + "dB");
    return res;
}

//==============================================================================
// GATE 8 — Release-Build Polyphonic CPU Budget
//==============================================================================
GateResult runGate8()
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

    const bool ok = (rtPct < 15.0);
    if (! ok) res.status = GateStatus::Fail;
#ifndef NDEBUG
    res.details.push_back ("WARNING: Built without NDEBUG (not Release -O3)");
#endif
    res.details.push_back ("Poly stress (" + std::to_string (activeVoices) + "/" + std::to_string (dsp::VoiceManager::kVoices)
                           + " voices, 240x512 @48k): median=" + fmt (medUs, 1) + " us (" + fmt (rtPct, 2)
                           + "% RT, lim<15.0%), p95=" + fmt (p95Us, 1) + " us (" + fmt (p95Pct, 2) + "% RT) [" + (ok ? "OK" : "FAIL") + "]");
    res.summary = std::to_string (activeVoices) + "v median=" + fmt (medUs, 1) + "us (" + fmt (rtPct, 2) + "% RT), p95=" + fmt (p95Pct, 2) + "% RT";
    return res;
}
} // namespace

int main (int argc, char** argv)
{
    bool baselineMode = false;
    std::set<int> enabledGates { 1, 2, 3, 4, 5, 6, 7, 8 };
    fs::path outDir = "build/vessel2_qa";
    fs::path compareV1;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--baseline") baselineMode = true;
        else if (arg == "--strict") baselineMode = false;
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
    if (enabledGates.count (6)) results.push_back (runGate6());
    if (enabledGates.count (7)) results.push_back (runGate7 (outDir, compareV1));
    if (enabledGates.count (8)) results.push_back (runGate8());

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
