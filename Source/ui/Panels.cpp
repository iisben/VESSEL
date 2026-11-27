#include "Panels.h"
#include "../PluginProcessor.h"
#include "../Presets.h"
#include "../dsp/Wavetable.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace vessel::ui {
using namespace ink;

namespace {
float plain(VesselProcessor &p, int pid) {
  auto *r = p.param(pid);
  return r->convertFrom0to1(r->getValue());
}
bool isOn(VesselProcessor &p, int pid) {
  return p.param(pid)->getValue() > 0.5f;
}

template <typename Fn>
void drawWave(juce::Graphics &g, juce::Rectangle<float> a, Fn &&fn,
              juce::Colour c, float thickness) {
  const int n = juce::jlimit(64, 1200, juce::roundToInt(a.getWidth() * 3.0f));
  std::array<float, 1201> ys{};
  float peak = 0.0f;
  for (int i = 0; i <= n; ++i) {
    ys[(size_t)i] = fn((float)i / (float)n);
    peak = std::max(peak, std::abs(ys[(size_t)i]));
  }
  const float scale = a.getHeight() * 0.42f / std::max(peak, 0.25f);
  juce::Path path;
  for (int i = 0; i <= n; ++i) {
    const juce::Point<float> pt(a.getX() + a.getWidth() * (float)i / (float)n,
                                a.getCentreY() - ys[(size_t)i] * scale);
    if (i == 0)
      path.startNewSubPath(pt);
    else
      path.lineTo(pt);
  }
  g.setColour(c.withAlpha(0.20f));
  g.strokePath(path, juce::PathStrokeType(thickness + 2.0f));
  g.setColour(c);
  g.strokePath(path, juce::PathStrokeType(thickness));
}

void meterBar(juce::Graphics &g, juce::Rectangle<float> r, float v,
              juce::Colour c, const juce::String &caption,
              const juce::String &valueText, bool bipolar = false) {
  drawCaps(g, caption, r.withHeight(11.0f), 8.2f, steel,
           juce::Justification::centredLeft, 0.12f);
  g.setColour(titanium);
  g.setFont(type::mono(9.5f));
  g.drawText(valueText, r.withHeight(11.0f), juce::Justification::centredRight,
             false);
  auto track =
      juce::Rectangle<float>(r.getX(), r.getY() + 14.0f, r.getWidth(), 4.0f);
  g.setColour(well);
  g.fillRoundedRectangle(track, 1.5f);
  g.setColour(seamHi.withAlpha(0.75f));
  g.drawRoundedRectangle(track, 1.5f, 0.7f);
  g.setColour(c);
  if (bipolar) {
    const float mid = track.getCentreX();
    const float x =
        mid + juce::jlimit(-1.0f, 1.0f, v) * track.getWidth() * 0.5f;
    g.fillRect(juce::Rectangle<float>(std::min(mid, x), track.getY() + 0.5f,
                                      std::abs(x - mid),
                                      track.getHeight() - 1.0f));
    g.setColour(steel);
    g.fillRect(juce::Rectangle<float>(mid - 0.5f, track.getY() - 1.5f, 1.0f,
                                      track.getHeight() + 3.0f));
  } else {
    g.fillRect(juce::Rectangle<float>(
        track.getX() + 0.5f, track.getY() + 0.5f,
        std::max(0.0f, (track.getWidth() - 1.0f) * juce::jlimit(0.0f, 1.0f, v)),
        track.getHeight() - 1.0f));
  }
}

void drawStar(juce::Graphics &g, juce::Point<float> c, float rOuter,
              bool filled, juce::Colour col) {
  juce::Path p;
  const float rInner = rOuter * 0.44f;
  for (int i = 0; i < 10; ++i) {
    const float r = (i % 2 == 0) ? rOuter : rInner;
    const float a = -std::numbers::pi_v<float> * 0.5f +
                    (float)i * std::numbers::pi_v<float> / 5.0f;
    const juce::Point<float> pt(c.x + r * std::cos(a), c.y + r * std::sin(a));
    if (i == 0)
      p.startNewSubPath(pt);
    else
      p.lineTo(pt);
  }
  p.closeSubPath();
  g.setColour(col);
  if (filled)
    g.fillPath(p);
  else
    g.strokePath(p, juce::PathStrokeType(1.1f));
}

const int kStagePids[5][5]{{sub_on, wt_on, drone_on, noise_on, gran_on},
                           {tube_on, bias_on, crush_on, vhs_on, tape_mode},
                           {flt_on, envf_on, unmask_on, form_on, res_on},
                           {z_on, conv_on, verb_on, haas_on, -1},
                           {ott_on, duck_on, mono_on, exc_on, clip_on}};
const char *kStageNames[5]{"Source", "Ruin", "Shape", "Space", "Pressure"};
const char *kStageCaps[5]{
    "sub / table / drone / fm", "tilt / tube / crush / tape",
    "ladders / env / vox / res", "z-depth / body / hole / haas",
    "ott / duck / exc / clip"};
const char *kStageNums[5]{"01", "02", "03", "04", "05"};
} // namespace

//==============================================================================
// ModuleColumn
//==============================================================================
ModuleColumn::ModuleColumn(VesselProcessor &p, Registry &r,
                           const juce::String &t, const juce::String &n,
                           int toggle)
    : proc(p), reg(r), title(t), number(n), togglePid(toggle) {
  if (togglePid >= 0) {
    header = std::make_unique<Rune>(proc, togglePid, title, true);
    addAndMakeVisible(*header);
  }
}

ParamSlider &ModuleColumn::halo(int pid, const juce::String &caption) {
  auto *s = new ParamSlider(proc, pid, ParamSlider::Kind::Halo, caption);
  owned.add(s);
  addAndMakeVisible(s);
  reg.sliders.push_back(s);
  return *s;
}

ParamSlider &ModuleColumn::vial(int pid, const juce::String &caption,
                                const juce::String &shortName) {
  auto *s =
      new ParamSlider(proc, pid, ParamSlider::Kind::Vial, caption, shortName);
  owned.add(s);
  addAndMakeVisible(s);
  reg.sliders.push_back(s);
  return *s;
}

Rune &ModuleColumn::rune(int pid, const juce::String &caption) {
  auto *b = new Rune(proc, pid, caption);
  owned.add(b);
  addAndMakeVisible(b);
  return *b;
}

Selector &ModuleColumn::selector(int pid, const juce::String &caption) {
  auto *s = new Selector(proc, pid, caption);
  owned.add(s);
  addAndMakeVisible(s);
  return *s;
}

juce::TextButton &ModuleColumn::button(const juce::String &text) {
  auto *b = new juce::TextButton(text);
  owned.add(b);
  addAndMakeVisible(b);
  return *b;
}

void ModuleColumn::row(std::initializer_list<juce::Component *> items,
                       int height, std::initializer_list<float> weights) {
  jassert(weights.size() == 0 || weights.size() == items.size());
  rows.push_back({std::vector<juce::Component *>(items), height,
                  std::vector<float>(weights)});
}

void ModuleColumn::resized() {
  const int w = getWidth();
  const float titleW =
      type::width(type::caps(10.5f, 0.16f), title.toUpperCase()) +
      (header ? 22.0f : 8.0f);
  showNumber = number.isNotEmpty() && titleW <= (float)(w - 36);
  if (header)
    header->setBounds(6, 4, showNumber ? w - 34 : w - 12, 22);

  int y = 30;
  for (auto &r : rows) {
    const int n = (int)r.items.size();
    float total = 0.0f;
    for (int i = 0; i < n; ++i)
      total += r.weights.empty() ? 1.0f : r.weights[(size_t)i];
    float acc = 0.0f;
    for (int i = 0; i < n; ++i) {
      const float wi = r.weights.empty() ? 1.0f : r.weights[(size_t)i];
      const int x0 = 4 + juce::roundToInt((float)(w - 8) * acc / total);
      acc += wi;
      const int x1 = 4 + juce::roundToInt((float)(w - 8) * acc / total);
      auto *c = r.items[(size_t)i];
      const bool slim = dynamic_cast<Selector *>(c) != nullptr ||
                        dynamic_cast<Rune *>(c) != nullptr ||
                        dynamic_cast<juce::TextButton *>(c) != nullptr;
      if (slim)
        c->setBounds(x0 + 3, y, std::max(12, x1 - x0 - 6), r.height);
      else
        c->setBounds(x0, y, std::max(12, x1 - x0), r.height);
    }
    y += r.height + 3;
  }
  instrumentArea = juce::Rectangle<float>(
      8.0f, (float)y + 1.0f, std::max(0.0f, (float)w - 16.0f),
      std::max(0.0f, (float)(getHeight() - y - 8)));
}

void ModuleColumn::paint(juce::Graphics &g) {
  const float w = (float)getWidth();
  if (!header)
    drawCaps(g, title, {10.0f, 4.0f, w - 36.0f, 22.0f}, 10.2f, titanium,
             juce::Justification::centredLeft, 0.16f);

  if (showNumber) {
    g.setColour(steel.withAlpha(0.65f));
    g.setFont(type::mono(10.0f));
    g.drawText(number, juce::Rectangle<float>(w - 30.0f, 4.0f, 24.0f, 22.0f),
               juce::Justification::centredRight, false);
  }
  drawRule(g, 28.0f, 6.0f, w - 6.0f, seam);

  if (instrument && instrumentArea.getHeight() > 14.0f) {
    juce::Graphics::ScopedSaveState ss(g);
    if (!lastOn)
      g.setOpacity(0.38f);
    instrument(g, instrumentArea);
  }
}

void ModuleColumn::refresh() {
  const bool on = togglePid < 0 || (vessel::spec(togglePid).type == PType::Bool
                                        ? isOn(proc, togglePid)
                                        : plain(proc, togglePid) > 0.5f);
  if (on != lastOn) {
    lastOn = on;
    for (auto *c : owned)
      c->setAlpha(on ? 1.0f : 0.40f);
    repaint();
  }
  if (instrument)
    repaint(instrumentArea.toNearestInt().expanded(2));
}

//==============================================================================
// StagePage
//==============================================================================
ModuleColumn &StagePage::add(std::unique_ptr<ModuleColumn> c, float weight) {
  auto *raw = columns.add(c.release());
  weights.push_back(weight);
  addAndMakeVisible(raw);
  return *raw;
}

void StagePage::resized() {
  float total = 0.0f;
  for (auto w : weights)
    total += w;
  float x = 2.0f;
  const float usable = (float)getWidth() - 4.0f;
  for (int i = 0; i < columns.size(); ++i) {
    const float cw = usable * weights[(size_t)i] / total;
    columns[i]->setBounds(juce::roundToInt(x), 2,
                          juce::roundToInt(x + cw) - juce::roundToInt(x),
                          getHeight() - 4);
    x += cw;
  }
}

void StagePage::paint(juce::Graphics &g) {
  drawMachinedPanel(g, getLocalBounds().toFloat(), 3.0f, true);
  for (int i = 1; i < columns.size(); ++i)
    drawSeam(g, (float)columns[i]->getX(), 8.0f, (float)getHeight() - 8.0f,
             seam);
}

//==============================================================================
// Stage Builders (0..4)
//==============================================================================
std::unique_ptr<StagePage> buildStage(int index, VesselProcessor &p,
                                      Registry &reg) {
  auto page = std::make_unique<StagePage>();
  auto col = [&](const juce::String &t, const juce::String &n, int toggle,
                 float weight) -> ModuleColumn & {
    return page->add(std::make_unique<ModuleColumn>(p, reg, t, n, toggle),
                     weight);
  };
  const auto H = ParamSlider::kHaloHeight, S = Selector::kHeight,
             V = ParamSlider::kVialHeight, R = Rune::kHeight;

  switch (index) {
  case 0: // ── 01 SOURCE
  {
    auto &sub = col("Sub", "01", sub_on, 1.0f);
    sub.row(
        {&sub.selector(sub1_oct, "Sub 1"), &sub.selector(sub2_oct, "Sub 2")},
        S);
    sub.row({&sub.halo(sub1_level, "Sub 1"), &sub.halo(sub2_level, "Sub 2")},
            H);
    sub.row(
        {&sub.halo(sub1_shape, "Shape 1"), &sub.halo(sub2_shape, "Shape 2")},
        H);
    sub.row({&sub.halo(sub_weight, "Weight"), &sub.halo(sub_send, "To Ruin")},
            H);
    sub.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      drawRecessedWell(g, r, 2.0f);
      drawCaps(g, "PHASE LOCK", r.reduced(5.0f, 3.0f).withHeight(10.0f), 7.5f,
               steel, juce::Justification::centredLeft, 0.12f);
      auto a = r.reduced(5.0f, 4.0f).withTrimmedTop(12.0f);
      if (a.getHeight() < 12.0f)
        return;
      const int o1 = juce::roundToInt(plain(p, sub1_oct)),
                o2 = juce::roundToInt(plain(p, sub2_oct)) + 1;
      g.setColour(seam);
      g.fillRect(
          juce::Rectangle<float>(a.getX(), a.getCentreY(), a.getWidth(), 1.0f));
      for (int k = 0; k < 2; ++k) {
        const int oct = k == 0 ? o1 : o2;
        juce::Path path;
        for (int i = 0; i <= 100; ++i) {
          const float t = (float)i / 100.0f;
          const float y = std::sin(dsp::kTwoPi * t * 4.0f / (float)(1 << oct));
          const juce::Point<float> pt(a.getX() + t * a.getWidth(),
                                      a.getCentreY() -
                                          y * a.getHeight() * 0.42f);
          if (i == 0)
            path.startNewSubPath(pt);
          else
            path.lineTo(pt);
        }
        g.setColour(k == 0 ? amber : cyan.withAlpha(0.75f));
        g.strokePath(path, juce::PathStrokeType(k == 0 ? 1.4f : 1.0f));
      }
    };

    auto &wt = col("Wavetable", "05", wt_on, 1.22f);
    wt.row({&wt.selector(wt_table, "Table"), &wt.selector(wt_oct, "Oct")}, S,
           {2.0f, 1.0f});
    wt.row({&wt.halo(wt_morph, "Morph"), &wt.halo(wt_pw, "Pulse W")}, H);
    wt.row({&wt.halo(wt_pwm, "PWM"), &wt.halo(wt_pwm_rate, "Rate")}, H);
    wt.row({&wt.halo(wt_semi, "Semi"), &wt.halo(wt_fine, "Fine")}, H);
    wt.row({&wt.halo(wt_level, "Level"), &wt.halo(wt_drift, "Drift")}, H);
    wt.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      if (r.getHeight() < 16.0f)
        return;
      drawRecessedWell(g, r, 2.0f);
      const auto a = r.reduced(4.0f, 2.0f);
      const float morph = plain(p, wt_morph),
                  pw = juce::jlimit(0.02f, 0.98f, plain(p, wt_pw));
      const auto bank = p.currentTableShared();
      if (bank == nullptr)
        return;
      drawWave(
          g, a,
          [&](float ph) {
            const float wp = ph < pw ? 0.5f * ph / pw
                                     : 0.5f + 0.5f * (ph - pw) / (1.0f - pw);
            return bank->readMorph(morph, 1, std::min(wp, 0.9999f));
          },
          amberHi, 1.1f);
    };

    auto &dr = col("Drone", "03", drone_on, 0.92f);
    dr.row({&dr.selector(drone_oct, "Oct"), &dr.rune(drone_hold, "Hold")}, S,
           {1.3f, 1.0f});
    dr.row({&dr.halo(drone_level, "Level"), &dr.halo(drone_tone, "Tone")}, H);
    dr.row({&dr.halo(drone_motion, "Motion"), &dr.halo(drone_spread, "Spread")},
           H);
    dr.row({&dr.halo(drone_attack, "Swell"), &dr.halo(drone_release, "Fade")},
           H);

    auto &nz = col("Dust", "04", noise_on, 0.88f);
    nz.row({&nz.selector(noise_hum_freq, "Mains")}, S);
    nz.row({&nz.halo(noise_hiss, "Hiss"), &nz.halo(noise_crackle, "Crackle")},
           H);
    nz.row({&nz.halo(noise_hum, "Hum"), &nz.halo(noise_tone, "Tone")}, H);
    nz.row({&nz.halo(noise_dyn, "Dynamic")}, H);

    auto &gr = col("Grain", "02", gran_on, 1.0f);
    gr.row({&gr.rune(gran_freeze, "Freeze"), &gr.selector(gran_div, "Stutter")},
           S);
    gr.row({&gr.halo(gran_size, "Size"), &gr.halo(gran_density, "Density")}, H);
    gr.row({&gr.halo(gran_pitch, "Pitch"), &gr.halo(gran_stretch, "Stretch")},
           H);
    gr.row({&gr.halo(gran_spray, "Spray"), &gr.halo(gran_reverse, "Reverse")},
           H);
    gr.row({&gr.halo(gran_chance, "Chance"), &gr.halo(gran_mix, "Mix")}, H);
    gr.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      if (r.getHeight() < 14.0f)
        return;
      const bool loop =
          p.telemetry.grainLooping.load(std::memory_order_relaxed);
      drawRecessedWell(g, r.withHeight(std::min(22.0f, r.getHeight())), 2.0f);
      const auto c = juce::Point<float>(
          r.getX() + 10.0f, r.getY() + std::min(22.0f, r.getHeight()) * 0.5f);
      g.setColour(loop ? cyan : steel.withAlpha(0.5f));
      if (loop)
        g.fillPath(diamond(c, 7.0f));
      else
        g.strokePath(diamond(c, 7.0f), juce::PathStrokeType(1.0f));
      drawCaps(
          g, loop ? "LOOPING" : "LISTENING",
          r.withHeight(std::min(22.0f, r.getHeight())).withTrimmedLeft(20.0f),
          8.0f, loop ? cyanHi : steel, juce::Justification::centredLeft, 0.14f);
    };

    auto &fm = col("TZ-FM", "27", fm_on, 0.98f);
    fm.row({&fm.selector(fm_ratio, "Ratio")}, S);
    fm.row({&fm.halo(fm_index, "Index"), &fm.halo(fm_feedback, "Feedbk")}, H);
    fm.row({&fm.halo(fm_env, "Env"), &fm.halo(fm_decay, "Decay")}, H);
    fm.row({&fm.halo(fm_fine, "Fine"), &fm.halo(fm_key, "Key Trk")}, H);
    fm.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      drawRecessedWell(g, r, 2.0f);
      drawCaps(g, "TZFM BEND", r.reduced(5.0f, 3.0f).withHeight(10.0f), 7.5f,
               steel, juce::Justification::centredLeft, 0.12f);
      auto a = r.reduced(5.0f, 4.0f).withTrimmedTop(12.0f);
      if (a.getHeight() < 12.0f)
        return;
      const float ratio = kFmRatios[(size_t)juce::jlimit(
                              0, (int)std::size(kFmRatios) - 1,
                              juce::roundToInt(plain(p, fm_ratio)))] *
                          std::exp2(plain(p, fm_fine) / 1200.0f);
      const float x = isOn(p, fm_on) ? plain(p, fm_index) : 0.0f;
      const float depth = 12.0f * x * x / dsp::kTwoPi;
      const float morph = plain(p, wt_morph);
      const auto bank = p.currentTableShared();
      if (bank == nullptr)
        return;
      g.setColour(seam);
      g.fillRect(
          juce::Rectangle<float>(a.getX(), a.getCentreY(), a.getWidth(), 1.0f));
      drawWave(
          g, a,
          [&](float u) {
            const float t = 2.0f * u;
            float ph = t + depth * std::sin(dsp::kTwoPi * t * ratio);
            ph -= std::floor(ph);
            return bank->readMorph(morph, 1, std::min(ph, 0.9999f));
          },
          cyan, 1.1f);
    };
    break;
  }

  case 1: // ── 02 RUIN
  {
    auto &tl = col("Tilt EQ", "15", -1, 1.0f);
    tl.row({&tl.halo(pre_tilt, "Pre"), &tl.halo(pre_pivot, "Pivot")}, H);
    tl.row({&tl.halo(post_tilt, "Post"), &tl.halo(post_pivot, "Pivot")}, H);
    tl.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      auto box = r.withHeight(std::min(r.getHeight(), 120.0f));
      drawRecessedWell(g, box, 2.0f);
      drawCaps(g, "PRE", box.reduced(6.0f, 4.0f).withHeight(10.0f), 7.8f, amber,
               juce::Justification::centredLeft, 0.12f);
      drawCaps(g, "POST", box.reduced(6.0f, 4.0f).withHeight(10.0f), 7.8f, cyan,
               juce::Justification::centredRight, 0.12f);
      auto a = box.reduced(6.0f, 6.0f).withTrimmedTop(14.0f);
      g.setColour(seamHi);
      g.fillRect(
          juce::Rectangle<float>(a.getX(), a.getCentreY(), a.getWidth(), 1.0f));
      for (int k = 0; k < 2; ++k) {
        const float tilt = plain(p, k == 0 ? pre_tilt : post_tilt),
                    piv = plain(p, k == 0 ? pre_pivot : post_pivot);
        juce::Path path;
        for (int i = 0; i <= 80; ++i) {
          const float t = (float)i / 80.0f, f = 20.0f * std::pow(1000.0f, t);
          const float x = f / piv;
          const float db = tilt * 0.5f * (x * x - 1.0f) / (x * x + 1.0f);
          const juce::Point<float> pt(a.getX() + t * a.getWidth(),
                                      a.getCentreY() -
                                          db / 12.0f * a.getHeight() * 0.45f);
          if (i == 0)
            path.startNewSubPath(pt);
          else
            path.lineTo(pt);
        }
        g.setColour(k == 0 ? amber : cyan);
        g.strokePath(path, juce::PathStrokeType(1.4f));
      }
    };

    auto &tb = col("Tube ADAA", "06", tube_on, 1.05f);
    tb.row({&tb.selector(tube_mode, "Topology")}, S);
    tb.row({&tb.halo(tube_drive, "Drive"), &tb.halo(tube_bias, "Bias")}, H);
    tb.row({&tb.halo(tube_mix, "Mix"), &tb.halo(tube_out, "Out")}, H);
    tb.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      const float side = std::min({r.getWidth(), r.getHeight(), 120.0f});
      auto a = juce::Rectangle<float>(side, side).withCentre(r.getCentre());
      drawRecessedWell(g, a, 2.0f);
      g.setColour(seam);
      g.fillRect(juce::Rectangle<float>(a.getX() + 2.0f, a.getCentreY(),
                                        a.getWidth() - 4.0f, 1.0f));
      g.fillRect(juce::Rectangle<float>(a.getCentreX(), a.getY() + 2.0f, 1.0f,
                                        a.getHeight() - 4.0f));
      const int mode = juce::roundToInt(plain(p, tube_mode));
      const float gain = std::pow(10.0f, plain(p, tube_drive) * 2.0f),
                  b = plain(p, tube_bias);
      juce::Path path;
      for (int i = 0; i <= 100; ++i) {
        const float x = -1.0f + 2.0f * (float)i / 100.0f;
        const float y = juce::jlimit(
            -1.15f, 1.15f, dsp::TubeDrive::shape(x * gain * 0.5f, b, mode));
        const juce::Point<float> pt(a.getCentreX() + x * side * 0.44f,
                                    a.getCentreY() - y * side * 0.39f);
        if (i == 0)
          path.startNewSubPath(pt);
        else
          path.lineTo(pt);
      }
      g.setColour(amber.withAlpha(0.22f));
      g.strokePath(path, juce::PathStrokeType(3.2f));
      g.setColour(amberHi);
      g.strokePath(path, juce::PathStrokeType(1.5f));
    };

    auto &bs = col("Bias Sag", "10", bias_on, 0.98f);
    bs.row({&bs.halo(bias_amount, "Offset"), &bs.halo(bias_sag, "Sag")}, H);
    bs.row({&bs.halo(bias_sat, "Tape Sat"), &bs.halo(bias_speed, "Recover")},
           H);

    auto &cr = col("Crush", "09", crush_on, 1.0f);
    cr.row({&cr.halo(crush_bits, "Bits"), &cr.halo(crush_rate, "Rate")}, H);
    cr.row({&cr.halo(crush_pre, "Pre Flt"), &cr.halo(crush_post, "Post Flt")},
           H);
    cr.row({&cr.halo(crush_mix, "Mix")}, H);
    cr.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      auto box = r.withHeight(std::min(r.getHeight(), 68.0f));
      drawRecessedWell(g, box, 2.0f);
      auto a = box.reduced(5.0f, 6.0f);
      const float levels = std::exp2(plain(p, crush_bits) - 1.0f);
      const float steps =
          juce::jlimit(2.0f, 80.0f, plain(p, crush_rate) / 600.0f);
      juce::Path path;
      for (int i = 0; i <= 180; ++i) {
        const float t = (float)i / 180.0f;
        const float held = std::floor(t * steps) / steps;
        float y = std::sin(dsp::kTwoPi * held);
        y = std::round(y * levels) / levels;
        const juce::Point<float> pt(a.getX() + t * a.getWidth(),
                                    a.getCentreY() - y * a.getHeight() * 0.44f);
        if (i == 0)
          path.startNewSubPath(pt);
        else
          path.lineTo(pt);
      }
      g.setColour(amberHi);
      g.strokePath(path, juce::PathStrokeType(1.25f));
    };

    auto &vh = col("VHS Tape", "07", vhs_on, 0.98f);
    vh.row({&vh.halo(vhs_wow, "Wow"), &vh.halo(vhs_flutter, "Flutter")}, H);
    vh.row({&vh.halo(vhs_drift, "Drift"), &vh.halo(vhs_rate, "Rate")}, H);
    vh.row({&vh.halo(vhs_age, "Age")}, H);

    auto &mt = col("Motor", "08", -1, 0.98f);
    mt.row({&mt.selector(tape_mode, "Trigger")}, S);
    mt.row({&mt.halo(tape_start, "Start"), &mt.halo(tape_stop, "Stop")}, H);
    mt.instrument = [&p, angle = 0.0f](juce::Graphics &g,
                                       juce::Rectangle<float> r) mutable {
      const float sp = p.telemetry.tapeSpeed.load(std::memory_order_relaxed);
      meterBar(g, r.withHeight(20.0f), sp / 1.3f, amber, "CAPSTAN",
               juce::String(sp, 2) + "x");
      angle = std::fmod(angle + sp * 0.10f, dsp::kTwoPi);
      if (angle < 0.0f)
        angle += dsp::kTwoPi;
      auto wellR = r.withTrimmedTop(28.0f);
      if (wellR.getHeight() < 36.0f)
        return;
      drawRecessedWell(g, wellR, 2.0f);
      const auto c = wellR.getCentre();
      const float rad = std::min(wellR.getWidth(), wellR.getHeight()) * 0.36f;
      g.setColour(seamHi);
      g.drawEllipse(
          juce::Rectangle<float>(rad * 2.0f, rad * 2.0f).withCentre(c), 1.2f);
      g.setColour(plate);
      g.fillEllipse(
          juce::Rectangle<float>(rad * 0.45f, rad * 0.45f).withCentre(c));
      for (int k = 0; k < 3; ++k) {
        const float a = angle + (float)k * dsp::kTwoPi / 3.0f;
        g.setColour(k == 0 ? amber : titanium.withAlpha(0.65f));
        g.fillPath(diamond(
            {c.x + rad * 0.65f * std::cos(a), c.y + rad * 0.65f * std::sin(a)},
            6.0f));
      }
    };
    break;
  }

  case 2: // ── 03 SHAPE
  {
    auto &ld = col("Dual ZDF Ladders", "11", flt_on, 1.95f);
    ld.row({&ld.selector(flt_route, "Routing")}, S);
    ld.row({&ld.halo(flt_a_cut, "A Cut"), &ld.halo(flt_a_res, "A Res"),
            &ld.halo(flt_b_cut, "B Cut"), &ld.halo(flt_b_res, "B Res")},
           H);
    ld.row({&ld.halo(flt_drive, "Drive"), &ld.halo(flt_env, "Env Amt"),
            &ld.halo(flt_key, "Key Trk")},
           H);
    ld.row({&ld.vial(fenv_a, "Attack", "A"), &ld.vial(fenv_d, "Decay", "D"),
            &ld.vial(fenv_s, "Sustain", "S"), &ld.vial(fenv_r, "Release", "R")},
           V);
    ld.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      if (r.getHeight() < 26.0f)
        return;
      drawRecessedWell(g, r, 2.0f);
      const auto a = r.reduced(6.0f, 4.0f);
      const float cutA = plain(p, flt_a_cut), resA = plain(p, flt_a_res);
      const float cutB = plain(p, flt_b_cut), resB = plain(p, flt_b_res);
      auto drawCurve = [&](float fc, float res, juce::Colour lineCol) {
        juce::Path path;
        const float q = 0.707f + res * 5.0f;
        for (int i = 0; i <= 96; ++i) {
          const float t = (float)i / 96.0f;
          const float f = 20.0f * std::pow(1000.0f, t);
          const float w = f / std::max(20.0f, fc);
          const float w2 = w * w;
          // 4-pole resonant magnitude approximation for clean UI readout
          const float mag =
              1.0f / std::sqrt(std::pow(1.0f - w2, 2.0f) + w2 / (q * q) +
                               0.12f * w2 * w2 * w2);
          const float db = juce::jlimit(
              -30.0f, 14.0f, 20.0f * std::log10(std::max(1e-4f, mag)));
          const float y =
              a.getBottom() -
              juce::jlimit(0.0f, 1.0f, (db + 30.0f) / 44.0f) * a.getHeight();
          const juce::Point<float> pt(a.getX() + t * a.getWidth(), y);
          if (i == 0)
            path.startNewSubPath(pt);
          else
            path.lineTo(pt);
        }
        g.setColour(lineCol);
        g.strokePath(path, juce::PathStrokeType(1.35f));
      };
      drawCurve(cutB, resB, cyan.withAlpha(0.8f));
      drawCurve(cutA, resA, amberHi);
      drawCaps(g, "A", a.withWidth(12.0f).withHeight(10.0f), 7.5f, amber,
               juce::Justification::centredLeft, 0.08f);
      drawCaps(g, "B",
               a.withTrimmedLeft(12.0f).withWidth(12.0f).withHeight(10.0f),
               7.5f, cyan, juce::Justification::centredLeft, 0.08f);
    };

    auto &ef = col("Follower", "12", envf_on, 0.98f);
    ef.row({&ef.halo(envf_cut, "Base"), &ef.halo(envf_depth, "Depth")}, H);
    ef.row({&ef.halo(envf_res, "Res"), &ef.halo(envf_mix, "Mix")}, H);
    ef.row({&ef.halo(envf_attack, "Attack"), &ef.halo(envf_release, "Release")},
           H);
    ef.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      const float e = p.telemetry.followerEnv.load(std::memory_order_relaxed);
      meterBar(g, r.withHeight(20.0f), e, amber, "ENVELOPE",
               juce::String(juce::roundToInt(e * 100.0f)) + "%");
    };

    auto &um = col("Unmask", "13", unmask_on, 0.88f);
    um.row({&um.halo(unmask_freq, "Sub Band"), &um.halo(unmask_depth, "Depth")},
           H);
    um.row({&um.halo(unmask_release, "Release")}, H);
    um.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      const float db = p.telemetry.unmaskDb.load(std::memory_order_relaxed);
      meterBar(g, r.withHeight(20.0f), db / 24.0f, cyan, "CARVING",
               db < 0.05f ? juce::String("0.0 dB")
                          : "-" + juce::String(db, 1) + " dB");
      g.setColour(steel);
      g.setFont(type::sans(10.5f, false));
      g.drawFittedText("Keyed by sub: carves drone & body where sub speaks.",
                       r.withTrimmedTop(28.0f).withHeight(56.0f).toNearestInt(),
                       juce::Justification::topLeft, 5);
    };

    auto &fm = col("Vox", "14", form_on, 0.86f);
    fm.row({&fm.halo(form_vowel, "Vowel"), &fm.halo(form_body, "Body")}, H);
    fm.row({&fm.halo(form_shift, "Shift"), &fm.halo(form_res, "Res")}, H);
    fm.row({&fm.halo(form_mix, "Mix")}, H);
    fm.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      if (r.getHeight() < 36.0f)
        return;
      drawRecessedWell(g, r, 2.0f);
      const float v = juce::jlimit(
          0.0f, 4.0f,
          plain(p, form_vowel) +
              p.telemetry.modOffset[form_vowel].load(
                  std::memory_order_relaxed) *
                  4.0f);
      static const char *letters = "AEIOU";
      static constexpr float kFormantVis[5][3]{
          {0.72f, 0.56f, 0.80f}, {0.48f, 0.78f, 0.86f}, {0.30f, 0.92f, 0.95f},
          {0.52f, 0.42f, 0.72f}, {0.32f, 0.36f, 0.68f}};
      const int a = juce::jlimit(0, 4, juce::roundToInt(v));
      const auto topR = r.withHeight(r.getHeight() * 0.60f);
      g.setColour(amberHi);
      g.setFont(type::display(std::min(38.0f, topR.getHeight() * 0.80f)));
      g.drawText(juce::String::charToString((juce::juce_wchar)letters[a]), topR,
                 juce::Justification::centred, false);

      const auto barArea = r.withTop(topR.getBottom()).reduced(10.0f, 6.0f);
      const float bw = (barArea.getWidth() - 8.0f) / 3.0f;
      for (int k = 0; k < 3; ++k) {
        const float bh = barArea.getHeight() * kFormantVis[a][k];
        const float bx = barArea.getX() + (float)k * (bw + 4.0f);
        g.setColour(seam);
        g.fillRect(
            juce::Rectangle<float>(bx, barArea.getY(), bw, barArea.getHeight()));
        g.setColour(k == 1 ? cyan : amber);
        g.fillRect(juce::Rectangle<float>(bx, barArea.getBottom() - bh, bw, bh));
      }
    };

    auto &rs = col("Resonator", "26", res_on, 1.65f);
    rs.row({&rs.selector(res_model, "Model")}, S);
    rs.row({&rs.halo(res_tune, "Tune"), &rs.halo(res_decay, "Decay"),
            &rs.halo(res_damper, "Damper")},
           H);
    rs.row({&rs.halo(res_bright, "Bright"), &rs.halo(res_stiff, "Stiff"),
            &rs.halo(res_pos, "Pos")},
           H);
    rs.row({&rs.halo(res_strike, "Strike"), &rs.halo(res_input, "Input"),
            &rs.halo(res_mix, "Mix")},
           H);
    rs.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      if (r.getHeight() < 36.0f)
        return;
      drawRecessedWell(g, r, 2.0f);
      const auto inner = r.reduced(6.0f, 4.0f);
      const int model = juce::roundToInt(plain(p, res_model));
      const float bright = plain(p, res_bright), stiff = plain(p, res_stiff),
                  pos = plain(p, res_pos);
      static const char *names[]{"STRING",   "TUBE",  "BELL", "BAR",
                                 "MEMBRANE", "PLATE", "BOWL", "GONG"};
      drawCaps(g, names[juce::jlimit(0, 7, model)], inner.withHeight(13.0f),
               8.5f, titanium, juce::Justification::centredLeft, 0.14f);
      drawCaps(g, model <= 1 ? "WAVEGUIDE" : "MODAL", inner.withHeight(13.0f),
               7.8f, cyan, juce::Justification::centredRight, 0.14f);

      auto ruler = inner.withTrimmedTop(16.0f).withTrimmedBottom(11.0f);
      if (ruler.getHeight() < 12.0f)
        return;
      const float lo = std::log2(0.5f), hi = std::log2(16.0f);
      auto xOf = [&](float ratio) {
        return ruler.getX() +
               (std::log2(ratio) - lo) / (hi - lo) * ruler.getWidth();
      };
      drawRule(g, ruler.getBottom(), ruler.getX(), ruler.getRight(), seamHi);

      using R = dsp::Resonator;
      std::array<float, 16> ratios{}, nodes{};
      size_t nPartials = 0;
      auto pushPartial = [&](float rVal, float nVal) {
        if (nPartials < ratios.size()) {
          ratios[nPartials] = rVal;
          nodes[nPartials] = nVal;
          ++nPartials;
        }
      };
      const float warpExp = 1.0f + 0.18f * (stiff - 0.15f);
      const float px = 0.05f + 0.9f * pos, py = 0.21f + 0.62f * px;
      auto addAll = [&](const float *xs, int n, bool warp) {
        for (int k = 0; k < n; ++k)
          pushPartial(warp ? std::pow(xs[k], warpExp) : xs[k], -1.0f);
      };
      if (model == 0)
        for (int k = 1; k <= 16; ++k)
          pushPartial((float)k *
                          std::sqrt(1.0f + 0.0016f * stiff * (float)(k * k)),
                      -1.0f);
      else if (model == 1)
        for (int k = 1; k <= 15; k += 2)
          pushPartial((float)k, -1.0f);
      else if (model == 2)
        addAll(R::kBell, 7, true);
      else if (model == 3)
        addAll(R::kBar, 5, true);
      else if (model == 5)
        for (int k = 0; k < 12; ++k)
          pushPartial(
              std::pow(R::kPlate[k], warpExp),
              0.15f +
                  0.85f *
                      std::abs(std::sin(dsp::kPi * (float)R::kPlateM[k] * px) *
                               std::sin(dsp::kPi * (float)R::kPlateN[k] * py)));
      else if (model == 6)
        addAll(R::kRing, 6, false);
      else if (model == 7)
        addAll(R::kGong, 12, true);
      else
        addAll(R::kMemb, 7, true);

      for (size_t k = 0; k < nPartials; ++k) {
        const float ratio = ratios[k];
        if (ratio > 16.0f)
          continue;
        const float ring =
            ratio <= 1.0f
                ? 1.0f
                : 1.0f / (1.0f + (1.0f - bright) * (ratio - 1.0f) * 0.7f);
        const float node =
            nodes[k] >= 0.0f ? nodes[k]
            : model <= 1
                ? std::abs(std::sin(dsp::kPi * ratio * pos))
                : 0.25f + 0.75f * std::abs(std::sin(dsp::kPi * (float)(k + 1) *
                                                    pos));
        const float hgt =
            ruler.getHeight() *
            juce::jlimit(0.08f, 1.0f, ring * (0.3f + 0.7f * node));
        const float x = xOf(ratio);
        g.setColour(amber.withAlpha(0.45f + 0.55f * ring));
        g.fillRect(juce::Rectangle<float>(x - 0.75f, ruler.getBottom() - hgt,
                                          1.5f, hgt));
        g.setColour(cyanHi);
        g.fillRect(juce::Rectangle<float>(
            x - 1.5f, ruler.getBottom() - hgt - 1.5f, 3.0f, 3.0f));
      }
      g.setColour(steel);
      g.setFont(type::mono(8.5f));
      g.drawText("1x",
                 juce::Rectangle<float>(xOf(1.0f) - 12.0f,
                                        ruler.getBottom() + 1.0f, 24.0f, 10.0f),
                 juce::Justification::centred, false);
      g.drawText("16x",
                 juce::Rectangle<float>(ruler.getRight() - 26.0f,
                                        ruler.getBottom() + 1.0f, 26.0f, 10.0f),
                 juce::Justification::centredRight, false);
    };
    break;
  }

  case 3: // ── 04 SPACE
  {
    auto &z = col("Z-Depth", "18", z_on, 1.0f);
    z.row({&z.halo(z_depth, "Depth")}, H);
    z.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      const float d =
          plain(p, z_depth) +
          p.telemetry.modOffset[z_depth].load(std::memory_order_relaxed) * 2.0f;
      const float side =
          std::min({r.getWidth(), r.getHeight() - 16.0f, 150.0f});
      auto a = juce::Rectangle<float>(side, side)
                   .withCentre({r.getCentreX(), r.getY() + side * 0.5f + 8.0f});
      drawRecessedWell(g, a, 2.0f);
      for (int k = 1; k <= 5; ++k) {
        const float s = 1.0f - (float)k * 0.15f;
        g.setColour(seamHi.withAlpha(0.30f + 0.10f * (float)k));
        g.drawRect(juce::Rectangle<float>(a.getWidth() * s, a.getHeight() * s)
                       .withCentre(a.getCentre()),
                   1.0f);
      }
      const float s =
          juce::jmap(juce::jlimit(-1.0f, 1.0f, d), -1.0f, 1.0f, 0.24f, 0.92f);
      g.setColour(amber);
      g.drawRect(juce::Rectangle<float>(a.getWidth() * s, a.getHeight() * s)
                     .withCentre(a.getCentre()),
                 1.5f);
      g.setColour(cyanHi);
      g.fillPath(diamond(a.getCentre(), 5.0f + 7.0f * s));
      drawCaps(g, d < -0.05f ? "BEHIND" : (d > 0.05f ? "IN FRONT" : "CENTER"),
               {r.getX(), a.getBottom() + 4.0f, r.getWidth(), 12.0f}, 8.2f,
               steel);
    };

    auto &cv = col("Body IR", "17", conv_on, 1.1f);
    cv.row({&cv.selector(conv_ir, "Space")}, S);
    cv.row({&cv.halo(conv_size, "Size"), &cv.halo(conv_mix, "Mix")}, H);
    auto &load = cv.button("LOAD IR");
    cv.row({&load}, R);
    load.onClick = [alive = p.aliveToken(), procPtr = &p] {
      auto chooser = std::make_shared<juce::FileChooser>(
          "Load an impulse response", juce::File(),
          "*.wav;*.aif;*.aiff;*.flac");
      chooser->launchAsync(
          juce::FileBrowserComponent::openMode |
              juce::FileBrowserComponent::canSelectFiles,
          [alive, procPtr, chooser](const juce::FileChooser &fc) {
            if (alive != nullptr && alive->load(std::memory_order_acquire) &&
                fc.getResult().existsAsFile())
              procPtr->loadUserImpulse(fc.getResult());
          });
    };
    cv.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      auto box = r.withHeight(std::min(64.0f, r.getHeight()));
      drawRecessedWell(g, box, 2.0f);
      const int t = juce::jlimit(0, 5, juce::roundToInt(plain(p, conv_ir)));
      static const char *names[]{"CONCRETE PIPE", "OLD TV SPEAKER", "STAIRWELL",
                                 "WATER TANK",    "CASSETTE SHELL", "USER IR"};
      juce::String n =
          t == 5 ? p.userImpulseName().toUpperCase() : juce::String(names[t]);
      drawCaps(g, n, box.reduced(6.0f), 9.5f, amberHi,
               juce::Justification::centred, 0.14f);
    };

    auto &bh = col("Black Hole FDN", "16", verb_on, 2.0f);
    bh.row({&bh.rune(verb_inf, "Infinite Freeze")}, R);
    bh.row({&bh.halo(verb_size, "Size"), &bh.halo(verb_decay, "Decay"),
            &bh.halo(verb_damp, "Damp"), &bh.halo(verb_mix, "Mix")},
           H);
    bh.row({&bh.halo(verb_mod, "Motion"), &bh.halo(verb_pre, "Pre-Dly"),
            &bh.halo(verb_low, "Low Cut")},
           H);
    bh.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      if (r.getHeight() < 40.0f)
        return;
      drawRecessedWell(g, r, 2.0f);
      const bool inf = isOn(p, verb_inf);
      const float size = plain(p, verb_size);
      const auto c = r.getCentre();
      const float maxR = std::min(r.getWidth() * 0.36f, r.getHeight() * 0.44f);
      for (int k = 0; k < 8; ++k) {
        const float t = (float)k / 7.0f;
        const float rr =
            maxR * (0.20f + 0.80f * std::pow(t, 0.65f + (1.0f - size)));
        g.setColour((inf ? cyan : amber).withAlpha(0.10f + 0.50f * (1.0f - t)));
        g.drawEllipse(
            juce::Rectangle<float>(rr * 2.5f, rr * 0.86f).withCentre(c), 1.0f);
      }
      g.setColour(inf ? cyanHi : chassis);
      g.fillEllipse(
          juce::Rectangle<float>(maxR * 0.34f, maxR * 0.34f).withCentre(c));
      g.setColour(inf ? titanium : amberHi);
      g.drawEllipse(
          juce::Rectangle<float>(maxR * 0.34f, maxR * 0.34f).withCentre(c),
          1.3f);
    };

    auto &hs = col("Haas Width", "19", haas_on, 1.0f);
    hs.row({&hs.halo(haas_time, "Time"), &hs.halo(haas_width, "Width")}, H);
    hs.row({&hs.halo(haas_xover, "Above")}, H);
    hs.instrument = [](juce::Graphics &g, juce::Rectangle<float> r) {
      g.setColour(steel);
      g.setFont(type::sans(10.5f, false));
      g.drawFittedText(
          "Side-channel delay only. Mono fold-down stays phase-intact.",
          r.withHeight(54.0f).toNearestInt(), juce::Justification::topLeft, 5);
    };
    break;
  }

  case 4: // ── 05 PRESSURE (Multiband + Ducker + Exciter & Mono +
          // Clipper/Output)
  default: {
    auto &mb = col("Multiband OTT", "21", ott_on, 2.35f);
    mb.row({&mb.halo(ott_depth, "Depth"), &mb.halo(ott_time, "Time"),
            &mb.halo(ott_up, "Upward"), &mb.halo(ott_down, "Down"),
            &mb.halo(ott_in, "In"), &mb.halo(ott_out, "Out")},
           H);
    mb.row({&mb.halo(ott_lox, "Low X"), &mb.halo(ott_hix, "High X"),
            &mb.halo(ott_low, "Low"), &mb.halo(ott_mid, "Mid"),
            &mb.halo(ott_high, "High")},
           H);
    mb.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      static const char *names[]{"LOW", "MID", "HIGH"};
      const float w = (r.getWidth() - 16.0f) / 3.0f;
      float gains[3];
      for (int b = 0; b < 3; ++b) {
        gains[b] = p.telemetry.ottGain[b].load(std::memory_order_relaxed);
        meterBar(g,
                 juce::Rectangle<float>(r.getX() + (float)b * (w + 8.0f),
                                        r.getY(), w, 20.0f),
                 gains[b] / 18.0f, gains[b] >= 0.0f ? cyan : amber, names[b],
                 (gains[b] > 0.0f ? "+" : "") + juce::String(gains[b], 1) +
                     "dB",
                 true);
      }

      auto a = r.withTrimmedTop(28.0f);
      if (a.getHeight() < 44.0f)
        return;
      drawRecessedWell(g, a, 2.0f);
      const auto inner = a.reduced(4.0f, 4.0f);
      auto xOf = [&](float hz) {
        return inner.getX() +
               inner.getWidth() * std::log(hz / 20.0f) / std::log(1000.0f);
      };
      const float zero = inner.getCentreY();
      g.setColour(seamHi);
      g.fillRect(
          juce::Rectangle<float>(inner.getX(), zero, inner.getWidth(), 1.0f));
      g.setFont(type::mono(8.5f));
      for (float f : {60.0f, 120.0f, 250.0f, 1000.0f, 4000.0f, 10000.0f}) {
        g.setColour(seam);
        g.fillRect(juce::Rectangle<float>(xOf(f), inner.getY(), 1.0f,
                                          inner.getHeight()));
        g.setColour(steel.withAlpha(0.65f));
        g.drawText(f >= 1000.0f ? juce::String((int)(f / 1000.0f)) + "k"
                                : juce::String((int)f),
                   juce::Rectangle<float>(
                       xOf(f) + 2.0f, inner.getBottom() - 11.0f, 26.0f, 10.0f),
                   juce::Justification::centredLeft, false);
      }
      const float lox = plain(p, ott_lox),
                  hix = std::max(plain(p, ott_hix), lox * 2.0f);
      const float edges[4]{inner.getX(), xOf(lox), xOf(hix), inner.getRight()};
      for (int b = 0; b < 3; ++b) {
        const float h = juce::jlimit(-1.0f, 1.0f, gains[b] / 18.0f) *
                        inner.getHeight() * 0.44f;
        auto plateR = juce::Rectangle<float>(
            edges[b] + 2.0f, std::min(zero, zero - h),
            std::max(2.0f, edges[b + 1] - edges[b] - 4.0f), std::abs(h) + 1.0f);
        g.setColour((gains[b] >= 0.0f ? cyan : amber).withAlpha(0.28f));
        g.fillRect(plateR);
        g.setColour(gains[b] >= 0.0f ? cyan : amber);
        g.fillRect(plateR.withHeight(1.5f).withY(zero - h -
                                                 (h >= 0.0f ? 0.0f : 1.5f)));
      }
      for (int k = 1; k < 3; ++k) {
        g.setColour(titanium.withAlpha(0.75f));
        g.fillRect(juce::Rectangle<float>(edges[k], inner.getY(), 1.0f,
                                          inner.getHeight()));
      }
    };

    auto &dk = col("Ducker", "23", duck_on, 0.95f);
    dk.row({&dk.selector(duck_mode, "Mode")}, S);
    dk.row({&dk.halo(duck_depth, "Depth"), &dk.halo(duck_release, "Release")},
           H);
    dk.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      const float gdb =
          dsp::gainToDb(p.telemetry.duckGain.load(std::memory_order_relaxed));
      meterBar(g, r.withHeight(20.0f), -gdb / 24.0f, amber, "DUCK",
               juce::String(std::abs(gdb) < 0.05f ? 0.0f : gdb, 1) + "dB");
      g.setColour(steel);
      g.setFont(type::sans(10.5f, false));
      g.drawFittedText("Ducks drone & dust so the sub punches cleanly through.",
                       r.withTrimmedTop(28.0f).withHeight(56.0f).toNearestInt(),
                       juce::Justification::topLeft, 5);
    };

    auto &ex = col("Exciter & Mono", "24", exc_on, 1.05f);
    ex.row({&ex.halo(exc_freq, "Above"), &ex.halo(exc_amount, "Amount")}, H);
    ex.row({&ex.halo(exc_harm, "Bite")}, H);
    auto &monoToggle = ex.rune(mono_on, "Mono Below");
    monoToggle.getProperties().set("cyan", true);
    ex.row({&monoToggle}, R);
    ex.row({&ex.halo(mono_freq, "Sub Cutoff")}, H);

    auto &cl = col("True-Peak Clip", "25", clip_on, 0.95f);
    cl.row({&cl.halo(clip_ceiling, "Ceiling"), &cl.halo(out_gain, "Output")},
           H);
    cl.row({&cl.halo(in_level, "Ext In")}, H);
    cl.instrument = [&p](juce::Graphics &g, juce::Rectangle<float> r) {
      const float sub = p.telemetry.subLevel.load(std::memory_order_relaxed);
      const float rms = p.telemetry.rms.load(std::memory_order_relaxed);
      auto dbText = [](float lin) {
        const float db = dsp::gainToDb(lin);
        return db < -80.0f ? juce::String("--") : juce::String(db, 1) + "dB";
      };
      meterBar(g, r.withHeight(20.0f), std::sqrt(sub), cyan, "SUB",
               dbText(sub));
      if (r.getHeight() > 48.0f)
        meterBar(g, r.withTrimmedTop(26.0f).withHeight(20.0f),
                 std::sqrt(rms) * 1.4f, amber, "RMS", dbText(rms));
    };
    break;
  }
  }
  return page;
}

//==============================================================================
// Spine (Left Signal Column + Voice Engine)
//==============================================================================
Spine::Spine(VesselProcessor &p, Registry &reg) : proc(p) {
  voice = std::make_unique<ModuleColumn>(p, reg, "Voice Engine", "", -1);
  auto &v = *voice;
  v.row({&v.selector(voice_mode, "Mode")}, Selector::kHeight);
  v.row({&v.halo(glide, "Glide"), &v.halo(v2_pb_range, "Bend")},
        ParamSlider::kHaloHeight);
  v.row({&v.selector(v2_vel_curve, "Vel Curve")}, Selector::kHeight);
  v.row({&v.vial(amp_a, "Attack", "A"), &v.vial(amp_d, "Decay", "D"),
         &v.vial(amp_s, "Sustain", "S"), &v.vial(amp_r, "Release", "R")},
        ParamSlider::kVialHeight);
  addAndMakeVisible(*voice);
  setRepaintsOnMouseActivity(false);
}

void Spine::resized() {
  const int top = kTop + 5 * kStageH + 8;
  voice->setBounds(2, top, getWidth() - 4, getHeight() - top - 2);
}

int Spine::stageAt(juce::Point<int> p) const {
  const int i = (p.y - kTop) / kStageH;
  return (p.y >= kTop && i >= 0 && i < 5) ? i : -1;
}

void Spine::mouseDown(const juce::MouseEvent &e) {
  const int s = stageAt(e.getPosition());
  if (s >= 0) {
    selected = s;
    repaint();
    if (onSelect)
      onSelect(s);
  }
}

void Spine::mouseMove(const juce::MouseEvent &e) {
  const int h = stageAt(e.getPosition());
  if (h != hover) {
    hover = h;
    repaint();
  }
  setMouseCursor(h >= 0 ? juce::MouseCursor::PointingHandCursor
                        : juce::MouseCursor::NormalCursor);
}

void Spine::paint(juce::Graphics &g) {
  drawMachinedPanel(g, getLocalBounds().toFloat(), 3.0f, true);
  const float w = (float)getWidth();

  for (int i = 0; i < 5; ++i) {
    const auto r = juce::Rectangle<float>(4.0f, (float)(kTop + i * kStageH),
                                          w - 8.0f, (float)kStageH - 2.0f);
    const bool sel = i == selected, hot = i == hover;

    if (sel) {
      g.setColour(plate);
      g.fillRoundedRectangle(r, 2.0f);
      g.setColour(amber.withAlpha(0.12f));
      g.fillRoundedRectangle(r, 2.0f);
      g.setColour(amber);
      g.fillRect(juce::Rectangle<float>(r.getX(), r.getY() + 4.0f, 3.0f,
                                        r.getHeight() - 8.0f));
      g.setColour(amber.withAlpha(0.45f));
      g.drawRoundedRectangle(r.reduced(0.5f), 2.0f, 0.9f);
    } else if (hot) {
      g.setColour(plate.withAlpha(0.6f));
      g.fillRoundedRectangle(r, 2.0f);
    }

    // Crisp geometric stage numeral
    g.setColour(sel ? amberHi : (hot ? titanium : steel));
    g.setFont(type::mono(14.0f));
    g.drawText(
        kStageNums[i],
        juce::Rectangle<float>(r.getX() + 8.0f, r.getY() + 6.0f, 26.0f, 18.0f),
        juce::Justification::centredLeft, false);

    drawCaps(g, kStageNames[i],
             {r.getX() + 36.0f, r.getY() + 7.0f, r.getWidth() - 42.0f, 14.0f},
             10.2f, sel || hot ? titanium : titanium.withAlpha(0.82f),
             juce::Justification::centredLeft, 0.16f);

    g.setColour(steel.withAlpha(0.82f));
    g.setFont(type::sans(9.5f, false));
    g.drawText(kStageCaps[i],
               juce::Rectangle<float>(r.getX() + 36.0f, r.getY() + 21.0f,
                                      r.getWidth() - 42.0f, 12.0f),
               juce::Justification::centredLeft, true);

    // Square module status LEDs
    for (int k = 0; k < 5; ++k) {
      const int pid = kStagePids[i][k];
      if (pid < 0)
        continue;
      const bool on = vessel::spec(pid).type == PType::Bool
                          ? isOn(proc, pid)
                          : plain(proc, pid) > 0.5f;
      const auto led =
          juce::Rectangle<float>(r.getX() + 36.0f + (float)k * 10.0f,
                                 r.getBottom() - 9.0f, 5.5f, 4.0f);
      g.setColour(on ? amber : seamHi);
      g.fillRect(led);
    }
  }
  drawRule(g, (float)(kTop + 5 * kStageH + 4), 6.0f, w - 6.0f, seamHi);
}

//==============================================================================
// Header (Top Command Bar)
//==============================================================================
Header::Header(VesselProcessor &p) : proc(p) {
  for (auto *b : {&prevBtn, &nextBtn, &saveBtn, &initBtn, &abBtnA, &abBtnB,
                  &abCopyBtn, &undoBtn, &redoBtn, &mpeBtn}) {
    addAndMakeVisible(*b);
    b->getProperties().set("textHeight", 8.6f);
  }

  prevBtn.setTooltip("Previous preset");
  nextBtn.setTooltip("Next preset");
  saveBtn.setTooltip("Save current sound as a V2 user preset");
  initBtn.setTooltip("Initialize to clean default patch");
  abBtnA.setTooltip("Compare Slot A");
  abBtnB.setTooltip("Compare Slot B");
  abCopyBtn.setTooltip("Copy active A/B slot to the other slot");
  undoBtn.setTooltip("Undo last parameter change (Cmd/Ctrl+Z)");
  redoBtn.setTooltip("Redo parameter change (Cmd/Ctrl+Shift+Z)");
  mpeBtn.setTooltip("Toggle MPE Per-Note Expression (Lower Zone Ch 2-16, "
                    "+/-48st Bend, CC74 Slide, Pressure)");
  mpeBtn.setClickingTogglesState(true);
  mpeBtn.getProperties().set("cyan", true);
  mpeAttach = std::make_unique<juce::ButtonParameterAttachment>(
      *proc.param(v2_mpe_on), mpeBtn, &proc.undoManager);

  prevBtn.onClick = [this] { stepPreset(-1); };
  nextBtn.onClick = [this] { stepPreset(1); };
  saveBtn.onClick = [this] {
    if (onSave)
      onSave();
  };
  initBtn.onClick = [this] {
    proc.setCurrentProgram(0);
    if (onInit)
      onInit();
    repaint();
  };

  abBtnA.onClick = [this] {
    proc.setActiveABSlot(0);
    repaint();
  };
  abBtnB.onClick = [this] {
    proc.setActiveABSlot(1);
    repaint();
  };
  abCopyBtn.onClick = [this] {
    if (proc.activeABSlot() == 0)
      proc.copyAToB();
    else
      proc.copyBToA();
    repaint();
  };
  undoBtn.onClick = [this] {
    proc.undo();
    repaint();
  };
  redoBtn.onClick = [this] {
    proc.redo();
    repaint();
  };

  static constexpr int kZooms[4]{75, 100, 125, 150};
  for (int i = 0; i < 4; ++i) {
    auto &zb = zoomBtns[(size_t)i];
    zb.setButtonText(juce::String(kZooms[i]) + "%");
    zb.getProperties().set("textHeight", 8.2f);
    zb.setTooltip("Set UI zoom to " + juce::String(kZooms[i]) + "%");
    zb.onClick = [this, pct = kZooms[i]] {
      setZoomPercent(pct);
      if (onZoom)
        onZoom((float)pct / 100.0f);
    };
    addAndMakeVisible(zb);
  }
  setZoomPercent(100);
}

void Header::setZoomPercent(int pct) {
  activeZoomPct = pct;
  static constexpr int kZooms[4]{75, 100, 125, 150};
  for (int i = 0; i < 4; ++i)
    zoomBtns[(size_t)i].setToggleState(kZooms[i] == pct,
                                       juce::dontSendNotification);
  repaint();
}

void Header::stepPreset(int delta) {
  const auto &all = proc.presetManager.allPresets();
  if (all.empty())
    return;
  int cur = -1;
  const auto curName = proc.currentPresetName();
  const auto curCat = proc.currentPresetCategory();
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i].name == curName && all[i].category == curCat) {
      cur = (int)i;
      break;
    }
  if (cur < 0)
    cur = juce::jlimit(0, (int)all.size() - 1, proc.getCurrentProgram());
  const int next = (cur + delta + (int)all.size()) % (int)all.size();
  if (next < proc.presetManager.factoryCount())
    proc.setCurrentProgram(next);
  else
    proc.applyPreset(all[(size_t)next]);
  repaint();
}

juce::Rectangle<int> Header::presetBarArea() const { return {204, 8, 364, 40}; }
juce::Rectangle<int> Header::presetClickArea() const {
  return {258, 10, 206, 36};
}
juce::Rectangle<int> Header::favStarArea() const { return {426, 14, 22, 28}; }
juce::Rectangle<float> Header::meterArea() const {
  return {1000.0f, 6.0f, 172.0f, 44.0f};
}
juce::Rectangle<float> Header::corrArea() const {
  return {1180.0f, 6.0f, 92.0f, 44.0f};
}

void Header::resized() {
  const auto pb = presetBarArea();
  prevBtn.setBounds(pb.getX() + 2, pb.getY() + 6, 24, 28);
  nextBtn.setBounds(pb.getX() + 28, pb.getY() + 6, 24, 28);
  saveBtn.setBounds(pb.getRight() - 96, pb.getY() + 6, 46, 28);
  initBtn.setBounds(pb.getRight() - 46, pb.getY() + 6, 42, 28);

  // Workflow cluster: x = 576..820
  abBtnA.setBounds(576, 14, 26, 28);
  abBtnB.setBounds(604, 14, 26, 28);
  abCopyBtn.setBounds(632, 14, 38, 28);
  undoBtn.setBounds(678, 14, 44, 28);
  redoBtn.setBounds(724, 14, 44, 28);
  mpeBtn.setBounds(776, 14, 44, 28);

  // Zoom selector: x = 828..991
  for (int i = 0; i < 4; ++i)
    zoomBtns[(size_t)i].setBounds(828 + i * 41, 14, 39, 28);
}

void Header::mouseDown(const juce::MouseEvent &e) {
  if (favStarArea().contains(e.getPosition())) {
    const auto &all = proc.presetManager.allPresets();
    const auto curName = proc.currentPresetName();
    const auto curCat = proc.currentPresetCategory();
    for (const auto &pr : all)
      if (pr.name == curName && pr.category == curCat) {
        proc.presetManager.toggleFavorite(pr);
        repaint();
        return;
      }
  }
  if (presetClickArea().contains(e.getPosition()) && onBrowse)
    onBrowse();
}

void Header::mouseMove(const juce::MouseEvent &e) {
  const bool hs = favStarArea().contains(e.getPosition());
  const bool hp = presetClickArea().contains(e.getPosition());
  if (hs != hoverStar || hp != hoverPreset) {
    hoverStar = hs;
    hoverPreset = hp;
    setMouseCursor((hs || hp) ? juce::MouseCursor::PointingHandCursor
                              : juce::MouseCursor::NormalCursor);
    repaint(presetBarArea());
  }
}

void Header::mouseExit(const juce::MouseEvent &) {
  if (hoverStar || hoverPreset) {
    hoverStar = hoverPreset = false;
    repaint(presetBarArea());
  }
}

void Header::refresh(float lufsMomentary, float sub120Corr, bool subHasSignal) {
  curLufs = lufsMomentary;
  curSubCorr = sub120Corr;
  curSubActive = subHasSignal;

  // Smooth peak ballistics + peak hold
  const float rawL = proc.telemetry.outPeakL.load(std::memory_order_relaxed);
  const float rawR = proc.telemetry.outPeakR.load(std::memory_order_relaxed);
  dispPeakL = rawL > dispPeakL ? rawL : dispPeakL * 0.88f;
  dispPeakR = rawR > dispPeakR ? rawR : dispPeakR * 0.88f;
  peakHoldL = rawL >= peakHoldL ? rawL : peakHoldL * 0.985f;
  peakHoldR = rawR >= peakHoldR ? rawR : peakHoldR * 0.985f;

  const int cc =
      proc.telemetry.clipCount.exchange(0, std::memory_order_relaxed);
  if (cc > 0)
    clipFlash = 1.0f;
  else
    clipFlash *= 0.90f;

  // Sync A/B, Undo/Redo button states
  const int slot = proc.activeABSlot();
  abBtnA.setToggleState(slot == 0, juce::dontSendNotification);
  abBtnB.setToggleState(slot == 1, juce::dontSendNotification);
  abCopyBtn.setButtonText(slot == 0 ? "A>B" : "B>A");
  undoBtn.setEnabled(proc.canUndo());
  redoBtn.setEnabled(proc.canRedo());

  const int abv = proc.abStateVersion.load(std::memory_order_relaxed);
  if (abv != lastAbVer) {
    lastAbVer = abv;
    repaint(presetBarArea());
  }

  repaint(juce::Rectangle<int>(998, 2, 278, 52));
}

void Header::paint(juce::Graphics &g) {
  const float w = (float)getWidth(), h = (float)getHeight();

  // Machined top command bar chassis
  g.setGradientFill(juce::ColourGradient(juce::Colour(0xff151923), 0.0f, 0.0f,
                                         juce::Colour(0xff0d1016), 0.0f, h,
                                         false));
  g.fillAll();
  g.setColour(bevelHi);
  g.fillRect(0.0f, 0.0f, w, 1.0f);
  drawRule(g, h - 1.0f, 0.0f, w, seamHi);

  // ── Wordmark: VESSEL II
  {
    // Geometric machined emblem
    const juce::Point<float> ec{22.0f, h * 0.5f};
    g.setColour(amber.withAlpha(0.16f));
    g.fillPath(invertedTriangle(ec, 22.0f));
    g.setColour(amber);
    g.fillPath(invertedTriangle(ec, 14.0f));
    g.setColour(chassis);
    g.fillPath(invertedTriangle({ec.x, ec.y - 1.0f}, 5.5f));

    // Bold geometric VESSEL wordmark
    g.setColour(titanium);
    g.setFont(type::display(22.0f).withExtraKerningFactor(0.14f));
    g.drawText("VESSEL", juce::Rectangle<float>(38.0f, 10.0f, 114.0f, 24.0f),
               juce::Justification::centredLeft, false);

    // Framed phosphor-amber II badge
    const auto badge = juce::Rectangle<float>(153.0f, 13.0f, 26.0f, 18.0f);
    g.setColour(amber.withAlpha(0.18f));
    g.fillRoundedRectangle(badge, 2.0f);
    g.setColour(amber);
    g.drawRoundedRectangle(badge.reduced(0.5f), 2.0f, 1.1f);
    g.setColour(amberHi);
    g.setFont(type::display(13.0f));
    g.drawText("II", badge, juce::Justification::centred, false);

    drawCaps(g, "DARK POLYPHONIC SYNTH", {38.0f, 34.0f, 152.0f, 12.0f}, 7.2f,
             steel.withAlpha(0.85f), juce::Justification::centredLeft, 0.14f);
  }

  // ── Preset Bar
  {
    const auto pb = presetBarArea().toFloat();
    drawRecessedWell(g, pb, 3.0f);
    const auto click = presetClickArea().toFloat();
    if (hoverPreset || browserOpen) {
      g.setColour(amber.withAlpha(browserOpen ? 0.12f : 0.06f));
      g.fillRoundedRectangle(click, 2.0f);
    }

    const auto pName = proc.currentPresetName();
    const auto pCat = proc.currentPresetCategory();

    drawCaps(g, pCat.isNotEmpty() ? pCat : "PRESET",
             {click.getX() + 4.0f, click.getY() + 2.0f, 160.0f, 10.0f}, 7.4f,
             amber, juce::Justification::centredLeft, 0.16f);
    g.setColour(titanium);
    g.setFont(type::sans(13.5f, true));
    g.drawText(pName.isNotEmpty() ? pName : "Init Vessel",
               juce::Rectangle<float>(click.getX() + 4.0f, click.getY() + 13.0f,
                                      164.0f, 19.0f),
               juce::Justification::centredLeft, true);

    // Favorite star toggle
    bool isFav = false;
    for (const auto &pr : proc.presetManager.allPresets())
      if (pr.name == pName && pr.category == pCat) {
        isFav = pr.favorite;
        break;
      }
    const auto sr = favStarArea().toFloat();
    drawStar(g, sr.getCentre(), 5.8f, isFav,
             isFav ? amberHi : (hoverStar ? titanium : steel.withAlpha(0.75f)));

    // Browser chevron
    const juce::Point<float> cc{click.getRight() - 9.0f, click.getCentreY()};
    const auto tri = invertedTriangle(cc, 6.5f);
    g.setColour(browserOpen ? amberHi : steel);
    g.fillPath(tri, browserOpen ? juce::AffineTransform::rotation(
                                      std::numbers::pi_v<float>, cc.x, cc.y)
                                : juce::AffineTransform());
  }

  // ── Stereo Output Peak / LUFS-style Meter
  {
    const auto mr = meterArea();
    drawRecessedWell(g, mr, 2.5f);

    drawCaps(g, "OUT", {mr.getX() + 6.0f, mr.getY() + 3.0f, 28.0f, 11.0f}, 7.5f,
             steel, juce::Justification::centredLeft, 0.12f);

    // Live LUFS-style numeric readout
    const juce::String lufsTxt =
        curLufs <= -68.0f ? "--.- LU" : juce::String(curLufs, 1) + " LU";
    g.setColour(curLufs > -8.0f ? amberHi : titanium);
    g.setFont(type::mono(9.5f));
    g.drawText(lufsTxt,
               juce::Rectangle<float>(mr.getX() + 34.0f, mr.getY() + 2.0f,
                                      114.0f, 12.0f),
               juce::Justification::centredRight, false);

    // Clip LED indicator (crimson only on clip)
    const auto clipR = juce::Rectangle<float>(mr.getRight() - 16.0f,
                                              mr.getY() + 4.0f, 10.0f, 8.0f);
    if (clipFlash > 0.08f) {
      g.setColour(crimson.withAlpha(clipFlash));
      g.fillRoundedRectangle(clipR, 1.5f);
    } else {
      g.setColour(seam);
      g.fillRoundedRectangle(clipR, 1.5f);
      g.setColour(seamHi);
      g.drawRoundedRectangle(clipR, 1.5f, 0.8f);
    }

    // Twin horizontal L/R peak bars (-60 .. 0 dBFS)
    const float ceilDb = plain(proc, clip_ceiling);
    auto dbToX = [&](float db, float x0, float bw) {
      const float t = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
      return x0 + t * bw;
    };
    const float barX = mr.getX() + 16.0f, barW = mr.getWidth() - 22.0f;
    const float peaks[2]{dispPeakL, dispPeakR};
    const float holds[2]{peakHoldL, peakHoldR};
    for (int c = 0; c < 2; ++c) {
      const float by = mr.getY() + 17.0f + (float)c * 9.0f;
      drawCaps(g, c == 0 ? "L" : "R", {mr.getX() + 5.0f, by - 1.0f, 9.0f, 8.0f},
               7.0f, steel, juce::Justification::centredLeft, 0.0f);
      const auto track = juce::Rectangle<float>(barX, by, barW, 6.0f);
      g.setColour(juce::Colour(0xff12151d));
      g.fillRect(track);

      const float pkDb = dsp::gainToDb(peaks[c]);
      const float fillW = std::max(0.0f, dbToX(pkDb, barX, barW) - barX);
      if (fillW > 0.5f) {
        juce::ColourGradient grad(cyan.withAlpha(0.85f), barX, by, amberHi,
                                  barX + barW * 0.85f, by, false);
        grad.addColour(0.55, amber);
        grad.addColour(0.96, crimson);
        g.setGradientFill(grad);
        g.fillRect(juce::Rectangle<float>(barX, by, fillW, 6.0f));
      }

      // Peak hold tick
      const float hDb = dsp::gainToDb(holds[c]);
      if (hDb > -58.0f) {
        const float hx = dbToX(hDb, barX, barW);
        g.setColour(hDb > ceilDb ? crimson : titanium);
        g.fillRect(juce::Rectangle<float>(hx - 1.0f, by, 1.5f, 6.0f));
      }

      // Ceiling line
      const float cx = dbToX(ceilDb, barX, barW);
      g.setColour(crimson.withAlpha(0.65f));
      g.fillRect(juce::Rectangle<float>(cx, by - 1.0f, 1.0f, 8.0f));
    }

    // Scale ticks (spacious placement inside recessed well)
    g.setFont(type::mono(7.4f));
    g.setColour(steel.withAlpha(0.74f));
    for (int db : {-48, -24, -12, 0}) {
      const float tx = dbToX((float)db, barX, barW);
      const auto align = (db == 0) ? juce::Justification::centredRight
                                   : juce::Justification::centred;
      const float lx = (db == 0) ? (tx - 16.0f) : (tx - 11.0f);
      g.drawText(juce::String(db),
                 juce::Rectangle<float>(lx, mr.getBottom() - 10.0f,
                                        db == 0 ? 16.0f : 22.0f, 9.0f),
                 align, false);
    }
  }

  // ── Sub-120 Hz Mono-Correlation Indicator
  {
    const auto cr = corrArea();
    drawRecessedWell(g, cr, 2.5f);

    drawCaps(g, "SUB<120", {cr.getX() + 5.0f, cr.getY() + 3.0f, 48.0f, 11.0f},
             7.2f, steel, juce::Justification::centredLeft, 0.08f);

    const auto corrCol = !curSubActive         ? steel
                         : curSubCorr >= 0.50f ? cyanHi
                         : curSubCorr >= 0.0f  ? amberHi
                                               : crimson;
    g.setColour(corrCol);
    g.setFont(type::mono(8.8f));
    const juce::String cTxt =
        curSubActive
            ? ((curSubCorr >= 0.0f ? "+" : "") + juce::String(curSubCorr, 2))
            : "MONO";
    g.drawText(cTxt,
               juce::Rectangle<float>(cr.getX() + 48.0f, cr.getY() + 2.0f,
                                      cr.getWidth() - 52.0f, 12.0f),
               juce::Justification::centredRight, false);

    // Bipolar correlation bar (-1 .. 0 .. +1)
    const auto track = juce::Rectangle<float>(
        cr.getX() + 6.0f, cr.getY() + 20.0f, cr.getWidth() - 12.0f, 7.0f);
    g.setColour(juce::Colour(0xff12151d));
    g.fillRect(track);
    const float mid = track.getCentreX();
    const float px =
        mid + juce::jlimit(-1.0f, 1.0f, curSubCorr) * track.getWidth() * 0.5f;
    g.setColour(corrCol.withAlpha(curSubActive ? 0.90f : 0.40f));
    g.fillRect(juce::Rectangle<float>(std::min(mid, px), track.getY() + 1.0f,
                                      std::max(2.0f, std::abs(px - mid)),
                                      track.getHeight() - 2.0f));
    g.setColour(titanium.withAlpha(0.7f));
    g.fillRect(juce::Rectangle<float>(mid - 0.5f, track.getY() - 1.5f, 1.0f,
                                      track.getHeight() + 3.0f));

    g.setFont(type::mono(7.2f));
    g.setColour(steel.withAlpha(0.72f));
    g.drawText("-1",
               juce::Rectangle<float>(track.getX(), cr.getBottom() - 11.0f,
                                      18.0f, 9.0f),
               juce::Justification::centredLeft, false);
    g.drawText(
        "0",
        juce::Rectangle<float>(mid - 8.0f, cr.getBottom() - 11.0f, 16.0f, 9.0f),
        juce::Justification::centred, false);
    g.drawText("+1",
               juce::Rectangle<float>(track.getRight() - 18.0f,
                                      cr.getBottom() - 11.0f, 18.0f, 9.0f),
               juce::Justification::centredRight, false);
  }
}

//==============================================================================
// ModPanel (6-Tab Modulation Matrix Deck: LFO 1..3, EXPR 5-source, MACRO 1..8,
// XY)
//==============================================================================
namespace {
const char *kSlotLetters = "ABCDEF";
const char *kTabNames[ModPanel::kPages]{"LFO 1", "LFO 2", "LFO 3",
                                        "EXPR",  "MACRO", "XY"};
const char *kExprTitles[5]{"VELOCITY", "MOD WHL", "PRESSURE", "SLIDE 74",
                           "BREATH"};
} // namespace

ModPanel::ModPanel(VesselProcessor &p, Registry &reg) : proc(p) {
  auto own = [&](juce::Component *c, std::vector<juce::Component *> &set) {
    owned.add(c);
    addChildComponent(c);
    set.push_back(c);
    return c;
  };
  auto halo = [&](int pid, const char *cap,
                  std::vector<juce::Component *> &set) {
    auto *s = new ParamSlider(p, pid, ParamSlider::Kind::Halo, cap);
    s->getProperties().set("compact", true);
    reg.sliders.push_back(s);
    own(s, set);
    return s;
  };
  auto bar = [&](int pid, const juce::String &cap,
                 std::vector<juce::Component *> &set, bool cyanStyle = false) {
    auto *s = new ParamSlider(p, pid, ParamSlider::Kind::Bar,
                              vessel::spec(pid).name, cap);
    if (cyanStyle)
      s->getProperties().set("cyan", true);
    reg.sliders.push_back(s);
    own(s, set);
    return s;
  };
  auto sel = [&](int pid, const juce::String &cap, bool compact,
                 std::vector<juce::Component *> &set) {
    auto *s = new Selector(p, pid, cap, compact);
    own(s, set);
    return s;
  };

  for (int l = 0; l < kNumLfos; ++l) {
    auto &pg = lfo[(size_t)l];
    const int b0 = lfoBase(l), e0 = lfoExt(l);
    pg.shape = sel(b0, "Shape", false, pg.all);
    pg.div = sel(b0 + 3, "Div", false, pg.all);
    pg.trig = sel(e0, "Trig", false, pg.all);
    pg.sync = new Rune(p, b0 + 2, "Sync");
    own(pg.sync, pg.all);
    pg.uni = new Rune(p, e0 + 4, "Uni");
    own(pg.uni, pg.all);
    pg.rate = halo(b0 + 1, "Rate", pg.all);
    pg.warp = halo(b0 + 4, "Warp", pg.all);
    pg.phase = halo(e0 + 1, "Phase", pg.all);
    pg.fade = halo(e0 + 2, "Fade", pg.all);
    pg.smooth = halo(e0 + 3, "Smooth", pg.all);
    for (int s = 0; s < kLfoSlots; ++s) {
      pg.dest[(size_t)s] = sel(
          lfoDestPid(l, s), vessel::spec(lfoDestPid(l, s)).name, true, pg.all);
      pg.amt[(size_t)s] =
          bar(lfoAmtPid(l, s),
              juce::String::charToString((juce::juce_wchar)kSlotLetters[s]),
              pg.all, true);
    }
  }

  // 5 Expression sources: Vel, Mod Wheel, Aftertouch, Slide (CC74), Breath
  // (CC2)
  for (int src = 0; src < 5; ++src)
    for (int s = 0; s < 2; ++s) {
      const int dPid = src < 3
                           ? exprDestPid(src, s)
                           : (src == 3 ? slideDestPid(s) : breathDestPid(s));
      const int aPid = src < 3 ? exprAmtPid(src, s)
                               : (src == 3 ? slideAmtPid(s) : breathAmtPid(s));
      expr.dest[(size_t)src][(size_t)s] =
          sel(dPid, vessel::spec(dPid).name, true, expr.all);
      expr.amt[(size_t)src][(size_t)s] = bar(
          aPid, juce::String::charToString((juce::juce_wchar)kSlotLetters[s]),
          expr.all, true);
    }
  expr.velAmp = bar(vel_amp, "AMP", expr.all, false);

  // Macros 1..8 (each has its value dial + 6 modulation route slots)
  std::vector<juce::Component *> macroSet;
  for (int m = 0; m < kNumMacros; ++m) {
    auto *k = new ParamSlider(p, macroPid(m), ParamSlider::Kind::Halo, "VALUE");
    reg.sliders.push_back(k);
    own(k, macroSet);
    mac.knob[(size_t)m] = k;
    for (int s = 0; s < kMacroSlots; ++s) {
      mac.dest[(size_t)m][(size_t)s] =
          sel(macroDestPid(m, s), vessel::spec(macroDestPid(m, s)).name, true,
              macroSet);
      mac.amt[(size_t)m][(size_t)s] =
          bar(macroAmtPid(m, s),
              juce::String::charToString((juce::juce_wchar)kSlotLetters[s]),
              macroSet, false);
    }
  }

  // XY Sculptor axis destinations
  for (int axis = 0; axis < 2; ++axis)
    for (int s = 0; s < kXySlots; ++s) {
      xy.dest[(size_t)axis][(size_t)s] =
          sel(xyDestPid(axis, s), vessel::spec(xyDestPid(axis, s)).name, true,
              xy.all);
      xy.amt[(size_t)axis][(size_t)s] =
          bar(xyAmtPid(axis, s),
              juce::String::charToString((juce::juce_wchar)kSlotLetters[s]),
              xy.all, axis == 1);
    }

  addChildComponent(scatter);
  scatter.getProperties().set("textHeight", 8.0f);
  scatter.onClick = [this] {
    if (current < kNumLfos)
      proc.randomiseDrawTable(current);
  };
  scatter.setTooltip("Generate a new random drawn LFO table");
  select(0); // default to LFO 1 (since Hero Macros 1-4 are always visible in
             // the Bottom Dock!)
}

ModPanel::~ModPanel() {
  if (rename != nullptr) {
    rename->onReturnKey = nullptr;
    rename->onEscapeKey = nullptr;
    rename->onFocusLost = nullptr;
    rename.reset();
  }
}

void ModPanel::selectMacro(int m) {
  macroSel = juce::jlimit(0, kNumMacros - 1, m);
  if (current != kMacroPage) {
    select(kMacroPage);
    return;
  }
  for (int k = 0; k < kNumMacros; ++k) {
    mac.knob[(size_t)k]->setVisible(k == macroSel);
    for (int s = 0; s < kMacroSlots; ++s) {
      mac.dest[(size_t)k][(size_t)s]->setVisible(k == macroSel);
      mac.amt[(size_t)k][(size_t)s]->setVisible(k == macroSel);
    }
  }
  refresh();
  repaint();
}

void ModPanel::select(int page) {
  current = juce::jlimit(0, kPages - 1, page);
  if (rename != nullptr)
    rename.reset();
  for (int l = 0; l < kNumLfos; ++l)
    for (auto *c : lfo[(size_t)l].all)
      c->setVisible(l == current);
  for (auto *c : expr.all)
    c->setVisible(current == kExprPage);
  for (auto *c : xy.all)
    c->setVisible(current == kXyPage);
  for (int m = 0; m < kNumMacros; ++m) {
    mac.knob[(size_t)m]->setVisible(current == kMacroPage && m == macroSel);
    for (int s = 0; s < kMacroSlots; ++s) {
      mac.dest[(size_t)m][(size_t)s]->setVisible(current == kMacroPage &&
                                                 m == macroSel);
      mac.amt[(size_t)m][(size_t)s]->setVisible(current == kMacroPage &&
                                                m == macroSel);
    }
  }
  scatter.setVisible(current < kNumLfos);
  refresh();
  repaint();
}

void ModPanel::setIdle(ParamSlider *s, bool idle) {
  if ((bool)s->getProperties()["idle"] == idle)
    return;
  s->getProperties().set("idle", idle);
  s->repaint();
}

void ModPanel::refresh() {
  auto destIs = [this](int pid) { return juce::roundToInt(plain(proc, pid)); };
  if (current < kNumLfos) {
    auto &pg = lfo[(size_t)current];
    const bool synced = isOn(proc, lfoBase(current) + 2);
    pg.rate->setAlpha(synced ? 0.38f : 1.0f);
    pg.div->setAlpha(synced ? 1.0f : 0.38f);
    for (int s = 0; s < kLfoSlots; ++s)
      setIdle(pg.amt[(size_t)s], destIs(lfoDestPid(current, s)) == 0);
    repaint(canvas.toNearestInt().expanded(3));
  } else if (current == kMacroPage) {
    for (int s = 0; s < kMacroSlots; ++s)
      setIdle(mac.amt[(size_t)macroSel][(size_t)s],
              destIs(macroDestPid(macroSel, s)) == 0);
    if (const int v = proc.macroNamesVersion.load(std::memory_order_relaxed);
        v != namesSeen) {
      namesSeen = v;
      repaint();
    }
  } else if (current == kExprPage) {
    for (int src = 0; src < 5; ++src)
      for (int s = 0; s < 2; ++s) {
        const int dPid = src < 3
                             ? exprDestPid(src, s)
                             : (src == 3 ? slideDestPid(s) : breathDestPid(s));
        setIdle(expr.amt[(size_t)src][(size_t)s], destIs(dPid) == 0);
      }
    repaint(juce::Rectangle<int>(86, 26, getWidth() - 90, 24));
  } else if (current == kXyPage) {
    for (int axis = 0; axis < 2; ++axis)
      for (int s = 0; s < kXySlots; ++s)
        setIdle(xy.amt[(size_t)axis][(size_t)s],
                destIs(xyDestPid(axis, s)) == 0);
    repaint(juce::Rectangle<int>(86, 26, getWidth() - 90, 26));
  }
  repaint(tabs.toNearestInt().expanded(2));

  const double now = juce::Time::getMillisecondCounterHiRes();
  if (messageUntil > 0.0 || flashUntil > 0.0) {
    if (now > messageUntil)
      messageUntil = 0.0;
    if (now > flashUntil)
      flashUntil = 0.0;
    repaint();
  }
}

juce::Rectangle<float> ModPanel::tabRect(int i) const {
  const float th = tabs.getHeight() / (float)kPages;
  return {tabs.getX(), tabs.getY() + (float)i * th, tabs.getWidth(), th - 1.0f};
}

juce::Rectangle<float> ModPanel::macroChip(int m) const {
  const float x0 = 92.0f, totalW = (float)getWidth() - 100.0f;
  const float cw = totalW / (float)kNumMacros;
  return {x0 + (float)m * cw, 28.0f, cw - 3.0f, 24.0f};
}

juce::Rectangle<float> ModPanel::exprHead(int src) const {
  const float x0 = 92.0f, totalW = (float)getWidth() - 100.0f;
  const float cw = totalW / 5.0f;
  return {x0 + (float)src * cw, 28.0f, cw - 6.0f, 18.0f};
}

juce::Rectangle<float> ModPanel::routeRow(int page, int slot) const {
  if (page < kNumLfos)
    return {588.0f, 28.0f + (float)slot * 34.0f, (float)getWidth() - 596.0f,
            32.0f};
  if (page == kMacroPage) {
    const float x0 = 186.0f, gw = ((float)getWidth() - 194.0f) / 3.0f;
    return {x0 + (float)(slot % 3) * gw, 62.0f + (float)(slot / 3) * 48.0f,
            gw - 6.0f, 36.0f};
  }
  if (page == kExprPage) {
    const int src = slot / 2, s = slot % 2;
    const auto h = exprHead(src);
    return {h.getX(), 54.0f + (float)s * 42.0f, h.getWidth(), 36.0f};
  }
  // XY
  const int axis = slot / 2, s = slot % 2;
  const float x0 = 92.0f, cw = ((float)getWidth() - 106.0f) * 0.5f;
  return {x0 + (float)axis * (cw + 8.0f), 60.0f + (float)s * 46.0f, cw, 38.0f};
}

void ModPanel::resized() {
  tabs = {6.0f, 28.0f, 76.0f, (float)getHeight() - 34.0f};

  // LFO 1..3 layout
  for (auto &pg : lfo) {
    pg.shape->setBounds(92, 28, 102, Selector::kHeight);
    pg.div->setBounds(198, 28, 76, Selector::kHeight);
    pg.trig->setBounds(278, 28, 78, Selector::kHeight);
    pg.sync->setBounds(360, 29, 64, 18);
    pg.uni->setBounds(360, 48, 64, 18);

    ParamSlider *halos[]{pg.rate, pg.warp, pg.phase, pg.fade, pg.smooth};
    for (int i = 0; i < 5; ++i)
      halos[i]->setBounds(92 + i * 67, 72, 66, 68);

    for (int s = 0; s < kLfoSlots; ++s) {
      const auto r = routeRow(0, s).toNearestInt();
      pg.dest[(size_t)s]->setBounds(r.withHeight(17));
      pg.amt[(size_t)s]->setBounds(r.withTrimmedTop(18).withHeight(14));
    }
  }

  // 5-column Expression layout
  for (int src = 0; src < 5; ++src)
    for (int s = 0; s < 2; ++s) {
      const auto r = routeRow(kExprPage, src * 2 + s).toNearestInt();
      expr.dest[(size_t)src][(size_t)s]->setBounds(r.withHeight(18));
      expr.amt[(size_t)src][(size_t)s]->setBounds(
          r.withTrimmedTop(19).withHeight(15));
    }
  expr.velAmp->setBounds(exprHead(0).toNearestInt().withY(142).withHeight(16));

  // Macros 1..8 layout
  for (int m = 0; m < kNumMacros; ++m) {
    mac.knob[(size_t)m]->setBounds(94, 62, 84, 88);
    for (int s = 0; s < kMacroSlots; ++s) {
      const auto r = routeRow(kMacroPage, s).toNearestInt();
      mac.dest[(size_t)m][(size_t)s]->setBounds(r.withHeight(18));
      mac.amt[(size_t)m][(size_t)s]->setBounds(
          r.withTrimmedTop(19).withHeight(15));
    }
  }

  // XY layout
  for (int axis = 0; axis < 2; ++axis)
    for (int s = 0; s < kXySlots; ++s) {
      const auto r = routeRow(kXyPage, axis * 2 + s).toNearestInt();
      xy.dest[(size_t)axis][(size_t)s]->setBounds(r.withHeight(18));
      xy.amt[(size_t)axis][(size_t)s]->setBounds(
          r.withTrimmedTop(20).withHeight(15));
    }

  canvas = {434.0f, 30.0f, 144.0f, 108.0f};
  scatter.setBounds(434, 142, 144, 18);
}

void ModPanel::paint(juce::Graphics &g) {
  drawMachinedPanel(g, getLocalBounds().toFloat(), 3.0f, true);
  const float w = (float)getWidth();

  const juce::String heading =
      current < kNumLfos      ? ("WARPED LFO " + juce::String(current + 1))
      : current == kExprPage  ? "EXPRESSION & MPE ROUTING"
      : current == kMacroPage ? "MACRO MATRIX (M1-M8)"
                              : "XY SCULPTOR ROUTING";
  drawCaps(g, heading, {10.0f, 4.0f, 220.0f, 18.0f}, 9.4f, titanium,
           juce::Justification::centredLeft, 0.16f);

  const bool msg = messageUntil > 0.0;
  g.setColour(msg ? cyanHi : steel.withAlpha(0.85f));
  g.setFont(type::sans(10.5f, false));
  const juce::String hint =
      current == kMacroPage
          ? juce::String::fromUTF8(
                "Drag any cyan diamond onto a control to map  \xc2\xb7  "
                "double-click macro chip to rename")
          : juce::String::fromUTF8(
                "Drag an LFO tab or Expression header onto any control  "
                "\xc2\xb7  right-click any control for MIDI Learn / routes");
  g.drawText(msg ? message : hint,
             juce::Rectangle<float>(230.0f, 4.0f, w - 240.0f, 18.0f),
             juce::Justification::centredRight, true);

  drawRule(g, 24.0f, 6.0f, w - 6.0f, seam);
  drawSeam(g, 86.0f, 28.0f, (float)getHeight() - 6.0f, seam);

  paintTabs(g);
  if (current < kNumLfos)
    paintLfo(g);
  else if (current == kExprPage)
    paintExpr(g);
  else if (current == kMacroPage)
    paintMacros(g);
  else
    paintXy(g);

  if (flashUntil > 0.0 && flashPage == current && flashSlot >= 0) {
    const float a = (float)juce::jlimit(
        0.0, 1.0,
        (flashUntil - juce::Time::getMillisecondCounterHiRes()) / 1800.0);
    const auto r = routeRow(current, flashSlot);
    g.setColour(cyan.withAlpha(0.14f * a));
    g.fillRoundedRectangle(r.expanded(2.0f, 1.0f), 2.0f);
    g.setColour(cyanHi.withAlpha(a));
    g.fillRect(
        juce::Rectangle<float>(r.getX() - 4.0f, r.getY(), 2.0f, r.getHeight()));
  }
}

void ModPanel::paintTabs(juce::Graphics &g) {
  for (int t = 0; t < kPages; ++t) {
    const int i = pageAtTab(t);
    const auto r = tabRect(t);
    const bool sel = i == current;
    if (sel) {
      g.setColour(plate);
      g.fillRoundedRectangle(r, 2.0f);
      g.setColour(amber);
      g.fillRect(juce::Rectangle<float>(r.getX(), r.getY() + 3.0f, 2.5f,
                                        r.getHeight() - 6.0f));
    }

    drawCaps(g, kTabNames[i], r.withTrimmedLeft(7.0f).withTrimmedRight(22.0f),
             8.2f, sel ? amberHi : titanium.withAlpha(0.78f),
             juce::Justification::centredLeft, 0.10f);

    // Live activity bar on right of tab
    const auto barR = juce::Rectangle<float>(
        r.getRight() - 18.0f, r.getCentreY() - 2.0f, 14.0f, 4.0f);
    g.setColour(well);
    g.fillRect(barR);
    float v = 0.0f;
    if (i < kNumLfos) {
      const float raw =
          proc.telemetry.lfoValue[i].load(std::memory_order_relaxed);
      v = isOn(proc, lfoExt(i) + 4) ? raw : 0.5f * (raw + 1.0f);
    } else if (i == kExprPage)
      v = std::max(
          {proc.telemetry.velocity.load(), proc.telemetry.modWheel.load(),
           proc.telemetry.aftertouch.load(), proc.telemetry.slide.load()});
    else if (i == kMacroPage)
      v = proc.param(macroPid(macroSel))->getValue();
    else
      v = std::max(std::abs(plain(proc, v2_xy_x) - 0.5f),
                   std::abs(plain(proc, v2_xy_y) - 0.5f)) *
          2.0f;

    g.setColour(i < kNumLfos || i == kExprPage ? cyan : amber);
    g.fillRect(barR.withWidth(barR.getWidth() * juce::jlimit(0.0f, 1.0f, v)));
  }
}

void ModPanel::paintLfo(juce::Graphics &g) {
  drawSeam(g, 428.0f, 28.0f, (float)getHeight() - 6.0f, seam);
  drawSeam(g, 582.0f, 28.0f, (float)getHeight() - 6.0f, seam);

  // Subtle trigger status footer under knobs
  const int trig = juce::roundToInt(plain(proc, lfoExt(current)));
  static const char *kTrigDesc[3]{"FREE: runs continuously",
                                  "RETRIG: resets phase on note-on",
                                  "ONE-SHOT: single cycle envelope"};
  g.setColour(steel);
  g.setFont(type::sans(9.8f, false));
  g.drawText(kTrigDesc[juce::jlimit(0, 2, trig)],
             juce::Rectangle<float>(94.0f, 144.0f, 330.0f, 16.0f),
             juce::Justification::centredLeft, true);

  // Waveform canvas
  drawRecessedWell(g, canvas, 2.0f);
  g.setColour(seam);
  g.fillRect(juce::Rectangle<float>(canvas.getX() + 2.0f, canvas.getCentreY(),
                                    canvas.getWidth() - 4.0f, 1.0f));
  for (int k = 1; k < 4; ++k)
    g.fillRect(juce::Rectangle<float>(
        canvas.getX() + canvas.getWidth() * (float)k * 0.25f,
        canvas.getY() + 2.0f, 1.0f, canvas.getHeight() - 4.0f));

  const int shape = juce::roundToInt(plain(proc, lfoBase(current)));
  const bool uni = isOn(proc, lfoExt(current) + 4);
  const auto inner = canvas.reduced(5.0f, 12.0f).withTrimmedTop(3.0f);
  auto yOf = [&](float v) {
    return uni ? inner.getBottom() - v * inner.getHeight()
               : inner.getCentreY() - v * inner.getHeight() * 0.5f;
  };
  const float base = uni ? inner.getBottom() : inner.getCentreY();
  juce::Path path, fill;
  for (int i = 0; i <= 140; ++i) {
    const float ph = (float)i / 140.0f;
    const float v = proc.previewLfo(current, std::min(ph, 0.9999f));
    const juce::Point<float> pt(inner.getX() + ph * inner.getWidth(), yOf(v));
    if (i == 0) {
      path.startNewSubPath(pt);
      fill.startNewSubPath(inner.getX(), base);
    } else
      path.lineTo(pt);
    fill.lineTo(pt);
  }
  fill.lineTo(inner.getRight(), base);
  fill.closeSubPath();
  g.setColour(cyan.withAlpha(0.14f));
  g.fillPath(fill);
  g.setColour(cyan);
  g.strokePath(path, juce::PathStrokeType(1.5f));

  if (shape == dsp::WarpedLfo::Drawn) {
    for (int i = 0; i < kDrawPoints; ++i) {
      const float v = proc.drawTables[(size_t)current][(size_t)i].load(
          std::memory_order_relaxed);
      g.setColour(amberHi.withAlpha(0.75f));
      g.fillRect(juce::Rectangle<float>(
          inner.getX() +
              ((float)i + 0.5f) / (float)kDrawPoints * inner.getWidth() - 1.5f,
          yOf(uni ? 0.5f * (v + 1.0f) : v) - 1.5f, 3.0f, 3.0f));
    }
    drawCaps(g, "DRAW SHAPE", canvas.withHeight(12.0f).translated(0.0f, 2.0f),
             7.4f, amberHi);
  } else
    drawCaps(g, proc.param(lfoBase(current))->getCurrentValueAsText(),
             canvas.withHeight(12.0f).translated(0.0f, 2.0f), 7.4f, steel);

  const float ph =
      proc.telemetry.lfoPhase[current].load(std::memory_order_relaxed);
  const float lv =
      proc.telemetry.lfoValue[current].load(std::memory_order_relaxed);
  const juce::Point<float> c{
      inner.getX() + ph * inner.getWidth(),
      juce::jlimit(canvas.getY() + 2.0f, canvas.getBottom() - 2.0f, yOf(lv))};
  g.setColour(amber.withAlpha(0.30f));
  g.fillRect(juce::Rectangle<float>(c.x, canvas.getY() + 2.0f, 1.0f,
                                    canvas.getHeight() - 4.0f));
  g.setColour(amberHi);
  g.fillPath(diamond(c, 7.0f));
}

void ModPanel::paintExpr(juce::Graphics &g) {
  const float vals[5]{proc.telemetry.velocity.load(std::memory_order_relaxed),
                      proc.telemetry.modWheel.load(std::memory_order_relaxed),
                      proc.telemetry.aftertouch.load(std::memory_order_relaxed),
                      proc.telemetry.slide.load(std::memory_order_relaxed),
                      proc.telemetry.breath.load(std::memory_order_relaxed)};

  for (int src = 0; src < 5; ++src) {
    const auto h = exprHead(src);
    if (src > 0)
      drawSeam(g, h.getX() - 3.0f, 28.0f, (float)getHeight() - 6.0f, seam);

    if (src < 3) {
      g.setColour(cyan);
      g.fillPath(diamond({h.getX() + 5.0f, h.getCentreY()}, 6.0f));
    }
    drawCaps(g, kExprTitles[src],
             h.withTrimmedLeft(src < 3 ? 13.0f : 2.0f).withTrimmedRight(30.0f),
             8.2f, titanium, juce::Justification::centredLeft, 0.10f);
    g.setColour(cyanHi);
    g.setFont(type::mono(9.0f));
    g.drawText(juce::String(juce::roundToInt(vals[src] * 100.0f)) + "%", h,
               juce::Justification::centredRight, false);

    const auto m = juce::Rectangle<float>(h.getX(), h.getBottom() + 2.0f,
                                          h.getWidth(), 3.0f);
    g.setColour(well);
    g.fillRect(m);
    g.setColour(cyan);
    g.fillRect(m.withWidth(m.getWidth() * juce::jlimit(0.0f, 1.0f, vals[src])));

    if (src > 0) {
      static const char *foot[5]{"", "MIDI CC 1", "Poly / Ch AT",
                                 "MPE Slide CC 74", "Breath CC 2"};
      g.setColour(steel.withAlpha(0.75f));
      g.setFont(type::sans(9.2f, false));
      g.drawText(foot[src],
                 juce::Rectangle<float>(h.getX(), 142.0f, h.getWidth(), 14.0f),
                 juce::Justification::centredLeft, true);
    }
  }
}

void ModPanel::paintMacros(juce::Graphics &g) {
  for (int m = 0; m < kNumMacros; ++m) {
    const auto ch = macroChip(m);
    const bool sel = m == macroSel;
    const bool hero = m < kHeroMacros;
    g.setColour(sel ? plate : well);
    g.fillRoundedRectangle(ch, 2.0f);
    g.setColour(sel ? amber : (hero ? amberDim : seamHi));
    g.drawRoundedRectangle(ch.reduced(0.5f), 2.0f, sel ? 1.1f : 0.8f);

    // Drag diamond
    const juce::Point<float> d{ch.getX() + 7.0f, ch.getCentreY()};
    g.setColour(cyan);
    g.fillPath(diamond(d, 5.5f));

    const auto nm = proc.macroName(m);
    const auto shown = nm.isNotEmpty() ? nm : ("M" + juce::String(m + 1));
    drawCaps(g, shown, ch.withTrimmedLeft(14.0f).withTrimmedRight(4.0f), 8.2f,
             sel ? amberHi : (nm.isNotEmpty() ? titanium : steel),
             juce::Justification::centredLeft, 0.08f);

    int used = 0;
    for (int s = 0; s < kMacroSlots; ++s)
      used += juce::roundToInt(plain(proc, macroDestPid(m, s))) != 0 ? 1 : 0;
    for (int s = 0; s < used; ++s) {
      g.setColour(cyan);
      g.fillRect(juce::Rectangle<float>(ch.getX() + 14.0f + (float)s * 5.0f,
                                        ch.getBottom() - 2.5f, 3.0f, 1.5f));
    }
  }
  drawRule(g, 56.0f, 92.0f, (float)getWidth() - 8.0f, seam);
  drawSeam(g, 180.0f, 60.0f, (float)getHeight() - 6.0f, seam);
}

void ModPanel::paintXy(juce::Graphics &g) {
  const float vals[2]{plain(proc, v2_xy_x), plain(proc, v2_xy_y)};
  const juce::String lbls[2]{"X AXIS: " + proc.xyLabelX(),
                             "Y AXIS: " + proc.xyLabelY()};
  const float x0 = 92.0f, cw = ((float)getWidth() - 106.0f) * 0.5f;

  for (int axis = 0; axis < 2; ++axis) {
    const float ax = x0 + (float)axis * (cw + 8.0f);
    if (axis == 1)
      drawSeam(g, ax - 4.0f, 28.0f, (float)getHeight() - 6.0f, seam);
    const auto col = axis == 0 ? amber : cyan;
    drawCaps(g, lbls[axis], {ax, 28.0f, cw - 48.0f, 16.0f}, 9.0f, col,
             juce::Justification::centredLeft, 0.14f);
    g.setColour(titanium);
    g.setFont(type::mono(9.8f));
    g.drawText(juce::String(juce::roundToInt(vals[axis] * 100.0f)) + "%",
               juce::Rectangle<float>(ax + cw - 46.0f, 28.0f, 46.0f, 16.0f),
               juce::Justification::centredRight, false);

    const auto barR = juce::Rectangle<float>(ax, 47.0f, cw, 4.0f);
    g.setColour(well);
    g.fillRect(barR);
    const float mid = barR.getCentreX();
    const float px = barR.getX() + vals[axis] * barR.getWidth();
    g.setColour(col);
    g.fillRect(juce::Rectangle<float>(std::min(mid, px), barR.getY(),
                                      std::max(2.0f, std::abs(px - mid)),
                                      barR.getHeight()));
    g.setColour(steel);
    g.fillRect(juce::Rectangle<float>(mid - 0.5f, barR.getY() - 1.0f, 1.0f,
                                      barR.getHeight() + 2.0f));
  }
}

void ModPanel::beginRename(int m) {
  selectMacro(m);
  rename = std::make_unique<juce::TextEditor>();
  auto &ed = *rename;
  addAndMakeVisible(ed);
  ed.setBounds(macroChip(m).withTrimmedLeft(12.0f).toNearestInt());
  ed.setFont(type::caps(9.5f, 0.08f));
  ed.setInputRestrictions(20);
  ed.setIndents(2, 1);
  ed.setText(proc.macroName(m), false);
  ed.selectAll();
  auto commit = [this, m](bool keep) {
    if (rename == nullptr)
      return;
    if (keep)
      proc.setMacroName(m, rename->getText().trim());
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<ModPanel>(this)] {
          if (safe != nullptr) {
            safe->rename.reset();
            safe->repaint();
          }
        });
  };
  ed.onReturnKey = [commit] { commit(true); };
  ed.onFocusLost = [commit] { commit(true); };
  ed.onEscapeKey = [commit] { commit(false); };
  ed.grabKeyboardFocus();
}

void ModPanel::mouseDoubleClick(const juce::MouseEvent &e) {
  if (current != kMacroPage)
    return;
  for (int m = 0; m < kNumMacros; ++m)
    if (macroChip(m).contains(e.position)) {
      beginRename(m);
      return;
    }
}

bool ModPanel::sourceAt(juce::Point<float> p, ModSource &out) const {
  for (int l = 0; l < kNumLfos; ++l)
    if (tabRect(tabOfPage(l)).contains(p)) {
      out = {ModSource::Lfo, l};
      return true;
    }
  if (current == kMacroPage)
    for (int m = 0; m < kNumMacros; ++m)
      if (macroChip(m).withWidth(16.0f).expanded(2.0f).contains(p)) {
        out = {ModSource::Macro, m};
        return true;
      }
  if (current == kExprPage)
    for (int src = 0; src < 3; ++src)
      if (exprHead(src).contains(p)) {
        out = {ModSource::Expr, src};
        return true;
      }
  return false;
}

void ModPanel::mouseDown(const juce::MouseEvent &e) {
  const auto p = e.position;
  pressAt = p;
  drawing = false;
  dragArmed = sourceAt(p, armed);
  if (tabs.contains(p)) {
    const int idx = juce::jlimit(
        0, kPages - 1,
        (int)((p.y - tabs.getY()) / (tabs.getHeight() / (float)kPages)));
    select(pageAtTab(idx));
    return;
  }
  if (current == kMacroPage)
    for (int m = 0; m < kNumMacros; ++m)
      if (macroChip(m).contains(p)) {
        selectMacro(m);
        break;
      }
  if (current < kNumLfos && canvas.contains(p)) {
    drawing = true;
    drawAt(p, false);
    lastDraw = p;
  }
}

void ModPanel::mouseDrag(const juce::MouseEvent &e) {
  if (dragArmed && e.position.getDistanceFrom(pressAt) > 5.0f) {
    dragArmed = false;
    if (auto *c = juce::DragAndDropContainer::findParentDragContainerFor(this))
      if (!c->isDragAndDropActive())
        c->startDragging(
            armed.dragId(), this,
            juce::ScaledImage(makeDragImage(armed.displayName(proc)), 2.0),
            false);
    return;
  }
  if (drawing && canvas.expanded(20.0f).contains(e.position)) {
    drawAt(e.position, true);
    lastDraw = e.position;
  }
}

void ModPanel::mouseUp(const juce::MouseEvent &) {
  drawing = dragArmed = false;
}

void ModPanel::mouseMove(const juce::MouseEvent &e) {
  ModSource s;
  setMouseCursor(sourceAt(e.position, s) ? juce::MouseCursor::DraggingHandCursor
                                         : juce::MouseCursor::NormalCursor);
}

void ModPanel::showMessage(const juce::String &m) {
  message = m;
  messageUntil = juce::Time::getMillisecondCounterHiRes() + 4000.0;
  repaint();
}

void ModPanel::showRoute(ModSource src, int slot, routing::Result res,
                         int target) {
  const int page = src.kind == ModSource::Lfo     ? src.index
                   : src.kind == ModSource::Macro ? kMacroPage
                                                  : kExprPage;
  if (page != current)
    select(page);
  if (src.kind == ModSource::Macro)
    selectMacro(src.index);
  const juce::String dest =
      target >= 0 ? juce::String(vessel::spec(target).name) : juce::String();
  const auto routeName = [&](int s) {
    return juce::String::charToString(
        (juce::juce_wchar)kSlotLetters[juce::jlimit(0, 5, s)]);
  };
  const int flatSlot =
      src.kind == ModSource::Expr ? src.index * 2 + slot : slot;
  const auto srcName = src.displayName(proc);
  flashPage = page;
  flashSlot = -1;
  using R = routing::Result;
  switch (res) {
  case R::Added:
    showMessage(srcName + " -> " + dest + "  [slot " + routeName(slot) + "]");
    flashSlot = flatSlot;
    break;
  case R::Exists:
    showMessage(srcName + " already routed to " + dest + " (slot " +
                routeName(slot) + ")");
    flashSlot = flatSlot;
    break;
  case R::Full:
    showMessage(srcName + " routes full (" + juce::String(src.slots()) + "/" +
                juce::String(src.slots()) + ")");
    break;
  case R::Removed:
    showMessage(srcName + " unrouted from " + dest);
    break;
  case R::NotADestination:
    showMessage(dest + " cannot be modulated");
    break;
  }
  if (flashSlot >= 0)
    flashUntil = juce::Time::getMillisecondCounterHiRes() + 1800.0;
  repaint();
}

void ModPanel::drawAt(juce::Point<float> p, bool interpolate) {
  if (juce::roundToInt(plain(proc, lfoBase(current))) != dsp::WarpedLfo::Drawn)
    return;
  const bool uni = isOn(proc, lfoExt(current) + 4);
  const auto inner = canvas.reduced(5.0f, 12.0f).withTrimmedTop(3.0f);
  auto idxOf = [&](float x) {
    return juce::jlimit(
        0, kDrawPoints - 1,
        (int)((x - inner.getX()) / inner.getWidth() * (float)kDrawPoints));
  };
  auto valOf = [&](float y) {
    return uni ? juce::jlimit(
                     -1.0f, 1.0f,
                     (inner.getBottom() - y) / inner.getHeight() * 2.0f - 1.0f)
               : juce::jlimit(-1.0f, 1.0f,
                              (inner.getCentreY() - y) /
                                  (inner.getHeight() * 0.5f));
  };

  const int i1 = idxOf(p.x);
  const int i0 = interpolate ? idxOf(lastDraw.x) : i1;
  const float v1 = valOf(p.y), v0 = interpolate ? valOf(lastDraw.y) : v1;
  const int lo = std::min(i0, i1), hi = std::max(i0, i1);
  for (int i = lo; i <= hi; ++i) {
    const float t = hi == lo ? 1.0f : (float)(i - i0) / (float)(i1 - i0);
    proc.drawTables[(size_t)current][(size_t)i] =
        dsp::lerp(v0, v1, juce::jlimit(0.0f, 1.0f, t));
  }
  repaint(canvas.toNearestInt().expanded(3));
}

//==============================================================================
// PresetBrowser (Searchable, Tagged, Categorized & Favorites-Enabled V2
// Overlay)
//==============================================================================
PresetBrowser::PresetBrowser(VesselProcessor &p) : proc(p) {
  setWantsKeyboardFocus(true);

  searchBox.setFont(type::sans(13.0f, false));
  searchBox.setTextToShowWhenEmpty("Search presets, tags, descriptions...",
                                   steel.withAlpha(0.65f));
  searchBox.setIndents(8, 5);
  searchBox.onTextChange = [this] {
    scrollY = 0.0f;
    rebuildItems();
  };
  searchBox.onEscapeKey = [this] {
    if (onClose)
      onClose();
  };
  addAndMakeVisible(searchBox);

  favBtn.setClickingTogglesState(true);
  favBtn.getProperties().set("textHeight", 8.6f);
  favBtn.onClick = [this] {
    favoritesOnly = favBtn.getToggleState();
    scrollY = 0.0f;
    rebuildItems();
  };
  addAndMakeVisible(favBtn);

  clearBtn.getProperties().set("textHeight", 8.4f);
  clearBtn.onClick = [this] {
    searchBox.setText("", false);
    favoritesOnly = false;
    favBtn.setToggleState(false, juce::dontSendNotification);
    selectedCat = "All";
    scrollY = 0.0f;
    rebuildItems();
  };
  addAndMakeVisible(clearBtn);

  closeBtn.getProperties().set("textHeight", 8.6f);
  closeBtn.onClick = [this] {
    if (onClose)
      onClose();
  };
  addAndMakeVisible(closeBtn);

  rebuildCategories();
  rebuildItems();
}

PresetBrowser::~PresetBrowser() {
  searchBox.onTextChange = nullptr;
  searchBox.onEscapeKey = nullptr;
}

void PresetBrowser::setSearchQuery(const juce::String &q) {
  searchBox.setText(q, false);
  scrollY = 0.0f;
  rebuildItems();
}

void PresetBrowser::setFavoritesOnly(bool fav) {
  favoritesOnly = fav;
  favBtn.setToggleState(fav, juce::dontSendNotification);
  scrollY = 0.0f;
  rebuildItems();
}

void PresetBrowser::rebuildCategories() {
  cats.clear();
  cats.add("All");
  const auto &all = proc.presetManager.allPresets();
  juce::StringArray present;
  for (const auto &pr : all)
    present.addIfNotAlreadyThere(pr.category);
  for (const auto &c : vessel::PresetBank::categoryOrder())
    if (present.contains(c))
      cats.addIfNotAlreadyThere(c);
  for (const auto &c : present)
    cats.addIfNotAlreadyThere(c);
  if (!cats.contains(selectedCat))
    selectedCat = "All";
}

void PresetBrowser::rebuildItems() {
  const juce::String catFilter =
      (selectedCat == "All") ? juce::String() : selectedCat;
  items =
      proc.presetManager.search(searchBox.getText(), catFilter, favoritesOnly);
  resized();
  repaint();
}

void PresetBrowser::open() {
  proc.presetManager.refresh();
  rebuildCategories();
  hoverItem = hoverCat = hoverStar = -1;
  scrollY = 0.0f;
  rebuildItems();
  grabKeyboardFocus();
}

int PresetBrowser::currentCombinedIndex() const {
  const auto &all = proc.presetManager.allPresets();
  const auto curName = proc.currentPresetName();
  const auto curCat = proc.currentPresetCategory();
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i].name == curName && all[i].category == curCat)
      return (int)i;
  return juce::jlimit(0, std::max(0, (int)all.size() - 1),
                      proc.getCurrentProgram());
}

juce::Rectangle<float>
PresetBrowser::starRectForItem(const juce::Rectangle<float> &r) const {
  return {r.getRight() - 24.0f, r.getY() + 4.0f, 20.0f, 20.0f};
}

void PresetBrowser::resized() {
  panel =
      juce::Rectangle<float>(944.0f, 560.0f)
          .withCentre({(float)getWidth() * 0.5f, (float)getHeight() * 0.5f});

  // Top filter toolbar inside panel
  const int ty = (int)panel.getY() + 12;
  searchBox.setBounds((int)panel.getX() + 224, ty, 376, 28);
  favBtn.setBounds((int)panel.getX() + 608, ty, 108, 28);
  clearBtn.setBounds((int)panel.getX() + 724, ty, 76, 28);
  closeBtn.setBounds((int)panel.getRight() - 92, ty, 76, 28);

  // Exact 7-row grid height (7 * 56 = 392px) so visible preset cards never slice cleanly in half
  listArea = {panel.getX() + 16.0f, panel.getY() + 56.0f, 192.0f, 392.0f};
  gridArea = {listArea.getRight() + 16.0f, listArea.getY(),
              panel.getRight() - 16.0f - (listArea.getRight() + 16.0f), 392.0f};
  infoArea = {panel.getX() + 16.0f, panel.getBottom() - 98.0f,
              panel.getWidth() - 32.0f, 84.0f};

  catRects.clear();
  const float pitch =
      std::min(30.0f, listArea.getHeight() / (float)std::max(1, cats.size()));
  for (int i = 0; i < cats.size(); ++i)
    catRects.push_back({listArea.getX(), listArea.getY() + (float)i * pitch,
                        listArea.getWidth(), pitch - 2.0f});

  itemRects.clear();
  cols = 3;
  const float cw = (gridArea.getWidth() - 12.0f) / (float)cols;
  const float rh = 56.0f;
  const int rows = std::max(1, ((int)items.size() + cols - 1) / cols);
  const float totalH = (float)rows * rh;
  maxScrollY = std::max(0.0f, totalH - gridArea.getHeight());
  scrollY = juce::jlimit(0.0f, maxScrollY, scrollY);

  for (size_t i = 0; i < items.size(); ++i)
    itemRects.push_back(
        {gridArea.getX() + (float)((int)i % cols) * cw,
         gridArea.getY() + (float)((int)i / cols) * rh - scrollY, cw - 8.0f,
         rh - 6.0f});
}

void PresetBrowser::paint(juce::Graphics &g) {
  const auto &all = proc.presetManager.allPresets();
  const int cur = currentCombinedIndex();

  // Backdrop scrim
  g.setColour(chassis.withAlpha(0.76f));
  g.fillAll();

  // Machined browser sheet
  drawMachinedPanel(g, panel, 4.0f, true);
  g.setColour(amber);
  g.fillRect(
      juce::Rectangle<float>(panel.getX() + 16.0f, panel.getY(), 192.0f, 2.0f));

  drawCaps(g, "PRESET BROWSER",
           {panel.getX() + 16.0f, panel.getY() + 10.0f, 196.0f, 16.0f}, 10.5f,
           titanium, juce::Justification::centredLeft, 0.16f);
  g.setColour(steel);
  g.setFont(type::mono(9.5f));
  g.drawText(juce::String((int)items.size()) + " / " +
                 juce::String((int)all.size()) + " PRESETS",
             juce::Rectangle<float>(panel.getX() + 16.0f, panel.getY() + 27.0f,
                                    196.0f, 14.0f),
             juce::Justification::centredLeft, false);

  drawRule(g, panel.getY() + 48.0f, panel.getX() + 12.0f,
           panel.getRight() - 12.0f, seamHi);

  // Categories column
  for (int i = 0; i < cats.size(); ++i) {
    const auto r = catRects[(size_t)i];
    const bool sel = cats[i] == selectedCat, hot = i == hoverCat;
    if (sel) {
      g.setColour(plate);
      g.fillRoundedRectangle(r, 2.0f);
      g.setColour(amber);
      g.fillRect(juce::Rectangle<float>(r.getX(), r.getY() + 4.0f, 2.5f,
                                        r.getHeight() - 8.0f));
    } else if (hot) {
      g.setColour(plate.withAlpha(0.55f));
      g.fillRoundedRectangle(r, 2.0f);
    }

    const int cnt =
        (int)proc.presetManager
            .search(searchBox.getText(),
                    cats[i] == "All" ? juce::String() : cats[i], favoritesOnly)
            .size();
    drawCaps(g, cats[i], r.withTrimmedLeft(10.0f).withTrimmedRight(32.0f), 9.2f,
             sel ? amberHi : (hot ? titanium : titanium.withAlpha(0.78f)),
             juce::Justification::centredLeft, 0.14f);
    g.setColour(sel ? titanium : steel);
    g.setFont(type::mono(10.0f));
    g.drawText(juce::String(cnt), r.withTrimmedRight(8.0f),
               juce::Justification::centredRight, false);
  }
  drawSeam(g, listArea.getRight() + 8.0f, listArea.getY(), listArea.getBottom(),
           seam);

  // Preset cards grid (clipped cleanly to gridArea)
  {
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(gridArea.toNearestInt());

    if (items.empty()) {
      g.setColour(steel);
      g.setFont(type::sans(14.0f, false));
      g.drawText("No presets match the current filter.", gridArea,
                 juce::Justification::centred, false);
    }

    for (size_t i = 0; i < items.size(); ++i) {
      const auto r = itemRects[i];
      if (r.getBottom() < gridArea.getY() - 2.0f ||
          r.getY() > gridArea.getBottom() + 2.0f)
        continue;

      const auto &pr = all[(size_t)items[i]];
      const bool isCur = items[i] == cur, hot = (int)i == hoverItem;
      g.setColour(isCur
                      ? plate
                      : (hot ? plate.withAlpha(0.72f) : well.withAlpha(0.85f)));
      g.fillRoundedRectangle(r, 2.5f);
      g.setColour(isCur ? amber : (hot ? seamHi.brighter(0.25f) : seam));
      g.drawRoundedRectangle(r.reduced(0.5f), 2.5f, isCur ? 1.1f : 0.8f);

      if (isCur) {
        g.setColour(amber);
        g.fillRect(juce::Rectangle<float>(r.getX(), r.getY() + 4.0f, 2.5f,
                                          r.getHeight() - 8.0f));
      }

      // Preset name
      g.setColour(isCur ? amberHi : titanium);
      g.setFont(type::sans(13.0f, true));
      g.drawText(pr.name,
                 juce::Rectangle<float>(r.getX() + 10.0f, r.getY() + 5.0f,
                                        r.getWidth() - 36.0f, 18.0f),
                 juce::Justification::centredLeft, true);

      // Favorite star button
      const auto sr = starRectForItem(r);
      drawStar(g, sr.getCentre(), 5.2f, pr.favorite,
               pr.favorite
                   ? amberHi
                   : ((int)i == hoverStar ? titanium : steel.withAlpha(0.55f)));

      // Tags line
      auto tags = pr.tagList();
      const juce::String tagText =
          (pr.isUser ? juce::String::fromUTF8("USER  \xc2\xb7  ")
                     : juce::String()) +
          tags.joinIntoString(juce::String::fromUTF8("  \xc2\xb7  "));
      drawCaps(
          g, tagText,
          {r.getX() + 10.0f, r.getY() + 28.0f, r.getWidth() - 18.0f, 14.0f},
          7.4f, pr.isUser ? cyan : steel, juce::Justification::centredLeft,
          0.10f);
    }

    // Scrollbar indicator
    if (maxScrollY > 1.0f) {
      const float sbH =
          std::max(24.0f, gridArea.getHeight() * gridArea.getHeight() /
                              (gridArea.getHeight() + maxScrollY));
      const float sbY = gridArea.getY() +
                        (scrollY / maxScrollY) * (gridArea.getHeight() - sbH);
      g.setColour(seam);
      g.fillRect(juce::Rectangle<float>(gridArea.getRight() - 4.0f,
                                        gridArea.getY(), 3.0f,
                                        gridArea.getHeight()));
      g.setColour(amber.withAlpha(0.75f));
      g.fillRoundedRectangle({gridArea.getRight() - 4.0f, sbY, 3.0f, sbH},
                             1.5f);
    }
  }

  // Bottom info & Hero Macro preview strip
  drawRule(g, infoArea.getY() - 8.0f, panel.getX() + 12.0f,
           panel.getRight() - 12.0f, seamHi);
  drawRecessedWell(g, infoArea, 2.5f);

  const int shown = (hoverItem >= 0 && hoverItem < (int)items.size())
                        ? items[(size_t)hoverItem]
                        : cur;
  if (shown >= 0 && shown < (int)all.size()) {
    const auto &pr = all[(size_t)shown];
    g.setColour(titanium);
    g.setFont(type::display(17.0f));
    g.drawText(pr.name,
               juce::Rectangle<float>(infoArea.getX() + 12.0f,
                                      infoArea.getY() + 6.0f, 360.0f, 22.0f),
               juce::Justification::centredLeft, true);

    const juce::String meta =
        pr.category + juce::String::fromUTF8("  \xc2\xb7  ") +
        pr.tagList().joinIntoString(juce::String::fromUTF8(" \xc2\xb7 ")) +
        (pr.author.isNotEmpty()
             ? (juce::String::fromUTF8("  \xc2\xb7  ") + pr.author)
             : "");
    drawCaps(g, meta,
             {infoArea.getX() + 380.0f, infoArea.getY() + 9.0f,
              infoArea.getWidth() - 392.0f, 14.0f},
             8.2f, amber, juce::Justification::centredRight, 0.12f);

    g.setColour(steel);
    g.setFont(type::sans(11.5f, false));
    g.drawFittedText(
        pr.description.isNotEmpty() ? pr.description : "VESSEL II patch.",
        juce::Rectangle<float>(infoArea.getX() + 12.0f, infoArea.getY() + 30.0f,
                               infoArea.getWidth() - 24.0f, 26.0f)
            .toNearestInt(),
        juce::Justification::topLeft, 2, 1.0f);

    // 4 Hero Macro Preview Badges
    float mx = infoArea.getX() + 12.0f;
    for (int m = 0; m < kHeroMacros; ++m) {
      const juce::String lbl =
          (m < pr.macroNames.size() && pr.macroNames[m].isNotEmpty())
              ? pr.macroNames[m]
              : ("Macro " + juce::String(m + 1));
      const juce::String chip =
          "M" + juce::String(m + 1) + ": " + lbl.toUpperCase();
      const float cw =
          std::min(150.0f, type::width(type::mono(8.5f), chip) + 14.0f);
      const auto cr =
          juce::Rectangle<float>(mx, infoArea.getBottom() - 22.0f, cw, 16.0f);
      g.setColour(plate);
      g.fillRoundedRectangle(cr, 2.0f);
      g.setColour(seamHi);
      g.drawRoundedRectangle(cr, 2.0f, 0.8f);
      g.setColour(cyan);
      g.setFont(type::mono(8.5f));
      g.drawText(chip, cr, juce::Justification::centred, true);
      mx += cw + 6.0f;
    }
  }

  g.setColour(steel.withAlpha(0.70f));
  g.setFont(type::sans(10.0f, false));
  g.drawText(
      juce::String::fromUTF8("Click loads  \xc2\xb7  Star toggles Favorite  "
                             "\xc2\xb7  Arrows audition  \xc2\xb7  Esc closes"),
      juce::Rectangle<float>(infoArea.getRight() - 360.0f,
                             infoArea.getBottom() - 21.0f, 348.0f, 14.0f),
      juce::Justification::centredRight, false);
}

void PresetBrowser::mouseMove(const juce::MouseEvent &e) {
  int hi = -1, hc = -1, hs = -1;
  if (gridArea.contains(e.position)) {
    for (size_t i = 0; i < itemRects.size(); ++i)
      if (itemRects[i].contains(e.position)) {
        hi = (int)i;
        if (starRectForItem(itemRects[i]).contains(e.position))
          hs = (int)i;
        break;
      }
  }
  for (size_t i = 0; i < catRects.size(); ++i)
    if (catRects[i].contains(e.position)) {
      hc = (int)i;
      break;
    }

  if (hi != hoverItem || hc != hoverCat || hs != hoverStar) {
    hoverItem = hi;
    hoverCat = hc;
    hoverStar = hs;
    repaint();
  }
  setMouseCursor((hi >= 0 || hc >= 0) ? juce::MouseCursor::PointingHandCursor
                                      : juce::MouseCursor::NormalCursor);
}

void PresetBrowser::mouseExit(const juce::MouseEvent &) {
  hoverItem = hoverCat = hoverStar = -1;
  repaint();
}

void PresetBrowser::mouseDown(const juce::MouseEvent &e) {
  if (!panel.contains(e.position)) {
    if (onClose)
      onClose();
    return;
  }
  for (size_t i = 0; i < catRects.size(); ++i)
    if (catRects[i].contains(e.position)) {
      selectedCat = cats[(int)i];
      scrollY = 0.0f;
      rebuildItems();
      return;
    }
  if (gridArea.contains(e.position)) {
    for (size_t i = 0; i < itemRects.size(); ++i)
      if (itemRects[i].contains(e.position)) {
        const int combinedIdx = items[i];
        const auto &all = proc.presetManager.allPresets();
        if (starRectForItem(itemRects[i]).contains(e.position) &&
            combinedIdx >= 0 && combinedIdx < (int)all.size()) {
          proc.presetManager.toggleFavorite(all[(size_t)combinedIdx]);
          rebuildItems();
          return;
        }
        load(combinedIdx, true);
        return;
      }
  }
}

void PresetBrowser::mouseWheelMove(const juce::MouseEvent &,
                                   const juce::MouseWheelDetails &wheel) {
  if (maxScrollY <= 0.0f)
    return;
  scrollY = juce::jlimit(0.0f, maxScrollY, scrollY - wheel.deltaY * 140.0f);
  resized();
  repaint();
}

void PresetBrowser::load(int combinedIndex, bool close) {
  const auto &all = proc.presetManager.allPresets();
  if (combinedIndex < 0 || combinedIndex >= (int)all.size())
    return;
  if (combinedIndex < proc.presetManager.factoryCount())
    proc.setCurrentProgram(combinedIndex);
  else
    proc.applyPreset(all[(size_t)combinedIndex]);
  if (onLoaded)
    onLoaded();
  repaint();
  if (close && onClose)
    onClose();
}

bool PresetBrowser::keyPressed(const juce::KeyPress &k) {
  if (k == juce::KeyPress::escapeKey) {
    if (onClose)
      onClose();
    return true;
  }
  if (k == juce::KeyPress::returnKey) {
    if (onClose)
      onClose();
    return true;
  }
  int step = 0;
  if (k == juce::KeyPress::leftKey)
    step = -1;
  else if (k == juce::KeyPress::rightKey)
    step = 1;
  else if (k == juce::KeyPress::upKey)
    step = -cols;
  else if (k == juce::KeyPress::downKey)
    step = cols;
  if (step == 0 || items.empty())
    return false;
  const int cur = currentCombinedIndex();
  int pos = -1;
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i] == cur)
      pos = (int)i;
  pos = pos < 0 ? 0 : juce::jlimit(0, (int)items.size() - 1, pos + step);
  load(items[(size_t)pos], false);
  return true;
}

//==============================================================================
// SavePresetOverlay
//==============================================================================
SavePresetOverlay::SavePresetOverlay(VesselProcessor &p) : proc(p) {
  setWantsKeyboardFocus(true);
  for (auto *ed : {&nameEd, &catEd, &tagsEd, &descEd}) {
    ed->setFont(type::sans(13.0f, false));
    ed->setIndents(6, 4);
    addAndMakeVisible(*ed);
  }
  descEd.setMultiLine(true);
  descEd.setReturnKeyStartsNewLine(false);
  nameEd.onReturnKey = [this] { commitSave(); };
  saveBtn.onClick = [this] { commitSave(); };
  cancelBtn.onClick = [this] {
    if (onFinished)
      onFinished(false);
  };
  saveBtn.setToggleState(true, juce::dontSendNotification);
  addAndMakeVisible(saveBtn);
  addAndMakeVisible(cancelBtn);
}

void SavePresetOverlay::open() {
  nameEd.setText(proc.currentPresetName() + " Mod", false);
  catEd.setText(proc.currentPresetCategory().isNotEmpty()
                    ? proc.currentPresetCategory()
                    : "User",
                false);
  tagsEd.setText(proc.currentPresetTags().isNotEmpty()
                     ? proc.currentPresetTags()
                     : "Dark|Custom",
                 false);
  descEd.setText(proc.currentPresetDescription(), false);
  nameEd.selectAll();
  nameEd.grabKeyboardFocus();
  repaint();
}

void SavePresetOverlay::resized() {
  dialog =
      juce::Rectangle<float>(460.0f, 296.0f)
          .withCentre({(float)getWidth() * 0.5f, (float)getHeight() * 0.5f});
  const int x = (int)dialog.getX() + 112;
  const int w = (int)dialog.getWidth() - 128;
  const int y0 = (int)dialog.getY() + 52;
  nameEd.setBounds(x, y0, w, 28);
  catEd.setBounds(x, y0 + 38, w, 28);
  tagsEd.setBounds(x, y0 + 76, w, 28);
  descEd.setBounds(x, y0 + 114, w, 64);
  cancelBtn.setBounds((int)dialog.getRight() - 216,
                      (int)dialog.getBottom() - 42, 92, 28);
  saveBtn.setBounds((int)dialog.getRight() - 116, (int)dialog.getBottom() - 42,
                    100, 28);
}

void SavePresetOverlay::commitSave() {
  const auto nm = nameEd.getText().trim();
  if (nm.isEmpty())
    return;
  auto preset = proc.captureCurrentPreset(nm, "User", catEd.getText().trim(),
                                          tagsEd.getText().trim(),
                                          descEd.getText().trim(), false);
  const bool ok = proc.presetManager.saveUserPreset(preset);
  if (ok)
    proc.applyPreset(preset);
  if (onFinished)
    onFinished(ok);
}

void SavePresetOverlay::mouseDown(const juce::MouseEvent &e) {
  if (!dialog.contains(e.position) && onFinished)
    onFinished(false);
}

bool SavePresetOverlay::keyPressed(const juce::KeyPress &k) {
  if (k == juce::KeyPress::escapeKey) {
    if (onFinished)
      onFinished(false);
    return true;
  }
  return false;
}

void SavePresetOverlay::paint(juce::Graphics &g) {
  g.setColour(chassis.withAlpha(0.76f));
  g.fillAll();
  drawMachinedPanel(g, dialog, 4.0f, true);
  g.setColour(amber);
  g.fillRect(juce::Rectangle<float>(dialog.getX() + 16.0f, dialog.getY(),
                                    160.0f, 2.0f));
  drawCaps(g, "SAVE V2 USER PRESET",
           {dialog.getX() + 16.0f, dialog.getY() + 14.0f,
            dialog.getWidth() - 32.0f, 18.0f},
           10.5f, titanium, juce::Justification::centredLeft, 0.16f);
  drawRule(g, dialog.getY() + 40.0f, dialog.getX() + 12.0f,
           dialog.getRight() - 12.0f, seamHi);

  static const char *kLabels[4]{"NAME", "CATEGORY", "TAGS", "NOTES"};
  const float ys[4]{dialog.getY() + 52.0f, dialog.getY() + 90.0f,
                    dialog.getY() + 128.0f, dialog.getY() + 166.0f};
  for (int i = 0; i < 4; ++i)
    drawCaps(g, kLabels[i], {dialog.getX() + 16.0f, ys[i], 88.0f, 28.0f}, 8.8f,
             steel, juce::Justification::centredLeft, 0.14f);
}
} // namespace vessel::ui
