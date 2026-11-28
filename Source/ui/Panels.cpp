#include "Panels.h"
#include "../Presets.h"
#include "../PluginProcessor.h"
#include "../dsp/Wavetable.h"

namespace vessel::ui
{
using namespace ink;

namespace
{

float plain (VesselProcessor& p, int pid)  { auto* r = p.param (pid); return r->convertFrom0to1 (r->getValue()); }
bool  isOn (VesselProcessor& p, int pid)   { return p.param (pid)->getValue() > 0.5f; }

/** Draws y = fn(t), t in [0, 1), across `a`: dense enough for tables whose energy sits in the upper harmonics
    (four points per pixel, near full band), and scaled to the curve's own peak so a quiet frame still shows its shape. */
template <typename Fn>
void drawWave (juce::Graphics& g, juce::Rectangle<float> a, Fn&& fn, juce::Colour c, float thickness)
{
    const int n = juce::jlimit (64, 1600, juce::roundToInt (a.getWidth() * 4.0f));
    std::array<float, 1601> ys {};
    float peak = 0.0f;
    for (int i = 0; i <= n; ++i) { ys[(size_t) i] = fn ((float) i / (float) n); peak = std::max (peak, std::abs (ys[(size_t) i])); }
    const float scale = a.getHeight() * 0.45f / std::max (peak, 0.25f);
    juce::Path path;
    for (int i = 0; i <= n; ++i)
    {
        const juce::Point<float> pt (a.getX() + a.getWidth() * (float) i / (float) n, a.getCentreY() - ys[(size_t) i] * scale);
        if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
    }
    g.setColour (c);
    g.strokePath (path, juce::PathStrokeType (thickness));
}

/** Horizontal meter: caption left, value right, thin bar beneath. bipolar draws from the centre. */
void bar (juce::Graphics& g, juce::Rectangle<float> r, float v, juce::Colour c, const juce::String& caption,
          const juce::String& valueText, bool bipolar = false)
{
    drawCaps (g, caption, r.withHeight (12.0f), 9.0f, pewter, juce::Justification::centredLeft);
    g.setColour (frost);
    g.setFont (type::value (11.5f));
    g.drawText (valueText, r.withHeight (12.0f), juce::Justification::centredRight, false);
    auto track = juce::Rectangle<float> (r.getX(), r.getY() + 15.0f, r.getWidth(), 2.0f);
    g.setColour (seamHi);
    g.fillRect (track);
    g.setColour (c);
    if (bipolar)
    {
        const float mid = track.getCentreX(), x = mid + juce::jlimit (-1.0f, 1.0f, v) * track.getWidth() * 0.5f;
        g.fillRect (juce::Rectangle<float> (std::min (mid, x), track.getY() - 1.0f, std::abs (x - mid), 4.0f));
        g.setColour (pewter); g.fillRect (juce::Rectangle<float> (mid, track.getY() - 3.0f, 1.0f, 8.0f));
    }
    else
        g.fillRect (track.withWidth (track.getWidth() * juce::jlimit (0.0f, 1.0f, v)).expanded (0.0f, 1.0f));
}

void frame (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (basin.withAlpha (0.45f));
    g.fillRect (r);
    g.setColour (seam);
    g.drawRect (r, 1.0f);
}

const int kStagePids[5][5] {
    { sub_on, wt_on, drone_on, noise_on, gran_on },
    { tube_on, bias_on, crush_on, vhs_on, tape_mode },
    { flt_on, envf_on, unmask_on, form_on, res_on },
    { z_on, conv_on, verb_on, haas_on, -1 },
    { ott_on, duck_on, mono_on, exc_on, clip_on } };
const char* kStageNames[5] { "Source", "Ruin", "Shape", "Space", "Pressure" };
const char* kStageCaps[5]  { "sub · table · drone · dust · grain", "drive · bias · crush · tape",
                             "ladders · follower · vox · strings", "depth · body · hole · width",
                             "multiband · duck · master" };
const char* kNumerals[5]   { "I", "II", "III", "IV", "V" };
} // namespace

//==============================================================================
ModuleColumn::ModuleColumn (VesselProcessor& p, Registry& r, const juce::String& t, const juce::String& n, int toggle)
    : proc (p), reg (r), title (t), number (n), togglePid (toggle)
{
    if (togglePid >= 0)
    {
        header = std::make_unique<Rune> (proc, togglePid, title, true);
        addAndMakeVisible (*header);
    }
}

ParamSlider& ModuleColumn::halo (int pid, const juce::String& caption)
{
    auto* s = new ParamSlider (proc, pid, ParamSlider::Kind::Halo, caption);
    owned.add (s);
    addAndMakeVisible (s);
    reg.sliders.push_back (s);
    return *s;
}

ParamSlider& ModuleColumn::vial (int pid, const juce::String& caption, const juce::String& shortName)
{
    auto* s = new ParamSlider (proc, pid, ParamSlider::Kind::Vial, caption, shortName);
    owned.add (s);
    addAndMakeVisible (s);
    reg.sliders.push_back (s);
    return *s;
}

Rune& ModuleColumn::rune (int pid, const juce::String& caption)
{
    auto* b = new Rune (proc, pid, caption);
    owned.add (b);
    addAndMakeVisible (b);
    return *b;
}

Selector& ModuleColumn::selector (int pid, const juce::String& caption)
{
    auto* s = new Selector (proc, pid, caption);
    owned.add (s);
    addAndMakeVisible (s);
    return *s;
}

juce::TextButton& ModuleColumn::button (const juce::String& text)
{
    auto* b = new juce::TextButton (text);
    owned.add (b);
    addAndMakeVisible (b);
    return *b;
}

void ModuleColumn::row (std::initializer_list<juce::Component*> items, int height, std::initializer_list<float> weights)
{
    jassert (weights.size() == 0 || weights.size() == items.size());
    rows.push_back ({ std::vector<juce::Component*> (items), height, std::vector<float> (weights) });
}

void ModuleColumn::resized()
{
    const int w = getWidth();
    // the feature number gives way when the title needs the room
    const float titleW = type::width (type::caps (11.5f, 0.22f), title.toUpperCase()) + (header ? 26.0f : 6.0f);
    showNumber = number.isNotEmpty() && titleW <= (float) (w - 54);
    if (header) header->setBounds (8, 6, showNumber ? w - 54 : w - 14, 24);

    int y = 44;
    for (auto& r : rows)
    {
        const int n = (int) r.items.size();
        float total = 0.0f;
        for (int i = 0; i < n; ++i) total += r.weights.empty() ? 1.0f : r.weights[(size_t) i];
        float acc = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float wi = r.weights.empty() ? 1.0f : r.weights[(size_t) i];
            const int x0 = 6 + juce::roundToInt ((float) (w - 12) * acc / total);
            acc += wi;
            const int x1 = 6 + juce::roundToInt ((float) (w - 12) * acc / total);
            auto* c = r.items[(size_t) i];
            const bool slim = dynamic_cast<Selector*> (c) != nullptr || dynamic_cast<Rune*> (c) != nullptr
                           || dynamic_cast<juce::TextButton*> (c) != nullptr;
            if (slim) c->setBounds (x0 + 4, y, x1 - x0 - 8, r.height);
            else      c->setBounds (x0, y, x1 - x0, r.height);
        }
        y += r.height + 6;
    }
    instrumentArea = juce::Rectangle<float> (12.0f, (float) y + 6.0f, (float) w - 24.0f, (float) (getHeight() - y - 18));
}

void ModuleColumn::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    if (! header) drawCaps (g, title, { 12.0f, 6.0f, w - 60.0f, 24.0f }, 11.5f, frost, juce::Justification::centredLeft, 0.22f);

    if (showNumber)
    {
        g.setColour (pewter.withAlpha (0.75f));
        g.setFont (type::display (15.0f));
        g.drawText (number, juce::Rectangle<float> (w - 50.0f, 6.0f, 40.0f, 24.0f), juce::Justification::centredRight, false);
    }
    drawRule (g, 36.0f, 0.0f, w, seam);

    if (instrument && instrumentArea.getHeight() > 12.0f)
    {
        juce::Graphics::ScopedSaveState ss (g);
        if (! lastOn) g.setOpacity (0.4f);
        instrument (g, instrumentArea);
    }
}

void ModuleColumn::refresh()
{
    const bool on = togglePid < 0 || (vessel::spec (togglePid).type == PType::Bool ? isOn (proc, togglePid)
                                                                                    : plain (proc, togglePid) > 0.5f);
    if (on != lastOn)
    {
        lastOn = on;
        for (auto* c : owned) c->setAlpha (on ? 1.0f : 0.38f);
        repaint();
    }
    if (instrument) repaint (instrumentArea.toNearestInt().expanded (2));
}

//==============================================================================
ModuleColumn& StagePage::add (std::unique_ptr<ModuleColumn> c, float weight)
{
    auto* raw = columns.add (c.release());
    weights.push_back (weight);
    addAndMakeVisible (raw);
    return *raw;
}

void StagePage::resized()
{
    float total = 0.0f;
    for (auto w : weights) total += w;
    float x = 0.0f;
    for (int i = 0; i < columns.size(); ++i)
    {
        const float cw = (float) getWidth() * weights[(size_t) i] / total;
        columns[i]->setBounds (juce::roundToInt (x), 0, juce::roundToInt (x + cw) - juce::roundToInt (x), getHeight());
        x += cw;
    }
}

void StagePage::paint (juce::Graphics& g)
{
    g.setColour (basin.withAlpha (0.38f));
    g.fillAll();
    for (int i = 1; i < columns.size(); ++i)
        drawSeam (g, (float) columns[i]->getX(), 8.0f, (float) getHeight() - 8.0f);
}

//==============================================================================
std::unique_ptr<StagePage> buildStage (int index, VesselProcessor& p, Registry& reg)
{
    auto page = std::make_unique<StagePage>();
    auto col = [&] (const juce::String& t, const juce::String& n, int toggle, float weight) -> ModuleColumn&
    {
        return page->add (std::make_unique<ModuleColumn> (p, reg, t, n, toggle), weight);
    };
    const auto H = ParamSlider::kHaloHeight, S = Selector::kHeight, V = ParamSlider::kVialHeight, R = Rune::kHeight;

    switch (index)
    {
        case 0:   // ── I · SOURCE
        {
            auto& sub = col ("Sub", "01", sub_on, 1);
            sub.row ({ &sub.selector (sub1_oct, "Sub 1 Oct"), &sub.selector (sub2_oct, "Sub 2 Oct") }, S);
            sub.row ({ &sub.halo (sub1_level, "Sub 1"), &sub.halo (sub2_level, "Sub 2") }, H);
            sub.row ({ &sub.halo (sub1_shape, "Shape 1"), &sub.halo (sub2_shape, "Shape 2") }, H);
            sub.row ({ &sub.halo (sub_weight, "Weight"), &sub.halo (sub_send, "To Ruin") }, H);
            sub.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {   // phase lock: both subs drawn from one master phase — every zero crossing lines up
                drawCaps (g, "phase lock", r.withHeight (12.0f), 9.0f, pewter, juce::Justification::centredLeft);
                auto a = r.withTrimmedTop (16.0f).withHeight (std::min (60.0f, r.getHeight() - 16.0f));
                const int o1 = juce::roundToInt (plain (p, sub1_oct)), o2 = juce::roundToInt (plain (p, sub2_oct)) + 1;
                for (int k = 0; k < 2; ++k)
                {
                    const int oct = k == 0 ? o1 : o2;
                    juce::Path path;
                    for (int i = 0; i <= 120; ++i)
                    {
                        const float t = (float) i / 120.0f;
                        const float y = std::sin (dsp::kTwoPi * t * 4.0f / (float) (1 << oct));
                        const auto pt = juce::Point<float> (a.getX() + t * a.getWidth(), a.getCentreY() - y * a.getHeight() * 0.42f);
                        if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
                    }
                    g.setColour (k == 0 ? violet : frost.withAlpha (0.55f));
                    g.strokePath (path, juce::PathStrokeType (k == 0 ? 1.4f : 1.0f));
                }
                g.setColour (seamHi);
                g.fillRect (juce::Rectangle<float> (a.getX(), a.getCentreY(), a.getWidth(), 1.0f));
            };

            auto& wt = col ("Wavetable", "05", wt_on, 1.25f);
            wt.row ({ &wt.selector (wt_table, "Table"), &wt.selector (wt_oct, "Octave") }, S, { 2.1f, 1.0f });
            wt.row ({ &wt.halo (wt_morph, "Morph"), &wt.halo (wt_pw, "Pulse W") }, H);
            wt.row ({ &wt.halo (wt_pwm, "PWM"), &wt.halo (wt_pwm_rate, "PWM Rate") }, H);
            wt.row ({ &wt.halo (wt_semi, "Semi"), &wt.halo (wt_fine, "Fine") }, H);
            wt.row ({ &wt.halo (wt_level, "Level"), &wt.halo (wt_drift, "Drift") }, H);
            wt.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                auto a = r.withHeight (std::min (r.getHeight(), 46.0f));
                const float morph = plain (p, wt_morph), pw = juce::jlimit (0.02f, 0.98f, plain (p, wt_pw));
                const auto bank = p.currentTableShared();
                if (bank == nullptr) return;
                drawWave (g, a, [&] (float ph)
                {
                    const float wp = ph < pw ? 0.5f * ph / pw : 0.5f + 0.5f * (ph - pw) / (1.0f - pw);
                    return bank->readMorph (morph, 1, std::min (wp, 0.9999f));
                }, violet, 1.0f);
            };

            auto& dr = col ("Drone", "03", drone_on, 0.92f);
            dr.row ({ &dr.selector (drone_oct, "Octave"), &dr.rune (drone_hold, "Hold") }, S);
            dr.row ({ &dr.halo (drone_level, "Level"), &dr.halo (drone_tone, "Tone") }, H);
            dr.row ({ &dr.halo (drone_motion, "Motion"), &dr.halo (drone_spread, "Spread") }, H);
            dr.row ({ &dr.halo (drone_attack, "Swell"), &dr.halo (drone_release, "Fade") }, H);

            auto& nz = col ("Dust", "04", noise_on, 0.88f);
            nz.row ({ &nz.selector (noise_hum_freq, "Mains") }, S);
            nz.row ({ &nz.halo (noise_hiss, "Hiss"), &nz.halo (noise_crackle, "Crackle") }, H);
            nz.row ({ &nz.halo (noise_hum, "Hum"), &nz.halo (noise_tone, "Tone") }, H);
            nz.row ({ &nz.halo (noise_dyn, "Dynamic") }, H);

            auto& gr = col ("Grain", "02", gran_on, 1);
            gr.row ({ &gr.rune (gran_freeze, "Freeze"), &gr.selector (gran_div, "Stutter") }, S);
            gr.row ({ &gr.halo (gran_size, "Size"), &gr.halo (gran_density, "Density") }, H);
            gr.row ({ &gr.halo (gran_pitch, "Pitch"), &gr.halo (gran_stretch, "Stretch") }, H);
            gr.row ({ &gr.halo (gran_spray, "Spray"), &gr.halo (gran_reverse, "Reverse") }, H);
            gr.row ({ &gr.halo (gran_chance, "Chance"), &gr.halo (gran_mix, "Mix") }, H);

            auto& fm = col ("FM", "27", fm_on, 1);
            fm.row ({ &fm.selector (fm_ratio, "Ratio") }, S);
            fm.row ({ &fm.halo (fm_index, "Index"), &fm.halo (fm_feedback, "Feedback") }, H);
            fm.row ({ &fm.halo (fm_env, "Env"), &fm.halo (fm_decay, "Decay") }, H);
            fm.row ({ &fm.halo (fm_fine, "Fine"), &fm.halo (fm_key, "Key Track") }, H);
            fm.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {   // two cycles of the oscillator as the operator bends it (the index at its knob, before the envelope)
                drawCaps (g, "phase bend", r.withHeight (12.0f), 9.0f, pewter, juce::Justification::centredLeft);
                auto a = r.withTrimmedTop (16.0f).withHeight (std::min (52.0f, r.getHeight() - 16.0f));
                if (a.getHeight() < 8.0f) return;
                const float ratio = kFmRatios[(size_t) juce::jlimit (0, (int) std::size (kFmRatios) - 1, juce::roundToInt (plain (p, fm_ratio)))]
                                  * std::exp2 (plain (p, fm_fine) / 1200.0f);
                const float x = isOn (p, fm_on) ? plain (p, fm_index) : 0.0f;
                const float depth = 12.0f * x * x / dsp::kTwoPi;   // cycles, as in the voice
                const float morph = plain (p, wt_morph);
                const auto bank = p.currentTableShared();
                if (bank == nullptr) return;
                drawWave (g, a, [&] (float u)
                {
                    const float t = 2.0f * u;   // two carrier cycles
                    float ph = t + depth * std::sin (dsp::kTwoPi * t * ratio);
                    ph -= std::floor (ph);
                    return bank->readMorph (morph, 1, std::min (ph, 0.9999f));
                }, violet, 1.0f);
                g.setColour (seamHi);
                g.fillRect (juce::Rectangle<float> (a.getX(), a.getCentreY(), a.getWidth(), 1.0f));
            };
            gr.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                const bool loop = p.telemetry.grainLooping.load();
                g.setColour (loop ? violet : pewter.withAlpha (0.5f));
                const auto c = juce::Point<float> (r.getX() + 6.0f, r.getY() + 8.0f);
                if (loop) g.fillPath (diamond (c, 9.0f)); else g.strokePath (diamond (c, 9.0f), juce::PathStrokeType (1.0f));
                drawCaps (g, loop ? "looping" : "listening", r.withHeight (16.0f).withTrimmedLeft (16.0f), 9.5f,
                          loop ? frost : pewter, juce::Justification::centredLeft);
            };
            break;
        }

        case 1:   // ── II · RUIN
        {
            auto& tl = col ("Tilt", "15", -1, 1);
            tl.row ({ &tl.halo (pre_tilt, "Pre"), &tl.halo (pre_pivot, "Pivot") }, H);
            tl.row ({ &tl.halo (post_tilt, "Post"), &tl.halo (post_pivot, "Pivot") }, H);
            tl.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                auto a = r.withHeight (std::min (r.getHeight(), 70.0f));
                drawCaps (g, "pre", a.withHeight (12.0f), 9.0f, violet, juce::Justification::centredLeft);
                drawCaps (g, "post", a.withHeight (12.0f), 9.0f, frost, juce::Justification::centredRight);
                a = a.withTrimmedTop (16.0f);
                g.setColour (seamHi);
                g.fillRect (juce::Rectangle<float> (a.getX(), a.getCentreY(), a.getWidth(), 1.0f));
                for (int k = 0; k < 2; ++k)
                {
                    const float tilt = plain (p, k == 0 ? pre_tilt : post_tilt), piv = plain (p, k == 0 ? pre_pivot : post_pivot);
                    juce::Path path;
                    for (int i = 0; i <= 80; ++i)
                    {
                        const float t = (float) i / 80.0f, f = 20.0f * std::pow (1000.0f, t);
                        const float x = f / piv;
                        const float db = tilt * 0.5f * (x * x - 1.0f) / (x * x + 1.0f);   // first-order shelf pair
                        const auto pt = juce::Point<float> (a.getX() + t * a.getWidth(), a.getCentreY() - db / 12.0f * a.getHeight() * 0.48f);
                        if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
                    }
                    g.setColour (k == 0 ? violet : frost.withAlpha (0.7f));
                    g.strokePath (path, juce::PathStrokeType (1.3f));
                }
            };

            auto& tb = col ("Tube", "06", tube_on, 1);
            tb.row ({ &tb.selector (tube_mode, "Topology") }, S);
            tb.row ({ &tb.halo (tube_drive, "Drive"), &tb.halo (tube_bias, "Bias") }, H);
            tb.row ({ &tb.halo (tube_mix, "Mix"), &tb.halo (tube_out, "Out") }, H);
            tb.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                const float side = std::min (r.getWidth(), r.getHeight());
                auto a = juce::Rectangle<float> (side, side).withCentre (r.getCentre());
                frame (g, a);
                g.setColour (seam);
                g.fillRect (juce::Rectangle<float> (a.getX(), a.getCentreY(), a.getWidth(), 1.0f));
                g.fillRect (juce::Rectangle<float> (a.getCentreX(), a.getY(), 1.0f, a.getHeight()));
                const int mode = juce::roundToInt (plain (p, tube_mode));
                const float gain = std::pow (10.0f, plain (p, tube_drive) * 2.0f), b = plain (p, tube_bias);
                juce::Path path;
                for (int i = 0; i <= 100; ++i)
                {
                    const float x = -1.0f + 2.0f * (float) i / 100.0f;
                    const float y = juce::jlimit (-1.2f, 1.2f, dsp::TubeDrive::shape (x * gain * 0.5f, b, mode));
                    const auto pt = juce::Point<float> (a.getCentreX() + x * side * 0.46f, a.getCentreY() - y * side * 0.4f);
                    if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
                }
                g.setColour (violet);
                g.strokePath (path, juce::PathStrokeType (1.5f));
            };

            auto& bs = col ("Bias", "10", bias_on, 1);
            bs.row ({ &bs.halo (bias_amount, "Offset"), &bs.halo (bias_sag, "Sag") }, H);
            bs.row ({ &bs.halo (bias_sat, "Tape Sat"), &bs.halo (bias_speed, "Recover") }, H);

            auto& cr = col ("Crush", "09", crush_on, 1);
            cr.row ({ &cr.halo (crush_bits, "Bits"), &cr.halo (crush_rate, "Rate") }, H);
            cr.row ({ &cr.halo (crush_pre, "Pre Flt"), &cr.halo (crush_post, "Post Flt") }, H);
            cr.row ({ &cr.halo (crush_mix, "Mix") }, H);
            cr.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                auto a = r.withHeight (std::min (r.getHeight(), 50.0f));
                const float levels = std::exp2 (plain (p, crush_bits) - 1.0f);
                const float steps = juce::jlimit (2.0f, 80.0f, plain (p, crush_rate) / 600.0f);
                juce::Path path;
                for (int i = 0; i <= 200; ++i)
                {
                    const float t = (float) i / 200.0f;
                    const float held = std::floor (t * steps) / steps;
                    float y = std::sin (dsp::kTwoPi * held);
                    y = std::round (y * levels) / levels;
                    const auto pt = juce::Point<float> (a.getX() + t * a.getWidth(), a.getCentreY() - y * a.getHeight() * 0.45f);
                    if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
                }
                g.setColour (violet);
                g.strokePath (path, juce::PathStrokeType (1.2f));
            };

            auto& vh = col ("VHS", "07", vhs_on, 1);
            vh.row ({ &vh.halo (vhs_wow, "Wow"), &vh.halo (vhs_flutter, "Flutter") }, H);
            vh.row ({ &vh.halo (vhs_drift, "Drift"), &vh.halo (vhs_rate, "Wow Rate") }, H);
            vh.row ({ &vh.halo (vhs_age, "Age") }, H);

            auto& mt = col ("Motor", "08", -1, 1);
            mt.row ({ &mt.selector (tape_mode, "Trigger") }, S);
            mt.row ({ &mt.halo (tape_start, "Start"), &mt.halo (tape_stop, "Stop") }, H);
            mt.instrument = [&p, angle = 0.0f] (juce::Graphics& g, juce::Rectangle<float> r) mutable
            {
                const float sp = p.telemetry.tapeSpeed.load();
                bar (g, r.withHeight (20.0f), sp / 1.3f, violet, "capstan", juce::String (sp, 2) + "x");
                // spinning reel: a ring of three diamonds rotating with tape speed (per-instance state)
                angle = std::fmod (angle + sp * 0.18f, dsp::kTwoPi);
                if (angle < 0.0f) angle += dsp::kTwoPi;
                const auto c = juce::Point<float> (r.getCentreX(), r.getY() + 34.0f + std::min (36.0f, (r.getHeight() - 34.0f) * 0.5f));
                const float rad = std::min (26.0f, (r.getHeight() - 34.0f) * 0.45f);
                if (rad > 6.0f)
                {
                    g.setColour (seamHi);
                    g.drawEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (c), 1.0f);
                    for (int k = 0; k < 3; ++k)
                    {
                        const float a = angle + (float) k * dsp::kTwoPi / 3.0f;
                        g.setColour (k == 0 ? violet : frost.withAlpha (0.6f));
                        g.fillPath (diamond ({ c.x + rad * 0.62f * std::cos (a), c.y + rad * 0.62f * std::sin (a) }, 6.0f));
                    }
                }
            };
            break;
        }

        case 2:   // ── III · SHAPE
        {
            auto& ld = col ("Ladders", "11", flt_on, 1.9f);
            ld.row ({ &ld.selector (flt_route, "Routing") }, S);
            ld.row ({ &ld.halo (flt_a_cut, "A Cutoff"), &ld.halo (flt_a_res, "A Res"), &ld.halo (flt_b_cut, "B Cutoff"), &ld.halo (flt_b_res, "B Res") }, H);
            ld.row ({ &ld.halo (flt_drive, "Drive"), &ld.halo (flt_env, "Env Amt"), &ld.halo (flt_key, "Key Trk") }, H);
            ld.row ({ &ld.vial (fenv_a, "Attack", "A"), &ld.vial (fenv_d, "Decay", "D"), &ld.vial (fenv_s, "Sustain", "S"), &ld.vial (fenv_r, "Release", "R") }, V);

            auto& ef = col ("Follower", "12", envf_on, 1.0f);
            ef.row ({ &ef.halo (envf_cut, "Base"), &ef.halo (envf_depth, "Depth") }, H);
            ef.row ({ &ef.halo (envf_res, "Res"), &ef.halo (envf_mix, "Mix") }, H);
            ef.row ({ &ef.halo (envf_attack, "Attack"), &ef.halo (envf_release, "Release") }, H);
            ef.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                const float e = p.telemetry.followerEnv.load();
                bar (g, r.withHeight (20.0f), e, violet, "envelope", juce::String (juce::roundToInt (e * 100.0f)) + "%");
            };

            auto& um = col ("Unmask", "13", unmask_on, 0.85f);
            um.row ({ &um.halo (unmask_freq, "Sub Band"), &um.halo (unmask_depth, "Depth") }, H);
            um.row ({ &um.halo (unmask_release, "Release") }, H);
            um.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                const float db = p.telemetry.unmaskDb.load();
                bar (g, r.withHeight (20.0f), db / 24.0f, chlorine, "carving", db < 0.05f ? juce::String ("0.0 dB") : "-" + juce::String (db, 1) + " dB");
                g.setColour (pewter);
                g.setFont (type::value (11.5f));
                g.drawFittedText ("Keyed by the sub. Cuts the drone and body only where the sub is speaking.",
                                  r.withTrimmedTop (28.0f).withHeight (80.0f).toNearestInt(), juce::Justification::topLeft, 6);
            };

            auto& fm = col ("Vox", "14", form_on, 0.85f);
            fm.row ({ &fm.halo (form_vowel, "Vowel"), &fm.halo (form_body, "Cello") }, H);
            fm.row ({ &fm.halo (form_shift, "Shift"), &fm.halo (form_res, "Res") }, H);
            fm.row ({ &fm.halo (form_mix, "Mix") }, H);
            fm.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                const float v = plain (p, form_vowel) + p.telemetry.modOffset[form_vowel].load() * 4.0f;
                static const char* letters = "AEIOU";
                const int a = juce::jlimit (0, 4, juce::roundToInt (v));
                g.setColour (frost.withAlpha (0.9f));
                g.setFont (type::display (std::min (54.0f, r.getHeight() * 0.9f)));
                g.drawText (juce::String::charToString ((juce::juce_wchar) letters[a]), r, juce::Justification::centred, false);
            };

            auto& rs = col ("Resonator", "26", res_on, 1.7f);
            rs.row ({ &rs.selector (res_model, "Model") }, S);
            rs.row ({ &rs.halo (res_tune, "Tune"), &rs.halo (res_decay, "Decay"), &rs.halo (res_damper, "Damper") }, H);
            rs.row ({ &rs.halo (res_bright, "Bright"), &rs.halo (res_stiff, "Stiff"), &rs.halo (res_pos, "Position") }, H);
            rs.row ({ &rs.halo (res_strike, "Strike"), &rs.halo (res_input, "Input"), &rs.halo (res_mix, "Mix") }, H);
            rs.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                // the model's partials on a log-frequency ruler (1..16x the note), height = how long each rings
                const int model = juce::roundToInt (plain (p, res_model));
                const float bright = plain (p, res_bright), stiff = plain (p, res_stiff), pos = plain (p, res_pos);
                static const char* names[] { "String", "Tube", "Bell", "Bar", "Membrane", "Plate", "Bowl", "Gong" };
                g.setColour (frost.withAlpha (0.9f));
                g.setFont (type::display (17.0f));
                g.drawText (names[juce::jlimit (0, 7, model)], r.withHeight (20.0f), juce::Justification::centredLeft, false);
                drawCaps (g, model <= 1 ? "waveguide" : "modal", r.withHeight (20.0f), 8.5f, pewter, juce::Justification::centredRight, 0.22f);

                auto ruler = r.withTrimmedTop (26.0f).withTrimmedBottom (14.0f);
                const float lo = std::log2 (0.5f), hi = std::log2 (16.0f);
                auto xOf = [&] (float ratio) { return ruler.getX() + (std::log2 (ratio) - lo) / (hi - lo) * ruler.getWidth(); };
                drawRule (g, ruler.getBottom(), ruler.getX(), ruler.getRight(), seamHi);
                for (float o : { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f })
                {
                    g.setColour (seamHi);
                    g.fillRect (juce::Rectangle<float> (xOf (o), ruler.getBottom(), 1.0f, 4.0f));
                }
                using R = dsp::Resonator;
                std::array<float, 16> ratios {}, nodes {};   // nodes < 0: the generic line weight from the mode's index
                size_t nPartials = 0;
                auto pushPartial = [&] (float rVal, float nVal) { if (nPartials < ratios.size()) { ratios[nPartials] = rVal; nodes[nPartials] = nVal; ++nPartials; } };
                const float warpExp = 1.0f + 0.18f * (stiff - 0.15f);
                const float px = 0.05f + 0.9f * pos, py = 0.21f + 0.62f * px;
                auto addAll = [&] (const float* xs, int n, bool warp) { for (int k = 0; k < n; ++k) pushPartial (warp ? std::pow (xs[k], warpExp) : xs[k], -1.0f); };
                if (model == 0) for (int k = 1; k <= 16; ++k) pushPartial ((float) k * std::sqrt (1.0f + 0.0016f * stiff * (float) (k * k)), -1.0f);
                else if (model == 1) for (int k = 1; k <= 15; k += 2) pushPartial ((float) k, -1.0f);
                else if (model == 2) addAll (R::kBell, 7, true);
                else if (model == 3) addAll (R::kBar, 5, true);
                else if (model == 5)
                    for (int k = 0; k < 12; ++k)
                        pushPartial (std::pow (R::kPlate[k], warpExp),
                                     0.15f + 0.85f * std::abs (std::sin (dsp::kPi * (float) R::kPlateM[k] * px) * std::sin (dsp::kPi * (float) R::kPlateN[k] * py)));
                else if (model == 6) addAll (R::kRing, 6, false);
                else if (model == 7) addAll (R::kGong, 12, true);
                else                 addAll (R::kMemb, 7, true);
                for (size_t k = 0; k < nPartials; ++k)
                {
                    const float ratio = ratios[k];
                    if (ratio > 16.0f) continue;
                    const float ring = ratio <= 1.0f ? 1.0f : 1.0f / (1.0f + (1.0f - bright) * (ratio - 1.0f) * 0.7f);
                    const float node = nodes[k] >= 0.0f ? nodes[k]
                                     : model <= 1 ? std::abs (std::sin (dsp::kPi * ratio * pos))
                                                  : 0.25f + 0.75f * std::abs (std::sin (dsp::kPi * (float) (k + 1) * pos));
                    const float hgt = ruler.getHeight() * juce::jlimit (0.06f, 1.0f, ring * (0.3f + 0.7f * node));
                    const float x = xOf (ratio);
                    g.setColour (violet.withAlpha (0.35f + 0.6f * ring));
                    g.fillRect (juce::Rectangle<float> (x - 0.75f, ruler.getBottom() - hgt, 1.5f, hgt));
                    g.fillPath (diamond ({ x, ruler.getBottom() - hgt }, 5.0f));
                }
                g.setColour (pewter);
                g.setFont (type::value (10.5f));
                g.drawText ("note", juce::Rectangle<float> (xOf (1.0f) - 20.0f, ruler.getBottom() + 3.0f, 40.0f, 12.0f), juce::Justification::centred, false);
                g.drawText ("16x", juce::Rectangle<float> (ruler.getRight() - 30.0f, ruler.getBottom() + 3.0f, 30.0f, 12.0f), juce::Justification::centredRight, false);
            };
            break;
        }

        case 3:   // ── IV · SPACE
        {
            auto& z = col ("Z-Depth", "18", z_on, 1);
            z.row ({ &z.halo (z_depth, "Depth") }, H);
            z.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {   // nested frames in perspective; the dot sits at the source's depth
                const float d = plain (p, z_depth) + p.telemetry.modOffset[z_depth].load() * 2.0f;
                const float side = std::min (r.getWidth(), r.getHeight());
                auto a = juce::Rectangle<float> (side, side).withCentre (r.getCentre());
                for (int k = 0; k < 6; ++k)
                {
                    const float s = 1.0f - (float) k * 0.15f;
                    g.setColour (pewter.withAlpha (0.12f + 0.06f * (float) k));
                    g.drawRect (juce::Rectangle<float> (a.getWidth() * s, a.getHeight() * s).withCentre (a.getCentre()), 1.0f);
                }
                const float s = juce::jmap (juce::jlimit (-1.0f, 1.0f, d), -1.0f, 1.0f, 0.25f, 1.0f);
                g.setColour (violet);
                g.drawRect (juce::Rectangle<float> (a.getWidth() * s, a.getHeight() * s).withCentre (a.getCentre()), 1.6f);
                g.fillPath (diamond (a.getCentre(), 6.0f + 8.0f * s));
                drawCaps (g, d < -0.05f ? "behind" : (d > 0.05f ? "in front" : "on the glass"), a.withTop (a.getBottom() + 2.0f).withHeight (14.0f), 9.0f, pewter);
            };

            auto& cv = col ("Body", "17", conv_on, 1);
            cv.row ({ &cv.selector (conv_ir, "Space") }, S);
            cv.row ({ &cv.halo (conv_size, "Size"), &cv.halo (conv_mix, "Mix") }, H);
            auto& load = cv.button ("Load IR");
            cv.row ({ &load }, R);
            load.onClick = [alive = p.aliveToken(), procPtr = &p]
            {
                auto chooser = std::make_shared<juce::FileChooser> ("Load an impulse response", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                      [alive, procPtr, chooser] (const juce::FileChooser& fc)
                                      {
                                          if (alive != nullptr && alive->load (std::memory_order_acquire) && fc.getResult().existsAsFile())
                                              procPtr->loadUserImpulse (fc.getResult());
                                      });
            };
            cv.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                const int t = juce::jlimit (0, 5, juce::roundToInt (plain (p, conv_ir)));
                static const char* names[] { "Concrete Pipe", "Old TV Speaker", "Stairwell", "Water Tank", "Cassette Shell", "User" };
                juce::String n = t == 5 ? p.userImpulseName() : juce::String (names[t]);
                g.setColour (frost);
                g.setFont (type::display (17.0f));
                g.drawFittedText (n, r.withHeight (40.0f).toNearestInt(), juce::Justification::centred, 2);
            };

            auto& bh = col ("Black Hole", "16", verb_on, 2);
            bh.row ({ &bh.rune (verb_inf, "Infinite") }, R);
            bh.row ({ &bh.halo (verb_size, "Size"), &bh.halo (verb_decay, "Decay"), &bh.halo (verb_damp, "Damping"), &bh.halo (verb_mix, "Mix") }, H);
            bh.row ({ &bh.halo (verb_mod, "Motion"), &bh.halo (verb_pre, "Pre-Delay"), &bh.halo (verb_low, "Low Cut") }, H);
            bh.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {   // an accretion disc: rings tighten toward the event — infinite shows a closed singularity
                const bool inf = isOn (p, verb_inf);
                const float size = plain (p, verb_size);
                const auto c = r.getCentre();
                const float maxR = std::min (r.getWidth(), r.getHeight()) * 0.48f;
                for (int k = 0; k < 9; ++k)
                {
                    const float t = (float) k / 8.0f;
                    const float rr = maxR * (0.18f + 0.82f * std::pow (t, 0.6f + (1.0f - size)));
                    g.setColour (violet.withAlpha (0.08f + 0.5f * (1.0f - t)));
                    g.drawEllipse (juce::Rectangle<float> (rr * 2.6f, rr * 0.9f).withCentre (c), 1.0f);
                }
                g.setColour (inf ? frost : voidc);
                g.fillEllipse (juce::Rectangle<float> (maxR * 0.3f, maxR * 0.3f).withCentre (c));
                g.setColour (violet);
                g.drawEllipse (juce::Rectangle<float> (maxR * 0.3f, maxR * 0.3f).withCentre (c), 1.2f);
            };

            auto& hs = col ("Haas", "19", haas_on, 1);
            hs.row ({ &hs.halo (haas_time, "Time"), &hs.halo (haas_width, "Width") }, H);
            hs.row ({ &hs.halo (haas_xover, "Above") }, H);
            hs.instrument = [] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                g.setColour (pewter);
                g.setFont (type::value (11.5f));
                g.drawFittedText ("Delay goes to the side channel only. Mono fold-down is untouched.",
                                  r.withHeight (48.0f).toNearestInt(), juce::Justification::topLeft, 4);
            };
            break;
        }

        case 4:   // ── V · PRESSURE
        default:
        {
            auto& mb = col ("Multiband", "21", ott_on, 3);
            mb.row ({ &mb.halo (ott_depth, "Depth"), &mb.halo (ott_time, "Time"), &mb.halo (ott_up, "Upward"),
                      &mb.halo (ott_down, "Downward"), &mb.halo (ott_in, "In"), &mb.halo (ott_out, "Out") }, H);
            mb.row ({ &mb.halo (ott_lox, "Low X"), &mb.halo (ott_hix, "High X"), &mb.halo (ott_low, "Low"),
                      &mb.halo (ott_mid, "Mid"), &mb.halo (ott_high, "High") }, H);
            mb.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                static const char* names[] { "low", "mid", "high" };
                const float w = (r.getWidth() - 24.0f) / 3.0f;
                float gains[3];
                for (int b = 0; b < 3; ++b)
                {
                    gains[b] = p.telemetry.ottGain[b].load();
                    bar (g, juce::Rectangle<float> (r.getX() + (float) b * (w + 12.0f), r.getY(), w, 20.0f), gains[b] / 18.0f,
                         gains[b] >= 0.0f ? chlorine : violet, names[b], (gains[b] > 0 ? "+" : "") + juce::String (gains[b], 1) + " dB", true);
                }

                // crossover map: log-frequency axis, each band's live gain as a raised or sunken plate
                auto a = r.withTrimmedTop (36.0f).withTrimmedBottom (4.0f);
                if (a.getHeight() < 40.0f) return;
                frame (g, a);
                auto xOf = [&] (float hz) { return a.getX() + a.getWidth() * std::log (hz / 20.0f) / std::log (1000.0f); };
                const float zero = a.getCentreY();
                g.setColour (seamHi);
                g.fillRect (juce::Rectangle<float> (a.getX(), zero, a.getWidth(), 1.0f));
                for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
                {
                    g.setColour (seam);
                    g.fillRect (juce::Rectangle<float> (xOf (f), a.getY(), 1.0f, a.getHeight()));
                    g.setColour (pewter.withAlpha (0.6f));
                    g.setFont (type::value (10.0f));
                    g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k" : juce::String ((int) f),
                                juce::Rectangle<float> (xOf (f) + 3.0f, a.getBottom() - 14.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);
                }
                const float lox = plain (p, ott_lox), hix = std::max (plain (p, ott_hix), lox * 2.0f);
                const float edges[4] { a.getX(), xOf (lox), xOf (hix), a.getRight() };
                for (int b = 0; b < 3; ++b)
                {
                    const float h = juce::jlimit (-1.0f, 1.0f, gains[b] / 18.0f) * a.getHeight() * 0.45f;
                    auto plate = juce::Rectangle<float> (edges[b] + 2.0f, std::min (zero, zero - h), edges[b + 1] - edges[b] - 4.0f, std::abs (h) + 1.0f);
                    g.setColour ((gains[b] >= 0.0f ? chlorine : violet).withAlpha (0.28f));
                    g.fillRect (plate);
                    g.setColour (gains[b] >= 0.0f ? chlorine : violet);
                    g.fillRect (plate.withHeight (1.5f).withY (zero - h - (h >= 0.0f ? 0.0f : 1.5f)));
                }
                for (int k = 1; k < 3; ++k)
                {
                    g.setColour (frost.withAlpha (0.8f));
                    g.fillRect (juce::Rectangle<float> (edges[k], a.getY(), 1.0f, a.getHeight()));
                    g.fillPath (invertedTriangle ({ edges[k], a.getY() + 6.0f }, 8.0f));
                }
                drawCaps (g, "upward  /  downward per band", a.withHeight (14.0f).translated (0.0f, 4.0f), 8.5f, pewter);
            };

            auto& dk = col ("Ducker", "23", duck_on, 1);
            dk.row ({ &dk.selector (duck_mode, "Mode") }, S);
            dk.row ({ &dk.halo (duck_depth, "Depth"), &dk.halo (duck_release, "Release") }, H);
            dk.instrument = [&p] (juce::Graphics& g, juce::Rectangle<float> r)
            {
                const float gdb = dsp::gainToDb (p.telemetry.duckGain.load());
                bar (g, r.withHeight (20.0f), -gdb / 24.0f, violet, "texture", juce::String (std::abs (gdb) < 0.05f ? 0.0f : gdb, 1) + " dB");
                g.setColour (pewter);
                g.setFont (type::value (11.5f));
                g.drawFittedText (juce::String::fromUTF8 ("Ducks the drone and dust so the sub punches through.\n\n"
                                                          "Follow \xe2\x80\x94 tracks the sub, sample-locked.\n"
                                                          "Pump \xe2\x80\x94 fires on every note-on, at the sub's zero crossing."),
                                  r.withTrimmedTop (30.0f).withHeight (std::min (120.0f, r.getHeight() - 30.0f)).toNearestInt(), juce::Justification::topLeft, 8);
            };
            break;
        }
    }
    return page;
}

//==============================================================================
Spine::Spine (VesselProcessor& p, Registry& reg) : proc (p)
{
    voice = std::make_unique<ModuleColumn> (p, reg, "Voice", "", -1);
    auto& v = *voice;
    v.row ({ &v.selector (voice_mode, "Mode"), &v.halo (glide, "Glide") }, ParamSlider::kHaloHeight);
    v.row ({ &v.vial (amp_a, "Attack", "A"), &v.vial (amp_d, "Decay", "D"), &v.vial (amp_s, "Sustain", "S"), &v.vial (amp_r, "Release", "R") },
           ParamSlider::kVialHeight);
    addAndMakeVisible (*voice);
    setRepaintsOnMouseActivity (false);
}

void Spine::resized()
{
    const int top = kTop + 5 * kStageH + 10;
    voice->setBounds (0, top, getWidth(), getHeight() - top);
}

int Spine::stageAt (juce::Point<int> p) const
{
    const int i = (p.y - kTop) / kStageH;
    return (p.y >= kTop && i >= 0 && i < 5) ? i : -1;
}

void Spine::mouseDown (const juce::MouseEvent& e)
{
    const int s = stageAt (e.getPosition());
    if (s >= 0) { selected = s; repaint(); if (onSelect) onSelect (s); }
}

void Spine::mouseMove (const juce::MouseEvent& e)
{
    const int h = stageAt (e.getPosition());
    if (h != hover) { hover = h; repaint(); }
}

void Spine::paint (juce::Graphics& g)
{
    g.setColour (basin.withAlpha (0.55f));
    g.fillAll();
    const float w = (float) getWidth();

    for (int i = 0; i < 5; ++i)
    {
        const auto r = juce::Rectangle<float> (0.0f, (float) (kTop + i * kStageH), w, (float) kStageH);
        const bool sel = i == selected, hot = i == hover;

        if (sel)
        {
            g.setColour (violet.withAlpha (0.07f));
            g.fillRect (r);
            g.setColour (violet);
            g.fillRect (juce::Rectangle<float> (0.0f, r.getY() + 8.0f, 2.0f, r.getHeight() - 16.0f));
            g.fillPath (invertedTriangle ({ w - 10.0f, r.getCentreY() }, 9.0f),
                        juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, w - 10.0f, r.getCentreY()));
        }

        g.setColour (sel ? violet : (hot ? frost : pewter.withAlpha (0.8f)));
        g.setFont (type::display (30.0f));
        g.drawText (kNumerals[i], juce::Rectangle<float> (12.0f, r.getY() + 4.0f, 46.0f, 40.0f), juce::Justification::centred, false);

        drawCaps (g, kStageNames[i], { 62.0f, r.getY() + 10.0f, w - 72.0f, 16.0f }, 11.5f, sel || hot ? frost : frost.withAlpha (0.75f),
                  juce::Justification::centredLeft, 0.24f);
        g.setColour (pewter);
        g.setFont (type::value (11.0f));
        g.drawText (juce::String::fromUTF8 (kStageCaps[i]), juce::Rectangle<float> (62.0f, r.getY() + 27.0f, w - 76.0f, 14.0f), juce::Justification::centredLeft, true);

        for (int k = 0; k < 5; ++k)
        {
            const int pid = kStagePids[i][k];
            if (pid < 0) continue;
            const bool on = vessel::spec (pid).type == PType::Bool ? isOn (proc, pid) : plain (proc, pid) > 0.5f;
            const juce::Point<float> c { 66.0f + (float) k * 12.0f, r.getY() + 50.0f };
            if (on) { g.setColour (violet); g.fillPath (diamond (c, 6.5f)); }
            else    { g.setColour (pewter.withAlpha (0.6f)); g.strokePath (diamond (c, 6.5f), juce::PathStrokeType (1.0f)); }
        }
        if (i < 4) drawRule (g, r.getBottom(), 10.0f, w - 10.0f, seam);
    }
    drawRule (g, (float) (kTop + 5 * kStageH + 4), 0.0f, w, seamHi);
}

//==============================================================================
Header::Header (VesselProcessor& p) : proc (p)
{
    for (auto* b : { &prev, &next }) addAndMakeVisible (*b);
    prev.onClick = [this] { const int n = proc.getNumPrograms(); proc.setCurrentProgram ((proc.getCurrentProgram() + n - 1) % n); repaint(); };
    next.onClick = [this] { const int n = proc.getNumPrograms(); proc.setCurrentProgram ((proc.getCurrentProgram() + 1) % n); repaint(); };
}

juce::Rectangle<int> Header::presetArea() const
{
    return juce::Rectangle<int> (300, 34).withCentre ({ getWidth() / 2, getHeight() / 2 + 2 });
}

void Header::resized()
{
    const auto a = presetArea();
    prev.setBounds (a.getX() - 4, a.getY(), 26, a.getHeight());
    next.setBounds (a.getRight() - 22, a.getY(), 26, a.getHeight());
}

void Header::mouseDown (const juce::MouseEvent& e)
{
    if (presetArea().reduced (26, 0).contains (e.getPosition()) && onBrowse) onBrowse();
    else if (lightsArea().contains (e.getPosition()) && onTorch) onTorch();
}

void Header::paint (juce::Graphics& g)
{
    const float w = (float) getWidth(), h = (float) getHeight();
    g.setColour (voidc.withAlpha (0.55f));
    g.fillAll();

    // sigil + wordmark
    g.setColour (violet);
    g.fillPath (invertedTriangle ({ 24.0f, h * 0.5f }, 15.0f));
    g.setColour (frost);
    g.setFont (type::display (36.0f));
    g.drawText ("Vessel", juce::Rectangle<float> (40.0f, 4.0f, 140.0f, h - 6.0f), juce::Justification::centredLeft, false);
    drawCaps (g, "sub  /  texture  /  ruin", { 162.0f, h * 0.5f - 9.0f, 200.0f, 14.0f }, 9.0f, pewter, juce::Justification::centredLeft, 0.28f);
    g.setColour (violet.withAlpha (0.85f));
    g.setFont (type::display (14.0f));
    g.drawText ("by iisben", juce::Rectangle<float> (162.0f, h * 0.5f + 3.0f, 120.0f, 16.0f), juce::Justification::centredLeft, false);

    // version badge (auto-synced with CMakeLists.txt via JucePlugin_VersionString)
    {
       #ifdef JucePlugin_VersionString
        const juce::String verText = "v" + juce::String (JucePlugin_VersionString);
       #else
        const juce::String verText = "v2.0.3";
       #endif
        const float byW = type::width (type::display (14.0f), "by iisben");
        const auto vf = type::value (10.5f);
        const float badgeW = type::width (vf, verText) + 12.0f;
        const juce::Rectangle<float> badge (162.0f + byW + 10.0f, h * 0.5f + 3.5f, badgeW, 15.0f);
        g.setColour (violet.withAlpha (0.14f));
        g.fillRoundedRectangle (badge, 3.5f);
        g.setColour (violet.withAlpha (0.48f));
        g.drawRoundedRectangle (badge, 3.5f, 1.0f);
        g.setColour (frost.withAlpha (0.92f));
        g.setFont (vf);
        g.drawText (verText, badge, juce::Justification::centred, false);
    }

    // preset
    const auto a = presetArea().toFloat();
    drawRule (g, a.getBottom(), a.getX(), a.getRight(), seamHi);
    g.setColour (frost);
    g.setFont (type::display (20.0f));
    const auto name = proc.getProgramName (proc.getCurrentProgram());
    g.drawText (name, a.reduced (26.0f, 0.0f), juce::Justification::centred, false);
    drawCaps (g, "preset", a.withY (a.getY() - 13.0f).withHeight (12.0f), 8.5f, pewter);
    {   // drop-down sigil after the name: violet and pointing up while the browser is open
        const float nx = a.getCentreX() + std::min (a.getWidth() * 0.5f - 34.0f, type::width (type::display (20.0f), name) * 0.5f + 12.0f);
        const auto tri = invertedTriangle ({ nx, a.getCentreY() + 1.0f }, 7.0f);
        g.setColour (browserOpen ? violet : pewter);
        g.fillPath (tri, browserOpen ? juce::AffineTransform::rotation (juce::MathConstants<float>::pi, nx, a.getCentreY() + 1.0f) : juce::AffineTransform());
    }

    // status runes, right
    struct Lamp { const char* name; bool on; juce::Colour c; };
    const auto& t = proc.telemetry;
    const Lamp lamps[] {
        { "sub",   t.subLevel.load() > 0.01f, violet },
        { "loop",  t.grainLooping.load(), violet },
        { "motor", t.tapeSpeed.load() < 0.995f, chlorine },
        { "clip",  t.clipCount.load() > 0, blood } };
    // lights out: a real button, far right, and it stays readable in the dark (the torch leaves it unlit)
    {
        const bool on = proc.torchOn.load();
        const juce::Colour amber (0xffffb45a);
        const auto r = lightsArea().toFloat().reduced (0.5f);
        g.setColour (on ? amber.withAlpha (0.16f) : frost.withAlpha (0.04f));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (on ? amber : pewter.withAlpha (0.8f));
        g.drawRoundedRectangle (r, 5.0f, 1.2f);
        const juce::Point<float> bulb { r.getX() + 15.0f, r.getCentreY() };   // a torch lens
        if (on) { g.setColour (amber.withAlpha (0.35f)); g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (bulb)); }
        g.setColour (on ? amber : frost.withAlpha (0.85f));
        g.drawEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (bulb), 1.3f);
        if (on) g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (bulb));
        drawCaps (g, "lights out", r.withTrimmedLeft (26.0f), 9.5f, on ? amber : frost, juce::Justification::centredLeft, 0.22f);
    }
    float x = (float) lightsArea().getX() - 20.0f;
    for (int i = 3; i >= 0; --i)
    {
        const auto& l = lamps[i];
        const float tw = type::width (type::caps (9.0f, 0.2f), juce::String (l.name).toUpperCase()) + 4.0f;
        x -= tw;
        drawCaps (g, l.name, { x, h * 0.5f - 7.0f, tw, 14.0f }, 9.0f, l.on ? frost : pewter.withAlpha (0.6f), juce::Justification::centredLeft, 0.2f);
        const juce::Point<float> c { x - 9.0f, h * 0.5f };
        if (l.on) { g.setColour (l.c); g.fillPath (diamond (c, 7.0f)); }
        else      { g.setColour (pewter.withAlpha (0.5f)); g.strokePath (diamond (c, 7.0f), juce::PathStrokeType (1.0f)); }
        x -= 26.0f;
    }
    drawRule (g, h - 1.0f, 0.0f, w, seamHi);
}

//==============================================================================
MasterRail::MasterRail (VesselProcessor& p, Registry& reg) : proc (p)
{
    const int H = ParamSlider::kHaloHeight;
    clip = std::make_unique<ModuleColumn> (p, reg, "Clipper", "25", clip_on);
    clip->row ({ &clip->halo (clip_ceiling, "Ceiling"), &clip->halo (out_gain, "Output"), &clip->halo (in_level, "Input") }, H);
    exciter = std::make_unique<ModuleColumn> (p, reg, "Exciter", "24", exc_on);
    exciter->row ({ &exciter->halo (exc_freq, "Above"), &exciter->halo (exc_amount, "Amount"), &exciter->halo (exc_harm, "Bite") }, H);
    mono = std::make_unique<ModuleColumn> (p, reg, "Mono Below", "22", mono_on);
    mono->row ({ &mono->halo (mono_freq, "Collapse") }, H);
    for (auto* c : { clip.get(), exciter.get(), mono.get() }) addAndMakeVisible (*c);
}

void MasterRail::resized()
{
    const int w = getWidth();
    meterArea = { 10.0f, 34.0f, 58.0f, 180.0f };
    scopeArea = { 78.0f, 44.0f, (float) w - 90.0f, (float) w - 90.0f };
    int y = 222;
    for (auto* c : { clip.get(), exciter.get(), mono.get() }) { c->setBounds (0, y, w, 132); y += 134; }
    telemetryArea = { 14.0f, (float) y + 6.0f, (float) w - 28.0f, (float) (getHeight() - y - 12) };
}

void MasterRail::refresh()
{
    for (auto* c : { clip.get(), exciter.get(), mono.get() }) c->refresh();
    const int cc = proc.telemetry.clipCount.exchange (0);
    if (cc > 0) clipFlash = 1.0f; else clipFlash *= 0.86f;
    lastClipCount = cc;
    repaint (meterArea.toNearestInt().expanded (6));
    repaint (scopeArea.toNearestInt().expanded (6));
    repaint (telemetryArea.toNearestInt().expanded (4));
}

void MasterRail::paint (juce::Graphics& g)
{
    g.setColour (basin.withAlpha (0.55f));
    g.fillAll();
    drawSeam (g, 0.0f, 0.0f, (float) getHeight());
    drawCaps (g, "Vessel", { 12.0f, 8.0f, 120.0f, 18.0f }, 11.5f, frost, juce::Justification::centredLeft, 0.22f);
    g.setColour (pewter.withAlpha (0.75f));
    g.setFont (type::display (15.0f));
    g.drawText ("out", juce::Rectangle<float> ((float) getWidth() - 60.0f, 6.0f, 48.0f, 22.0f), juce::Justification::centredRight, false);
    drawRule (g, 32.0f, 0.0f, (float) getWidth(), seam);

    paintMeter (g, meterArea);
    paintScope (g, scopeArea);
    for (auto* c : { clip.get(), exciter.get(), mono.get() }) drawRule (g, (float) c->getY() - 1.0f, 0.0f, (float) getWidth(), seam);
    paintTelemetry (g, telemetryArea);
}

void MasterRail::paintMeter (juce::Graphics& g, juce::Rectangle<float> r)
{
    auto toY = [&] (float db, juce::Rectangle<float> glass)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 63.0f);
        return glass.getBottom() - t * glass.getHeight();
    };
    const float ceilDb = plain (proc, clip_ceiling);
    const float peaks[2] { proc.telemetry.outPeakL.load(), proc.telemetry.outPeakR.load() };

    for (int c = 0; c < 2; ++c)
    {
        auto glass = juce::Rectangle<float> (r.getX() + 4.0f + (float) c * 18.0f, r.getY() + 14.0f, 13.0f, r.getHeight() - 22.0f);
        juce::Path cap;
        cap.addRoundedRectangle (glass, 6.5f);
        const float ly = toY (dsp::gainToDb (peaks[c]), glass);
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (cap);
            g.setColour (voidc.withAlpha (0.6f));
            g.fillPath (cap);
            juce::ColourGradient liquid (violet.withAlpha (0.85f), 0, ly, violet.withAlpha (0.12f), 0, glass.getBottom(), false);
            g.setGradientFill (liquid);
            g.fillRect (glass.withTop (ly));
            juce::Path m;
            m.startNewSubPath (glass.getX(), ly - 1.5f);
            m.quadraticTo (glass.getCentreX(), ly + 1.8f, glass.getRight(), ly - 1.5f);
            g.setColour (frost);
            g.strokePath (m, juce::PathStrokeType (1.3f));
        }
        g.setColour (frost.withAlpha (0.22f));
        g.strokePath (cap, juce::PathStrokeType (1.0f));
        g.setColour (blood.withAlpha (0.8f));
        g.fillRect (juce::Rectangle<float> (glass.getX() - 2.0f, toY (ceilDb, glass), glass.getWidth() + 4.0f, 1.0f));
        drawCaps (g, c == 0 ? "L" : "R", { glass.getX() - 2.0f, glass.getBottom() + 2.0f, glass.getWidth() + 4.0f, 10.0f }, 8.5f, pewter);
    }

    // scale
    const auto glass = juce::Rectangle<float> (r.getX() + 4.0f, r.getY() + 14.0f, 13.0f, r.getHeight() - 22.0f);
    g.setFont (type::value (10.0f));
    for (int db : { 0, -6, -12, -24, -48 })
    {
        const float y = toY ((float) db, glass);
        g.setColour (pewter.withAlpha (0.6f));
        g.drawText (juce::String (db), juce::Rectangle<float> (r.getX() + 40.0f, y - 6.0f, 20.0f, 12.0f), juce::Justification::centredLeft, false);
    }

    // clip rune
    const juce::Point<float> c { r.getX() + 19.0f, r.getY() + 4.0f };
    if (clipFlash > 0.05f) { g.setColour (blood.withAlpha (clipFlash)); g.fillPath (diamond (c, 9.0f)); }
    else { g.setColour (pewter.withAlpha (0.5f)); g.strokePath (diamond (c, 9.0f), juce::PathStrokeType (1.0f)); }
}

void MasterRail::paintScope (juce::Graphics& g, juce::Rectangle<float> r)
{
    frame (g, r);
    const auto c = r.getCentre();
    g.setColour (seam);
    g.drawLine (r.getX(), r.getY(), r.getRight(), r.getBottom(), 1.0f);
    g.drawLine (r.getRight(), r.getY(), r.getX(), r.getBottom(), 1.0f);
    g.setColour (seamHi);
    g.fillRect (juce::Rectangle<float> (c.x, r.getY(), 1.0f, r.getHeight()));
    drawCaps (g, "M", { c.x - 10.0f, r.getY() + 2.0f, 20.0f, 10.0f }, 8.0f, pewter);
    drawCaps (g, "L", { r.getX() + 3.0f, r.getY() + 2.0f, 10.0f, 10.0f }, 8.0f, pewter);
    drawCaps (g, "R", { r.getRight() - 13.0f, r.getY() + 2.0f, 10.0f, 10.0f }, 8.0f, pewter);

    const auto& t = proc.telemetry;
    const int w = t.scopeWrite.load (std::memory_order_acquire);
    const float half = r.getWidth() * 0.5f;
    g.setColour (frost.withAlpha (0.42f));
    for (int i = 0; i < 700; ++i)
    {
        const int idx = (w - 1 - i + Telemetry::kScope) % Telemetry::kScope;
        const float L = t.scopeL[(size_t) idx].load (std::memory_order_relaxed), R = t.scopeR[(size_t) idx].load (std::memory_order_relaxed);
        const float x = juce::jlimit (-1.0f, 1.0f, (L - R) * 0.75f), y = juce::jlimit (-1.0f, 1.0f, (L + R) * 0.75f);
        g.fillRect (juce::Rectangle<float> (c.x + x * half - 0.6f, c.y - y * half - 0.6f, 1.3f, 1.3f));
    }
}

void MasterRail::paintTelemetry (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (r.getHeight() < 30.0f) return;
    const auto& t = proc.telemetry;
    const float sub = t.subLevel.load();
    auto dbText = [] (float lin) { const float db = dsp::gainToDb (lin); return db < -90.0f ? juce::String::fromUTF8 ("\xe2\x80\x94") : juce::String (db, 1) + " dB"; };
    bar (g, r.withHeight (20.0f), std::sqrt (sub), violet, "sub", dbText (sub));
    const float rms = t.rms.load();
    bar (g, r.withTrimmedTop (28.0f).withHeight (20.0f), std::sqrt (rms) * 1.4f, frost.withAlpha (0.7f), "energy", dbText (rms));
}

//==============================================================================
namespace
{
const char* kTrigHelp[3] {
    "Free  \xc2\xb7  runs on regardless of the keyboard",
    "Retrig  \xc2\xb7  every note restarts the cycle",
    "One-shot  \xc2\xb7  plays one cycle per note, then holds: an envelope" };
const char* kExprFoot[3] { "voice controls follow each note's own velocity", "CC 1", "channel or poly pressure" };
const char* kSlotLetters = "ABCDEF";
} // namespace

ModPanel::ModPanel (VesselProcessor& p, Registry& reg) : proc (p)
{
    auto own = [&] (juce::Component* c, std::vector<juce::Component*>& set) { owned.add (c); addChildComponent (c); set.push_back (c); return c; };
    auto halo = [&] (int pid, const char* cap, std::vector<juce::Component*>& set)
    {
        auto* s = new ParamSlider (p, pid, ParamSlider::Kind::Halo, cap);
        reg.sliders.push_back (s);
        own (s, set);
        return s;
    };
    auto bar = [&] (int pid, const juce::String& cap, std::vector<juce::Component*>& set)
    {
        auto* s = new ParamSlider (p, pid, ParamSlider::Kind::Bar, vessel::spec (pid).name, cap);
        reg.sliders.push_back (s);
        own (s, set);
        return s;
    };
    auto sel = [&] (int pid, const juce::String& cap, bool compact, std::vector<juce::Component*>& set)
    {
        auto* s = new Selector (p, pid, cap, compact);
        own (s, set);
        return s;
    };

    for (int l = 0; l < kNumLfos; ++l)
    {
        auto& pg = lfo[(size_t) l];
        const int b0 = lfoBase (l), e0 = lfoExt (l);
        pg.shape = sel (b0, "Shape", false, pg.all);
        pg.div   = sel (b0 + 3, "Division", false, pg.all);
        pg.trig  = sel (e0, "Trigger", false, pg.all);
        pg.sync  = new Rune (p, b0 + 2, "Sync");     own (pg.sync, pg.all);
        pg.uni   = new Rune (p, e0 + 4, "Unipolar"); own (pg.uni, pg.all);
        pg.rate   = halo (b0 + 1, "Rate", pg.all);
        pg.warp   = halo (b0 + 4, "Warp", pg.all);
        pg.phase  = halo (e0 + 1, "Phase", pg.all);
        pg.fade   = halo (e0 + 2, "Fade In", pg.all);
        pg.smooth = halo (e0 + 3, "Smooth", pg.all);
        for (int s = 0; s < kLfoSlots; ++s)
        {
            pg.dest[(size_t) s] = sel (lfoDestPid (l, s), vessel::spec (lfoDestPid (l, s)).name, true, pg.all);
            pg.amt[(size_t) s]  = bar (lfoAmtPid (l, s), juce::String::charToString ((juce::juce_wchar) kSlotLetters[s]), pg.all);
        }
    }
    for (int src = 0; src < 3; ++src)
        for (int s = 0; s < 2; ++s)
        {
            expr.dest[(size_t) src][(size_t) s] = sel (exprDestPid (src, s), vessel::spec (exprDestPid (src, s)).name, true, expr.all);
            expr.amt[(size_t) src][(size_t) s]  = bar (exprAmtPid (src, s), juce::String::charToString ((juce::juce_wchar) kSlotLetters[s]), expr.all);
        }
    expr.velAmp = bar (vel_amp, "To Amp", expr.all);

    std::vector<juce::Component*> macroSet;
    for (int m = 0; m < kNumMacros; ++m)
    {
        auto* k = new ParamSlider (p, macroPid (m), ParamSlider::Kind::Halo, " ");
        reg.sliders.push_back (k);
        own (k, macroSet);
        k->onDragStart = [this, m] { selectMacro (m); };
        mac.knob[(size_t) m] = k;
        for (int s = 0; s < kMacroSlots; ++s)
        {
            mac.dest[(size_t) m][(size_t) s] = sel (macroDestPid (m, s), vessel::spec (macroDestPid (m, s)).name, true, macroSet);
            mac.amt[(size_t) m][(size_t) s]  = bar (macroAmtPid (m, s), juce::String::charToString ((juce::juce_wchar) kSlotLetters[s]), macroSet);
        }
    }

    addChildComponent (scatter);
    scatter.onClick = [this] { if (current < kNumLfos) proc.randomiseDrawTable (current); };
    scatter.setTooltip ("New random drawn shape");
    select (kMacroPage);
}

void ModPanel::selectMacro (int m)
{
    macroSel = juce::jlimit (0, kNumMacros - 1, m);
    if (current != kMacroPage) { select (kMacroPage); return; }
    for (int k = 0; k < kNumMacros; ++k)
        for (int s = 0; s < kMacroSlots; ++s)
        {
            mac.dest[(size_t) k][(size_t) s]->setVisible (k == macroSel);
            mac.amt[(size_t) k][(size_t) s]->setVisible (k == macroSel);
        }
    refresh();
    repaint();
}

void ModPanel::select (int page)
{
    current = juce::jlimit (0, kPages - 1, page);
    if (rename != nullptr) rename.reset();
    for (int l = 0; l < kNumLfos; ++l)
        for (auto* c : lfo[(size_t) l].all) c->setVisible (l == current);
    for (auto* c : expr.all) c->setVisible (current == kExprPage);
    for (int m = 0; m < kNumMacros; ++m)
    {
        mac.knob[(size_t) m]->setVisible (current == kMacroPage);
        for (int s = 0; s < kMacroSlots; ++s)
        {
            mac.dest[(size_t) m][(size_t) s]->setVisible (current == kMacroPage && m == macroSel);
            mac.amt[(size_t) m][(size_t) s]->setVisible (current == kMacroPage && m == macroSel);
        }
    }
    scatter.setVisible (current < kNumLfos);
    refresh();
    repaint();
}

void ModPanel::setIdle (ParamSlider* s, bool idle)
{
    if ((bool) s->getProperties()["idle"] == idle) return;
    s->getProperties().set ("idle", idle);
    s->repaint();
}

void ModPanel::refresh()
{
    auto destIs = [this] (int pid) { return juce::roundToInt (plain (proc, pid)); };
    if (current < kNumLfos)
    {
        auto& pg = lfo[(size_t) current];
        // the unused clock (Hz vs division) dims
        const bool synced = isOn (proc, lfoBase (current) + 2);
        pg.rate->setAlpha (synced ? 0.35f : 1.0f);
        pg.div->setAlpha (synced ? 1.0f : 0.35f);
        for (int s = 0; s < kLfoSlots; ++s) setIdle (pg.amt[(size_t) s], destIs (lfoDestPid (current, s)) == 0);
        repaint (canvas.toNearestInt().expanded (4));
    }
    else if (current == kMacroPage)
    {
        for (int s = 0; s < kMacroSlots; ++s) setIdle (mac.amt[(size_t) macroSel][(size_t) s], destIs (macroDestPid (macroSel, s)) == 0);
        if (const int v = proc.macroNamesVersion.load(); v != namesSeen) { namesSeen = v; repaint(); }
        else repaint (juce::Rectangle<int> (90, 32, getWidth() - 90, 22));   // headers: route counts
    }
    else
    {
        for (int src = 0; src < 3; ++src)
            for (int s = 0; s < 2; ++s) setIdle (expr.amt[(size_t) src][(size_t) s], destIs (exprDestPid (src, s)) == 0);
        repaint (juce::Rectangle<int> (90, 32, getWidth() - 90, 32));   // live meters
    }
    repaint (tabs.toNearestInt().expanded (2));
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (messageUntil > 0.0 || flashUntil > 0.0)
    {
        if (now > messageUntil) messageUntil = 0.0;
        if (now > flashUntil)   flashUntil = 0.0;
        repaint();
    }
}

juce::Rectangle<float> ModPanel::tabRect (int i) const
{
    const float th = tabs.getHeight() / (float) kPages;
    return { tabs.getX(), tabs.getY() + (float) i * th, tabs.getWidth(), th };
}

juce::Rectangle<float> ModPanel::macroHead (int m) const
{
    return { 96.0f + (float) m * 87.0f, 33.0f, 83.0f, 18.0f };
}

juce::Rectangle<float> ModPanel::exprHead (int src) const
{
    return { 96.0f + (float) src * 232.0f, 36.0f, 224.0f, 22.0f };
}

juce::Rectangle<float> ModPanel::routeRow (int page, int slot) const
{
    if (page < kNumLfos) return { 658.0f, 50.0f + (float) slot * 41.0f, 134.0f, 38.0f };
    if (page == kMacroPage) return { 96.0f + (float) (slot % 3) * 232.0f, 136.0f + (float) (slot / 3) * 40.0f, 224.0f, 38.0f };
    const int src = slot / 2, s = slot % 2;
    return { 96.0f + (float) src * 232.0f, 70.0f + (float) s * 44.0f, 224.0f, 38.0f };
}

void ModPanel::resized()
{
    tabs = { 8.0f, 36.0f, 74.0f, (float) getHeight() - 42.0f };
    for (auto& pg : lfo)
    {
        pg.shape->setBounds (96, 36, 112, Selector::kHeight);
        pg.div->setBounds (214, 36, 92, Selector::kHeight);
        pg.trig->setBounds (312, 36, 92, Selector::kHeight);
        pg.sync->setBounds (412, 37, 76, 22);
        pg.uni->setBounds (412, 59, 76, 22);
        ParamSlider* halos[] { pg.rate, pg.warp, pg.phase, pg.fade, pg.smooth };
        for (int i = 0; i < 5; ++i) halos[i]->setBounds (96 + i * 78, 86, 78, ParamSlider::kHaloHeight);
        for (int s = 0; s < kLfoSlots; ++s)
        {
            const auto r = routeRow (0, s).toNearestInt();
            pg.dest[(size_t) s]->setBounds (r.withHeight (Selector::kCompactHeight));
            pg.amt[(size_t) s]->setBounds (r.withTrimmedTop (21).withHeight (ParamSlider::kBarHeight));
        }
    }
    for (int src = 0; src < 3; ++src)
        for (int s = 0; s < 2; ++s)
        {
            const auto r = routeRow (kExprPage, src * 2 + s).toNearestInt();
            expr.dest[(size_t) src][(size_t) s]->setBounds (r.withHeight (Selector::kCompactHeight));
            expr.amt[(size_t) src][(size_t) s]->setBounds (r.withTrimmedTop (21).withHeight (ParamSlider::kBarHeight));
        }
    expr.velAmp->setBounds (96, 166, 224, ParamSlider::kBarHeight);
    for (int m = 0; m < kNumMacros; ++m)
    {
        mac.knob[(size_t) m]->setBounds (macroHead (m).toNearestInt().getX(), 50, 83, 82);
        for (int s = 0; s < kMacroSlots; ++s)
        {
            const auto r = routeRow (kMacroPage, s).toNearestInt();
            mac.dest[(size_t) m][(size_t) s]->setBounds (r.withHeight (Selector::kCompactHeight));
            mac.amt[(size_t) m][(size_t) s]->setBounds (r.withTrimmedTop (21).withHeight (ParamSlider::kBarHeight));
        }
    }
    canvas = { 502.0f, 38.0f, 140.0f, 146.0f };
    scatter.setBounds (502, 190, 140, 22);
}

ModPanel::~ModPanel()
{
    if (rename != nullptr)
    {
        rename->onReturnKey = nullptr;
        rename->onEscapeKey = nullptr;
        rename->onFocusLost = nullptr;
        rename.reset();
    }
}

void ModPanel::paint (juce::Graphics& g)
{
    g.setColour (basin.withAlpha (0.5f));
    g.fillAll();
    const float w = (float) getWidth();
    drawRule (g, 0.0f, 0.0f, w, seamHi);
    const bool isExpr = current == kExprPage, isMacro = current == kMacroPage;
    drawCaps (g, isExpr ? "Expression" : isMacro ? "Macros" : "Warped LFO", { 14.0f, 6.0f, 200.0f, 22.0f }, 12.5f, frost, juce::Justification::centredLeft, 0.22f);
    if (! isExpr && ! isMacro)
    {
        g.setColour (pewter.withAlpha (0.75f));
        g.setFont (type::display (15.0f));
        g.drawText ("20", juce::Rectangle<float> (130.0f, 6.0f, 40.0f, 22.0f), juce::Justification::centredLeft, false);
    }

    // hint line, or the last routing message
    const bool msg = messageUntil > 0.0;
    g.setColour (msg ? frost : pewter);
    g.setFont (type::value (13.0f));
    const auto hint = isMacro ? juce::String::fromUTF8 ("Drag a macro\xe2\x80\x99s diamond onto any control  \xc2\xb7  double-click a name to rename it")
                              : juce::String::fromUTF8 ("Drag a tab onto any control to modulate it  \xc2\xb7  right-click a control for its routes");
    g.drawText (msg ? message : hint, juce::Rectangle<float> (200.0f, 6.0f, w - 214.0f, 22.0f), juce::Justification::centredRight, true);
    if (msg) { g.setColour (chlorine); g.fillPath (diamond ({ w - 14.0f - type::width (type::value (13.0f), message) - 10.0f, 17.0f }, 6.0f)); }

    drawRule (g, 30.0f, 0.0f, w, seam);
    drawSeam (g, 88.0f, 36.0f, (float) getHeight() - 6.0f);
    paintTabs (g);
    if (isExpr) paintExpr (g); else if (isMacro) paintMacros (g); else paintLfo (g);

    // flash: the route that was just made
    if (flashUntil > 0.0 && flashPage == current && flashSlot >= 0)
    {
        const float a = (float) juce::jlimit (0.0, 1.0, (flashUntil - juce::Time::getMillisecondCounterHiRes()) / 1800.0);
        const auto r = routeRow (current, flashSlot);
        g.setColour (chlorine.withAlpha (0.12f * a));
        g.fillRect (r.expanded (3.0f, 1.0f));
        g.setColour (chlorine.withAlpha (a));
        g.fillRect (juce::Rectangle<float> (r.getX() - 5.0f, r.getY(), 2.0f, r.getHeight()));
    }
}

void ModPanel::paintTabs (juce::Graphics& g)
{
    for (int t = 0; t < kPages; ++t)
    {
        const int i = pageAtTab (t);
        const auto r = tabRect (t);
        const bool sel = i == current;
        if (sel) { g.setColour (violet); g.fillRect (juce::Rectangle<float> (r.getX(), r.getY() + 6.0f, 2.0f, r.getHeight() - 12.0f)); }
        if (i < kNumLfos)
        {
            g.setColour (sel ? violet : frost.withAlpha (0.75f));
            g.setFont (type::display (24.0f));
            g.drawText (juce::String (i + 1), r.withWidth (40.0f).translated (6.0f, 0.0f), juce::Justification::centred, false);
            const float v = proc.telemetry.lfoValue[i].load();
            const bool uni = isOn (proc, lfoExt (i) + 4);
            const float bx = r.getX() + 54.0f, cy = r.getCentreY(), bh = r.getHeight() * 0.34f;
            g.setColour (seamHi);
            g.fillRect (juce::Rectangle<float> (bx, cy - bh, 1.0f, bh * 2.0f));
            g.setColour (chlorine);
            g.fillPath (diamond ({ bx + 0.5f, uni ? cy + bh - v * 2.0f * bh : cy - v * bh }, 6.0f));
        }
        else if (i == kMacroPage)
        {
            drawCaps (g, "Macro", r.withWidth (44.0f).translated (4.0f, 0.0f), 10.5f, sel ? violet : frost.withAlpha (0.8f), juce::Justification::centred, 0.16f);
            for (int k = 0; k < kNumMacros; ++k)
            {
                const float v = proc.param (macroPid (k))->getValue();
                const float bx = r.getX() + 50.0f + (float) k * 3.0f, bh = r.getHeight() * 0.6f, by = r.getCentreY() + bh * 0.5f;
                g.setColour (seamHi);
                g.fillRect (juce::Rectangle<float> (bx, by - bh, 2.0f, bh));
                g.setColour (violet);
                g.fillRect (juce::Rectangle<float> (bx, by - bh * v, 2.0f, bh * v));
            }
        }
        else
        {
            drawCaps (g, "Expr", r.withWidth (44.0f).translated (4.0f, 0.0f), 10.5f, sel ? violet : frost.withAlpha (0.8f), juce::Justification::centred, 0.2f);
            const float vals[3] { proc.telemetry.velocity.load(), proc.telemetry.modWheel.load(), proc.telemetry.aftertouch.load() };
            for (int k = 0; k < 3; ++k)
            {
                const float bx = r.getX() + 52.0f + (float) k * 6.0f, bh = r.getHeight() * 0.6f, by = r.getCentreY() + bh * 0.5f;
                g.setColour (seamHi);
                g.fillRect (juce::Rectangle<float> (bx, by - bh, 2.0f, bh));
                g.setColour (chlorine);
                g.fillRect (juce::Rectangle<float> (bx, by - bh * juce::jlimit (0.0f, 1.0f, vals[k]), 2.0f, bh * juce::jlimit (0.0f, 1.0f, vals[k])));
            }
        }
    }
}

void ModPanel::paintLfo (juce::Graphics& g)
{
    drawSeam (g, 494.0f, 36.0f, (float) getHeight() - 6.0f);
    drawSeam (g, 650.0f, 36.0f, (float) getHeight() - 6.0f);

    // trigger mode, in words
    const int trig = juce::roundToInt (plain (proc, lfoExt (current)));
    g.setColour (pewter);
    g.setFont (type::value (11.5f));
    g.drawText (juce::String::fromUTF8 (kTrigHelp[juce::jlimit (0, 2, trig)]), juce::Rectangle<float> (100.0f, 178.0f, 386.0f, 16.0f),
                juce::Justification::centredLeft, true);
    const float fade = plain (proc, lfoExt (current) + 2), smooth = plain (proc, lfoExt (current) + 3);
    juce::String extra;
    if (fade > 0.005f) extra << "fades in over " << vessel::formatValue (lfoExt (current) + 2, fade);
    if (smooth > 0.005f) extra << (extra.isEmpty() ? "" : juce::String::fromUTF8 ("  \xc2\xb7  ")) << "slewed " << juce::roundToInt (smooth * 100.0f) << "%";
    if (isOn (proc, lfoExt (current) + 4)) extra << (extra.isEmpty() ? "" : juce::String::fromUTF8 ("  \xc2\xb7  ")) << "unipolar: moves up from the knob";
    if (extra.isNotEmpty()) g.drawText (extra, juce::Rectangle<float> (100.0f, 194.0f, 386.0f, 16.0f), juce::Justification::centredLeft, true);

    drawCaps (g, "Routes", { 658.0f, 33.0f, 134.0f, 14.0f }, 9.0f, pewter, juce::Justification::centredLeft, 0.24f);

    // canvas
    frame (g, canvas);
    g.setColour (seam);
    g.fillRect (juce::Rectangle<float> (canvas.getX(), canvas.getCentreY(), canvas.getWidth(), 1.0f));
    for (int k = 1; k < 4; ++k) g.fillRect (juce::Rectangle<float> (canvas.getX() + canvas.getWidth() * (float) k / 4.0f, canvas.getY(), 1.0f, canvas.getHeight()));

    const int shape = juce::roundToInt (plain (proc, lfoBase (current)));
    const bool uni = isOn (proc, lfoExt (current) + 4);
    const auto inner = canvas.reduced (6.0f, 14.0f).withTrimmedTop (4.0f);
    auto yOf = [&] (float v) { return uni ? inner.getBottom() - v * inner.getHeight() : inner.getCentreY() - v * inner.getHeight() * 0.5f; };
    const float base = uni ? inner.getBottom() : inner.getCentreY();
    juce::Path path, fill;
    for (int i = 0; i <= 160; ++i)
    {
        const float ph = (float) i / 160.0f;
        const float v = proc.previewLfo (current, std::min (ph, 0.9999f));
        const auto pt = juce::Point<float> (inner.getX() + ph * inner.getWidth(), yOf (v));
        if (i == 0) { path.startNewSubPath (pt); fill.startNewSubPath (inner.getX(), base); }
        else path.lineTo (pt);
        fill.lineTo (pt);
    }
    fill.lineTo (inner.getRight(), base);
    fill.closeSubPath();
    g.setColour (violet.withAlpha (0.12f));
    g.fillPath (fill);
    g.setColour (violet);
    g.strokePath (path, juce::PathStrokeType (1.6f));

    if (shape == dsp::WarpedLfo::Drawn)
    {
        for (int i = 0; i < kDrawPoints; ++i)
        {
            const float v = proc.drawTables[(size_t) current][(size_t) i].load();
            g.setColour (frost.withAlpha (0.55f));
            g.fillPath (diamond ({ inner.getX() + ((float) i + 0.5f) / (float) kDrawPoints * inner.getWidth(), yOf (uni ? 0.5f * (v + 1.0f) : v) }, 4.0f));
        }
        drawCaps (g, "draw to shape", canvas.withHeight (14.0f).translated (0.0f, 3.0f), 8.5f, frost.withAlpha (0.7f));
    }
    else
        drawCaps (g, proc.param (lfoBase (current))->getCurrentValueAsText(), canvas.withHeight (14.0f).translated (0.0f, 3.0f), 8.5f, pewter);

    // playhead: the phase being read (offset included) and the output
    const float ph = proc.telemetry.lfoPhase[current].load(), v = proc.telemetry.lfoValue[current].load();
    const juce::Point<float> c { inner.getX() + ph * inner.getWidth(), juce::jlimit (canvas.getY(), canvas.getBottom(), yOf (v)) };
    g.setColour (chlorine.withAlpha (0.25f));
    g.fillRect (juce::Rectangle<float> (c.x, canvas.getY(), 1.0f, canvas.getHeight()));
    g.setColour (chlorine);
    g.fillPath (diamond (c, 8.0f));
}

void ModPanel::paintExpr (juce::Graphics& g)
{
    static const char* names[3] { "Velocity", "Mod Wheel", "Aftertouch" };
    const float vals[3] { proc.telemetry.velocity.load(), proc.telemetry.modWheel.load(), proc.telemetry.aftertouch.load() };
    for (int src = 0; src < 3; ++src)
    {
        const auto h = exprHead (src);
        if (src > 0) drawSeam (g, h.getX() - 4.0f, 36.0f, (float) getHeight() - 6.0f);
        g.setColour (chlorine);
        g.strokePath (diamond ({ h.getX() + 7.0f, h.getCentreY() }, 9.0f), juce::PathStrokeType (1.2f));
        g.fillPath (diamond ({ h.getX() + 7.0f, h.getCentreY() }, 4.0f));
        drawCaps (g, names[src], h.withTrimmedLeft (20.0f), 10.5f, frost, juce::Justification::centredLeft, 0.22f);
        g.setColour (frost);
        g.setFont (type::value (11.5f));
        g.drawText (juce::String (juce::roundToInt (vals[src] * 100.0f)) + "%", h, juce::Justification::centredRight, false);
        const auto m = juce::Rectangle<float> (h.getX(), h.getBottom() + 3.0f, h.getWidth(), 2.0f);
        g.setColour (seamHi);
        g.fillRect (m);
        g.setColour (chlorine);
        g.fillRect (m.withWidth (m.getWidth() * juce::jlimit (0.0f, 1.0f, vals[src])));
        g.setColour (pewter);
        g.setFont (type::value (11.0f));
        g.drawText (kExprFoot[src], juce::Rectangle<float> (h.getX(), 192.0f, h.getWidth(), 16.0f), juce::Justification::centredLeft, true);
    }
}

void ModPanel::paintMacros (juce::Graphics& g)
{
    for (int m = 0; m < kNumMacros; ++m)
    {
        const auto h = macroHead (m);
        const bool sel = m == macroSel;
        if (m > 0) drawSeam (g, h.getX() - 2.5f, 36.0f, 130.0f);
        if (sel)
        {
            g.setColour (violet.withAlpha (0.07f));
            g.fillRect (juce::Rectangle<float> (h.getX(), h.getY(), h.getWidth(), 99.0f));
            g.setColour (violet);
            g.fillRect (juce::Rectangle<float> (h.getX(), h.getBottom() - 1.0f, h.getWidth(), 1.5f));
        }
        // the drag handle: a chlorine diamond, like the expression headers
        const juce::Point<float> d { h.getX() + 7.0f, h.getCentreY() };
        g.setColour (chlorine);
        g.strokePath (diamond (d, 9.0f), juce::PathStrokeType (1.2f));
        g.fillPath (diamond (d, 4.0f));
        const auto name = proc.macroName (m);
        const auto shown = name.isNotEmpty() ? name : "Macro " + juce::String (m + 1);
        drawCaps (g, shown, h.withTrimmedLeft (16.0f).withTrimmedRight (2.0f), 10.5f,
                  sel ? violet : name.isNotEmpty() ? frost : pewter, juce::Justification::centredLeft, 0.12f);
        // how many routes it drives: small ticks under the name
        int used = 0;
        for (int s = 0; s < kMacroSlots; ++s) used += juce::roundToInt (plain (proc, macroDestPid (m, s))) != 0 ? 1 : 0;
        for (int s = 0; s < used; ++s)
            g.fillRect (juce::Rectangle<float> (h.getX() + 16.0f + (float) s * 5.0f, h.getBottom() + 2.0f, 3.0f, 2.0f));
    }
    drawRule (g, 132.0f, 92.0f, (float) getWidth() - 4.0f, seam);
}

void ModPanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (current != kMacroPage) return;
    for (int m = 0; m < kNumMacros; ++m)
        if (macroHead (m).contains (e.position)) { beginRename (m); return; }
}

void ModPanel::beginRename (int m)
{
    selectMacro (m);
    rename = std::make_unique<juce::TextEditor>();
    auto& ed = *rename;
    addAndMakeVisible (ed);
    ed.setBounds (macroHead (m).withTrimmedLeft (14.0f).toNearestInt());
    ed.setFont (type::caps (11.0f, 0.08f));
    ed.setInputRestrictions (24);
    ed.setIndents (2, 1);
    ed.setText (proc.macroName (m), false);
    ed.selectAll();
    auto commit = [this, m] (bool keep)
    {
        if (rename == nullptr) return;
        if (keep) proc.setMacroName (m, rename->getText());
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ModPanel> (this)] { if (safe != nullptr) { safe->rename.reset(); safe->repaint(); } });
    };
    ed.onReturnKey = [commit] { commit (true); };
    ed.onFocusLost = [commit] { commit (true); };
    ed.onEscapeKey = [commit] { commit (false); };
    ed.grabKeyboardFocus();
}

bool ModPanel::sourceAt (juce::Point<float> p, ModSource& out) const
{
    for (int l = 0; l < kNumLfos; ++l)
        if (tabRect (tabOfPage (l)).contains (p)) { out = { ModSource::Lfo, l }; return true; }
    if (current == kMacroPage)
        for (int m = 0; m < kNumMacros; ++m)
            if (macroHead (m).withWidth (18.0f).expanded (0.0f, 2.0f).contains (p)) { out = { ModSource::Macro, m }; return true; }
    if (current == kExprPage)
        for (int src = 0; src < 3; ++src)
            if (exprHead (src).contains (p)) { out = { ModSource::Expr, src }; return true; }
    return false;
}

void ModPanel::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.position;
    pressAt = p;
    drawing = false;
    dragArmed = sourceAt (p, armed);
    if (tabs.contains (p)) { select (pageAtTab (juce::jlimit (0, kPages - 1, (int) ((p.y - tabs.getY()) / (tabs.getHeight() / (float) kPages))))); return; }
    if (current == kMacroPage)
        for (int m = 0; m < kNumMacros; ++m)
            if (macroHead (m).contains (p)) { selectMacro (m); break; }
    if (current < kNumLfos && canvas.contains (p)) { drawing = true; drawAt (p, false); lastDraw = p; }
}

void ModPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragArmed && e.position.getDistanceFrom (pressAt) > 5.0f)
    {
        dragArmed = false;
        if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
            if (! c->isDragAndDropActive())
                c->startDragging (armed.dragId(), this, juce::ScaledImage (makeDragImage (armed.displayName (proc)), 2.0), false);
        return;
    }
    if (drawing && canvas.expanded (20.0f).contains (e.position)) { drawAt (e.position, true); lastDraw = e.position; }
}

void ModPanel::mouseUp (const juce::MouseEvent&) { drawing = dragArmed = false; }

void ModPanel::mouseMove (const juce::MouseEvent& e)
{
    ModSource s;
    setMouseCursor (sourceAt (e.position, s) ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void ModPanel::showMessage (const juce::String& m)
{
    message = m;
    messageUntil = juce::Time::getMillisecondCounterHiRes() + 4000.0;
    repaint();
}

void ModPanel::showRoute (ModSource src, int slot, routing::Result res, int target)
{
    const int page = src.kind == ModSource::Lfo ? src.index : src.kind == ModSource::Macro ? kMacroPage : kExprPage;
    if (page != current) select (page);
    if (src.kind == ModSource::Macro) selectMacro (src.index);
    const juce::String dest = target >= 0 ? juce::String (vessel::spec (target).name) : juce::String();
    const auto routeName = [&] (int s) { return juce::String::charToString ((juce::juce_wchar) kSlotLetters[juce::jlimit (0, 5, s)]); };
    const int flatSlot = src.kind == ModSource::Expr ? src.index * 2 + slot : slot;
    const auto srcName = src.displayName (proc);
    flashPage = page; flashSlot = -1;
    using R = routing::Result;
    switch (res)
    {
        case R::Added:
            showMessage (srcName + juce::String::fromUTF8 (" \xe2\x86\x92 ") + dest + juce::String::fromUTF8 ("  \xc2\xb7  route ") + routeName (slot)
                         + juce::String::fromUTF8 (", set its depth here"));
            flashSlot = flatSlot;
            break;
        case R::Exists:
            showMessage (srcName + " already moves " + dest + " (route " + routeName (slot) + ")");
            flashSlot = flatSlot;
            break;
        case R::Full:
            showMessage (srcName + juce::String::fromUTF8 ("\xe2\x80\x99s ") + juce::String (src.slots()) + " routes are in use: set one to None first");
            break;
        case R::Removed:         showMessage (srcName + " no longer moves " + dest); break;
        case R::NotADestination: showMessage (dest + " can't be modulated"); break;
    }
    if (flashSlot >= 0) flashUntil = juce::Time::getMillisecondCounterHiRes() + 1800.0;
    repaint();
}

void ModPanel::drawAt (juce::Point<float> p, bool interpolate)
{
    if (juce::roundToInt (plain (proc, lfoBase (current))) != dsp::WarpedLfo::Drawn) return;
    const bool uni = isOn (proc, lfoExt (current) + 4);
    const auto inner = canvas.reduced (6.0f, 14.0f).withTrimmedTop (4.0f);
    auto idxOf = [&] (float x) { return juce::jlimit (0, kDrawPoints - 1, (int) ((x - inner.getX()) / inner.getWidth() * (float) kDrawPoints)); };
    auto valOf = [&] (float y)
    {
        return uni ? juce::jlimit (-1.0f, 1.0f, (inner.getBottom() - y) / inner.getHeight() * 2.0f - 1.0f)
                   : juce::jlimit (-1.0f, 1.0f, (inner.getCentreY() - y) / (inner.getHeight() * 0.5f));
    };

    const int i1 = idxOf (p.x);
    const int i0 = interpolate ? idxOf (lastDraw.x) : i1;
    const float v1 = valOf (p.y), v0 = interpolate ? valOf (lastDraw.y) : v1;
    const int lo = std::min (i0, i1), hi = std::max (i0, i1);
    for (int i = lo; i <= hi; ++i)
    {
        const float t = hi == lo ? 1.0f : (float) (i - i0) / (float) (i1 - i0);
        proc.drawTables[(size_t) current][(size_t) i] = dsp::lerp (v0, v1, juce::jlimit (0.0f, 1.0f, t));
    }
    repaint (canvas.toNearestInt().expanded (4));
}

//==============================================================================
PresetBrowser::PresetBrowser (VesselProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    const auto& bank = vessel::PresetBank::factory();
    for (auto& c : vessel::PresetBank::categoryOrder()) if (bank.categories().contains (c)) cats.add (c);
    for (auto& c : bank.categories()) cats.addIfNotAlreadyThere (c);
}

std::vector<int> PresetBrowser::itemsOf (int c) const
{
    std::vector<int> out;
    const auto& bank = vessel::PresetBank::factory();
    for (int i = 0; i < bank.size(); ++i)
        if (bank.all()[(size_t) i].category == cats[c]) out.push_back (i);
    return out;
}

void PresetBrowser::open()
{
    const auto& bank = vessel::PresetBank::factory();
    const int cur = proc.getCurrentProgram();
    int c = 0;
    if (cur >= 0 && cur < bank.size()) c = std::max (0, cats.indexOf (bank.all()[(size_t) cur].category));
    hoverItem = hoverCat = -1;
    setCategory (c);
    grabKeyboardFocus();
}

void PresetBrowser::setCategory (int c)
{
    cat = juce::jlimit (0, std::max (0, cats.size() - 1), c);
    items = itemsOf (cat);
    resized();
    repaint();
}

void PresetBrowser::resized()
{
    // the sheet hangs from the header's preset name (centred on the editor)
    panel = juce::Rectangle<float> (820.0f, 520.0f).withCentre ({ (float) getWidth() * 0.5f, 260.0f });
    list = panel.reduced (16.0f).withWidth (176.0f).withTrimmedTop (34.0f).withTrimmedBottom (96.0f);
    info = juce::Rectangle<float> (panel.getX() + 16.0f, panel.getBottom() - 92.0f, panel.getWidth() - 32.0f, 78.0f);
    grid = juce::Rectangle<float> (list.getRight() + 24.0f, list.getY(), panel.getRight() - 16.0f - list.getRight() - 24.0f, list.getHeight());

    // categories share the column's height once there are more than fit at full pitch
    catRects.clear();
    const float pitch = std::min (34.0f, list.getHeight() / (float) std::max (1, cats.size()));
    for (int i = 0; i < cats.size(); ++i) catRects.push_back ({ list.getX(), list.getY() + (float) i * pitch, list.getWidth(), pitch - 2.0f });
    // three columns of 54 px rows hold 18 presets; a bigger category goes to four columns and shorter rows
    itemRects.clear();
    cols = items.size() > 18 ? 4 : 3;
    const int rows = std::max (1, ((int) items.size() + cols - 1) / cols);
    const float cw = grid.getWidth() / (float) cols, rh = std::min (54.0f, grid.getHeight() / (float) rows);
    for (size_t i = 0; i < items.size(); ++i)
        itemRects.push_back ({ grid.getX() + (float) ((int) i % cols) * cw, grid.getY() + (float) ((int) i / cols) * rh, cw - 8.0f, rh - 4.0f });
}

void PresetBrowser::paint (juce::Graphics& g)
{
    const auto& bank = vessel::PresetBank::factory();
    const int cur = proc.getCurrentProgram();

    // veil over the instrument, then the sheet
    g.setColour (voidc.withAlpha (0.62f));
    g.fillAll();
    g.setColour (basin.withAlpha (0.97f));
    g.fillRect (panel);
    g.setColour (seamHi);
    g.drawRect (panel, 1.0f);
    g.setColour (violet);
    g.fillRect (juce::Rectangle<float> (panel.getCentreX() - 150.0f, panel.getY(), 300.0f, 1.5f));   // hangs from the name

    drawCaps (g, "Factory bank", { panel.getX() + 16.0f, panel.getY() + 12.0f, 200.0f, 16.0f }, 10.5f, frost, juce::Justification::centredLeft, 0.24f);
    g.setColour (pewter);
    g.setFont (type::value (12.0f));
    g.drawText (juce::String (bank.size()) + juce::String::fromUTF8 (" presets  \xc2\xb7  ") + juce::String (cats.size()) + " categories",
                juce::Rectangle<float> (panel.getX() + 140.0f, panel.getY() + 12.0f, 260.0f, 16.0f), juce::Justification::centredLeft, false);
    g.setColour (violet.withAlpha (0.85f));
    g.setFont (type::display (15.0f));
    g.drawText ("by iisben", juce::Rectangle<float> (panel.getRight() - 216.0f, panel.getY() + 10.0f, 200.0f, 20.0f), juce::Justification::centredRight, false);
    drawRule (g, panel.getY() + 36.0f, panel.getX() + 12.0f, panel.getRight() - 12.0f, seam);

    // categories
    for (int i = 0; i < cats.size(); ++i)
    {
        const auto r = catRects[(size_t) i];
        const bool sel = i == cat, hot = i == hoverCat;
        if (sel)
        {
            g.setColour (violet.withAlpha (0.08f));
            g.fillRect (r);
            g.setColour (violet);
            g.fillRect (juce::Rectangle<float> (r.getX(), r.getY() + 6.0f, 2.0f, r.getHeight() - 12.0f));
        }
        drawCaps (g, cats[i], r.withTrimmedLeft (14.0f), 11.0f, sel ? violet : (hot ? frost : frost.withAlpha (0.72f)), juce::Justification::centredLeft, 0.22f);
        g.setColour (sel ? frost : pewter);
        g.setFont (type::value (12.0f));
        g.drawText (juce::String ((int) itemsOf (i).size()), r.withTrimmedRight (10.0f), juce::Justification::centredRight, false);
    }
    drawSeam (g, list.getRight() + 12.0f, list.getY(), list.getBottom());

    // presets
    for (size_t i = 0; i < items.size(); ++i)
    {
        const auto& pr = bank.all()[(size_t) items[i]];
        const auto r = itemRects[i];
        const bool isCur = items[i] == cur, hot = (int) i == hoverItem;
        if (hot) { g.setColour (violet.withAlpha (0.10f)); g.fillRect (r); }
        if (isCur)
        {
            g.setColour (violet);
            g.fillPath (diamond ({ r.getX() + 9.0f, r.getY() + 16.0f }, 8.0f));
        }
        g.setColour (isCur ? violet : (hot ? frost : frost.withAlpha (0.88f)));
        const bool dense = cols > 3;
        g.setFont (type::display (dense ? 17.0f : 19.0f));
        g.drawText (pr.name, juce::Rectangle<float> (r.getX() + 20.0f, r.getY() + 3.0f, r.getWidth() - 22.0f, dense ? 22.0f : 26.0f), juce::Justification::centredLeft, true);
        // whole tags only: drop trailing ones that would not fit the column (the info strip below shows them all)
        auto tags = juce::StringArray::fromTokens (pr.character, "|", "");
        const float capsH = dense ? 7.5f : 8.0f, capsW = r.getWidth() - 22.0f;
        const auto tagLine = [&tags] { return tags.joinIntoString (juce::String::fromUTF8 (" \xc2\xb7 ")); };
        while (tags.size() > 1 && type::width (type::caps (capsH, 0.18f), tagLine().toUpperCase()) > capsW)
            tags.remove (tags.size() - 1);
        drawCaps (g, tagLine(), { r.getX() + 20.0f, r.getY() + (dense ? 25.0f : 29.0f), capsW, 12.0f }, capsH, pewter,
                  juce::Justification::centredLeft, 0.18f);
    }

    // info strip: the preset under the mouse, else the current one
    drawRule (g, info.getY() - 6.0f, panel.getX() + 12.0f, panel.getRight() - 12.0f, seam);
    const int shown = hoverItem >= 0 && hoverItem < (int) items.size() ? items[(size_t) hoverItem] : cur;
    if (shown >= 0 && shown < bank.size())
    {
        const auto& pr = bank.all()[(size_t) shown];
        g.setColour (frost);
        g.setFont (type::display (22.0f));
        g.drawText (pr.name, juce::Rectangle<float> (info.getX(), info.getY(), info.getWidth() * 0.6f, 28.0f), juce::Justification::centredLeft, true);
        drawCaps (g, pr.category + juce::String::fromUTF8 ("  \xc2\xb7  ") + juce::StringArray::fromTokens (pr.character, "|", "").joinIntoString (juce::String::fromUTF8 (" \xc2\xb7 ")),
                  { info.getX() + info.getWidth() * 0.4f, info.getY() + 6.0f, info.getWidth() * 0.6f, 14.0f }, 9.0f, violet, juce::Justification::centredRight, 0.22f);
        g.setColour (pewter);
        g.setFont (type::value (12.5f));
        g.drawFittedText (pr.description, juce::Rectangle<float> (info.getX(), info.getY() + 30.0f, info.getWidth(), 34.0f).toNearestInt(),
                          juce::Justification::topLeft, 2, 1.0f);
    }
    g.setColour (pewter.withAlpha (0.7f));
    g.setFont (type::value (11.0f));
    g.drawText (juce::String::fromUTF8 ("click to load  \xc2\xb7  arrows audition  \xc2\xb7  page up / down: category  \xc2\xb7  esc closes"),
                juce::Rectangle<float> (info.getX(), info.getBottom() - 12.0f, info.getWidth(), 14.0f), juce::Justification::centredRight, false);
}

void PresetBrowser::mouseMove (const juce::MouseEvent& e)
{
    int hi = -1, hc = -1;
    for (size_t i = 0; i < itemRects.size(); ++i) if (itemRects[i].contains (e.position)) hi = (int) i;
    for (size_t i = 0; i < catRects.size(); ++i)  if (catRects[i].contains (e.position)) hc = (int) i;
    if (hi != hoverItem || hc != hoverCat) { hoverItem = hi; hoverCat = hc; repaint(); }
    setMouseCursor (hi >= 0 || hc >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void PresetBrowser::mouseExit (const juce::MouseEvent&) { hoverItem = hoverCat = -1; repaint(); }

void PresetBrowser::mouseDown (const juce::MouseEvent& e)
{
    if (! panel.contains (e.position)) { if (onClose) onClose(); return; }
    for (size_t i = 0; i < catRects.size(); ++i) if (catRects[i].contains (e.position)) { setCategory ((int) i); return; }
    for (size_t i = 0; i < itemRects.size(); ++i) if (itemRects[i].contains (e.position)) { load (items[i], true); return; }
}

void PresetBrowser::load (int index, bool close)
{
    proc.setCurrentProgram (index);
    if (onLoaded) onLoaded();
    repaint();
    if (close && onClose) onClose();
}

bool PresetBrowser::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey || k == juce::KeyPress::returnKey) { if (onClose) onClose(); return true; }
    if (k == juce::KeyPress::pageUpKey)   { setCategory ((cat + cats.size() - 1) % cats.size()); return true; }
    if (k == juce::KeyPress::pageDownKey) { setCategory ((cat + 1) % cats.size()); return true; }
    int step = 0;
    if (k == juce::KeyPress::leftKey) step = -1;
    else if (k == juce::KeyPress::rightKey) step = 1;
    else if (k == juce::KeyPress::upKey) step = -cols;
    else if (k == juce::KeyPress::downKey) step = cols;
    if (step == 0 || items.empty()) return false;
    int pos = -1;
    for (size_t i = 0; i < items.size(); ++i) if (items[i] == proc.getCurrentProgram()) pos = (int) i;
    pos = pos < 0 ? 0 : juce::jlimit (0, (int) items.size() - 1, pos + step);
    load (items[(size_t) pos], false);
    return true;
}
} // namespace vessel::ui
