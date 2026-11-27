// VESSEL II — Stage 6 Closed-Loop 20-Preset Audio QA & 24-bit WAV Audition Exporter
// Headless C++20 binary (zero GUI, OpenGL, or OS audio-device dependencies).
//
// Usage:
//   Vessel2PresetQA [--bank path/to/V2FlagshipBank.xml] [--audition-dir build/vessel2_qa/audition]
//                   [--report build/vessel2_qa/preset_qa_report.txt] [--only substr] [--no-wav]
#include "PluginProcessor.h"
#include "Presets.h"
#include "dsp/Tables.h"
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numbers>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace vessel;

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;
constexpr double kPiD = std::numbers::pi_v<double>;
constexpr double kTwoPiD = 2.0 * kPiD;
constexpr uint32_t kQaSeed = 0x5EED2027u;

std::string fmt (double v, int prec = 2)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision (prec) << v;
    return os.str();
}

double linToDb (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-15)); }

struct NoteEv
{
    double startSec = 0.0;
    double durSec = 0.5;
    int note = 60;
    float vel = 0.85f;
    int channel = 1;
};

struct CcRamp
{
    int cc = 1;          // 1 = MW, 74 = Slide, -1 = Channel Pressure
    float from = 0.0f;
    float to = 0.0f;
    double startSec = 0.0;
    double endSec = 2.0;
    int channel = 1;
};

struct MacroRamp
{
    int macroIdx = -1;   // 0..3
    float from = 0.0f;
    float to = 0.0f;
    double startSec = 0.0;
    double endSec = 2.0;
};

struct RenderResult
{
    std::vector<float> l, r;
    int nonFinite = 0;
};

struct AudioMetrics
{
    double rmsDb = -300.0;
    double monoRmsDb = -300.0;
    double monoDropDb = 0.0;
    double truePeakDb = -300.0;
    double samplePeakDb = -300.0;
    double dcDb = -300.0;
    double steadyDcDb = -300.0;
    double sub120Corr = 1.0;
    double centroidHz = 0.0;
    double stereoWidth = 0.0;
    double crestDb = 0.0;
    double envContrastDb = 0.0;
    double lowRatio = 0.0;
    double highRatio = 0.0;
    std::vector<double> bandsDb;   // 1/3-octave level-normalised spectral shape
    double flux = 0.0;
    int nonFinite = 0;
};

float presetPlain (const FactoryPreset& p, int pid)
{
    float v = spec (pid).def;
    for (const auto& [id, x] : p.values)
        if (id == pid) v = x;
    return v;
}

std::unique_ptr<VesselProcessor> makePrepared (const FactoryPreset& preset,
                                               const std::array<float, 4>& macroOverrides = { -1.0f, -1.0f, -1.0f, -1.0f })
{
    auto p = std::make_unique<VesselProcessor>();
    p->setPlayConfigDetails (2, 2, kSr, kBlock);
    p->applyPreset (preset);
    for (int m = 0; m < 4; ++m)
        if (macroOverrides[(size_t) m] >= 0.0f)
            p->param (macroPid (m))->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, macroOverrides[(size_t) m]));
    p->prepareToPlay (kSr, kBlock);
    p->setQaDeterministicSeed (kQaSeed);
    if (presetPlain (preset, conv_on) > 0.5f)
        juce::Thread::sleep (140);   // wait for JUCE background Convolution IR queue
    return p;
}

RenderResult renderPhrase (const FactoryPreset& preset,
                           double totalSec,
                           const std::vector<NoteEv>& notes,
                           const std::vector<CcRamp>& ccRamps = {},
                           const std::vector<MacroRamp>& macroRamps = {},
                           const std::array<float, 4>& macroInit = { -1.0f, -1.0f, -1.0f, -1.0f })
{
    auto proc = makePrepared (preset, macroInit);
    const int total = (int) std::llround (totalSec * kSr);
    RenderResult out;
    out.l.assign ((size_t) total, 0.0f);
    out.r.assign ((size_t) total, 0.0f);

    struct MidiItem { int sample; juce::MidiMessage msg; };
    std::vector<MidiItem> events;
    events.reserve (notes.size() * 2 + 16);
    for (const auto& n : notes)
    {
        const int sOn = juce::jlimit (0, total - 1, (int) std::llround (n.startSec * kSr));
        const int sOff = juce::jlimit (sOn + 1, total, (int) std::llround ((n.startSec + n.durSec) * kSr));
        events.push_back ({ sOn, juce::MidiMessage::noteOn (n.channel, n.note, n.vel) });
        if (sOff < total)
            events.push_back ({ sOff, juce::MidiMessage::noteOff (n.channel, n.note) });
    }
    std::stable_sort (events.begin(), events.end(), [] (const MidiItem& a, const MidiItem& b) { return a.sample < b.sample; });

    juce::AudioBuffer<float> buf (2, kBlock);
    size_t evIdx = 0;

    for (int pos = 0; pos < total; pos += kBlock)
    {
        const int n = std::min (kBlock, total - pos);
        const double tSec = (double) pos / kSr;

        for (const auto& mr : macroRamps)
        {
            if (mr.macroIdx < 0 || mr.macroIdx >= 4) continue;
            const double span = std::max (1.0e-6, mr.endSec - mr.startSec);
            const float u = (float) std::clamp ((tSec - mr.startSec) / span, 0.0, 1.0);
            const float val = mr.from + (mr.to - mr.from) * u;
            proc->param (macroPid (mr.macroIdx))->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, val));
        }

        juce::MidiBuffer midi;
        for (const auto& cr : ccRamps)
        {
            const double span = std::max (1.0e-6, cr.endSec - cr.startSec);
            const float u = (float) std::clamp ((tSec - cr.startSec) / span, 0.0, 1.0);
            const int v7 = juce::jlimit (0, 127, (int) std::lround (127.0f * (cr.from + (cr.to - cr.from) * u)));
            if (cr.cc == -1)
                midi.addEvent (juce::MidiMessage::channelPressureChange (cr.channel, v7), 0);
            else
                midi.addEvent (juce::MidiMessage::controllerEvent (cr.channel, cr.cc, v7), 0);
        }

        while (evIdx < events.size() && events[evIdx].sample < pos + n)
        {
            midi.addEvent (events[evIdx].msg, std::max (0, events[evIdx].sample - pos));
            ++evIdx;
        }

        buf.setSize (2, n, false, false, true);
        buf.clear();
        proc->processBlock (buf, midi);

        const float* rl = buf.getReadPointer (0);
        const float* rr = buf.getReadPointer (1);
        for (int i = 0; i < n; ++i)
        {
            if (! std::isfinite (rl[i]) || ! std::isfinite (rr[i])) ++out.nonFinite;
            out.l[(size_t) (pos + i)] = rl[i];
            out.r[(size_t) (pos + i)] = rr[i];
        }
    }
    return out;
}

double computeTruePeakDb (const std::vector<float>& l, const std::vector<float>& r)
{
    juce::dsp::Oversampling<float> os (2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
    os.initProcessing (4096);
    float pk = 0.0f;
    std::vector<float> a (4096), b (4096);
    for (size_t pos = 0; pos < l.size(); pos += 4096)
    {
        const size_t n = std::min<size_t> (4096, l.size() - pos);
        std::copy_n (l.data() + pos, n, a.data());
        std::copy_n (r.data() + pos, n, b.data());
        float* ch[2] { a.data(), b.data() };
        juce::dsp::AudioBlock<float> blk (ch, 2, n);
        auto up = os.processSamplesUp (blk);
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < up.getNumSamples(); ++i)
                pk = std::max (pk, std::abs (up.getSample (c, (int) i)));
    }
    return linToDb (pk);
}

AudioMetrics analyze (const RenderResult& rr, double rmsStartSec = 0.0, double rmsEndSec = -1.0)
{
    AudioMetrics m;
    m.nonFinite = rr.nonFinite;
    const size_t N = rr.l.size();
    if (N == 0) return m;

    const size_t i0 = std::min (N, (size_t) std::llround (rmsStartSec * kSr));
    const size_t i1 = (rmsEndSec < 0.0) ? N : std::min (N, (size_t) std::llround (rmsEndSec * kSr));
    const size_t win = (i1 > i0) ? (i1 - i0) : N;

    double sumStSq = 0.0, sumMonoSq = 0.0, sumMidSq = 0.0, sumSideSq = 0.0;
    double sumL = 0.0, sumR = 0.0, sumW = 0.0;
    float spk = 0.0f;
    const double invNm1 = (N > 1) ? (1.0 / (double) (N - 1)) : 0.0;
    for (size_t i = 0; i < N; ++i)
    {
        const double sl = rr.l[i], sr = rr.r[i];
        spk = std::max ({ spk, (float) std::abs (sl), (float) std::abs (sr) });
        // 4-term Blackman-Harris window for unbiased DC estimation (0 dB DC gain, >110 dB AC bass rejection)
        const double ph = kTwoPiD * (double) i * invNm1;
        const double w = 0.35875 - 0.48829 * std::cos (ph) + 0.14128 * std::cos (2.0 * ph) - 0.01168 * std::cos (3.0 * ph);
        sumL += sl * w;
        sumR += sr * w;
        sumW += w;
        if (i >= i0 && i < i1)
        {
            const double mid = 0.5 * (sl + sr);
            const double side = 0.5 * (sl - sr);
            sumStSq += 0.5 * (sl * sl + sr * sr);
            sumMonoSq += mid * mid;
            sumMidSq += mid * mid;
            sumSideSq += side * side;
        }
    }

    const double rmsLin = std::sqrt (sumStSq / (double) win);
    const double monoLin = std::sqrt (sumMonoSq / (double) win);
    m.rmsDb = linToDb (rmsLin);
    m.monoRmsDb = linToDb (monoLin);
    m.monoDropDb = (rmsLin > 1.0e-7) ? std::max (0.0, m.rmsDb - m.monoRmsDb) : 0.0;
    m.samplePeakDb = linToDb (spk);
    m.truePeakDb = computeTruePeakDb (rr.l, rr.r);
    const double invW = (sumW > 1.0e-12) ? (1.0 / sumW) : 0.0;
    m.dcDb = linToDb (std::max (std::abs (sumL * invW), std::abs (sumR * invW)));

    // Steady-state windowed DC over [rmsStartSec, rmsEndSec] (isolates true DC from start/end edge transients)
    double ssl = 0.0, ssr = 0.0, ssw = 0.0;
    const double invWm1 = (win > 1) ? (1.0 / (double) (win - 1)) : 0.0;
    for (size_t i = i0; i < i1; ++i)
    {
        const double ph = kTwoPiD * (double) (i - i0) * invWm1;
        const double w = 0.35875 - 0.48829 * std::cos (ph) + 0.14128 * std::cos (2.0 * ph) - 0.01168 * std::cos (3.0 * ph);
        ssl += (double) rr.l[i] * w;
        ssr += (double) rr.r[i] * w;
        ssw += w;
    }
    if (ssw > 1.0e-12)
        m.steadyDcDb = linToDb (std::max (std::abs (ssl / ssw), std::abs (ssr / ssw)));
    m.stereoWidth = (sumMidSq > 1.0e-12) ? std::sqrt (sumSideSq / sumMidSq) : 0.0;
    m.crestDb = m.samplePeakDb - m.rmsDb;

    // Sub-120 Hz stereo phase correlation (LR4 = cascaded 2nd-order Butterworth SVF lowpass at 120 Hz)
    dsp::Svf lpL1, lpL2, lpR1, lpR2;
    lpL1.setup (120.0, 0.70710678, kSr);
    lpL2.setup (120.0, 0.70710678, kSr);
    lpR1.setup (120.0, 0.70710678, kSr);
    lpR2.setup (120.0, 0.70710678, kSr);
    double sLL = 0.0, sRR = 0.0, sLR = 0.0;
    for (size_t i = i0; i < i1; ++i)
    {
        const double sl = (double) lpL2.lowpass (lpL1.lowpass (rr.l[i]));
        const double srS = (double) lpR2.lowpass (lpR1.lowpass (rr.r[i]));
        sLL += sl * sl;
        sRR += srS * srS;
        sLR += sl * srS;
    }
    const double subRms = std::sqrt (0.5 * (sLL + sRR) / (double) win);
    m.sub120Corr = (subRms < 1.0e-4) ? 1.0 : (sLR / std::sqrt (sLL * sRR + 1.0e-30));

    // Short-window dynamic envelope contrast (40 ms frames)
    const size_t hopEnv = (size_t) (0.04 * kSr);
    std::vector<double> frameDb;
    for (size_t s = i0; s + hopEnv <= i1; s += hopEnv)
    {
        double sq = 0.0;
        for (size_t k = s; k < s + hopEnv; ++k)
            sq += 0.5 * ((double) rr.l[k] * rr.l[k] + (double) rr.r[k] * rr.r[k]);
        const double db = linToDb (std::sqrt (sq / (double) hopEnv));
        if (db > m.rmsDb - 36.0) frameDb.push_back (db);
    }
    if (frameDb.size() > 1)
    {
        const double mean = std::accumulate (frameDb.begin(), frameDb.end(), 0.0) / (double) frameDb.size();
        double var = 0.0;
        for (double v : frameDb) var += (v - mean) * (v - mean);
        m.envContrastDb = std::sqrt (var / (double) frameDb.size());
    }

    // 1/3-octave spectral profile, centroid, low/high band ratios & temporal spectral flux
    constexpr int order = 12, F = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> centres;
    for (double f = 31.5; f < 16000.0; f *= std::pow (2.0, 1.0 / 3.0)) centres.push_back (f);
    const double bandMul = std::pow (2.0, 1.0 / 6.0);

    std::vector<float> buf ((size_t) F * 2, 0.0f);
    std::vector<double> meanBands (centres.size(), 0.0);
    std::vector<std::vector<double>> frames;
    double centNum = 0.0, centDen = 0.0;
    double eLow = 0.0, eHigh = 0.0, eAll = 0.0;

    for (size_t s = i0; s + F <= i1; s += F / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < F; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (kTwoPiD * (double) i / (double) (F - 1));
            buf[(size_t) i] = (float) (0.5 * ((double) rr.l[s + (size_t) i] + (double) rr.r[s + (size_t) i]) * w);
        }
        fft.performFrequencyOnlyForwardTransform (buf.data());

        std::vector<double> bands (centres.size(), 1.0e-20);
        for (int k = 1; k < F / 2; ++k)
        {
            const double f = (double) k * kSr / (double) F;
            const double mag = (double) buf[(size_t) k];
            const double pwr = mag * mag;
            centNum += f * mag;
            centDen += mag;
            eAll += pwr;
            if (f < 200.0) eLow += pwr;
            if (f > 3000.0) eHigh += pwr;
            for (size_t b = 0; b < centres.size(); ++b)
            {
                if (f >= centres[b] / bandMul && f < centres[b] * bandMul)
                {
                    bands[b] += pwr;
                    break;
                }
            }
        }
        for (size_t b = 0; b < centres.size(); ++b) meanBands[b] += bands[b];
        frames.push_back (std::move (bands));
    }

    m.centroidHz = (centDen > 1.0e-12) ? (centNum / centDen) : 0.0;
    m.lowRatio = (eAll > 1.0e-20) ? (eLow / eAll) : 0.0;
    m.highRatio = (eAll > 1.0e-20) ? (eHigh / eAll) : 0.0;

    m.bandsDb.assign (centres.size(), -80.0);
    double totalBand = 0.0;
    for (double v : meanBands) totalBand += v;
    if (totalBand > 1.0e-20)
        for (size_t b = 0; b < centres.size(); ++b)
            m.bandsDb[b] = 10.0 * std::log10 (meanBands[b] / totalBand + 1.0e-20);

    if (frames.size() > 1)
    {
        double peakB = 1.0e-20;
        for (const auto& fr : frames)
            for (double v : fr) peakB = std::max (peakB, v);
        const double flr = 10.0 * std::log10 (peakB) - 70.0;
        double acc = 0.0;
        int cnt = 0;
        for (size_t b = 0; b < centres.size(); ++b)
        {
            for (size_t t = 1; t < frames.size(); ++t)
            {
                const double a = std::max (flr, 10.0 * std::log10 (frames[t][b] + 1.0e-20));
                const double prev = std::max (flr, 10.0 * std::log10 (frames[t - 1][b] + 1.0e-20));
                acc += std::min (6.0, std::abs (a - prev));
                ++cnt;
            }
        }
        m.flux = (cnt > 0) ? (acc / (double) cnt) : 0.0;
    }

    return m;
}

double spectralDistanceDb (const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size() || a.empty()) return 0.0;
    const double topA = *std::max_element (a.begin(), a.end());
    const double topB = *std::max_element (b.begin(), b.end());
    const double top = std::max (topA, topB);
    double sq = 0.0;
    int cnt = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i] > top - 40.0 || b[i] > top - 40.0)
        {
            const double d = std::clamp (a[i] - b[i], -24.0, 24.0);
            sq += d * d;
            ++cnt;
        }
    }
    return cnt > 0 ? std::sqrt (sq / (double) cnt) : 0.0;
}

//==============================================================================
// Musical Showcase & Standardized QA MIDI Phrases
//==============================================================================
std::vector<NoteEv> auditionPhraseFor (const FactoryPreset& p)
{
    const auto cat = p.category;
    const bool mpe = presetPlain (p, v2_mpe_on) > 0.5f;
    const int ch1 = mpe ? 2 : 1;
    const int ch2 = mpe ? 3 : 1;
    const int ch3 = mpe ? 4 : 1;
    const int ch4 = mpe ? 5 : 1;

    if (cat == "Sub" || cat == "Bass")
    {
        // Dark club/IDM bassline (C2 -> G1 -> Eb2 -> F2 -> C2)
        return {
            { 0.00, 0.88, 36, 0.90f, ch1 },
            { 0.92, 0.42, 36, 0.78f, ch1 },
            { 1.38, 0.48, 39, 0.88f, ch1 },
            { 1.90, 0.92, 31, 0.92f, ch1 },
            { 2.86, 0.42, 41, 0.86f, ch1 },
            { 3.30, 0.58, 36, 0.90f, ch1 }
        };
    }
    if (cat == "Lead")
    {
        // Brooding cinematic minor lead phrase with expressive dynamics
        return {
            { 0.00, 0.95, 60, 0.84f, ch1 },
            { 0.98, 0.46, 63, 0.88f, ch1 },
            { 1.48, 0.88, 67, 0.92f, ch1 },
            { 2.40, 0.44, 65, 0.82f, ch1 },
            { 2.88, 0.94, 60, 0.90f, ch1 }
        };
    }
    if (cat == "Pad")
    {
        // Monumental minor-9th chord resolving to bVI maj7
        return {
            { 0.00, 1.95, 48, 0.84f, ch1 }, { 0.00, 1.95, 55, 0.84f, ch2 },
            { 0.00, 1.95, 58, 0.82f, ch3 }, { 0.00, 1.95, 62, 0.82f, ch4 },
            { 1.98, 1.72, 44, 0.86f, ch1 }, { 1.98, 1.72, 51, 0.86f, ch2 },
            { 1.98, 1.72, 55, 0.84f, ch3 }, { 1.98, 1.72, 60, 0.84f, ch4 }
        };
    }
    if (cat == "Pluck" || cat == "Keys")
    {
        // Fractured dark-IDM arpeggio + syncopated chord stabs
        return {
            { 0.00, 0.42, 48, 0.88f, ch1 }, { 0.00, 0.42, 55, 0.82f, ch2 }, { 0.00, 0.42, 63, 0.86f, ch3 },
            { 0.37, 0.28, 67, 0.72f, ch1 },
            { 0.68, 0.32, 70, 0.94f, ch2 },
            { 1.00, 0.42, 46, 0.86f, ch1 }, { 1.00, 0.42, 53, 0.80f, ch2 }, { 1.00, 0.42, 62, 0.86f, ch3 },
            { 1.38, 0.28, 65, 0.76f, ch1 },
            { 1.70, 0.32, 72, 0.95f, ch2 },
            { 2.00, 0.44, 44, 0.88f, ch1 }, { 2.00, 0.44, 51, 0.82f, ch2 }, { 2.00, 0.44, 60, 0.88f, ch3 },
            { 2.38, 0.28, 67, 0.78f, ch1 },
            { 2.70, 0.32, 63, 0.90f, ch2 },
            { 3.00, 0.78, 48, 0.90f, ch1 }, { 3.00, 0.78, 55, 0.86f, ch2 }, { 3.00, 0.78, 58, 0.86f, ch3 }, { 3.00, 0.78, 62, 0.86f, ch4 }
        };
    }
    // Club: festival anthem chord progression with punchy rhythmic energy
    return {
        { 0.00, 0.86, 48, 0.90f, ch1 }, { 0.00, 0.86, 55, 0.88f, ch2 }, { 0.00, 0.86, 58, 0.88f, ch3 }, { 0.00, 0.86, 62, 0.86f, ch4 },
        { 0.96, 0.86, 44, 0.90f, ch1 }, { 0.96, 0.86, 51, 0.88f, ch2 }, { 0.96, 0.86, 55, 0.88f, ch3 }, { 0.96, 0.86, 60, 0.86f, ch4 },
        { 1.92, 0.86, 41, 0.92f, ch1 }, { 1.92, 0.86, 48, 0.88f, ch2 }, { 1.92, 0.86, 51, 0.88f, ch3 }, { 1.92, 0.86, 56, 0.88f, ch4 },
        { 2.88, 0.94, 43, 0.92f, ch1 }, { 2.88, 0.94, 50, 0.88f, ch2 }, { 2.88, 0.94, 53, 0.88f, ch3 }, { 2.88, 0.94, 59, 0.88f, ch4 }
    };
}

std::vector<NoteEv> fourNoteChord110 (const FactoryPreset& p, double dur = 1.75)
{
    const bool isBass = (p.category == "Sub" || p.category == "Bass");
    const bool mpe = presetPlain (p, v2_mpe_on) > 0.5f;
    const float v110 = 110.0f / 127.0f;
    const int base = isBass ? 36 : 48;
    return {
        { 0.0, dur, base,      v110, mpe ? 2 : 1 },
        { 0.0, dur, base + 7,  v110, mpe ? 3 : 1 },
        { 0.0, dur, base + 10, v110, mpe ? 4 : 1 },
        { 0.0, dur, base + 14, v110, mpe ? 5 : 1 }
    };
}

std::vector<NoteEv> sustainedNotePhrase (const FactoryPreset& p, double dur = 1.75)
{
    const bool isBass = (p.category == "Sub" || p.category == "Bass");
    const bool mpe = presetPlain (p, v2_mpe_on) > 0.5f;
    return { { 0.0, dur, isBass ? 36 : 48, 105.0f / 127.0f, mpe ? 2 : 1 } };
}

std::vector<NoteEv> leadMelodyPhrase (const FactoryPreset& p)
{
    const bool mpe = presetPlain (p, v2_mpe_on) > 0.5f;
    const int ch = mpe ? 2 : 1;
    const float v = 100.0f / 127.0f;
    return {
        { 0.00, 0.36, 60, v, ch }, { 0.38, 0.36, 63, v, ch }, { 0.76, 0.36, 67, v, ch },
        { 1.14, 0.44, 72, v, ch }, { 1.60, 0.32, 70, v, ch }, { 1.94, 0.38, 67, v, ch }
    };
}

std::vector<NoteEv> macroEvalPhrase (const FactoryPreset& p)
{
    if (p.category == "Sub" || p.category == "Bass")
        return { { 0.00, 0.72, 36, 0.88f, 1 }, { 0.76, 0.72, 31, 0.88f, 1 } };
    if (p.category == "Lead")
    {
        const int ch = presetPlain (p, v2_mpe_on) > 0.5f ? 2 : 1;
        return { { 0.00, 0.72, 60, 0.88f, ch }, { 0.76, 0.72, 67, 0.88f, ch } };
    }
    if (p.category == "Pluck" || p.category == "Keys")
    {
        const bool mpe = presetPlain (p, v2_mpe_on) > 0.5f;
        const int c1 = mpe ? 2 : 1, c2 = mpe ? 3 : 1, c3 = mpe ? 4 : 1;
        return {
            { 0.00, 0.35, 48, 0.88f, c1 }, { 0.00, 0.35, 55, 0.84f, c2 }, { 0.00, 0.35, 63, 0.86f, c3 },
            { 0.42, 0.32, 67, 0.86f, c1 },
            { 0.80, 0.38, 46, 0.88f, c1 }, { 0.80, 0.38, 53, 0.84f, c2 }, { 0.80, 0.38, 62, 0.86f, c3 },
            { 1.22, 0.32, 70, 0.88f, c2 }
        };
    }
    return fourNoteChord110 (p, 1.50);
}

bool writeWav24 (const fs::path& path, const std::vector<float>& l, const std::vector<float>& r)
{
    fs::create_directories (path.parent_path());
    juce::File f (path.string());
    f.deleteFile();
    auto stream = std::unique_ptr<juce::OutputStream> (f.createOutputStream());
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    auto opts = juce::AudioFormatWriterOptions()
                    .withSampleRate (kSr)
                    .withNumChannels (2)
                    .withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, opts);
    if (writer == nullptr) return false;
    juce::AudioBuffer<float> buf (2, (int) l.size());
    std::copy (l.begin(), l.end(), buf.getWritePointer (0));
    std::copy (r.begin(), r.end(), buf.getWritePointer (1));
    return writer->writeFromAudioSampleBuffer (buf, 0, (int) l.size());
}

std::string sanitizeFileSlug (const juce::String& name)
{
    std::string s;
    for (auto c : name.toStdString())
    {
        if (std::isalnum ((unsigned char) c)) s.push_back (c);
        else if (c == ' ' || c == '-' || c == '_') { if (! s.empty() && s.back() != '_') s.push_back ('_'); }
    }
    while (! s.empty() && s.back() == '_') s.pop_back();
    return s.empty() ? "Preset" : s;
}

bool containsThirdPartyBrand (const juce::String& haystack, std::string& hitWord)
{
    static const char* kBlocked[] {
        "aphex", "boards of canada", "burial", "autechre", "skrillex", "deadmau5", "flume",
        "hans zimmer", "zimmer", "vangelis", "trent reznor", "nine inch", "gesaffelstein",
        "daft punk", "kraftwerk", "radiohead", "thom yorke", "four tet", "floating points",
        "bicep", "anyma", "tale of us", "amelie lens", "charlotte de witte", "eric prydz",
        "moog", "roland", "juno", "jupiter", "prophet", "sequential", "oberheim", "korg",
        "yamaha", "dx7", "rhodes", "wurlitzer", "wurli", "fender", "gibson", "nord",
        "serum", "zebra", "massive", "sylenth", "diva", "repro", "omnisphere", "pigments",
        "vital", "phase plant", "fabfilter", "arturia", "u-he", "native instruments",
        "ableton", "fl studio", "logic pro", "pro tools", "fairlight", "minimoog", "sh101",
        "sh-101", "ms20", "ms-20", "tb303", "tb-303", "jp8000", "fm8"
    };
    const auto lower = haystack.toLowerCase();
    for (const char* w : kBlocked)
    {
        if (lower.contains (w))
        {
            hitWord = w;
            return true;
        }
    }
    return false;
}

struct PresetQaSummary
{
    int idx = 0;
    std::string name, category, macros;
    double audRms = -300.0, chordRms = -300.0;
    double worstTp = -300.0, worstDc = -300.0;
    double minSubCorr = 1.0, maxMonoDrop = 0.0;
    std::array<double, 4> macDelta {};
    double mwDelta = 0.0, atDelta = 0.0;
    AudioMetrics sig;
    bool pass = true;
    std::vector<std::string> issues;
};
} // namespace

int main (int argc, char** argv)
{
    fs::path bankPath;
    fs::path auditionDir = "build/vessel2_qa/audition";
    fs::path reportPath = "build/vessel2_qa/preset_qa_report.txt";
    std::string onlyFilter;
    bool exportWav = true;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--bank" && i + 1 < argc) bankPath = argv[++i];
        else if (a == "--audition-dir" && i + 1 < argc) auditionDir = argv[++i];
        else if (a == "--report" && i + 1 < argc) reportPath = argv[++i];
        else if (a == "--only" && i + 1 < argc) onlyFilter = argv[++i];
        else if (a == "--no-wav") exportWav = false;
    }

#ifdef VESSEL_TABLES_DIR
    vessel::dsp::setTablesFolder (juce::File (VESSEL_TABLES_DIR));
#endif

    std::vector<FactoryPreset> presets;
    juce::StringArray parseErrors;
    if (! bankPath.empty())
    {
        auto xml = juce::XmlDocument::parse (juce::File (fs::absolute (bankPath).string()));
        if (xml == nullptr)
        {
            std::cerr << "ERROR: Failed to parse XML bank: " << bankPath << "\n";
            return 2;
        }
        auto parsed = PresetBank::fromXml (*xml, &parseErrors);
        presets = parsed.all();
    }
    else
    {
        parseErrors = PresetBank::factoryErrors();
        presets = PresetBank::v2Flagship().all();
    }

    bool overallPass = true;
    std::ostringstream rep;
    auto logLine = [&] (const std::string& s) { std::cout << s << "\n"; rep << s << "\n"; };

    logLine ("=== VESSEL II — Stage 6 Flagship 20-Preset Closed-Loop Audio QA ===");
    if (! parseErrors.isEmpty())
    {
        overallPass = false;
        logLine ("PARSE ERRORS (" + std::to_string (parseErrors.size()) + "):");
        for (const auto& e : parseErrors) logLine ("  - " + e.toStdString());
    }
    if (onlyFilter.empty() && presets.size() != 20)
    {
        overallPass = false;
        logLine ("FAIL: Expected exactly 20 flagship presets, found " + std::to_string (presets.size()));
    }

    std::vector<PresetQaSummary> summaries;
    summaries.reserve (presets.size());

    for (size_t i = 0; i < presets.size(); ++i)
    {
        const auto& pr = presets[i];
        if (! onlyFilter.empty() && ! pr.name.toLowerCase().contains (juce::String (onlyFilter).toLowerCase()))
            continue;

        PresetQaSummary s;
        s.idx = (int) i + 1;
        s.name = pr.name.toStdString();
        s.category = pr.category.toStdString();

        // 1. Metadata, Expressive Wiring & Zero Third-Party Name Audit
        if (pr.name.trim().isEmpty() || pr.category.trim().isEmpty()
            || pr.character.trim().isEmpty() || pr.description.trim().isEmpty())
            s.issues.push_back ("missing metadata field");

        juce::String allText = pr.name + " " + pr.category + " " + pr.character + " "
                             + pr.description + " " + pr.xyLabelX + " " + pr.xyLabelY;
        for (int m = 0; m < 4; ++m)
        {
            const juce::String mn = (m < pr.macroNames.size()) ? pr.macroNames[m].trim() : juce::String();
            if (mn.isEmpty()) s.issues.push_back ("empty Macro " + std::to_string (m + 1) + " name");
            allText += " " + mn;
            if (m > 0) s.macros += "|";
            s.macros += mn.toStdString();

            bool routed = false;
            for (int sl = 0; sl < kMacroSlots; ++sl)
                if (presetPlain (pr, macroDestPid (m, sl)) > 0.5f && std::abs (presetPlain (pr, macroAmtPid (m, sl))) > 1.0e-4f)
                    routed = true;
            if (! routed) s.issues.push_back ("Macro " + std::to_string (m + 1) + " has 0 routes");
        }
        // Check wt_table string too for zero third-party brand risk
        const int wtIdx = juce::roundToInt (presetPlain (pr, wt_table));
        const auto wtNames = dsp::tableMenuNames();
        if (wtIdx >= 0 && wtIdx < wtNames.size()) allText += " " + wtNames[wtIdx];

        std::string brandHit;
        if (containsThirdPartyBrand (allText, brandHit))
            s.issues.push_back ("third-party name found: '" + brandHit + "'");

        const bool mwRouted = (presetPlain (pr, mw_dest1) > 0.5f && std::abs (presetPlain (pr, mw_amt1)) > 1.0e-4f)
                           || (presetPlain (pr, mw_dest2) > 0.5f && std::abs (presetPlain (pr, mw_amt2)) > 1.0e-4f);
        const bool atRouted = (presetPlain (pr, at_dest1) > 0.5f && std::abs (presetPlain (pr, at_amt1)) > 1.0e-4f)
                           || (presetPlain (pr, at_dest2) > 0.5f && std::abs (presetPlain (pr, at_amt2)) > 1.0e-4f)
                           || (presetPlain (pr, v2_slide_dest1) > 0.5f && std::abs (presetPlain (pr, v2_slide_amt1)) > 1.0e-4f);
        const bool xyRouted = (presetPlain (pr, v2_xy_x_dest1) > 0.5f && std::abs (presetPlain (pr, v2_xy_x_amt1)) > 1.0e-4f)
                           && (presetPlain (pr, v2_xy_y_dest1) > 0.5f && std::abs (presetPlain (pr, v2_xy_y_amt1)) > 1.0e-4f);
        if (! mwRouted) s.issues.push_back ("MW (CC1) unrouted");
        if (! atRouted) s.issues.push_back ("AT/MPE unrouted");
        if (! xyRouted) s.issues.push_back ("XY Sculptor unrouted");

        // 2. Render 4.0-second Audition Showcase Clip (with musical Macro 1 / MW swell in second half)
        const bool mpe = presetPlain (pr, v2_mpe_on) > 0.5f;
        const int exprCh = mpe ? 2 : 1;
        auto audNotes = auditionPhraseFor (pr);
        std::vector<CcRamp> audCc {
            { 1,  0.0f, 0.55f, 1.80, 3.30, 1 },       // Mod Wheel expressive swell
            { -1, 0.0f, 0.45f, 1.20, 2.80, exprCh }   // Aftertouch bloom
        };
        std::vector<MacroRamp> audMac {
            { 0, 0.0f, 0.65f, 2.00, 3.50 }            // Hero Macro 1 live showcase lift
        };
        auto audAudio = renderPhrase (pr, 4.0, audNotes, audCc, audMac);
        auto audM = analyze (audAudio, 0.0, 3.65);
        s.audRms = audM.rmsDb;
        s.sig = audM;

        // 3. Standardized QA Renders: Sustained C2/C3, 4-note vel-110 chord, C4/C5 lead melody
        auto sustAudio = renderPhrase (pr, 2.4, sustainedNotePhrase (pr, 2.0));
        auto sustM = analyze (sustAudio, 0.35, 1.95);

        auto chordAudio = renderPhrase (pr, 2.4, fourNoteChord110 (pr, 2.0));
        auto chordM = analyze (chordAudio, 0.35, 1.95);

        auto melAudio = renderPhrase (pr, 2.4, leadMelodyPhrase (pr));
        auto melM = analyze (melAudio, 0.0, 2.2);

        // Primary standardized chord/phrase RMS (4-note vel-110 chord for polyphonic, sustained C2 for mono sub/bass, melody for mono lead)
        const bool isMono = (juce::roundToInt (presetPlain (pr, voice_mode)) != 0);
        if (pr.category == "Sub" || pr.category == "Bass")
            s.chordRms = analyze (sustAudio, 0.0, 1.8).rmsDb;
        else if (pr.category == "Lead" && isMono)
            s.chordRms = melM.rmsDb;
        else
            s.chordRms = analyze (chordAudio, 0.0, 1.8).rmsDb;

        // Track worst metrics across standardized phrases (steady-state DC measured on held sustained/chord windows)
        auto absorb = [&] (const AudioMetrics& x, bool checkMono, bool checkDc = false)
        {
            s.worstTp = std::max (s.worstTp, x.truePeakDb);
            if (checkDc) s.worstDc = std::max (s.worstDc, x.steadyDcDb);
            if (x.nonFinite > 0) s.issues.push_back ("NaN/Inf detected");
            if (checkMono)
            {
                s.minSubCorr = std::min (s.minSubCorr, x.sub120Corr);
                s.maxMonoDrop = std::max (s.maxMonoDrop, x.monoDropDb);
            }
        };
        absorb (audM, true, false);
        absorb (sustM, true, true);
        absorb (chordM, true, true);
        absorb (melM, false, false);

        // 4. Active Macro 1..4 Sweeps (0% -> 100%) + Delta Verification
        const auto mNotes = macroEvalPhrase (pr);
        auto baseM = analyze (renderPhrase (pr, 1.75, mNotes), 0.0, 1.55);

        auto measureDelta = [&] (const AudioMetrics& a, const AudioMetrics& b)
        {
            const double sd = spectralDistanceDb (a.bandsDb, b.bandsDb);
            const double cPct = (a.centroidHz > 10.0) ? (100.0 * std::abs (b.centroidHz - a.centroidHz) / a.centroidHz) : 0.0;
            const double wDiff = std::abs (b.stereoWidth - a.stereoWidth);
            const double fDiff = std::abs (b.flux - a.flux);
            // Unified acoustic delta score (normalized so >= 1.0 means clearly active & audible)
            return std::max ({ sd / 1.2, cPct / 10.0, wDiff / 0.08, fDiff / 0.30 });
        };

        for (int m = 0; m < 4; ++m)
        {
            // Sweep 0 -> 1 check for peak/NaN/DC
            auto sweepAudio = renderPhrase (pr, 1.75, mNotes, {}, { { m, 0.0f, 1.0f, 0.0, 1.55 } });
            auto sweepM = analyze (sweepAudio, 0.0, 1.55);
            absorb (sweepM, false);

            // Steady 100% vs 0% acoustic delta
            std::array<float, 4> ov { 0.0f, 0.0f, 0.0f, 0.0f };
            ov[(size_t) m] = 1.0f;
            auto fullAudio = renderPhrase (pr, 1.75, mNotes, {}, {}, ov);
            auto fullM = analyze (fullAudio, 0.0, 1.55);
            absorb (fullM, false);

            const double delta = measureDelta (baseM, fullM);
            s.macDelta[(size_t) m] = delta;
            if (delta < 1.0)
                s.issues.push_back ("M" + std::to_string (m + 1) + " weak/dead (" + fmt (delta, 2) + "x)");
        }

        // All 4 Macros + MW + AT maxed stress check on 4-note vel-110 chord
        {
            std::vector<CcRamp> maxCc { { 1, 1.0f, 1.0f, 0.0, 1.0, 1 }, { -1, 1.0f, 1.0f, 0.0, 1.0, exprCh } };
            auto stressAudio = renderPhrase (pr, 1.8, fourNoteChord110 (pr, 1.4), maxCc, {}, { 1.0f, 1.0f, 1.0f, 1.0f });
            auto stressM = analyze (stressAudio, 0.0, 1.4);
            absorb (stressM, false);
        }

        // 5. Mod Wheel (CC1) & Aftertouch / MPE Slide Expressive Sweep Verification
        {
            auto mwFull = analyze (renderPhrase (pr, 1.75, mNotes, { { 1, 1.0f, 1.0f, 0.0, 1.5, 1 } }), 0.0, 1.55);
            absorb (mwFull, false);
            s.mwDelta = measureDelta (baseM, mwFull);
            if (s.mwDelta < 0.75)
                s.issues.push_back ("MW weak (" + fmt (s.mwDelta, 2) + "x)");

            std::vector<CcRamp> atCc { { -1, 1.0f, 1.0f, 0.0, 1.5, exprCh } };
            if (mpe) atCc.push_back ({ 74, 1.0f, 1.0f, 0.0, 1.5, exprCh });
            auto atFull = analyze (renderPhrase (pr, 1.75, mNotes, atCc), 0.0, 1.55);
            absorb (atFull, false);
            s.atDelta = measureDelta (baseM, atFull);
            if (s.atDelta < 0.65)
                s.issues.push_back ("AT/MPE weak (" + fmt (s.atDelta, 2) + "x)");
        }

        // 6. Hard Stage 6 Production Assertions
        if (s.worstTp > -0.50) s.issues.push_back ("TP=" + fmt (s.worstTp, 2) + "dB > -0.5");
        if (s.audRms < -18.0 || s.audRms > -10.0) s.issues.push_back ("audRMS=" + fmt (s.audRms, 1) + " out of [-18,-10]");
        if (s.chordRms < -18.0 || s.chordRms > -10.0) s.issues.push_back ("stdRMS=" + fmt (s.chordRms, 1) + " out of [-18,-10]");
        if (s.worstDc >= -80.0) s.issues.push_back ("steadyDC=" + fmt (s.worstDc, 1) + " >= -80");
        if (s.minSubCorr < 0.50) s.issues.push_back ("sub120Corr=" + fmt (s.minSubCorr, 3) + " < 0.50");
        if (s.maxMonoDrop > 3.50) s.issues.push_back ("monoDrop=" + fmt (s.maxMonoDrop, 2) + "dB > 3.5");

        s.pass = s.issues.empty();
        if (! s.pass) overallPass = false;

        // 7. Export 4.0s 48 kHz / 24-bit Stereo WAV Audition Clip
        if (exportWav)
        {
            std::ostringstream fn;
            fn << std::setw (2) << std::setfill ('0') << s.idx << "_" << sanitizeFileSlug (pr.name) << ".wav";
            if (! writeWav24 (auditionDir / fn.str(), audAudio.l, audAudio.r))
            {
                s.issues.push_back ("WAV write failed");
                s.pass = false;
                overallPass = false;
            }
        }

        std::ostringstream row;
        row << std::setw (2) << std::setfill ('0') << s.idx << " " << std::left << std::setw (20) << std::setfill (' ') << s.name
            << " [" << std::setw (5) << s.category << "] "
            << (s.pass ? "PASS" : "FAIL")
            << " | audRMS=" << fmt (s.audRms, 1) << " stdRMS=" << fmt (s.chordRms, 1)
            << " TP=" << fmt (s.worstTp, 2) << " DC=" << fmt (s.worstDc, 1)
            << " subC=" << fmt (s.minSubCorr, 3) << " mDrop=" << fmt (s.maxMonoDrop, 2)
            << " cent=" << fmt (s.sig.centroidHz, 0) << "Hz W=" << fmt (s.sig.stereoWidth, 2)
            << " M=[" << fmt (s.macDelta[0], 1) << "," << fmt (s.macDelta[1], 1) << ","
            << fmt (s.macDelta[2], 1) << "," << fmt (s.macDelta[3], 1) << "]"
            << " MW=" << fmt (s.mwDelta, 1) << " AT=" << fmt (s.atDelta, 1);
        if (! s.issues.empty())
        {
            row << " !! ";
            for (size_t k = 0; k < s.issues.size(); ++k)
                row << (k ? "; " : "") << s.issues[k];
        }
        logLine (row.str());
        summaries.push_back (std::move (s));
    }

    // 8. Cross-Preset Timbral Diversity Matrix (zero near-duplicates)
    if (summaries.size() >= 2)
    {
        double minPairSpec = 1.0e9, minPairFeat = 1.0e9;
        std::string closestSpecPair, closestFeatPair;
        int nearDupCount = 0;

        for (size_t i = 0; i < summaries.size(); ++i)
        {
            for (size_t j = i + 1; j < summaries.size(); ++j)
            {
                const auto& a = summaries[i].sig;
                const auto& b = summaries[j].sig;
                const double sd = spectralDistanceDb (a.bandsDb, b.bandsDb);
                const double dLogC = std::log2 (std::max (20.0, a.centroidHz) / std::max (20.0, b.centroidHz));
                const double dW = (a.stereoWidth - b.stereoWidth) * 3.5;
                const double dEnv = (a.envContrastDb - b.envContrastDb) * 0.45;
                const double dCr = (a.crestDb - b.crestDb) * 0.25;
                const double dLo = (a.lowRatio - b.lowRatio) * 3.5;
                const double dHi = (a.highRatio - b.highRatio) * 3.5;
                const double fd = std::sqrt (dLogC * dLogC + dW * dW + dEnv * dEnv + dCr * dCr + dLo * dLo + dHi * dHi);

                if (sd < minPairSpec)
                {
                    minPairSpec = sd;
                    closestSpecPair = summaries[i].name + " <-> " + summaries[j].name;
                }
                if (fd < minPairFeat)
                {
                    minPairFeat = fd;
                    closestFeatPair = summaries[i].name + " <-> " + summaries[j].name;
                }
                if (sd < 1.8 && fd < 0.85)
                {
                    ++nearDupCount;
                    overallPass = false;
                    logLine ("NEAR-DUPLICATE: " + summaries[i].name + " <-> " + summaries[j].name
                             + " (specDist=" + fmt (sd, 2) + "dB, featDist=" + fmt (fd, 2) + ")");
                }
            }
        }
        logLine ("------------------------------------------------------------------");
        logLine ("Timbral Diversity (" + std::to_string (summaries.size() * (summaries.size() - 1) / 2)
                 + " pairs): minSpecDist=" + fmt (minPairSpec, 2) + "dB (" + closestSpecPair
                 + "), minFeatDist=" + fmt (minPairFeat, 2) + " (" + closestFeatPair
                 + "), nearDups=" + std::to_string (nearDupCount) + " [" + (nearDupCount == 0 ? "OK" : "FAIL") + "]");
    }

    int nPass = 0, nFail = 0;
    for (const auto& s : summaries) (s.pass ? ++nPass : ++nFail);
    logLine ("TOTAL: PASS=" + std::to_string (nPass) + "/" + std::to_string (summaries.size())
             + " FAIL=" + std::to_string (nFail) + " OVERALL=" + (overallPass ? "PASS" : "FAIL"));

    if (! reportPath.empty())
    {
        fs::create_directories (reportPath.parent_path());
        std::ofstream f (reportPath);
        f << rep.str();
    }

    return overallPass ? 0 : 1;
}
