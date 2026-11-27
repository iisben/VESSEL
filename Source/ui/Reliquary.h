#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/*  RELIQUARY — VESSEL's visual language.

    Cold, wet, nocturnal. A dark-glass instrument floating over slow ultraviolet ink (an OpenGL shader).
    Nothing is a card or a box: modules are vertical ranks separated by condensation hairlines.
    Geometry is triangles and diamonds (witch-house sigils): inverted-triangle pointers, diamond runes for
    switches. Levels are glass vials with a liquid meniscus. Values are engraved tick-halos, not knobs.
    Type pairs a high-contrast Didone italic (wordmark, numerals) with a condensed grotesk in tracked caps.

    Ink is used semantically:
        violet   = a value / an active state
        chlorine = modulation (live LFO movement on any control)
        blood    = the clipper hitting the wall, and nothing else                                       */

namespace vessel::ui
{
namespace ink
{
    const juce::Colour voidc   { 0xff07080b };   // ground (behind the shader)
    const juce::Colour basin   { 0xff0c0e13 };   // glass panels (used with alpha)
    const juce::Colour seam    { 0xff1b1f28 };   // structural hairlines
    const juce::Colour seamHi  { 0xff343b49 };
    const juce::Colour pewter  { 0xffa4adc0 };   // secondary text (lifted for legibility: 7.9:1 on the ground)
    const juce::Colour frost   { 0xffe9eef6 };   // primary text, values
    const juce::Colour violet  { 0xffcc8eff };   // active / value (brightened electric neon purple)
    const juce::Colour violetHi{ 0xffe6c6ff };   // specular lavender-violet highlight
    const juce::Colour chlorine{ 0xff4fe3cf };   // modulation
    const juce::Colour blood   { 0xffff3b5c };   // clipping only
}

namespace type
{
    juce::Font display (float height);                     // Didone italic: wordmark, numerals
    juce::Font caps (float height, float tracking = 0.16f); // condensed grotesk, tracked caps
    juce::Font value (float height);                        // condensed grotesk for numbers
    float width (const juce::Font&, const juce::String&);
}

/** Draws text in tracked caps, horizontally centred (or left aligned) in r. */
void drawCaps (juce::Graphics&, const juce::String&, juce::Rectangle<float> r, float height, juce::Colour,
               juce::Justification = juce::Justification::centred, float tracking = 0.16f);

/** The VESSEL sigil: an inverted triangle. */
juce::Path invertedTriangle (juce::Point<float> centre, float size);
juce::Path diamond (juce::Point<float> centre, float size);

/** Vertical hairline that fades out at both ends (condensation line). */
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
    juce::Font getComboBoxFont (juce::ComboBox&) override { return type::value (13.0f); }
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>&, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcut,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>&, const juce::String&) override;
    juce::Font getPopupMenuFont() override { return type::value (14.0f); }
    int getPopupMenuBorderSize() override { return 1; }

    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    juce::Font getLabelFont (juce::Label&) override { return type::value (13.0f); }
    void drawTooltip (juce::Graphics&, const juce::String&, int w, int h) override;
    /** The window-size grip: a sigil of three diamonds stepping into a bracketed corner. */
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};
} // namespace vessel::ui
