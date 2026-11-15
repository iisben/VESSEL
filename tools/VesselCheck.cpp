// Offline preset QA.
//
//   VesselCheck check [outDir]            render every factory preset, report loudness/peaks/expression, write WAVs + UI PNGs
//   VesselCheck ui [outDir]               only the UI PNGs (stages, mapping, expression deck, preset browser, Cinematic page)
//   VesselCheck calibrate <bank.xml> [names]  loudness-match the presets in a bank file to the target (rewrites out_gain)
//   VesselCheck audit <bank.xml> <outDir> [names|category]
//                                         the same gates on a bank *file* (no rebuild), plus character measurements
//                                         (low end, top end, width, tail, density) and a labelled spectrogram sheet
//
// Loudness is ITU-R BS.1770-4 integrated (K-weighting, 400 ms blocks, 75 % overlap, -70 LUFS absolute and
// -10 LU relative gates). True peak is measured on a 4x linear-phase upsampled copy (BS.1770 Annex 2).
// Each category is auditioned with a phrase that suits it (a bass line for basses, held chords for pads...).
// Expression check: velocity, mod wheel and aftertouch must each change the *timbre* (level-normalised 1/3-octave
// spectral shape, or its movement over time), not just the volume.
// Macro check: every factory preset names and routes all eight macros; each one, swept 0 -> 1, must change the timbre
// by at least 1 dB and move the loudness by no more than 6 LU, and all eight at full with full expression must not clip.
#include "dsp/Tables.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Presets.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>

using namespace vessel;

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 256;
constexpr double kTargetLufs = -16.0;
constexpr double kTol = 0.5;

//==============================================================================
struct Biquad
{
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double process (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

/** BS.1770-4 integrated loudness of a stereo 48 kHz signal. */
double integratedLufs (const std::vector<float>& l, const std::vector<float>& r)
{
    std::vector<double> msL, msR;
    for (int c = 0; c < 2; ++c)
    {
        Biquad shelf { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 };
        Biquad hp    { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };
        const auto& x = c == 0 ? l : r;
        auto& ms = c == 0 ? msL : msR;
        std::vector<double> sq (x.size());
        for (size_t i = 0; i < x.size(); ++i) { const double y = hp.process (shelf.process (x[i])); sq[i] = y * y; }
        const size_t blk = (size_t) (0.4 * kSr), hop = (size_t) (0.1 * kSr);
        for (size_t s = 0; s + blk <= sq.size(); s += hop)
        {
            double a = 0.0;
            for (size_t i = s; i < s + blk; ++i) a += sq[i];
            ms.push_back (a / (double) blk);
        }
    }
    auto loud = [] (double z) { return -0.691 + 10.0 * std::log10 (z + 1e-30); };
    double sum = 0.0; int count = 0;
    for (size_t j = 0; j < msL.size(); ++j) if (loud (msL[j] + msR[j]) > -70.0) { sum += msL[j] + msR[j]; ++count; }
    if (count == 0) return -120.0;
    const double gateRel = loud (sum / count) - 10.0;
    sum = 0.0; count = 0;
    for (size_t j = 0; j < msL.size(); ++j)
    {
        const double z = msL[j] + msR[j];
        if (loud (z) > -70.0 && loud (z) > gateRel) { sum += z; ++count; }
    }
    return count > 0 ? loud (sum / count) : -120.0;
}

double samplePeakDb (const std::vector<float>& l, const std::vector<float>& r)
{
    float p = 0.0f;
    for (size_t i = 0; i < l.size(); ++i) p = std::max ({ p, std::abs (l[i]), std::abs (r[i]) });
    return juce::Decibels::gainToDecibels ((double) p, -150.0);
}

double truePeakDb (const std::vector<float>& l, const std::vector<float>& r)
{
    juce::dsp::Oversampling<float> os (2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
    os.initProcessing (4096);
    float peak = 0.0f;
    std::vector<float> a (4096), b (4096);
    for (size_t pos = 0; pos < l.size(); pos += 4096)
    {
        const size_t n = std::min<size_t> (4096, l.size() - pos);
        std::copy_n (l.data() + pos, n, a.data()); std::copy_n (r.data() + pos, n, b.data());
        float* ch[2] { a.data(), b.data() };
        juce::dsp::AudioBlock<float> blk (ch, 2, n);
        auto up = os.processSamplesUp (blk);
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < up.getNumSamples(); ++i) peak = std::max (peak, std::abs (up.getSample (c, (int) i)));
    }
    return juce::Decibels::gainToDecibels ((double) peak, -150.0);
}

/** Level-normalised 1/3-octave band levels (dB) of the long-term spectrum, plus each band's movement over time. */
struct Spectrum { std::vector<double> shape, motion, flux; };

Spectrum analyse (const std::vector<float>& l, const std::vector<float>& r, size_t from)
{
    constexpr int order = 12, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> centres;
    for (double f = 31.5; f < 16000.0; f *= std::pow (2.0, 1.0 / 3.0)) centres.push_back (f);
    std::vector<std::vector<double>> frames;
    std::vector<float> buf ((size_t) N * 2);
    for (size_t s = from; s + N <= l.size(); s += N / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (N - 1));
            buf[(size_t) i] = (float) (0.5 * (l[s + (size_t) i] + r[s + (size_t) i]) * w);
        }
        fft.performFrequencyOnlyForwardTransform (buf.data());
        std::vector<double> bands (centres.size(), 1e-20);
        for (int k = 1; k < N / 2; ++k)
        {
            const double f = k * kSr / N;
            for (size_t b = 0; b < centres.size(); ++b)
                if (f >= centres[b] / std::pow (2.0, 1.0 / 6.0) && f < centres[b] * std::pow (2.0, 1.0 / 6.0)) { bands[b] += (double) buf[(size_t) k] * buf[(size_t) k]; break; }
        }
        frames.push_back (bands);
    }
    Spectrum sp;
    sp.shape.assign (centres.size(), 0.0);
    sp.motion.assign (centres.size(), 0.0);
    if (frames.empty()) return sp;
    double total = 0.0;
    std::vector<double> mean (centres.size(), 0.0);
    for (auto& fr : frames) for (size_t b = 0; b < fr.size(); ++b) { mean[b] += fr[b]; total += fr[b]; }
    for (size_t b = 0; b < mean.size(); ++b) sp.shape[b] = 10.0 * std::log10 (mean[b] / total + 1e-20);
    for (size_t b = 0; b < mean.size(); ++b)
    {
        double acc = 0.0;
        const double m = 10.0 * std::log10 (mean[b] / (double) frames.size() + 1e-20);
        for (auto& fr : frames) { const double d = 10.0 * std::log10 (fr[b] + 1e-20) - m; acc += d * d; }
        sp.motion[b] = std::sqrt (acc / (double) frames.size());
    }
    // flux: the mean frame-to-frame change per band (how fast it moves, which `motion` cannot see: a 0.1 Hz drift and a
    // 5 Hz throb of the same depth have the same spread). Each step is capped at 6 dB so note starts and ends cannot
    // swamp it, and levels are floored 80 dB under the loudest band so the noise floor between notes cannot count.
    sp.flux.assign (centres.size(), 0.0);
    double peak = 1.0e-20;
    for (auto& fr : frames) for (double v : fr) peak = std::max (peak, v);
    const double floorDb = 10.0 * std::log10 (peak) - 80.0;
    if (frames.size() > 1)
        for (size_t b = 0; b < centres.size(); ++b)
        {
            double acc = 0.0;
            for (size_t t = 1; t < frames.size(); ++t)
            {
                const double d = std::max (floorDb, 10.0 * std::log10 (frames[t][b] + 1e-20)) - std::max (floorDb, 10.0 * std::log10 (frames[t - 1][b] + 1e-20));
                acc += std::min (std::abs (d), 6.0);
            }
            sp.flux[b] = acc / (double) (frames.size() - 1);
        }
    return sp;
}

/** Band-weighted RMS distance in dB, counting only bands within 40 dB of the loudest (ignores the noise floor). */
double distance (const std::vector<double>& a, const std::vector<double>& b, const std::vector<double>& ref)
{
    const double top = *std::max_element (ref.begin(), ref.end());
    double acc = 0.0; int n = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (ref[i] > top - 40.0) { const double d = std::clamp (a[i] - b[i], -24.0, 24.0); acc += d * d; ++n; }
    return n ? std::sqrt (acc / n) : 0.0;
}

//==============================================================================
struct Note { double at, len; int note; float vel; };

/** Audition phrase per category (seconds, notes). */
std::vector<Note> phraseFor (const juce::String& category, float vel)
{
    if (category == "Sub" || category == "Bass")
        return { { 0.0, 0.9, 33, vel }, { 1.0, 0.45, 33, vel }, { 1.5, 0.45, 36, vel }, { 2.0, 1.8, 28, vel },
                 { 4.0, 0.9, 31, vel }, { 5.0, 0.45, 33, vel }, { 5.5, 1.5, 26, vel } };
    if (category == "Lead")
        return { { 0.0, 0.8, 72, vel }, { 0.9, 0.4, 75, vel }, { 1.4, 1.2, 79, vel }, { 2.8, 0.4, 77, vel },
                 { 3.3, 0.4, 75, vel }, { 3.8, 2.2, 72, vel } };
    if (category == "Keys" || category == "Pluck")
        return { { 0.0, 0.5, 60, vel }, { 0.0, 0.5, 63, vel }, { 0.0, 0.5, 67, vel }, { 0.75, 0.3, 70, vel },
                 { 1.25, 0.5, 58, vel }, { 1.25, 0.5, 62, vel }, { 1.25, 0.5, 65, vel }, { 2.0, 0.3, 74, vel },
                 { 2.5, 0.5, 56, vel }, { 2.5, 0.5, 60, vel }, { 2.5, 0.5, 63, vel }, { 3.25, 0.3, 67, vel },
                 { 3.75, 1.5, 55, vel }, { 3.75, 1.5, 58, vel }, { 3.75, 1.5, 62, vel } };
    if (category == "Cinematic")
        // a low power chord held, a darker one under it, then 2.2 s of tail: braams, swells and hits all need room
        return { { 0.0, 3.0, 36, vel }, { 0.0, 3.0, 43, vel }, { 0.0, 3.0, 48, vel },
                 { 3.3, 2.5, 32, vel }, { 3.3, 2.5, 39, vel }, { 3.3, 2.5, 44, vel } };
    // Pad, Drone, Texture, FX, Init: a held minor chord, then a second voicing
    return { { 0.0, 3.4, 48, vel }, { 0.0, 3.4, 55, vel }, { 0.0, 3.4, 63, vel },
             { 3.6, 2.6, 46, vel }, { 3.6, 2.6, 53, vel }, { 3.6, 2.6, 62, vel } };
}

struct RenderOpts { float mw = 0.0f, at = 0.0f; bool rampExpr = false; double seconds = 8.0; };

std::pair<std::vector<float>, std::vector<float>> render (VesselProcessor& p, const std::vector<Note>& notes, const RenderOpts& o)
{
    const int total = (int) (o.seconds * kSr);
    std::vector<float> L ((size_t) total), R ((size_t) total);
    struct Ev { int at; juce::MidiMessage m; };
    std::vector<Ev> evs;
    for (auto& n : notes)
    {
        evs.push_back ({ (int) (n.at * kSr), juce::MidiMessage::noteOn (1, n.note, n.vel) });
        evs.push_back ({ (int) ((n.at + n.len) * kSr), juce::MidiMessage::noteOff (1, n.note) });
    }
    juce::AudioBuffer<float> buf (2, kBlock);
    for (int pos = 0; pos < total; pos += kBlock)
    {
        const int n = std::min (kBlock, total - pos);
        juce::MidiBuffer midi;
        const float t = o.rampExpr ? std::min (1.0f, (float) pos / (float) (total / 2)) : 1.0f;
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, juce::roundToInt (127.0f * o.mw * t)), 0);
        midi.addEvent (juce::MidiMessage::channelPressureChange (1, juce::roundToInt (127.0f * o.at * t)), 0);
        for (auto& e : evs) if (e.at >= pos && e.at < pos + n) midi.addEvent (e.m, e.at - pos);
        buf.setSize (2, n, false, false, true);
        buf.clear();
        p.processBlock (buf, midi);
        std::copy_n (buf.getReadPointer (0), n, L.data() + pos);
        std::copy_n (buf.getReadPointer (1), n, R.data() + pos);
    }
    return { L, R };
}

std::unique_ptr<VesselProcessor> fresh (const FactoryPreset& preset, float outGainOverride = -1000.0f, const float* macros = nullptr)
{
    auto p = std::make_unique<VesselProcessor>();
    p->setPlayConfigDetails (2, 2, kSr, kBlock);
    p->applyPreset (preset);
    if (outGainOverride > -999.0f) p->param (out_gain)->setValueNotifyingHost (p->param (out_gain)->convertTo0to1 (outGainOverride));
    if (macros != nullptr)
        for (int m = 0; m < kNumMacros; ++m) p->param (macroPid (m))->setValueNotifyingHost (macros[m]);
    p->prepareToPlay (kSr, kBlock);   // smoothers and bypass fades start at the preset's values
    juce::Thread::sleep (180);        // the convolution engine loads its IR on a background thread
    return p;
}

float presetValue (const FactoryPreset& p, int pid)
{
    float v = spec (pid).def;
    for (auto [id, x] : p.values) if (id == pid) v = x;
    return v;
}

struct Report
{
    double lufs = 0, tp = 0, sp = 0, stressSp = 0, stressTp = 0;
    double vel = 0, mw = 0, at = 0;          // timbre change, dB
    double lowSide = 0;                       // side energy below 120 Hz, relative to the whole programme (mid)
    bool finite = true;
    std::array<double, kNumMacros> macro {};    // timbre change 0 -> 1, dB (-1: not routed)
    std::array<double, kNumMacros> macroLu {};  // loudness change 0 -> 1, LU
    double macroStressSp = -200;               // all macros at 1 + full expression, sample peak
    int unnamed = 0, unrouted = 0;
};

Report measure (const FactoryPreset& preset, float outGain, bool full)
{
    Report rep;
    auto p = fresh (preset, outGain);
    auto [L, R] = render (*p, phraseFor (preset.category, 0.75f), {});
    for (size_t i = 0; i < L.size(); ++i) if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) rep.finite = false;
    rep.lufs = integratedLufs (L, R);
    rep.sp = samplePeakDb (L, R);
    if (! full) return rep;
    rep.tp = truePeakDb (L, R);

    // low-end mono safety: how much of the programme lives in the side channel below 120 Hz, i.e. what a mono
    // fold-down (club PA, phone speaker, vinyl cut) would cancel. Referenced to the whole programme, so a lead with
    // no low end is not judged on a ratio of two near-silent numbers.
    {
        dsp::Svf c, d; for (auto* f : { &c, &d }) f->setup (120.0f, 0.707f, (float) kSr);
        double em = 0, es = 0;
        for (size_t i = 0; i < L.size(); ++i)
        {
            const float m = 0.5f * (L[i] + R[i]), s = d.lowpass (c.lowpass (0.5f * (L[i] - R[i])));
            em += (double) m * m; es += (double) s * s;
        }
        rep.lowSide = 10.0 * std::log10 ((es + 1e-30) / (em + 1e-30));
    }

    // stress: full velocity, mod wheel and aftertouch swept to maximum
    {
        auto q = fresh (preset, outGain);
        auto [l2, r2] = render (*q, phraseFor (preset.category, 1.0f), { 1.0f, 1.0f, true });
        rep.stressSp = samplePeakDb (l2, r2);
        rep.stressTp = truePeakDb (l2, r2);
        for (size_t i = 0; i < l2.size(); ++i) if (! std::isfinite (l2[i])) rep.finite = false;
    }

    // expression: timbre (shape) and movement distance between the extremes of each source
    auto timbre = [&] (float velA, float velB, RenderOpts a, RenderOpts b)
    {
        auto p1 = fresh (preset, outGain); auto x = render (*p1, phraseFor (preset.category, velA), a);
        auto p2 = fresh (preset, outGain); auto y = render (*p2, phraseFor (preset.category, velB), b);
        const auto sa = analyse (x.first, x.second, (size_t) (0.2 * kSr));
        const auto sb = analyse (y.first, y.second, (size_t) (0.2 * kSr));
        return std::max ({ distance (sa.shape, sb.shape, sa.shape), distance (sa.motion, sb.motion, sa.shape), distance (sa.flux, sb.flux, sa.shape) });
    };
    rep.vel = timbre (0.25f, 1.0f, {}, {});
    rep.mw  = timbre (0.75f, 0.75f, { 0.0f, 0.0f }, { 1.0f, 0.0f });
    rep.at  = timbre (0.75f, 0.75f, { 0.0f, 0.0f }, { 0.0f, 1.0f });

    // macros: each swept 0 -> 1 against all at 0
    {
        const float zero[kNumMacros] {};
        auto p0 = fresh (preset, outGain, zero);
        auto base = render (*p0, phraseFor (preset.category, 0.75f), {});
        const auto sBase = analyse (base.first, base.second, (size_t) (0.2 * kSr));
        const double luBase = integratedLufs (base.first, base.second);
        for (int m = 0; m < kNumMacros; ++m)
        {
            bool routed = false;
            for (int s = 0; s < kMacroSlots; ++s) routed |= presetValue (preset, macroDestPid (m, s)) > 0.5f && std::abs (presetValue (preset, macroAmtPid (m, s))) > 1.0e-3f;
            if (preset.macroNames[m].isEmpty()) ++rep.unnamed;
            if (! routed) { ++rep.unrouted; rep.macro[(size_t) m] = -1; continue; }
            float one[kNumMacros] {};
            one[m] = 1.0f;
            auto pm = fresh (preset, outGain, one);
            auto y = render (*pm, phraseFor (preset.category, 0.75f), {});
            const auto sy = analyse (y.first, y.second, (size_t) (0.2 * kSr));
            rep.macro[(size_t) m] = std::max ({ distance (sBase.shape, sy.shape, sBase.shape), distance (sBase.motion, sy.motion, sBase.shape), distance (sBase.flux, sy.flux, sBase.shape) });
            rep.macroLu[(size_t) m] = integratedLufs (y.first, y.second) - luBase;
            for (size_t i = 0; i < y.first.size(); ++i) if (! std::isfinite (y.first[i])) rep.finite = false;
        }
        float all[kNumMacros];
        std::fill (all, all + kNumMacros, 1.0f);
        auto pa = fresh (preset, outGain, all);
        auto z = render (*pa, phraseFor (preset.category, 1.0f), { 1.0f, 1.0f, true });
        rep.macroStressSp = samplePeakDb (z.first, z.second);
        for (size_t i = 0; i < z.first.size(); ++i) if (! std::isfinite (z.first[i])) rep.finite = false;
    }
    return rep;
}

void writeWav (const juce::File& f, const std::vector<float>& L, const std::vector<float>& R)
{
    f.deleteFile();
    if (auto os = f.createOutputStream())
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (std::move (os));
        const auto opts = juce::AudioFormatWriterOptions{}.withSampleRate (kSr).withNumChannels (2).withBitsPerSample (24);
        if (auto w = wav.createWriterFor (out, opts))
        {
            const float* ch[2] { L.data(), R.data() };
            w->writeFromFloatArrays (ch, 2, (int) L.size());
        }
    }
}

//==============================================================================
void savePng (const juce::Image& img, const juce::File& f)
{
    f.deleteFile();
    if (auto os = f.createOutputStream()) juce::PNGImageFormat().writeImageToStream (img, *os);
}

/** UI snapshots (rendered offscreen from the editor component, never the screen): the five stage pages,
    the expression deck, a drag in progress with a fresh route, and the preset browser. */
void snapshots (const juce::File& outDir)
{
    VesselProcessor proc;
    proc.prepareToPlay (kSr, kBlock);
    proc.setCurrentProgram (1);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto* ve = dynamic_cast<VesselEditor*> (ed.get());
    auto shot = [&] (const juce::String& name) { savePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), outDir.getChildFile (name + ".png")); };
    ve->setTorch (false);   // every page shot fully lit; the torch has its own shots below
    for (int s = 0; s < 5; ++s) { ve->showStage (s); shot ("stage" + juce::String (s)); }

    // SOURCE with an imported table playing and the FM operator on
    {
        auto setPlain = [&] (int pid, float v) { proc.param (pid)->setValueNotifyingHost (proc.param (pid)->convertTo0to1 (v)); };
        int monks = 0;
        for (int i = 0; i < vessel::dsp::kNumTables; ++i) if (juce::String (vessel::dsp::kTables[i].name) == "Formant Bot 01") monks = i + 1;
        setPlain (wt_table, (float) monks); setPlain (wt_morph, 0.4f); setPlain (wt_pw, 0.5f);
        setPlain (fm_on, 1); setPlain (fm_ratio, 5); setPlain (fm_index, 0.35f); setPlain (fm_env, 0.5f); setPlain (fm_feedback, 0.2f);
        proc.prepareToPlay (kSr, kBlock);   // loads the table now (in the plug-in the background loader does it)
        ve->showStage (0);
        shot ("stage0_fm");
        setPlain (res_on, 1); setPlain (res_model, 7);   // the gong's partials on the SHAPE stage's ruler
        ve->showStage (2);
        shot ("stage2_gong");
        proc.setCurrentProgram (1);
        proc.prepareToPlay (kSr, kBlock);
    }

    // mapping: a drag in progress (every destination ringed) and a route just made from LFO 2
    ve->showStage (2);
    ve->setDropPreview (true);
    int slot = -1;
    const auto res = vessel::ui::routing::add (proc, { vessel::ui::ModSource::Lfo, 1 }, res_decay, slot);
    ve->routeChanged ({ vessel::ui::ModSource::Lfo, 1 }, slot, res, res_decay);
    shot ("mapping");
    ve->setDropPreview (false);
    vessel::ui::routing::remove (proc, { vessel::ui::ModSource::Lfo, 1 }, res_decay);

    ve->selectModPage (3);
    shot ("expression");
    ve->selectModPage (0);
    shot ("lfo");
    ve->selectModPage (4);   // MACROS, the deck's default page
    shot ("macros");

    ve->showBrowser (true);
    shot ("browser");
    ve->showBrowser (false);

    // the torch: the panel in the dark, the beam low on the SHAPE stage and then far up on the header and SOURCE
    ve->showStage (2);
    ve->setTorch (true);
    ve->aimTorch (juce::Point<float> { 600.0f, 470.0f });
    shot ("torch");
    ve->aimTorch (juce::Point<float> { 330.0f, 150.0f });
    shot ("torch_far");
    {   // the cost of a moving beam: each frame repaints the union of the old and new beam areas
        const int n = 40;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < n; ++i)
        {
            const juce::Point<float> p { 300.0f + 15.0f * (float) i, 300.0f + 4.0f * (float) i };
            ve->aimTorch (p);
            const auto r = juce::Rectangle<float> (p.x - 300.0f, p.y - 250.0f, 600.0f, 500.0f).getIntersection (ed->getLocalBounds().toFloat()).toNearestInt();
            (void) ed->createComponentSnapshot (r, true, 1.0f);
        }
        std::cout << "torch: software repaint of a moving beam area " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / n, 2) << " ms per frame" << std::endl;
    }
    ve->aimTorch (std::nullopt);
    ve->setTorch (false);

    // the largest category, where the grid switches to four columns
    const auto& all = PresetBank::factory().all();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].category == "Cinematic") { proc.setCurrentProgram ((int) i); break; }
    ve->showBrowser (true);
    shot ("browser_cinematic");
    ve->showBrowser (false);
    ed.reset();
}

//==============================================================================
/** What a cinematic sound is judged on besides the gates: how much lives in the low end and the top, how wide the
    stereo field is above the mono-collapsed lows, how much space rings on after the notes, and how dense it is. */
struct Character { double sub = -99, hf = -99, width = -99, tail = -99, plr = 0; };

Character character (const std::vector<float>& L, const std::vector<float>& R, double tailFrom, double lufs, double tp)
{
    Character c;
    constexpr int order = 12, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> bm ((size_t) N * 2), bs ((size_t) N * 2);
    double eSub = 0, eHf = 0, eAll = 0, eMidHi = 0, eSideHi = 0;
    for (size_t s = 0; s + N <= L.size(); s += N / 2)
    {
        std::fill (bm.begin(), bm.end(), 0.0f); std::fill (bs.begin(), bs.end(), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (N - 1));
            bm[(size_t) i] = (float) (0.5 * (L[s + (size_t) i] + R[s + (size_t) i]) * w);
            bs[(size_t) i] = (float) (0.5 * (L[s + (size_t) i] - R[s + (size_t) i]) * w);
        }
        fft.performFrequencyOnlyForwardTransform (bm.data());
        fft.performFrequencyOnlyForwardTransform (bs.data());
        for (int k = 1; k < N / 2; ++k)
        {
            const double f = k * kSr / N, m = (double) bm[(size_t) k] * bm[(size_t) k], sd = (double) bs[(size_t) k] * bs[(size_t) k];
            eAll += m + sd;
            if (f < 100.0) eSub += m + sd;
            if (f > 6000.0) eHf += m + sd;
            if (f > 300.0) { eMidHi += m; eSideHi += sd; }
        }
    }
    c.sub = 10.0 * std::log10 (eSub / (eAll + 1e-30) + 1e-12);
    c.hf = 10.0 * std::log10 (eHf / (eAll + 1e-30) + 1e-12);
    c.width = 10.0 * std::log10 (eSideHi / (eMidHi + 1e-30) + 1e-12);
    // tail: the last second against the loudest second, in dB (closer to 0 = more space ringing on)
    auto rmsDb = [&] (size_t a, size_t b)
    {
        double e = 0; for (size_t i = a; i < b && i < L.size(); ++i) e += (double) L[i] * L[i] + (double) R[i] * R[i];
        return 10.0 * std::log10 (e / (double) std::max<size_t> (1, b - a) + 1e-20);
    };
    double loudest = -200;
    for (size_t a = 0; a + (size_t) kSr <= (size_t) (tailFrom * kSr); a += (size_t) (kSr / 10)) loudest = std::max (loudest, rmsDb (a, a + (size_t) kSr));
    c.tail = rmsDb (L.size() - (size_t) kSr, L.size()) - loudest;
    c.plr = tp - lufs;
    return c;
}

/** One labelled tile: mid-channel spectrogram (log frequency), the level envelope and the stereo width over time. */
juce::Image spectrogramTile (const juce::String& title, const juce::String& line2, const std::vector<float>& L, const std::vector<float>& R)
{
    constexpr int W = 316, H = 196, top = 34, specH = 118, graphH = 36;
    juce::Image img (juce::Image::RGB, W, H, true);
    juce::Graphics g (img);
    g.fillAll (juce::Colour (0xff0b0a10));
    constexpr int order = 12, N = 1 << order, hop = 512;
    juce::dsp::FFT fft (order);
    const int frames = (int) ((L.size() - N) / hop) + 1;
    std::vector<std::vector<float>> spec ((size_t) frames, std::vector<float> ((size_t) specH, -200.0f));
    std::vector<float> lev ((size_t) frames), wid ((size_t) frames);
    std::vector<float> bm ((size_t) N * 2), bs ((size_t) N * 2);
    float maxDb = -200;
    for (int fr = 0; fr < frames; ++fr)
    {
        const size_t s = (size_t) fr * hop;
        std::fill (bm.begin(), bm.end(), 0.0f); std::fill (bs.begin(), bs.end(), 0.0f);
        double e = 0;
        for (int i = 0; i < N; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (N - 1));
            bm[(size_t) i] = 0.5f * (L[s + (size_t) i] + R[s + (size_t) i]) * w;
            bs[(size_t) i] = 0.5f * (L[s + (size_t) i] - R[s + (size_t) i]) * w;
            e += (double) L[s + (size_t) i] * L[s + (size_t) i] + (double) R[s + (size_t) i] * R[s + (size_t) i];
        }
        lev[(size_t) fr] = (float) (10.0 * std::log10 (e / N + 1e-20));
        fft.performFrequencyOnlyForwardTransform (bm.data());
        fft.performFrequencyOnlyForwardTransform (bs.data());
        double hm = 0, hs = 0;
        for (int k = 1; k < N / 2; ++k) if (k * kSr / N > 300.0) { hm += (double) bm[(size_t) k] * bm[(size_t) k]; hs += (double) bs[(size_t) k] * bs[(size_t) k]; }
        wid[(size_t) fr] = (float) (10.0 * std::log10 (hs / (hm + 1e-30) + 1e-12));
        for (int y = 0; y < specH; ++y)
        {
            const double f0 = 25.0 * std::pow (800.0, (double) y / specH), f1 = 25.0 * std::pow (800.0, (double) (y + 1) / specH);
            const int k0 = std::max (1, (int) (f0 * N / kSr)), k1 = std::max (k0 + 1, (int) (f1 * N / kSr));
            double acc = 0; for (int k = k0; k < k1 && k < N / 2; ++k) acc = std::max (acc, (double) bm[(size_t) k]);
            const float db = (float) (20.0 * std::log10 (acc + 1e-12));
            spec[(size_t) fr][(size_t) y] = db; maxDb = std::max (maxDb, db);
        }
    }
    auto colourFor = [] (float t)   // 0..1: black, deep violet, magenta, amber, white
    {
        const juce::Colour stops[] { juce::Colour (0xff0b0a10), juce::Colour (0xff2a1250), juce::Colour (0xff8a2a8a),
                                     juce::Colour (0xffe0702a), juce::Colour (0xfffff4d0) };
        t = juce::jlimit (0.0f, 1.0f, t) * 4.0f;
        const int i = std::min (3, (int) t);
        return stops[i].interpolatedWith (stops[i + 1], t - (float) i);
    };
    for (int x = 0; x < W; ++x)
    {
        const int a = x * frames / W, b = std::max (a + 1, (x + 1) * frames / W);
        for (int y = 0; y < specH; ++y)
        {
            float v = -200; for (int fr = a; fr < b && fr < frames; ++fr) v = std::max (v, spec[(size_t) fr][(size_t) y]);
            img.setPixelAt (x, top + specH - 1 - y, colourFor ((v - (maxDb - 80.0f)) / 80.0f));
        }
    }
    // frequency guides: 100 Hz, 1 kHz, 10 kHz
    g.setColour (juce::Colours::white.withAlpha (0.18f));
    for (double f : { 100.0, 1000.0, 10000.0 })
    {
        const float y = (float) (top + specH) - (float) (std::log (f / 25.0) / std::log (800.0) * specH);
        g.drawHorizontalLine ((int) y, 0.0f, (float) W);
    }
    // level (white, -60..0 dB under its peak) and width (green, -30..0 dB side/mid above 300 Hz)
    const float gy = (float) (top + specH + 4);
    g.setColour (juce::Colour (0xff1a1822)); g.fillRect (0.0f, gy, (float) W, (float) graphH);
    const float levMax = *std::max_element (lev.begin(), lev.end());
    juce::Path pl, pw;
    for (int x = 0; x < W; ++x)
    {
        const int fr = std::min (frames - 1, x * frames / W);
        const float yl = gy + graphH * (1.0f - juce::jlimit (0.0f, 1.0f, (lev[(size_t) fr] - levMax + 60.0f) / 60.0f));
        const float yw = gy + graphH * (1.0f - juce::jlimit (0.0f, 1.0f, (wid[(size_t) fr] + 30.0f) / 30.0f));
        if (x == 0) { pl.startNewSubPath ((float) x, yl); pw.startNewSubPath ((float) x, yw); }
        else { pl.lineTo ((float) x, yl); pw.lineTo ((float) x, yw); }
    }
    g.setColour (juce::Colour (0xff6fe0a0)); g.strokePath (pw, juce::PathStrokeType (1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.85f)); g.strokePath (pl, juce::PathStrokeType (1.2f));
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.drawText (title, 4, 1, W - 8, 16, juce::Justification::centredLeft);
    g.setColour (juce::Colours::white.withAlpha (0.7f));
    g.setFont (juce::Font (juce::FontOptions (10.5f)));
    g.drawText (line2, 4, 17, W - 8, 14, juce::Justification::centredLeft);
    return img;
}

void savePng (const juce::Image& img, const juce::File& f);

int checkBank (const PresetBank& bank, const juce::StringArray& bankErrors, const juce::File& outDir,
               const juce::StringArray& only, bool fromFile)
{
    outDir.createDirectory();
    int failures = 0;
    for (auto& e : bankErrors) { std::cout << "BANK ERROR (value ignored by the plug-in): " << e << "\n"; ++failures; }
    std::cout << "preset                     category   LUFS    TP     stress-TP  vel   MW    AT    lowSide  mac-min mac-LU mac-SP  result"
              << (fromFile ? "      sub    hf  width  tail   PLR" : "") << "\n";
    std::vector<double> loud;
    std::vector<juce::Image> tiles;
    for (int i = 0; i < bank.size(); ++i)
    {
        const auto& pr = bank.all()[(size_t) i];
        if (! only.isEmpty() && ! only.contains (pr.name) && ! only.contains (pr.category)) continue;
        const auto rep = measure (pr, presetValue (pr, out_gain), true);
        juce::StringArray why;
        const bool isInit = pr.category == "Init";
        if (! rep.finite) why.add ("NaN");
        if (std::abs (rep.lufs - kTargetLufs) > 1.0) why.add ("loudness");
        if (rep.tp > -0.5) why.add ("true-peak");
        if (rep.stressSp > 0.0) why.add ("stress clips 0 dBFS");
        if (! isInit && (rep.vel < 1.0 || rep.mw < 1.0 || rep.at < 1.0)) why.add ("expression");
        if (rep.lowSide > -30.0) why.add ("low-end side");
        double macMin = 99.0, macLu = 0.0;
        for (int m = 0; m < kNumMacros; ++m)
            if (rep.macro[(size_t) m] >= 0.0)
            {
                macMin = std::min (macMin, rep.macro[(size_t) m]);
                if (std::abs (rep.macroLu[(size_t) m]) > std::abs (macLu)) macLu = rep.macroLu[(size_t) m];
            }
        if (! isInit)
        {
            if (rep.unnamed > 0 || rep.unrouted > 0) why.add (juce::String (rep.unnamed) + " unnamed / " + juce::String (rep.unrouted) + " unrouted macros");
            if (macMin < 1.0) why.add ("macro timbre");
            if (std::abs (macLu) > 6.0) why.add ("macro level jump");
        }
        if (rep.macroStressSp > 0.0) why.add ("macros clip 0 dBFS");
        failures += why.isEmpty() ? 0 : 1;
        loud.push_back (rep.lufs);
        std::cout << juce::String (pr.name).paddedRight (' ', 27).substring (0, 27) << juce::String (pr.category).paddedRight (' ', 10)
                  << juce::String (rep.lufs, 1).paddedLeft (' ', 6) << juce::String (rep.tp, 1).paddedLeft (' ', 7)
                  << juce::String (rep.stressTp, 1).paddedLeft (' ', 9)
                  << juce::String (rep.vel, 1).paddedLeft (' ', 7) << juce::String (rep.mw, 1).paddedLeft (' ', 6) << juce::String (rep.at, 1).paddedLeft (' ', 6)
                  << juce::String (juce::jmax (-99.9, rep.lowSide), 1).paddedLeft (' ', 9)
                  << juce::String (macMin > 98.0 ? 0.0 : macMin, 1).paddedLeft (' ', 9) << juce::String (macLu, 1).paddedLeft (' ', 7)
                  << juce::String (rep.macroStressSp, 1).paddedLeft (' ', 7) << "   " << (why.isEmpty() ? juce::String (fromFile ? "ok  " : "ok") : "FAIL " + why.joinIntoString (", "));

        auto p = fresh (pr, presetValue (pr, out_gain));
        auto [L, R] = render (*p, phraseFor (pr.category, 0.75f), {});
        writeWav (outDir.getChildFile (juce::String (i).paddedLeft ('0', 2) + " " + pr.name + ".wav"), L, R);
        if (fromFile)
        {
            double end = 0; for (auto& n : phraseFor (pr.category, 0.75f)) end = std::max (end, n.at + n.len);
            const auto c = character (L, R, end, rep.lufs, rep.tp);
            std::cout << juce::String (c.sub, 1).paddedLeft (' ', 9) << juce::String (c.hf, 1).paddedLeft (' ', 6)
                      << juce::String (c.width, 1).paddedLeft (' ', 7) << juce::String (c.tail, 1).paddedLeft (' ', 6) << juce::String (c.plr, 1).paddedLeft (' ', 6);
            std::cout << "   macros";   // timbre change / loudness change of each macro, for tuning
            for (int m = 0; m < kNumMacros; ++m)
                std::cout << " " << juce::String (rep.macro[(size_t) m], 1) << "/" << juce::String (rep.macroLu[(size_t) m], 1);
            tiles.push_back (spectrogramTile (pr.name, "sub " + juce::String (c.sub, 1) + "  hf " + juce::String (c.hf, 1) + "  width " + juce::String (c.width, 1)
                                                       + "  tail " + juce::String (c.tail, 1) + "  PLR " + juce::String (c.plr, 1)
                                                       + "  v/m/a " + juce::String (rep.vel, 1) + "/" + juce::String (rep.mw, 1) + "/" + juce::String (rep.at, 1), L, R));
        }
        std::cout << "\n";
    }
    if (! tiles.empty())
    {
        const int cols = 5, rows = ((int) tiles.size() + cols - 1) / cols;
        const int tw = tiles[0].getWidth() + 4, th = tiles[0].getHeight() + 4;
        juce::Image sheet (juce::Image::RGB, cols * tw, rows * th, true);
        juce::Graphics g (sheet);
        g.fillAll (juce::Colour (0xff000000));
        for (size_t t = 0; t < tiles.size(); ++t) g.drawImageAt (tiles[t], (int) (t % cols) * tw + 2, (int) (t / cols) * th + 2);
        savePng (sheet, outDir.getChildFile ("sheet.png"));
    }
    if (loud.empty()) { std::cout << "no preset matched\n"; return 1; }
    const auto [mn, mx] = std::minmax_element (loud.begin(), loud.end());
    std::cout << "\n" << (int) loud.size() << " presets, loudness spread " << juce::String (*mn, 1) << " .. " << juce::String (*mx, 1) << " LUFS\n";

    if (only.isEmpty() && ! fromFile) snapshots (outDir);

    std::cout << (failures == 0 ? "ALL OK" : juce::String (failures) + " PRESET(S) FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}

int check (const juce::File& outDir, const juce::StringArray& only = {})
{
    return checkBank (PresetBank::factory(), PresetBank::factoryErrors(), outDir, only, false);
}

int audit (const juce::File& file, const juce::File& outDir, const juce::StringArray& only)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr) { std::cerr << "cannot parse " << file.getFullPathName() << "\n"; return 2; }
    juce::StringArray errors;
    auto bank = PresetBank::fromXml (*xml, &errors);
    return checkBank (bank, errors, outDir, only, true);
}

/** Iteratively sets each preset's out_gain so integrated loudness lands on the target, then rewrites the file. */
int calibrate (const juce::File& file, const juce::StringArray& only = {})
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr) { std::cerr << "cannot parse " << file.getFullPathName() << "\n"; return 2; }
    juce::StringArray errors;
    auto bank = PresetBank::fromXml (*xml, &errors);
    for (auto& e : errors) std::cerr << "bad value: " << e << "\n";
    if (! errors.isEmpty()) return 2;

    int i = 0;
    for (auto* e : xml->getChildWithTagNameIterator ("Preset"))
    {
        const auto& pr = bank.all()[(size_t) i++];
        if (! only.isEmpty() && ! only.contains (pr.name) && ! only.contains (pr.category)) continue;
        float g = presetValue (pr, out_gain);
        double lufs = 0.0;
        for (int it = 0; it < 6; ++it)
        {
            lufs = measure (pr, g, false).lufs;
            if (std::abs (lufs - kTargetLufs) < 0.15) break;
            g = juce::jlimit (-24.0f, 12.0f, g + (float) (kTargetLufs - lufs));
        }
        g = std::round (g * 10.0f) / 10.0f;
        juce::XmlElement* gainEl = nullptr;
        for (auto* v : e->getChildWithTagNameIterator ("P")) if (v->getStringAttribute ("id") == "out_gain") gainEl = v;
        if (gainEl == nullptr) { gainEl = e->createNewChildElement ("P"); gainEl->setAttribute ("id", "out_gain"); }
        gainEl->setAttribute ("v", juce::String (g, 1));
        std::cout << juce::String (pr.name).paddedRight (' ', 28) << " out_gain " << juce::String (g, 1).paddedLeft (' ', 6)
                  << " dB  ->  " << juce::String (lufs, 2) << " LUFS" << (std::abs (lufs - kTargetLufs) > kTol ? "   (did not converge)" : "") << "\n";
    }
    juce::XmlElement::TextFormat fmt;
    fmt.lineWrapLength = 1000;
    if (! xml->writeTo (file, fmt)) { std::cerr << "cannot write\n"; return 2; }
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    vessel::dsp::setTablesFolder (juce::File (VESSEL_TABLES_DIR));   // no bundle: read the source tree's tables
    const juce::String mode = argc > 1 ? juce::String (argv[1]) : "check";
    auto names = [&] (int idx) { return argc > idx ? juce::StringArray::fromTokens (juce::String (argv[idx]), ",", "") : juce::StringArray(); };
    if (mode == "calibrate" && argc > 2) return calibrate (juce::File (juce::String (argv[2])), names (3));
    if (mode == "audit" && argc > 3) return audit (juce::File (juce::String (argv[2])), juce::File (juce::String (argv[3])), names (4));
    const juce::File outDir (argc > 2 ? juce::String (argv[2]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    if (mode == "ui") { snapshots (outDir); return 0; }
    // optional third argument: a comma-separated list of preset names (or categories) to check instead of the whole bank
    return check (outDir, names (3));
}
