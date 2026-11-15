#include "Torch.h"

namespace vessel::ui
{
namespace
{
const juce::Colour kDark { 0xff020309 };    // blue-black: the cool of an unlit room
const juce::Colour kWarm { 0xffffbe6e };    // the beam's colour: an old incandescent bulb behind a cheap reflector
const juce::Colour kRim  { 0xff2a1806 };    // the brown-amber fringe where an old beam fades out

// The beam's profile from the centre (0) to the rim (1): how dark the panel stays under the light.
// Hotspot, its shoulder, a faint reflector ring just outside it, the wide spill, then a soft but defined rim.
struct Stop { float t, dark; };
constexpr Stop kProfile[] {
    { 0.00f, 0.00f }, { 0.30f, 0.00f }, { 0.38f, 0.10f }, { 0.43f, 0.05f }, { 0.48f, 0.16f },
    { 0.62f, 0.24f }, { 0.78f, 0.36f }, { 0.88f, 0.52f }, { 0.94f, 0.74f }, { 1.00f, Torch::kAmbient } };
}

Torch::Torch()
    : vblank (this, [this] (double t) { tick (t); })
{
    setInterceptsMouseClicks (false, false);
    setAlwaysOnTop (true);
}

void Torch::setOn (bool o)
{
    if (o == on) return;
    on = o;
    level = 0.0f;
    repaint();
}

void Torch::aim (std::optional<juce::Point<float>> p)
{
    pinned = p;
    if (p) { pos = target = *p; level = 1.0f; }
    repaint();
}

Torch::Beam Torch::beam() const
{
    // the light is held below the middle of the window: further up the panel it lands at a shallower angle, so the
    // spot grows, stretches along the line from the hand, and dims
    const float w = (float) getWidth(), h = (float) getHeight();
    const juce::Point<float> hand { w * 0.5f, h * 1.6f };
    const auto v = pos - hand;
    const float reach = juce::jlimit (0.0f, 1.3f, (v.getDistanceFromOrigin() - h * 0.6f) / h);
    const float size = kRadius * (1.0f + 0.12f * reach);
    return { size * (1.0f + 0.28f * reach), size, std::atan2 (v.y, v.x), reach };
}

juce::Rectangle<int> Torch::beamBounds() const
{
    const auto b = beam();
    const float c = std::cos (b.angle), s = std::sin (b.angle);
    const float hx = std::sqrt (b.rx * b.rx * c * c + b.ry * b.ry * s * s);
    const float hy = std::sqrt (b.rx * b.rx * s * s + b.ry * b.ry * c * c);
    return juce::Rectangle<float> (pos.x - hx, pos.y - hy, 2.0f * hx, 2.0f * hy).getSmallestIntegerContainer().expanded (3);
}

void Torch::tick (double t)
{
    const float dt = lastTime > 0.0 ? (float) juce::jlimit (0.0, 0.1, t - lastTime) : 1.0f / 60.0f;
    lastTime = t;
    if (! on || pinned) return;

    const auto screen = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition();
    target = getLocalPoint (nullptr, screen);
    const bool inside = isShowing() && getLocalBounds().toFloat().contains (target);

    const auto before = pos;
    const float before_level = level;
    if (level < 0.01f) pos = target;                                   // from darkness the light appears where you are
    else pos += (target - pos) * (1.0f - std::exp (-dt / 0.028f));      // a hand-held lag
    level += ((inside ? 1.0f : 0.0f) - level) * (1.0f - std::exp (-dt / (inside ? 0.09f : 0.35f)));
    if (! inside && level < 0.002f) level = 0.0f;

    if (pos.getDistanceFrom (before) < 0.05f && std::abs (level - before_level) < 0.001f) return;
    const auto now = beamBounds();
    repaint (lastPainted.isEmpty() ? now : lastPainted.getUnion (now));
    lastPainted = now;
}

void Torch::paint (juce::Graphics& g)
{
    if (! on) return;
    if (level <= 0.0f) { if (! clear.isEmpty()) g.excludeClipRegion (clear); g.fillAll (kDark.withAlpha (kAmbient)); return; }

    if (! clear.isEmpty()) g.excludeClipRegion (clear);
    const auto b = beam();
    const auto shape = juce::AffineTransform::scale (b.rx, b.ry).rotated (b.angle).translated (pos);
    const float dim = 0.07f * b.reach;   // a farther spot is a little dimmer

    // the darkness, with the beam cut out of it (beyond the rim the gradient holds its last stop: the ambient dark)
    {
        auto alpha = [&] (float lit) { return kAmbient + (std::min (kAmbient, lit + dim) - kAmbient) * level; };
        juce::ColourGradient dark (kDark.withAlpha (alpha (kProfile[0].dark)), 0.0f, 0.0f,
                                   kDark.withAlpha (kAmbient), 1.0f, 0.0f, true);
        for (size_t i = 1; i + 1 < std::size (kProfile); ++i)
            dark.addColour (kProfile[i].t, kDark.withAlpha (alpha (kProfile[i].dark)));
        juce::FillType fill (dark);
        fill.transform = shape;
        g.setFillType (fill);
        g.fillAll();
    }
    // the light itself: a yellowed incandescent wash, brightest in the hotspot with the bulb's faint shadow at its
    // heart, the bright reflector rings old torches threw, and a brown-amber fringe where the beam dies
    {
        const float a = 0.17f * level * (1.0f - 0.4f * b.reach);
        juce::ColourGradient warm (kWarm.withAlpha (a * 0.78f), 0.0f, 0.0f, kRim.withAlpha (0.0f), 1.0f, 0.0f, true);
        warm.addColour (0.07f, kWarm.withAlpha (a * 0.80f));
        warm.addColour (0.16f, kWarm.withAlpha (a));
        warm.addColour (0.30f, kWarm.withAlpha (a * 0.92f));
        warm.addColour (0.37f, kWarm.withAlpha (a * 0.45f));
        warm.addColour (0.42f, kWarm.withAlpha (a * 0.70f));
        warm.addColour (0.48f, kWarm.withAlpha (a * 0.32f));
        warm.addColour (0.70f, kWarm.withAlpha (a * 0.24f));
        warm.addColour (0.90f, kRim.withAlpha (std::min (1.0f, a * 2.2f)));
        juce::FillType fill (warm);
        fill.transform = shape;
        g.setFillType (fill);
        g.fillRect (beamBounds());
    }
}
} // namespace vessel::ui
