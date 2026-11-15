// DSP lab: objective measurements used by the audit (run before and after every DSP change).
//   zipper   – HF artefact energy while parameters are automated hard
//   toggles  – click energy when modules are switched on/off mid-note
//   alias    – inharmonic (aliased) energy of steady tones through the nonlinear stages
//   cpu      – per-stage cost on a dense 8-voice patch
//   res      – physical-model resonator: tuning, decay accuracy, stability under maximum drive
//   lfo      – LFO movement options: ranges, retrigger, one-shot hold, fade-in, smoothing
//   truepeak – the output stage's true-peak ceiling: inter-sample peaks with and without it, transparency, latency, cost
#include "dsp/Tables.h"
#include "PluginProcessor.h"
#include <iostream>

using namespace vessel;

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 256;

void setP (VesselProcessor& p, int pid, float plain)
{
    auto* prm = p.param (pid);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
}

/** Resets to defaults and switches off every colouring/noisy/time-varying stage, so what remains is measurable. */
void clean (VesselProcessor& p)
{
    for (int i = 0; i < NUM_PARAMS; ++i) p.param (i)->setValueNotifyingHost (p.param (i)->getDefaultValue());
    for (int pid : { sub_on, drone_on, noise_on, tube_on, vhs_on, flt_on, unmask_on, z_on, verb_on, mono_on, duck_on, clip_on })
        setP (p, pid, 0.0f);
    setP (p, wt_drift, 0.0f); setP (p, wt_pwm, 0.0f); setP (p, wt_morph, 0.0f);
    setP (p, amp_a, 1.0f); setP (p, amp_s, 1.0f); setP (p, amp_r, 50.0f);
    setP (p, tape_mode, 0.0f);
}

struct Render
{
    std::vector<float> l, r;
};

/** Renders `seconds`; `perBlock(blockIndex)` may change parameters between blocks. */
template <typename F>
Render run (VesselProcessor& p, double seconds, int note, F&& perBlock, float vel = 0.8f)
{
    const int total = (int) (seconds * kSr);
    Render out; out.l.resize ((size_t) total); out.r.resize ((size_t) total);
    juce::AudioBuffer<float> buf (2, kBlock);
    for (int pos = 0, b = 0; pos < total; pos += kBlock, ++b)
    {
        const int n = std::min (kBlock, total - pos);
        perBlock (b);
        juce::MidiBuffer midi;
        if (pos == 0 && note >= 0) midi.addEvent (juce::MidiMessage::noteOn (1, note, vel), 0);
        buf.setSize (2, n, false, false, true);
        buf.clear();
        p.processBlock (buf, midi);
        std::copy_n (buf.getReadPointer (0), n, out.l.data() + pos);
        std::copy_n (buf.getReadPointer (1), n, out.r.data() + pos);
    }
    return out;
}

std::unique_ptr<VesselProcessor> make()
{
    auto p = std::make_unique<VesselProcessor>();
    p->setPlayConfigDetails (2, 2, kSr, kBlock);
    p->prepareToPlay (kSr, kBlock);
    return p;
}

/** Energy above `hz` (2nd-order SVF high-pass, run twice) relative to total energy, in dB. */
double hfRatioDb (const std::vector<float>& x, size_t from, float hz)
{
    dsp::Svf a, b; a.setup (hz, 0.707f, (float) kSr); b.setup (hz, 0.707f, (float) kSr);
    double e = 0.0, t = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const float h = b.highpass (a.highpass (x[i]));
        if (i >= from) { e += (double) h * h; t += (double) x[i] * x[i]; }
    }
    return 10.0 * std::log10 ((e + 1e-30) / (t + 1e-30));
}

/** Inharmonic energy / harmonic energy, Blackman-Harris windowed 64k FFT on the tail of the render. */
double aliasDb (const std::vector<float>& x, float f0)
{
    constexpr int order = 16, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) N * 2, 0.0f);
    const size_t start = x.size() - (size_t) N;
    for (int i = 0; i < N; ++i)
    {
        const double t = (double) i / (double) (N - 1);
        const double w = 0.35875 - 0.48829 * std::cos (2 * M_PI * t) + 0.14128 * std::cos (4 * M_PI * t) - 0.01168 * std::cos (6 * M_PI * t);
        buf[(size_t) i] = (float) (x[start + (size_t) i] * w);
    }
    fft.performFrequencyOnlyForwardTransform (buf.data());
    const double binHz = kSr / N;
    double harm = 0.0, inh = 0.0;
    for (int k = 1; k < N / 2; ++k)
    {
        const double f = k * binHz;
        if (f < 20.0 || f > 20000.0) continue;   // only what can be heard
        const double m = (double) buf[(size_t) k] * buf[(size_t) k];
        const double nearest = std::round (f / f0) * f0;
        if (nearest > 0.0 && std::abs (f - nearest) <= 6.0 * binHz) harm += m; else inh += m;
    }
    return 10.0 * std::log10 ((inh + 1e-30) / (harm + 1e-30));
}

void zipper()
{
    std::cout << "\n[zipper] HF artefact energy (> 3 kHz) in excess of the static patch, relative to signal, lower is better\n";
    struct Case { const char* name; int pid; float a, b; };
    const Case cases[] {
        { "Output gain  -12 <-> 0 dB", out_gain, -12.0f, 0.0f },
        { "WT level     0.2 <-> 0.9",  wt_level, 0.2f, 0.9f },
        { "Sub 1 level  0.2 <-> 1.0",  sub1_level, 0.2f, 1.0f },
        { "Tilt         -9 <-> +9 dB", pre_tilt, -9.0f, 9.0f },
        { "Ladder A cut 1k <-> 4k",    flt_a_cut, 1000.0f, 4000.0f },
        { "Formant vowel A <-> U",     form_vowel, 0.0f, 4.0f },
        { "Hole mix     0 <-> 0.6",    verb_mix, 0.0f, 0.6f },
        { "Duck depth   0 <-> 1",      duck_depth, 0.0f, 1.0f },
    };
    for (auto& c : cases)
    {
        auto p = make();
        clean (*p);
        setP (*p, wt_morph, 0.0f);   // sine frame: harmonically empty above the fundamental
        if (c.pid == sub1_level) { setP (*p, sub_on, 1); setP (*p, wt_on, 0); }
        if (c.pid == flt_a_cut)  { setP (*p, flt_on, 1); setP (*p, flt_route, 2); setP (*p, flt_env, 0); setP (*p, flt_key, 0); setP (*p, flt_a_res, 0.2f); }
        if (c.pid == form_vowel) { setP (*p, form_on, 1); setP (*p, form_mix, 1.0f); }
        if (c.pid == verb_mix)   { setP (*p, verb_on, 1); setP (*p, verb_low, 20.0f); }
        if (c.pid == duck_depth) { setP (*p, drone_on, 1); setP (*p, duck_on, 1); setP (*p, duck_mode, 0); setP (*p, sub_on, 1); setP (*p, drone_tone, 0.0f); setP (*p, drone_motion, 0); }
        const int note = c.pid == sub1_level ? 57 : 45;   // 110 Hz body / 27.5 Hz-55 Hz sub
        // Reference: the same patch held static at each end. Whatever HF the moving version has beyond the louder
        // static end is artefact (zipper / stepping), not the filter's own legitimate output.
        auto staticHf = [&] (float v)
        {
            auto q = make();
            for (int i = 0; i < NUM_PARAMS; ++i) q->param (i)->setValueNotifyingHost (p->param (i)->getValue());
            setP (*q, c.pid, v);
            auto rr = run (*q, 3.0, note, [] (int) {});
            return hfRatioDb (rr.l, (size_t) (0.3 * kSr), 3000.0f);
        };
        const double ref = std::max (staticHf (c.a), staticHf (c.b));
        // flip every 9 blocks (48 ms): a fast automation lane
        auto r = run (*p, 3.0, note, [&] (int b) { if (b > 20) setP (*p, c.pid, ((b / 9) & 1) ? c.b : c.a); });
        const double moving = hfRatioDb (r.l, (size_t) (0.3 * kSr), 3000.0f);
        const double excess = 10.0 * std::log10 (std::max (1e-12, std::pow (10.0, moving / 10.0) - std::pow (10.0, ref / 10.0)));
        std::cout << "  " << juce::String (c.name).paddedRight (' ', 28) << juce::String (excess, 1) << " dB"
                  << "   (moving " << juce::String (moving, 1) << ", static " << juce::String (ref, 1) << ")\n";
    }
}

void toggles()
{
    std::cout << "\n[toggles] click energy (> 3 kHz) when a module is switched on/off every 120 ms on a 110 Hz sine, lower is better.\n"
                 "  'held on' is the same patch with the module left on: a toggle figure close to it means no click of its own\n";
    const int mods[] { crush_on, bias_on, vhs_on, form_on, envf_on, unmask_on, ott_on, gran_on, exc_on, tube_on, verb_on, res_on };
    auto setup = [] (VesselProcessor& q)
    {
        clean (q);
        setP (q, crush_bits, 16); setP (q, crush_rate, 48000); setP (q, crush_post, 0.0f);
        setP (q, vhs_wow, 0); setP (q, vhs_flutter, 0); setP (q, vhs_drift, 0); setP (q, vhs_age, 0);
        setP (q, form_mix, 0.3f); setP (q, envf_depth, 0); setP (q, envf_cut, 18000); setP (q, envf_mix, 1.0f);
        setP (q, tube_drive, 0.0f); setP (q, tube_bias, 0.0f); setP (q, verb_mix, 0.15f);
        setP (q, gran_mix, 0.3f); setP (q, exc_amount, 0.2f); setP (q, bias_sat, 0.1f); setP (q, bias_sag, 0); setP (q, bias_amount, 0);
        setP (q, res_mix, 0.3f); setP (q, res_strike, 0.0f);
    };
    for (int pid : mods)
    {
        auto p = make();
        setup (*p);
        auto r = run (*p, 3.0, 45, [&] (int b) { if (b > 20) setP (*p, pid, ((b / 22) & 1) ? 1.0f : 0.0f); });
        auto q = make();
        setup (*q);
        setP (*q, pid, 1.0f);
        auto h = run (*q, 3.0, 45, [] (int) {});
        std::cout << "  " << juce::String (spec (pid).name).paddedRight (' ', 28) << juce::String (hfRatioDb (r.l, (size_t) (0.3 * kSr), 3000.0f), 1)
                  << " dB   (held on " << juce::String (hfRatioDb (h.l, (size_t) (0.3 * kSr), 3000.0f), 1) << ")\n";
    }
}

void alias()
{
    std::cout << "\n[alias] inharmonic energy 20 Hz-20 kHz relative to the harmonics, lower is better\n";
    struct Case { const char* name; int note; std::function<void (VesselProcessor&)> setup; };
    const Case cases[] {
        { "WT saw  C7  pw 50%",            96, [] (VesselProcessor& p) { setP (p, wt_morph, 2.0f / 7.0f); } },
        { "WT saw  C7  pw 15% (warp)",     96, [] (VesselProcessor& p) { setP (p, wt_morph, 2.0f / 7.0f); setP (p, wt_pw, 0.15f); } },
        { "WT sqr  C6  pw 25% (warp)",     84, [] (VesselProcessor& p) { setP (p, wt_morph, 3.0f / 7.0f); setP (p, wt_pw, 0.25f); } },
        { "WT grit C6  morph sweep pos",   84, [] (VesselProcessor& p) { setP (p, wt_morph, 0.93f); } },
        { "Ladder drive 100% C6 saw",      84, [] (VesselProcessor& p) { setP (p, wt_morph, 2.0f / 7.0f); setP (p, flt_on, 1); setP (p, flt_route, 2); setP (p, flt_a_cut, 6000); setP (p, flt_a_res, 0.3f); setP (p, flt_drive, 1.0f); setP (p, flt_env, 0); setP (p, flt_key, 0); } },
        { "Tube triode 100% sine C6",      84, [] (VesselProcessor& p) { setP (p, tube_on, 1); setP (p, tube_drive, 1.0f); setP (p, tube_mode, 0); } },
        { "Tube starved 80% sine C6",      84, [] (VesselProcessor& p) { setP (p, tube_on, 1); setP (p, tube_drive, 0.8f); setP (p, tube_mode, 2); } },
        { "Tape bias sat 100% sine C6",    84, [] (VesselProcessor& p) { setP (p, bias_on, 1); setP (p, bias_sat, 1.0f); setP (p, bias_sag, 0); setP (p, wt_level, 1.0f); } },
        { "Exciter 100% sine 4.4 kHz",    109, [] (VesselProcessor& p) { setP (p, exc_on, 1); setP (p, exc_amount, 1.0f); setP (p, exc_harm, 1.0f); setP (p, exc_freq, 3000); } },
        { "Clipper +12 dB sine C6",        84, [] (VesselProcessor& p) { setP (p, clip_on, 1); setP (p, out_gain, 12.0f); setP (p, wt_level, 1.0f); } },
    };
    for (auto& c : cases)
    {
        auto p = make();
        clean (*p);
        c.setup (*p);
        auto r = run (*p, 2.0, c.note, [] (int) {});
        const float f0 = 440.0f * std::exp2 ((float) (c.note - 69) / 12.0f);
        std::cout << "  " << juce::String (c.name).paddedRight (' ', 30) << juce::String (aliasDb (r.l, f0), 1) << " dB\n";
    }
}

/** Renders a host-input sine through the effect chain (no notes); returns output RMS in dB. */
double sineThrough (VesselProcessor& p, float hz, double seconds = 1.0)
{
    const int total = (int) (seconds * kSr);
    juce::AudioBuffer<float> buf (2, kBlock);
    double sum = 0.0; int count = 0; double ph = 0.0;
    for (int pos = 0; pos < total; pos += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float v = 0.1f * (float) std::sin (ph);
            ph += 2.0 * M_PI * hz / kSr;
            buf.setSample (0, i, v); buf.setSample (1, i, v);
        }
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
        if (pos > total / 2)
            for (int i = 0; i < kBlock; ++i) { sum += (double) buf.getSample (0, i) * buf.getSample (0, i); ++count; }
    }
    return 10.0 * std::log10 (sum / count + 1e-30);
}

void coherence()
{
    std::cout << "\n[phase] parallel-path coherence, 0 dB is ideal\n";
    // (a) tube dry/wet: 50% blend vs 0% blend, drive 0 (near-linear). A phase mismatch between the dry and the
    //     oversampled wet path shows as comb dips that deepen toward the top of the band.
    std::cout << "  Tube mix 50% vs 0%, drive 0:";
    for (float hz : { 100.0f, 1000.0f, 5000.0f, 10000.0f, 15000.0f, 18000.0f })
    {
        auto lvl = [&] (float mix)
        {
            auto p = make(); clean (*p);
            setP (*p, in_level, 1.0f); setP (*p, tube_on, 1); setP (*p, tube_drive, 0.0f); setP (*p, tube_bias, 0.0f); setP (*p, tube_mix, mix);
            return sineThrough (*p, hz);
        };
        std::cout << "  " << (hz >= 1000 ? juce::String (hz / 1000.0f, 0) + "k" : juce::String ((int) hz)) << " " << juce::String (lvl (0.5f) - lvl (0.0f), 2);
    }
    std::cout << " dB\n";

    // (b) sub send: half the sub goes through RUIN, half bypasses it clean. Any delay mismatch between the two
    //     halves cancels low end. Compare total sub level at 50% send vs 0% send.
    std::cout << "  Sub send 50% vs 0% (tube on, drive 0):";
    for (int note : { 45, 57, 69 })
    {
        auto lvl = [&] (float send)
        {
            auto p = make(); clean (*p);
            setP (*p, sub_on, 1); setP (*p, wt_on, 0); setP (*p, sub_weight, 0.0f); setP (*p, sub2_level, 0.0f);
            setP (*p, tube_on, 1); setP (*p, tube_drive, 0.0f); setP (*p, tube_bias, 0.0f); setP (*p, sub_send, send);
            auto r = run (*p, 2.0, note, [] (int) {});
            double sum = 0.0; for (size_t i = r.l.size() / 2; i < r.l.size(); ++i) sum += (double) r.l[i] * r.l[i];
            return 10.0 * std::log10 (sum + 1e-30);
        };
        const float hz = 440.0f * std::exp2 ((float) (note - 69) / 12.0f) * 0.5f;
        std::cout << "  " << juce::String (hz, 0) << " Hz " << juce::String (lvl (0.5f) - lvl (0.0f), 3);
    }
    std::cout << " dB\n";
}

void lowend()
{
    std::cout << "\n[low end] Init patch, low chord: side energy below 120 Hz relative to mid below 120 Hz (mono safety)\n";
    for (bool monoOn : { true, false })
    {
        auto p = make();
        for (int i = 0; i < NUM_PARAMS; ++i) p->param (i)->setValueNotifyingHost (p->param (i)->getDefaultValue());
        setP (*p, mono_on, monoOn ? 1.0f : 0.0f);
        const int total = (int) (kSr * 4.0);
        juce::AudioBuffer<float> buf (2, kBlock);
        dsp::Svf lm1, lm2, ls1, ls2;
        for (auto* f : { &lm1, &lm2, &ls1, &ls2 }) f->setup (120.0f, 0.707f, (float) kSr);
        double em = 0.0, es = 0.0;
        for (int pos = 0; pos < total; pos += kBlock)
        {
            juce::MidiBuffer midi;
            if (pos == 0) for (int nn : { 33, 40, 45 }) midi.addEvent (juce::MidiMessage::noteOn (1, nn, 0.9f), 0);
            buf.clear();
            p->processBlock (buf, midi);
            for (int i = 0; i < kBlock; ++i)
            {
                const float l = buf.getSample (0, i), r = buf.getSample (1, i);
                const float m = lm2.lowpass (lm1.lowpass (0.5f * (l + r))), sd = ls2.lowpass (ls1.lowpass (0.5f * (l - r)));
                if (pos > total / 4) { em += (double) m * m; es += (double) sd * sd; }
            }
        }
        std::cout << "  mono collapse " << (monoOn ? "on " : "off") << "   " << juce::String (10.0 * std::log10 ((es + 1e-30) / (em + 1e-30)), 1) << " dB\n";
    }
}

void cpu()
{
    std::cout << "\n[cpu] dense patch: 8 voices held, every module on. % of one core in real time at 48 kHz\n";
    auto setupDense = [] (VesselProcessor& p)
    {
        for (int i = 0; i < NUM_PARAMS; ++i) p.param (i)->setValueNotifyingHost (p.param (i)->getDefaultValue());
        for (int pid : { gran_on, bias_on, crush_on, envf_on, form_on, conv_on, haas_on, ott_on, exc_on, drone_hold, res_on })
            setP (p, pid, 1.0f);
        setP (p, tape_mode, 3); setP (p, crush_rate, 30000); setP (p, crush_bits, 14);
    };
    auto measure = [&] (int offPid) -> double
    {
        auto p = make();
        setupDense (*p);
        if (offPid >= 0) setP (*p, offPid, 0.0f);
        juce::Thread::sleep (200);
        const int total = (int) (kSr * 6.0);
        juce::AudioBuffer<float> buf (2, kBlock);
        double ms = 0.0;
        for (int pos = 0; pos < total; pos += kBlock)
        {
            juce::MidiBuffer midi;
            if (pos == 0) for (int k = 0; k < 8; ++k) midi.addEvent (juce::MidiMessage::noteOn (1, 36 + k * 5, 0.8f), k);
            buf.clear();
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            p->processBlock (buf, midi);
            ms += juce::Time::getMillisecondCounterHiRes() - t0;
        }
        return ms / 6000.0 * 100.0;
    };
    const double all = measure (-1);
    std::cout << "  total                         " << juce::String (all, 2) << " %\n";
    const int stages[] { wt_on, sub_on, flt_on, drone_on, noise_on, gran_on, tube_on, bias_on, crush_on, vhs_on, envf_on, unmask_on, form_on, res_on, z_on, conv_on, verb_on, haas_on, ott_on, mono_on, exc_on };
    for (int pid : stages)
    {
        const double without = measure (pid);
        std::cout << "  " << juce::String (spec (pid).name).paddedRight (' ', 30) << juce::String (all - without, 2) << " %\n";
    }
}
/** Amplitude of the component at `hz` in x[from, from+len) (Goertzel, Hann window). */
double goertzel (const std::vector<float>& x, size_t from, size_t len, double hz)
{
    const double w = 2.0 * M_PI * hz / kSr, c = 2.0 * std::cos (w);
    double s1 = 0, s2 = 0, wsum = 0;
    for (size_t i = 0; i < len; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * M_PI * (double) i / (double) (len - 1));
        const double s0 = x[from + i] * win + c * s1 - s2;
        s2 = s1; s1 = s0; wsum += win;
    }
    return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0 / wsum;
}

/** Strongest spectral peak within ±12 % of `hz`, parabolic-interpolated (64k FFT). */
double peakNear (const std::vector<float>& x, size_t from, double hz)
{
    constexpr int order = 16, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) N * 2, 0.0f);
    for (int i = 0; i < N && from + (size_t) i < x.size(); ++i)
        buf[(size_t) i] = (float) (x[from + (size_t) i] * (0.5 - 0.5 * std::cos (2 * M_PI * i / (N - 1))));
    fft.performFrequencyOnlyForwardTransform (buf.data());
    const double bin = kSr / N;
    const int lo = (int) (hz * 0.88 / bin), hi = (int) (hz * 1.12 / bin);
    int best = lo;
    for (int k = lo; k <= hi; ++k) if (buf[(size_t) k] > buf[(size_t) best]) best = k;
    const double a = std::log (buf[(size_t) best - 1] + 1e-20), b = std::log (buf[(size_t) best] + 1e-20), c = std::log (buf[(size_t) best + 1] + 1e-20);
    const double off = 0.5 * (a - c) / (a - 2 * b + c);
    return (best + off) * bin;
}

void resonatorLab()
{
    std::cout << "\n[res] physical-model resonator, struck at 0.8 velocity, no input; tuning in cents, T60 of the fundamental\n";
    const char* names[] { "String", "Tube", "Bell", "Bar", "Membrane", "Plate", "Bowl", "Gong" };
    const int notes[] { 33, 45, 57, 69, 81 };
    for (int model = 0; model < 8; ++model)
    {
        std::cout << "  " << juce::String (names[model]).paddedRight (' ', 10);
        double worst = 0.0, t60err = 0.0, peak = 0.0;
        for (int note : notes)
        {
            dsp::Resonator r;
            r.prepare ((float) kSr);
            dsp::Resonator::Params rp;
            rp.on = true; rp.model = model; rp.decay = 1.5f; rp.bright = 0.6f; rp.stiff = 0.0f; rp.pos = 0.13f;
            rp.input = 0.0f; rp.strike = 1.0f; rp.mix = 1.0f;
            r.model = model;
            r.noteOn (note, 0.8f);
            const int total = (int) (kSr * 2.5);
            std::vector<float> L ((size_t) total), R ((size_t) total);
            for (int pos = 0; pos < total; pos += 64)
            {
                const int n = std::min (64, total - pos);
                std::fill_n (L.data() + pos, n, 0.0f); std::fill_n (R.data() + pos, n, 0.0f);
                r.process (L.data() + pos, R.data() + pos, n, rp);
            }
            std::vector<float> mono ((size_t) total);
            for (int i = 0; i < total; ++i) { mono[(size_t) i] = 0.5f * (L[(size_t) i] + R[(size_t) i]); peak = std::max (peak, (double) std::abs (L[(size_t) i])); }
            // the fundamental the model should ring at (bell: the prime; tube: the bore's fundamental)
            double f0 = 440.0 * std::pow (2.0, (note - 69) / 12.0);
            while (f0 < 20.0) f0 *= 2.0;
            const double est = peakNear (mono, (size_t) (0.05 * kSr), f0);
            const double cents = 1200.0 * std::log2 (est / f0);
            if (std::abs (cents) > std::abs (worst)) worst = cents;
            // T60 from the fundamental's decay between 0.2 s and 1.2 s
            const size_t win = (size_t) (0.1 * kSr);
            const double a0 = goertzel (mono, (size_t) (0.2 * kSr), win, est), a1 = goertzel (mono, (size_t) (1.2 * kSr), win, est);
            const double t60 = 60.0 / (20.0 * std::log10 (a0 / std::max (a1, 1e-12)));
            const double target = (model == 2) ? 1.5 : 1.5;   // bell prime: ratio 1 → nominal decay
            t60err = std::max (t60err, std::abs (t60 / target - 1.0));
            if (std::getenv ("RES_DEBUG")) std::cout << "\n     note " << note << " f " << est << " t60 " << t60;
        }
        std::cout << "worst tuning " << juce::String (worst, 2).paddedLeft (' ', 6) << " ct   worst T60 error "
                  << juce::String (t60err * 100.0, 1).paddedLeft (' ', 5) << " %   peak " << juce::String (20.0 * std::log10 (peak + 1e-12), 1) << " dBFS\n";
    }

    // the new models' signatures: the bowl's doublet beat and the gong's bloom
    auto strikeModel = [] (int model, int note, float vel, float stiff, double seconds)
    {
        dsp::Resonator r; r.prepare ((float) kSr);
        dsp::Resonator::Params rp;
        rp.on = true; rp.model = model; rp.decay = 4.0f; rp.bright = 0.6f; rp.stiff = stiff; rp.pos = 0.13f;
        rp.input = 0.0f; rp.strike = 1.0f; rp.mix = 1.0f;
        r.model = model;
        r.noteOn (note, vel);
        const int total = (int) (kSr * seconds);
        std::vector<float> L ((size_t) total), R ((size_t) total), mono ((size_t) total);
        for (int pos = 0; pos < total; pos += 64)
        {
            const int n = std::min (64, total - pos);
            std::fill_n (L.data() + pos, n, 0.0f); std::fill_n (R.data() + pos, n, 0.0f);
            r.process (L.data() + pos, R.data() + pos, n, rp);
        }
        for (int i = 0; i < total; ++i) mono[(size_t) i] = 0.5f * (L[(size_t) i] + R[(size_t) i]);
        return mono;
    };
    for (float stiff : { 0.15f, 0.6f })
    {
        const auto x = strikeModel (6, 57, 0.8f, stiff, 6.0);
        const double f0 = 220.0;
        const size_t win = (size_t) (0.05 * kSr);
        std::vector<double> env;
        for (size_t at = (size_t) (0.3 * kSr); at + win < x.size(); at += win / 2) env.push_back (goertzel (x, at, win, f0));
        // detrend the envelope in dB (the decay is a straight line there), then count dips with 1 dB hysteresis
        std::vector<double> db (env.size());
        for (size_t i = 0; i < env.size(); ++i) db[i] = 20.0 * std::log10 (env[i] + 1e-12);
        double sx = 0, sy = 0, sxx = 0, sxy = 0; const double N = (double) db.size();
        for (size_t i = 0; i < db.size(); ++i) { sx += (double) i; sy += db[i]; sxx += (double) i * i; sxy += (double) i * db[i]; }
        const double slope = (N * sxy - sx * sy) / (N * sxx - sx * sx), icpt = (sy - slope * sx) / N;
        int crossings = 0; bool falling = false; double ext = 0.0, swingSum = 0.0, lastMax = 0.0;
        for (size_t i = 0; i < db.size(); ++i)
        {
            const double v = db[i] - (icpt + slope * (double) i);
            if (i == 0) { ext = lastMax = v; continue; }
            if (! falling) { if (v > ext) ext = v; else if (v < ext - 1.0) { falling = true; lastMax = ext; ext = v; } }
            else { if (v < ext) ext = v; else if (v > ext + 1.0) { falling = false; ++crossings; swingSum += lastMax - ext; ext = v; } }
        }
        const double depth = crossings > 0 ? swingSum / crossings : 0.0;
        std::cout << "  Bowl stiff " << juce::String (stiff, 2) << ": fundamental beats " << crossings << " times in "
                  << juce::String ((double) env.size() * 0.025, 1) << " s (" << juce::String ((double) crossings / ((double) env.size() * 0.025), 2)
                  << " Hz, expected " << juce::String (f0 * (0.0015 + 0.012 * stiff), 2) << "), average swing " << juce::String (depth, 1) << " dB\n";
    }
    for (float vel : { 1.0f, 0.3f })
    {
        const auto x = strikeModel (7, 45, vel, 0.15f, 1.5);
        // the upper modes' band (above 2.2 x f0, past the first three modes) in 50 ms windows: a bloom peaks after the hit
        dsp::Svf a, b; a.setup (110.0f * 2.2f, 0.707f, (float) kSr); b.setup (110.0f * 2.2f, 0.707f, (float) kSr);
        const size_t win = (size_t) (0.05 * kSr);
        std::vector<double> band (x.size() / win, 0.0), all (x.size() / win, 0.0);
        for (size_t i = 0; i < band.size() * win; ++i)
        {
            const float h = b.highpass (a.highpass (x[i]));
            band[i / win] += (double) h * h; all[i / win] += (double) x[i] * x[i];
        }
        size_t pk = 0; for (size_t w = 0; w < band.size(); ++w) if (band[w] > band[pk]) pk = w;
        double mx = 0.0; for (double v : all) mx = std::max (mx, v);
        std::cout << "  Gong vel " << juce::String (vel, 1) << ": upper band peaks at " << (int) (pk * 50) << "-" << (int) (pk * 50 + 50) << " ms, "
                  << juce::String (10.0 * std::log10 (band[pk] / std::max (band[0], 1e-30)), 1) << " dB above its first 50 ms; at 0.5 s it is "
                  << juce::String (10.0 * std::log10 (band[10] / std::max (all[10], 1e-30)), 1) << " dB of the sound; peak "
                  << juce::String (10.0 * std::log10 (mx / (double) win), 1) << " dB RMS\n";
    }

    // stability: maximum drive (white noise into every model at full input, 20 s decay, stiff 1), 10 s
    std::cout << "  stability: white noise at 0 dBFS, input 100 %, decay 20 s, stiffness 100 %, 8 notes from C1 to F7 held 8 s,\n"
                 "  then released (damper 100 %) into 4 s of silence. Tail = peak of the last second.\n";
    for (int model = 0; model < 8; ++model)
        for (float bright : { 1.0f, 0.0f })
        {
            dsp::Resonator r;
            r.prepare ((float) kSr);
            dsp::Resonator::Params rp;
            rp.on = true; rp.model = model; rp.decay = 20.0f; rp.bright = bright; rp.stiff = 1.0f; rp.pos = 0.02f;
            rp.input = 1.0f; rp.strike = 1.0f; rp.mix = 1.0f; rp.damper = 1.0f;
            r.model = model;
            for (int k = 0; k < 8; ++k) r.noteOn (24 + k * 11, 1.0f);
            dsp::Rng rng (7);
            float pk = 0.0f, tail = 0.0f; bool finite = true;
            const int total = (int) (kSr * 12.0), held = (int) (kSr * 8.0);
            float L[64], R[64];
            for (int pos = 0; pos < total; pos += 64)
            {
                if (pos == held) for (int k = 0; k < 8; ++k) r.noteOff (24 + k * 11);
                for (int i = 0; i < 64; ++i) { L[i] = pos < held ? rng.bip() : 0.0f; R[i] = pos < held ? rng.bip() : 0.0f; }
                r.process (L, R, 64, rp);
                for (int i = 0; i < 64; ++i)
                {
                    finite &= std::isfinite (L[i]) && std::isfinite (R[i]);
                    pk = std::max (pk, std::abs (L[i]));
                    if (pos > total - (int) kSr) tail = std::max (tail, std::abs (L[i]));
                }
            }
            std::cout << "    " << juce::String (names[model]).paddedRight (' ', 10) << (bright > 0.5f ? "bright " : "dark   ")
                      << (finite ? "finite" : "NaN/inf") << ", peak " << juce::String (20.0 * std::log10 (pk + 1e-12), 1).paddedLeft (' ', 5)
                      << " dBFS, tail " << juce::String (20.0 * std::log10 (tail + 1e-12), 1) << " dBFS\n";
        }
}

void lfoLab()
{
    std::cout << "\n[lfo] movement options\n";
    dsp::WarpedLfo::Table tab;
    for (int i = 0; i < dsp::WarpedLfo::kPoints; ++i) tab[(size_t) i] = std::sin (dsp::kTwoPi * (float) i / dsp::WarpedLfo::kPoints);
    // ranges over every shape, bipolar and unipolar, with and without warp
    for (int uni = 0; uni < 2; ++uni)
    {
        float lo = 9, hi = -9;
        for (int shape = 0; shape < dsp::WarpedLfo::kNumShapes; ++shape)
            for (float warp : { -1.0f, 0.0f, 1.0f })
            {
                dsp::WarpedLfo l (3);
                dsp::WarpedLfo::Params p; p.rateHz = 3.0f; p.shape = shape; p.warp = warp; p.unipolar = uni == 1;
                for (int c = 0; c < 4000; ++c) { const float v = l.advance (64, 48000.0f, p, tab, false, 0.0); lo = std::min (lo, v); hi = std::max (hi, v); }
            }
        std::cout << "  " << (uni ? "unipolar" : "bipolar ") << " range over 11 shapes x 3 warps: " << juce::String (lo, 3) << " .. " << juce::String (hi, 3) << "\n";
    }
    // retrigger: a saw restarts at -1 on the note
    {
        dsp::WarpedLfo l (3);
        dsp::WarpedLfo::Params p; p.rateHz = 1.0f; p.shape = dsp::WarpedLfo::Saw; p.mode = dsp::WarpedLfo::Retrig;
        for (int c = 0; c < 400; ++c) l.advance (64, 48000.0f, p, tab, false, 0.0);
        l.trigger (p.mode, false);
        const float v = l.advance (1, 48000.0f, p, tab, false, 0.0);
        std::cout << "  retrigger: saw value right after note-on " << juce::String (v, 4) << " (expect -1)\n";
    }
    // one-shot: holds the end of the cycle
    {
        dsp::WarpedLfo l (3);
        dsp::WarpedLfo::Params p; p.rateHz = 2.0f; p.shape = dsp::WarpedLfo::RampDown; p.mode = dsp::WarpedLfo::OneShot;
        l.trigger (p.mode, true);
        float v = 0; for (int c = 0; c < 750 * 3; ++c) v = l.advance (64, 48000.0f, p, tab, false, 0.0);   // 3 s at 2 Hz
        std::cout << "  one-shot: ramp-down after 6 cycles' time " << juce::String (v, 4) << " (expect -1, held)\n";
    }
    // fade-in: 50 % of the way through a 1 s fade (smoothstep → 0.5) on a square held at +1
    {
        dsp::WarpedLfo l (3);
        dsp::WarpedLfo::Params p; p.rateHz = 0.01f; p.shape = dsp::WarpedLfo::Square; p.fadeSec = 1.0f; p.mode = dsp::WarpedLfo::Retrig;
        l.trigger (p.mode, true);
        float v = 0; for (int c = 0; c < 375; ++c) v = l.advance (64, 48000.0f, p, tab, false, 0.0);   // 0.5 s
        std::cout << "  fade-in: square at half the fade time " << juce::String (v, 3) << " (expect 0.5)\n";
    }
    // smoothing: 10-90 % rise time of a 1 Hz square at 50 % smooth
    {
        dsp::WarpedLfo l (3);
        dsp::WarpedLfo::Params p; p.rateHz = 1.0f; p.shape = dsp::WarpedLfo::Square; p.smooth = 0.5f; p.phaseOffset = 0.5f;   // starts low
        std::vector<float> v;
        for (int c = 0; c < 1500; ++c) v.push_back (l.advance (64, 48000.0f, p, tab, false, 0.0));
        int i10 = -1, i90 = -1;
        for (size_t i = 375; i < v.size(); ++i) { if (i10 < 0 && v[i] > -0.8f) i10 = (int) i; if (i90 < 0 && v[i] > 0.8f) { i90 = (int) i; break; } }
        std::cout << "  smooth 50 %: square rise 10-90 % in " << juce::String ((i90 - i10) * 64.0 / 48.0, 1) << " ms (tau 127 ms -> expect ~280 ms)\n";
    }
}

//------------------------------------------------------------------------------
// true peak: the output stage alone, fed signals that are known to leave inter-sample peaks after a clipper

/** The 4x meter VesselCheck uses (JUCE equiripple FIR half-bands, BS.1770-style). */
double tp4Db (const std::vector<float>& l, const std::vector<float>& r)
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

/** Reference: 16x reconstruction with a 64-tap-per-phase Kaiser-windowed sinc (independent of the stage's own 8x one). */
double tp16Db (const std::vector<float>& l, const std::vector<float>& r)
{
    constexpr int half = 32, taps = 2 * half, phases = 16;
    auto i0 = [] (double x) { double s = 1, t = 1; for (int k = 1; k < 50; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
    std::vector<double> h ((size_t) phases * taps);
    for (int p = 1; p < phases; ++p)
    {
        double sum = 0;
        for (int k = 0; k < taps; ++k)
        {
            const double d = (double) p / phases + (half - 1) - k, u = d / half;
            const double v = std::sin (M_PI * d) / (M_PI * d) * (u * u < 1 ? i0 (10.0 * std::sqrt (1 - u * u)) / i0 (10.0) : 0.0);
            h[(size_t) (p * taps + k)] = v; sum += v;
        }
        for (int k = 0; k < taps; ++k) h[(size_t) (p * taps + k)] /= sum;
    }
    double peak = 0;
    for (const auto* x : { &l, &r })
        for (size_t s = half; s + half < x->size(); ++s)
        {
            peak = std::max (peak, (double) std::abs ((*x)[s]));
            // only where an inter-sample peak could matter
            if (std::abs ((*x)[s]) < 0.25f && std::abs ((*x)[s + 1]) < 0.25f) continue;
            for (int p = 1; p < phases; ++p)
            {
                double a = 0;
                for (int k = 0; k < taps; ++k) a += h[(size_t) (p * taps + k)] * (*x)[s - (half - 1) + (size_t) k];
                peak = std::max (peak, std::abs (a));
            }
        }
    return juce::Decibels::gainToDecibels (peak, -150.0);
}

struct TpRun { std::vector<float> l, r; float minGain = 1.0f; int latency = 0; };

/** Feeds gen(channel, sample) through a fresh OutputStage (clipper on, exciter off) in 64-sample chunks. */
template <typename G>
TpRun throughOutput (G&& gen, int total, bool truePeak, float ceilingDb, float gainDb, double sr = kSr)
{
    dsp::OutputStage os;
    os.prepare ((float) sr, 64);
    os.truePeak = truePeak;
    os.snap (false, true);
    dsp::OutputStage::Params p; p.excOn = false; p.clipOn = true; p.ceilingDb = ceilingDb; p.gainDb = gainDb;
    TpRun out; out.l.resize ((size_t) total); out.r.resize ((size_t) total);
    out.latency = (int) std::lround (os.latency());
    for (int i = 0; i < total; ++i) { out.l[(size_t) i] = gen (0, i); out.r[(size_t) i] = gen (1, i); }
    for (int pos = 0; pos < total; pos += 64)
        os.process (out.l.data() + pos, out.r.data() + pos, std::min (64, total - pos), p);
    out.minGain = os.tp.takeMinGain();
    return out;
}

void truePeakLab()
{
    std::cout << "\n[true peak] output stage alone, clipper on, ceiling -1.0 dBFS. dBTP from the 4x meter the preset check uses,\n"
                 "  and from a 16x reference reconstruction. 'off' = clipper and sample clamp only; 'on' = with the true-peak ceiling\n";
    const int total = (int) (2.0 * kSr);
    const float ceil = -1.0f;
    struct Case { const char* name; float gainDb; std::function<float (int, int)> gen; };
    const Case cases[] {
        { "fs/4 sine, 45 deg, +3 dB", 3.0f, [] (int, int i) { return 0.89f * (float) std::sin (M_PI * 0.5 * i + M_PI * 0.25); } },
        { "sine 11 kHz +12 dB",      12.0f, [] (int c, int i) { return 0.89f * (float) std::sin (2 * M_PI * 11000.0 * i / kSr + c); } },
        { "saw C6 +12 dB",           12.0f, [] (int, int i) { const double ph = std::fmod (1046.5 * i / kSr, 1.0); return 0.89f * (float) (2 * ph - 1); } },
        { "noise bursts +12 dB",     12.0f, [] (int c, int i) {
              static dsp::Rng rng (12345u);
              const int k = i % 4800; const float env = std::exp (-(float) k / 240.0f);
              juce::ignoreUnused (c);
              return 0.89f * env * rng.bip(); } },
        { "kick 55 Hz +12 dB",       12.0f, [] (int, int i) {
              const int k = i % 24000; const double t = k / kSr;
              return (float) (0.89 * std::exp (-t * 8.0) * std::sin (2 * M_PI * (55.0 * t + 40.0 * (1 - std::exp (-t * 30.0)) / 30.0))); } },
    };
    for (auto& c : cases)
    {
        const auto off = throughOutput (c.gen, total, false, ceil, c.gainDb);
        const auto on  = throughOutput (c.gen, total, true,  ceil, c.gainDb);
        auto sp = [] (const TpRun& t) { float p = 0; for (size_t i = 0; i < t.l.size(); ++i) p = std::max ({ p, std::abs (t.l[i]), std::abs (t.r[i]) }); return juce::Decibels::gainToDecibels ((double) p, -150.0); };
        std::cout << "  " << juce::String (c.name).paddedRight (' ', 26)
                  << "off: sample " << juce::String (sp (off), 2) << "  4x " << juce::String (tp4Db (off.l, off.r), 2) << "  16x " << juce::String (tp16Db (off.l, off.r), 2)
                  << "   on: sample " << juce::String (sp (on), 2) << "  4x " << juce::String (tp4Db (on.l, on.r), 2) << "  16x " << juce::String (tp16Db (on.l, on.r), 2)
                  << "  (most reduction " << juce::String (juce::Decibels::gainToDecibels ((double) on.minGain), 2) << " dB)\n";
    }

    // transparency: below the ceiling the stage must be an exact delay
    {
        auto gen = [] (int c, int i) { return 0.5f * (float) std::sin (2 * M_PI * (997.0 + 50 * c) * i / kSr) + 0.2f * (float) std::sin (2 * M_PI * 15000.0 * i / kSr); };
        dsp::TruePeakCeiling tp; tp.prepare ((float) kSr);
        std::vector<float> l ((size_t) total), r ((size_t) total), l0 ((size_t) total), r0 ((size_t) total);
        for (int i = 0; i < total; ++i) { l0[(size_t) i] = l[(size_t) i] = gen (0, i); r0[(size_t) i] = r[(size_t) i] = gen (1, i); }
        for (int pos = 0; pos < total; pos += 64) tp.process (l.data() + pos, r.data() + pos, std::min (64, total - pos), juce::Decibels::decibelsToGain (ceil));
        double maxDiff = 0;
        for (int i = tp.latency(); i < total; ++i)
            maxDiff = std::max ({ maxDiff, (double) std::abs (l[(size_t) i] - l0[(size_t) (i - tp.latency())]), (double) std::abs (r[(size_t) i] - r0[(size_t) (i - tp.latency())]) });
        std::cout << "  below the ceiling (-3.1 dBTP programme): largest difference from the delayed input " << (maxDiff == 0.0 ? juce::String ("0 (bit-exact)") : juce::String (maxDiff, 9)) << "\n";
    }
    // the ramps: the most the gain moves from one sample to the next on the worst case above
    {
        dsp::TruePeakCeiling tp; tp.prepare ((float) kSr);
        std::vector<float> l ((size_t) total, 0.0f), r ((size_t) total, 0.0f);
        // a 1.0 step held for one sample in silence: the most abrupt thing the stage can be asked to hold down
        l[(size_t) (total / 2)] = 2.0f;
        std::vector<float> g ((size_t) total);
        for (int i = 0; i < total; ++i) g[(size_t) i] = 1.0f;
        for (int pos = 0; pos < total; pos += 64) tp.process (l.data() + pos, r.data() + pos, std::min (64, total - pos), juce::Decibels::decibelsToGain (ceil));
        const float outPk = std::abs (l[(size_t) (total / 2 + tp.latency())]);
        std::cout << "  one +6 dBFS sample in silence comes out at " << juce::String (juce::Decibels::gainToDecibels (outPk), 2)
                  << " dBFS; gain ramps last " << tp.rampLength() << " samples\n";
    }
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        dsp::OutputStage os; os.prepare ((float) sr, 64);
        dsp::TruePeakCeiling tp; tp.prepare ((float) sr);
        std::cout << "  latency at " << juce::String (sr / 1000.0, 1) << " kHz: output stage " << juce::String (os.latency(), 1)
                  << " samples (true-peak look-ahead " << tp.latency() << " = " << juce::String (1000.0 * tp.latency() / sr, 2) << " ms)\n";
    }
    // cost: loud noise all the way through (detector always active)
    {
        dsp::TruePeakCeiling tp; tp.prepare ((float) kSr);
        const int n = (int) (20.0 * kSr);
        std::vector<float> l ((size_t) n), r ((size_t) n);
        dsp::Rng rng (7u);
        for (int i = 0; i < n; ++i) { l[(size_t) i] = 1.2f * rng.bip(); r[(size_t) i] = 1.2f * rng.bip(); }
        const auto t0 = juce::Time::getHighResolutionTicks();
        for (int pos = 0; pos < n; pos += 64) tp.process (l.data() + pos, r.data() + pos, 64, 0.89f);
        const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
        std::cout << "  cost, worst case (full-scale noise, every interval near the ceiling): " << juce::String (100.0 * secs / 20.0, 3) << " % of one core at 48 kHz\n";
    }
    // cost on programme: a loud saw chord whose peaks reach the ceiling now and then, whole output stage on vs off
    {
        const int n = (int) (20.0 * kSr);
        auto chord = [] (int c, int i) {
            double v = 0;
            for (double f : { 65.4, 130.8, 164.8, 196.0, 261.6 }) v += 2.0 * std::fmod (f * (1.0 + 0.002 * c) * i / kSr, 1.0) - 1.0;
            return (float) (0.3 * v * (0.6 + 0.4 * std::sin (2 * M_PI * 0.5 * i / kSr)));
        };
        double secs[2] {};
        for (int on = 0; on < 2; ++on)
        {
            dsp::OutputStage os; os.prepare ((float) kSr, 64); os.truePeak = on == 1; os.snap (false, true);
            dsp::OutputStage::Params p; p.clipOn = true; p.ceilingDb = -1.0f; p.gainDb = 0.0f;
            std::vector<float> l ((size_t) n), r ((size_t) n);
            for (int i = 0; i < n; ++i) { l[(size_t) i] = chord (0, i); r[(size_t) i] = chord (1, i); }
            const auto t0 = juce::Time::getHighResolutionTicks();
            for (int pos = 0; pos < n; pos += 64) os.process (l.data() + pos, r.data() + pos, 64, p);
            secs[on] = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            if (on == 1) std::cout << "  cost on a loud chord (peaks to the ceiling, most reduction "
                                   << juce::String (juce::Decibels::gainToDecibels ((double) os.tp.takeMinGain()), 2) << " dB): "
                                   << juce::String (100.0 * (secs[1] - secs[0]) / 20.0, 3) << " % of one core at 48 kHz\n";
        }
    }
}

/** Every imported table: decode time, frame count, finiteness, and the aliasing of a C6 note with the morph halfway
    (the worst place: two frames crossfaded, the mip chosen for the note). */
/** Character of every imported table, for choosing them in presets: harmonic centroid, richness (harmonics within
    40 dB of the strongest), odd-harmonic share, and how far the spectrum travels across the frames (morph range). */
void tableCharacter()
{
    juce::dsp::FFT fft (11);
    const int N = 2048;
    std::vector<float> buf ((size_t) N * 2);
    std::cout << "index;category;name;frames;centroid;richness;odd;morph;profile\n";
    for (int i = 1; i <= dsp::kNumTables; ++i)
    {
        const auto& info = dsp::kTables[i - 1];
        auto bank = dsp::loadTable (i, nullptr);
        if (bank == nullptr) continue;
        const int F = bank->frames();
        std::vector<std::vector<double>> spectra;
        double cent = 0, rich = 0, odd = 0;
        juce::String profile;   // harmonic centroid at 9 evenly spaced morph positions
        const int picks = std::min (F, 9);
        for (int k = 0; k < picks; ++k)
        {
            const int f = picks == 1 ? 0 : k * (F - 1) / (picks - 1);
            const float* t = std::as_const (*bank).table (f, 0);
            std::fill (buf.begin(), buf.end(), 0.0f);
            std::copy_n (t, N, buf.begin());
            fft.performFrequencyOnlyForwardTransform (buf.data());
            std::vector<double> h (257, 0.0);
            double tot = 0, c = 0, o = 0, mx = 0;
            for (int n = 1; n <= 256; ++n) { h[(size_t) n] = (double) buf[(size_t) n] * buf[(size_t) n]; tot += h[(size_t) n]; c += n * h[(size_t) n]; if (n % 2) o += h[(size_t) n]; mx = std::max (mx, h[(size_t) n]); }
            int r = 0; for (int n = 1; n <= 256; ++n) r += h[(size_t) n] > mx * 1e-4 ? 1 : 0;
            cent += c / (tot + 1e-30); rich += r; odd += o / (tot + 1e-30);
            profile << juce::String (c / (tot + 1e-30), 1) << " ";
            for (auto& x : h) x = 10.0 * std::log10 (x / (tot + 1e-30) + 1e-9);
            spectra.push_back (h);
        }
        double morph = 0; int pairs = 0;
        for (size_t a = 1; a < spectra.size(); ++a)
        {
            double acc = 0; for (int n = 1; n <= 48; ++n) { const double d = std::clamp (spectra[a][(size_t) n] - spectra[0][(size_t) n], -30.0, 30.0); acc += d * d; }
            morph = std::max (morph, std::sqrt (acc / 48.0)); ++pairs;
        }
        std::cout << i << ";" << info.category << ";" << juce::String::fromUTF8 (info.name) << ";" << F << ";" << juce::String (cent / picks, 1) << ";"
                  << juce::String (juce::roundToInt (rich / picks)) << ";" << juce::String (odd / picks, 2) << ";" << juce::String (morph, 1) << ";" << profile.trim() << "\n";
    }
}

void tablesLab()
{
    std::cout << "\n[tables] " << dsp::kNumTables << " imported tables from " << dsp::tablesFolder().getFullPathName() << "\n";
    double totalMs = 0.0, worstMs = 0.0; int bad = 0;
    struct Row { double alias; float peak; juce::String name; };
    std::vector<Row> rows;
    auto p = make();
    clean (*p);
    setP (*p, wt_morph, 0.5f);
    for (int i = 1; i <= dsp::kNumTables; ++i)
    {
        const auto& info = dsp::kTables[i - 1];
        const juce::String name = juce::String (info.category) + " - " + juce::String::fromUTF8 (info.name);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        juce::String err;
        auto bank = dsp::loadTable (i, &err);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        totalMs += ms; worstMs = std::max (worstMs, ms);
        if (bank == nullptr) { std::cout << "  FAIL " << name << ": " << err << "\n"; ++bad; continue; }
        bool finite = true; float peak = 0.0f;
        for (int f = 0; f < bank->frames(); ++f)
            for (int m = 0; m < dsp::WavetableBank::kMips; ++m)
            {
                const float* t = std::as_const (*bank).table (f, m);
                for (int k = 0; k <= dsp::WavetableBank::kSize; ++k) { finite &= std::isfinite (t[k]); if (m == 0) peak = std::max (peak, std::abs (t[k])); }
            }
        if (! finite || bank->frames() != info.frames) { std::cout << "  FAIL " << name << ": finite " << (int) finite << " frames " << bank->frames() << "/" << info.frames << "\n"; ++bad; continue; }

        setP (*p, wt_table, (float) i);
        p->prepareToPlay (kSr, kBlock);   // loads the table synchronously, as a preset load does
        if (p->currentTableIndex() != i) { std::cout << "  FAIL " << name << ": not current after load\n"; ++bad; continue; }
        auto r = run (*p, 1.5, 84, [] (int) {});
        float outPeak = 0.0f; bool outFinite = true;
        for (float x : r.l) { outFinite &= std::isfinite (x); outPeak = std::max (outPeak, std::abs (x)); }
        if (! outFinite) { std::cout << "  FAIL " << name << ": non-finite output\n"; ++bad; continue; }
        const float f0 = 440.0f * std::exp2 ((84 - 69) / 12.0f);
        rows.push_back ({ aliasDb (r.l, f0), outPeak, name });
        run (*p, 0.3, -1, [] (int) {});   // let the release ring out before the next table
    }
    std::sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b) { return a.alias > b.alias; });
    std::cout << "  decode: " << juce::String (totalMs / dsp::kNumTables, 1) << " ms average, " << juce::String (worstMs, 1)
              << " ms worst; " << bad << " failures\n  C6 aliasing, worst five (inharmonic / harmonic):\n";
    for (size_t k = 0; k < std::min<size_t> (5, rows.size()); ++k)
        std::cout << "    " << rows[k].name.paddedRight (' ', 34) << juce::String (rows[k].alias, 1) << " dB\n";
    double sum = 0.0; for (auto& r : rows) sum += r.alias;
    if (! rows.empty()) std::cout << "  median " << juce::String (rows[rows.size() / 2].alias, 1) << " dB, mean " << juce::String (sum / (double) rows.size(), 1) << " dB\n";
}

/** The FM operator: aliasing across ratio / index / feedback / pitch, and click-free index sweeps. */
void fmLab()
{
    std::cout << "\n[fm] inharmonic energy relative to the harmonics (integer and half ratios: harmonics of f0 or f0/2)\n";
    struct Case { const char* name; int note; float ratio; float index; float fb; float morph; float div; };
    const Case cases[] {
        { "sine C3  1:1   idx 0.5",          48,  3, 0.5f, 0.0f, 0.0f, 1.0f },
        { "sine C3  2:1   idx 1.0",          48,  5, 1.0f, 0.0f, 0.0f, 1.0f },
        { "sine C5  3:1   idx 0.8",          72,  7, 0.8f, 0.0f, 0.0f, 1.0f },
        { "sine C6  2:1   idx 1.0",          84,  5, 1.0f, 0.0f, 0.0f, 1.0f },
        { "sine C7  1:1   idx 0.7",          96,  3, 0.7f, 0.0f, 0.0f, 1.0f },
        { "sine C6  14:1  idx 0.6",          84, 16, 0.6f, 0.0f, 0.0f, 1.0f },
        { "sine C5  1.5:1 idx 0.8",          72,  4, 0.8f, 0.0f, 0.0f, 2.0f },
        { "sine C4  1:1   fb 100% idx 0.6",  60,  3, 0.6f, 1.0f, 0.0f, 1.0f },
        { "saw  C5  1:1   idx 0.6",          72,  3, 0.6f, 0.0f, 2.0f / 7.0f, 1.0f },
        { "saw  C6  2:1   idx 0.8",          84,  5, 0.8f, 0.0f, 2.0f / 7.0f, 1.0f },
    };
    for (auto& c : cases)
    {
        auto p = make();
        clean (*p);
        setP (*p, wt_morph, c.morph);
        setP (*p, fm_on, 1); setP (*p, fm_ratio, c.ratio); setP (*p, fm_index, c.index); setP (*p, fm_feedback, c.fb);
        auto r = run (*p, 2.0, c.note, [] (int) {});
        const float f0 = 440.0f * std::exp2 ((float) (c.note - 69) / 12.0f) / c.div;
        float pk = 0.0f; for (float x : r.l) pk = std::max (pk, std::abs (x));
        std::cout << "  " << juce::String (c.name).paddedRight (' ', 32) << juce::String (aliasDb (r.l, f0), 1) << " dB   peak "
                  << juce::String (juce::Decibels::gainToDecibels (pk), 1) << " dBFS\n";
    }

    // index swept 0 -> 1 -> 0 over 2 s at C3: the largest sample-to-sample step relative to a static index 1 render
    auto stepMax = [] (const std::vector<float>& x) { float m = 0.0f; for (size_t i = 1; i < x.size(); ++i) m = std::max (m, std::abs (x[i] - x[i - 1])); return m; };
    auto p = make(); clean (*p); setP (*p, fm_on, 1); setP (*p, fm_ratio, 5); setP (*p, fm_index, 1.0f);
    const float refStep = stepMax (run (*p, 1.0, 48, [] (int) {}).l);
    auto q = make(); clean (*q); setP (*q, fm_on, 1); setP (*q, fm_ratio, 5);
    const int blocks = (int) (2.0 * kSr / kBlock);
    auto sw = run (*q, 2.0, 48, [&] (int b) { const float t = (float) b / (float) blocks; setP (*q, fm_index, 1.0f - std::abs (2.0f * t - 1.0f)); });
    std::cout << "  index sweep 0-1-0 (block steps): largest sample step " << juce::String (stepMax (sw.l) / std::max (1e-9f, refStep), 2)
              << " x the static index-1 render's\n";

    // toggling FM on and off mid-note: largest step relative to the steady FM sound's (same settings, always on)
    auto u = make(); clean (*u); setP (*u, fm_on, 1); setP (*u, fm_ratio, 5); setP (*u, fm_index, 0.7f);
    const float steadyStep = stepMax (run (*u, 1.5, 48, [] (int) {}).l);
    auto t = make(); clean (*t); setP (*t, fm_ratio, 5); setP (*t, fm_index, 0.7f);
    auto tg = run (*t, 1.5, 48, [&] (int b) { setP (*t, fm_on, (b / 47) % 2 == 1 ? 1.0f : 0.0f); });
    std::cout << "  on/off every 0.25 s: largest sample step " << juce::String (stepMax (tg.l) / std::max (1e-9f, steadyStep), 2)
              << " x the steady FM render's\n";
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    vessel::dsp::setTablesFolder (juce::File (VESSEL_TABLES_DIR));   // no bundle: read the source tree's tables
    const juce::String what = argc > 1 ? juce::String (argv[1]) : "all";
    if (what == "all" || what == "zipper")  zipper();
    if (what == "all" || what == "toggles") toggles();
    if (what == "all" || what == "alias")   alias();
    if (what == "all" || what == "phase")   coherence();
    if (what == "all" || what == "lowend")  lowend();
    if (what == "all" || what == "res")     resonatorLab();
    if (what == "all" || what == "lfo")     lfoLab();
    if (what == "all" || what == "truepeak") truePeakLab();
    if (what == "all" || what == "fm")      fmLab();
    if (what == "tables")                   tablesLab();   // ~20 s: not part of "all"
    if (what == "tablechar")                tableCharacter();
    if (what == "all" || what == "cpu")     cpu();
    return 0;
}
