#include "Routing.h"
#include "Reliquary.h"
#include "../PluginProcessor.h"
#include "../Presets.h"

namespace vessel::ui
{
namespace
{
const char* kExprNames[3] { "Velocity", "Mod Wheel", "Aftertouch" };
constexpr const char* kPrefix = "vessel-mod:";

int choiceIndex (VesselProcessor& p, int pid)
{
    auto* r = p.param (pid);
    return juce::roundToInt (r->convertFrom0to1 (r->getValue()));
}

void setPlain (VesselProcessor& p, int pid, float v)
{
    auto* r = p.param (pid);
    r->beginChangeGesture();
    r->setValueNotifyingHost (r->convertTo0to1 (v));
    r->endChangeGesture();
}
} // namespace

int ModSource::slots() const noexcept { return kind == Lfo ? vessel::kLfoSlots : kind == Macro ? vessel::kMacroSlots : 2; }
int ModSource::destPid (int slot) const noexcept
{
    return kind == Lfo ? vessel::lfoDestPid (index, slot) : kind == Macro ? vessel::macroDestPid (index, slot) : vessel::exprDestPid (index, slot);
}
int ModSource::amtPid (int slot) const noexcept  { return destPid (slot) + 1; }

juce::String ModSource::name() const
{
    if (kind == Lfo)   return "LFO " + juce::String (index + 1);
    if (kind == Macro) return "Macro " + juce::String (index + 1);
    return kExprNames[juce::jlimit (0, 2, index)];
}

juce::String ModSource::displayName (VesselProcessor& p) const
{
    if (kind == Macro)
        if (const auto n = p.macroName (index); n.isNotEmpty()) return n;
    return name();
}

juce::String ModSource::dragId() const
{
    return juce::String (kPrefix) + (kind == Lfo ? "lfo:" : kind == Macro ? "mac:" : "exp:") + juce::String (index);
}

bool ModSource::fromDragId (const juce::var& v, ModSource& out)
{
    const auto s = v.toString();
    if (! s.startsWith (kPrefix)) return false;
    const auto rest = s.fromFirstOccurrenceOf (kPrefix, false, false);
    const int idx = rest.fromFirstOccurrenceOf (":", false, false).getIntValue();
    if (rest.startsWith ("mac:") && idx >= 0 && idx < vessel::kNumMacros) { out = { Macro, idx }; return true; }
    if (idx < 0 || idx > 2) return false;
    if (rest.startsWith ("lfo:")) { out = { Lfo, idx }; return true; }
    if (rest.startsWith ("exp:")) { out = { Expr, idx }; return true; }
    return false;
}

namespace routing
{
std::vector<ModSource> allSources()
{
    std::vector<ModSource> v { { ModSource::Lfo, 0 }, { ModSource::Lfo, 1 }, { ModSource::Lfo, 2 },
                               { ModSource::Expr, 0 }, { ModSource::Expr, 1 }, { ModSource::Expr, 2 } };
    for (int m = 0; m < vessel::kNumMacros; ++m) v.push_back ({ ModSource::Macro, m });
    return v;
}

bool isDestination (int pid) { return vessel::destIndexOf (pid) > 0; }

int slotFor (VesselProcessor& p, ModSource src, int target)
{
    const int idx = vessel::destIndexOf (target);
    if (idx <= 0) return -1;
    for (int s = 0; s < src.slots(); ++s)
        if (choiceIndex (p, src.destPid (s)) == idx) return s;
    return -1;
}

int freeSlot (VesselProcessor& p, ModSource src)
{
    for (int s = 0; s < src.slots(); ++s)
        if (choiceIndex (p, src.destPid (s)) == 0) return s;
    return -1;
}

Result add (VesselProcessor& p, ModSource src, int target, int& slotOut)
{
    slotOut = -1;
    const int idx = vessel::destIndexOf (target);
    if (idx <= 0) return Result::NotADestination;
    if ((slotOut = slotFor (p, src, target)) >= 0) return Result::Exists;
    if ((slotOut = freeSlot (p, src)) < 0) return Result::Full;
    setPlain (p, src.destPid (slotOut), (float) idx);
    // a moderate start: an LFO sweeps ±20 % of the range, a performance source or macro 50 % of it
    setPlain (p, src.amtPid (slotOut), src.kind == ModSource::Lfo ? 0.4f : 0.5f);
    return Result::Added;
}

Result remove (VesselProcessor& p, ModSource src, int target)
{
    const int idx = vessel::destIndexOf (target);
    if (idx <= 0) return Result::NotADestination;
    bool any = false;
    for (int s = 0; s < src.slots(); ++s)
        if (choiceIndex (p, src.destPid (s)) == idx)
        {
            setPlain (p, src.destPid (s), 0.0f);
            setPlain (p, src.amtPid (s), 0.0f);
            any = true;
        }
    return any ? Result::Removed : Result::Exists;
}

std::vector<std::pair<ModSource, int>> routesTo (VesselProcessor& p, int target)
{
    std::vector<std::pair<ModSource, int>> out;
    const int idx = vessel::destIndexOf (target);
    if (idx <= 0) return out;
    for (auto src : allSources())
        for (int s = 0; s < src.slots(); ++s)
            if (choiceIndex (p, src.destPid (s)) == idx) out.push_back ({ src, s });
    return out;
}
} // namespace routing

juce::Image makeDragImage (const juce::String& name)
{
    using namespace ink;
    const auto font = type::caps (10.0f, 0.2f);
    const int w = juce::roundToInt (type::width (font, name.toUpperCase())) + 40, h = 26;
    juce::Image img (juce::Image::ARGB, w * 2, h * 2, true);
    juce::Graphics g (img);
    g.addTransform (juce::AffineTransform::scale (2.0f));
    const auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f);
    g.setColour (basin.withAlpha (0.94f));
    g.fillRect (r);
    g.setColour (chlorine);
    g.drawRect (r, 1.0f);
    g.fillPath (diamond ({ 14.0f, (float) h * 0.5f }, 8.0f));
    drawCaps (g, name, r.withTrimmedLeft (24.0f), 10.0f, frost, juce::Justification::centredLeft, 0.2f);
    return img;
}
} // namespace vessel::ui
