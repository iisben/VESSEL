#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vessel::ui
{
/** The torch: the whole editor sits in the dark and the mouse is a flashlight.

    A layer above every other component (it never takes the mouse). It polls the mouse once per display frame,
    trails it with a short lag like a hand-held light, and fades in when the mouse enters the window and out when
    it leaves. The beam is one radial gradient: a warm-white hotspot, a faint reflector ring at its edge, a dimmer
    spill and a soft but defined rim, then cool blue-black darkness. The light is held below the window, so the beam
    stretches into an ellipse and dims a little the further up the panel it lands. */
class Torch : public juce::Component
{
public:
    Torch();

    void paint (juce::Graphics&) override;

    /** Off: no darkness at all. */
    void setOn (bool);
    bool isOn() const noexcept { return on; }

    /** A region left out of the dark (the Lights Out button, so it can always be found). */
    void setClear (juce::Rectangle<int> r) { clear = r; repaint(); }

    /** For snapshots: hold the beam at a point (in this component's coordinates), fully lit, ignoring the mouse.
        Pass nullopt to follow the mouse again. */
    void aim (std::optional<juce::Point<float>>);

    static constexpr float kRadius = 215.0f;     // beam radius at the bottom of the panel, logical pixels
    static constexpr float kAmbient = 0.955f;     // how dark the unlit panel is (0 = no darkness)

private:
    void tick (double timestampSec);
    juce::Rectangle<int> beamBounds() const;
    struct Beam { float rx, ry, angle, reach; };
    Beam beam() const;

    bool on = true;
    std::optional<juce::Point<float>> pinned;
    juce::Point<float> pos { -1000.0f, -1000.0f }, target;
    float level = 0.0f;                          // 0 = dark, 1 = beam fully on
    double lastTime = 0.0;
    juce::Rectangle<int> lastPainted, clear;
    juce::VBlankAttachment vblank;
};
} // namespace vessel::ui
