#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/*  VESSEL II — Brutalist-Industrial Hardware Visual Language (NAMM 2027).

    Machined obsidian & anodized gunmetal panels on a strict 8px alignment grid,
    razor-crisp 1px specular bevels, subtle baked matte metal grain, embedded
    SIL OFL geometric sans + tabular mono typography (identical across macOS,
    Windows, and Linux), and strict semantic illumination:
        amber   = active parameter values, hardware LEDs, primary focus
        cyan    = live modulation, MPE/XY trajectories, spectral contour, sub-120 lock
        crimson = hard clipping or low-end phase warning ONLY                        */

namespace vessel::ui
{
namespace ink
{
    // Core brutalist-industrial surfaces
    const juce::Colour chassis   { 0xff07080b };   // outer instrument void
    const juce::Colour deck      { 0xff0c0e13 };   // recessed main deck
    const juce::Colour panel     { 0xff12151c };   // anodized gunmetal panel face
    const juce::Colour plate     { 0xff191d27 };   // raised hardware plate / active button
    const juce::Colour well      { 0xff080a0f };   // deep recessed instrument display well
    const juce::Colour bevelHi   { 0x16ffffff };   // 1px top specular hairline
    const juce::Colour bevelLo   { 0xa0000000 };   // 1px bottom recessed shadow
    const juce::Colour seam      { 0xff1f2430 };   // fine structural hairline
    const juce::Colour seamHi    { 0xff333b4d };   // crisp machined border

    // Typography tones (high contrast >= 6.5:1 on panel)
    const juce::Colour steel     { 0xff949eb3 };   // secondary captions & inactive ticks
    const juce::Colour titanium  { 0xffeaedf4 };   // primary headings & crisp values

    // Semantic accents
    const juce::Colour amber     { 0xfff0a43a };   // primary active value / hardware phosphor
    const juce::Colour amberHi   { 0xffffc46b };   // drag/hover bright phosphor
    const juce::Colour amberDim  { 0xff5e3f15 };   // muted amber track glow
    const juce::Colour cyan      { 0xff52e0d8 };   // modulation / MPE / XY / spectrum / sub-lock
    const juce::Colour cyanHi    { 0xff92f7f1 };   // bright reticle / active mod endpoint
    const juce::Colour crimson   { 0xffff3d57 };   // clipping & phase warning ONLY

    // Legacy aliases preserved for internal module instruments
    const juce::Colour voidc     = chassis;
    const juce::Colour basin     = panel;
    const juce::Colour pewter    = steel;
    const juce::Colour frost     = titanium;
    const juce::Colour violet    = amber;
    const juce::Colour chlorine  = cyan;
    const juce::Colour blood     = crimson;
}

namespace type
{
    /** Geometric bold display face (Space Grotesk Bold, embedded SIL OFL). */
    juce::Font display (float height);
    /** Geometric sans face (Space Grotesk Medium/Bold, embedded SIL OFL). */
    juce::Font sans (float height, bool bold = false);
    /** Tracked uppercase geometric headers & micro-labels. */
    juce::Font caps (float height, float tracking = 0.14f);
    /** Tabular technical monospace for all numeric readouts, meters, and rulers (JetBrains Mono Medium). */
    juce::Font mono (float height);
    /** Alias for numeric values (uses tabular mono so moving numbers never jitter). */
    juce::Font value (float height);

    float width (const juce::Font&, const juce::String&);
}

/** Draws tracked uppercase geometric text in r. */
void drawCaps (juce::Graphics&, const juce::String&, juce::Rectangle<float> r, float height, juce::Colour,
               juce::Justification = juce::Justification::centred, float tracking = 0.14f);

/** Machined brutalist panel with subtle vertical anodized sheen, crisp border, and 1px top specular bevel. */
void drawMachinedPanel (juce::Graphics&, juce::Rectangle<float> r, float cornerRadius = 3.0f, bool elevated = true);

/** Deep recessed instrument well (for oscilloscopes, XY pad, meters, wavetable/filter graphs). */
void drawRecessedWell (juce::Graphics&, juce::Rectangle<float> r, float cornerRadius = 2.0f);

/** Generates a pre-rendered high-DPI static chassis background image (2x Retina) with subtle matte anodized grain
    and architectural 8px grid registration lines so 60 Hz paint() only blits and draws live telemetry. */
juce::Image renderChassisCache (int logicalW, int logicalH, int scaleFactor = 2);

/** VESSEL geometric sigils. */
juce::Path invertedTriangle (juce::Point<float> centre, float size);
juce::Path diamond (juce::Point<float> centre, float size);

/** Hairline structural seams & rules. */
void drawSeam (juce::Graphics&, float x, float y0, float y1, juce::Colour = ink::seamHi);
void drawRule (juce::Graphics&, float y, float x0, float x1, juce::Colour = ink::seamHi);

class ReliquaryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ReliquaryLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;

    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return type::sans (12.0f, false); }
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>&, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcut,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>&, const juce::String&) override;
    juce::Font getPopupMenuFont() override { return type::sans (13.0f, false); }
    int getPopupMenuBorderSize() override { return 1; }

    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    juce::Font getLabelFont (juce::Label&) override { return type::sans (12.5f, false); }
    void drawTooltip (juce::Graphics&, const juce::String&, int w, int h) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};
} // namespace vessel::ui
