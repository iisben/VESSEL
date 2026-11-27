#include "Reliquary.h"

namespace vessel::ui
{
//==============================================================================
namespace type
{
namespace
{
   #if JUCE_MAC
    const char* kDisplay = "Didot";                 const char* kDisplayStyle = "Italic";
    const char* kGrotesk = "Avenir Next Condensed"; const char* kCapsStyle = "Demi Bold"; const char* kValueStyle = "Medium";
   #elif JUCE_WINDOWS
    const char* kDisplay = "Georgia";               const char* kDisplayStyle = "Italic";
    const char* kGrotesk = "Bahnschrift";           const char* kCapsStyle = "SemiBold"; const char* kValueStyle = "Regular";
   #else
    const char* kDisplay = "DejaVu Serif";          const char* kDisplayStyle = "Italic";
    const char* kGrotesk = "DejaVu Sans Condensed"; const char* kCapsStyle = "Bold"; const char* kValueStyle = "Book";
   #endif
}

// Legibility: small captions and values are drawn one point larger than their layout size (the 8-12 pt range is
// where the condensed grotesk got hard to read), with the caps tracking eased so the wider glyphs keep their width.
inline float legible (float h) noexcept { return h < 12.5f ? h + 1.0f : h; }

juce::Font display (float h) { return juce::Font (juce::FontOptions (kDisplay, kDisplayStyle, h)); }
juce::Font caps (float h, float tracking)
{
    return juce::Font (juce::FontOptions (kGrotesk, kCapsStyle, legible (h))).withExtraKerningFactor (h < 12.5f ? tracking * 0.8f : tracking);
}
juce::Font value (float h) { return juce::Font (juce::FontOptions (kGrotesk, kValueStyle, legible (h))).withExtraKerningFactor (0.02f); }
float width (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }
} // namespace type

void drawCaps (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, float h, juce::Colour c,
               juce::Justification just, float tracking)
{
    g.setColour (c);
    const auto font = type::caps (h, tracking);
    g.setFont (font);
    // tracking adds trailing space after the last glyph; shift centred text back by half of it
    if (just.testFlags (juce::Justification::horizontallyCentred)) r = r.translated (font.getExtraKerningFactor() * font.getHeight() * 0.5f, 0.0f);
    g.drawText (text.toUpperCase(), r, just, false);
}

juce::Path invertedTriangle (juce::Point<float> c, float s)
{
    juce::Path p;
    p.addTriangle (c.x - s * 0.5f, c.y - s * 0.36f, c.x + s * 0.5f, c.y - s * 0.36f, c.x, c.y + s * 0.5f);
    return p;
}

juce::Path diamond (juce::Point<float> c, float s)
{
    juce::Path p;
    const float h = s * 0.5f;
    p.startNewSubPath (c.x, c.y - h);
    p.lineTo (c.x + h, c.y);
    p.lineTo (c.x, c.y + h);
    p.lineTo (c.x - h, c.y);
    p.closeSubPath();
    return p;
}

void drawSeam (juce::Graphics& g, float x, float y0, float y1, juce::Colour c)
{
    juce::ColourGradient grad (c.withAlpha (0.0f), x, y0, c.withAlpha (0.0f), x, y1, false);
    grad.addColour (0.18, c);
    grad.addColour (0.82, c);
    g.setGradientFill (grad);
    g.fillRect (juce::Rectangle<float> (x, y0, 1.0f, y1 - y0));
}

void drawRule (juce::Graphics& g, float y, float x0, float x1, juce::Colour c)
{
    juce::ColourGradient grad (c.withAlpha (0.0f), x0, y, c.withAlpha (0.0f), x1, y, false);
    grad.addColour (0.12, c);
    grad.addColour (0.88, c);
    g.setGradientFill (grad);
    g.fillRect (juce::Rectangle<float> (x0, y, x1 - x0, 1.0f));
}

//==============================================================================
ReliquaryLookAndFeel::ReliquaryLookAndFeel()
{
    using namespace ink;
    setColour (juce::ResizableWindow::backgroundColourId, voidc);
    setColour (juce::Label::textColourId, frost);
    setColour (juce::ComboBox::textColourId, frost);
    setColour (juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::PopupMenu::backgroundColourId, basin);
    setColour (juce::PopupMenu::textColourId, frost);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, violet.withAlpha (0.18f));
    setColour (juce::PopupMenu::highlightedTextColourId, frost);
    setColour (juce::TextEditor::textColourId, frost);
    setColour (juce::TextEditor::highlightColourId, violet.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, frost);
    setColour (juce::CaretComponent::caretColourId, violet);
    setColour (juce::TextButton::textColourOffId, frost);
    setColour (juce::TextButton::textColourOnId, frost);
    setColour (juce::TooltipWindow::textColourId, frost);
}

juce::Slider::SliderLayout ReliquaryLookAndFeel::getSliderLayout (juce::Slider& s)
{
    juce::Slider::SliderLayout l;
    l.sliderBounds = s.getLocalBounds();
    return l;
}

//==============================================================================
/** HALO DIAL — an engraved ring of 37 ticks, a blade needle, a chlorine arc for live modulation. */
void ReliquaryLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float, float start, float end, juce::Slider& s)
{
    using namespace ink;
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const bool hot = s.isMouseOverOrDragging();
    const bool dragging = s.isMouseButtonDown();
    const bool bipolar = (bool) s.getProperties()["bipolar"];
    const float modOff = (float) s.getProperties()["mod"];
    const float pos = (float) s.valueToProportionOfLength (s.getValue());

    const float capH = 13.0f, valH = 15.0f;
    auto ringArea = bounds.withTrimmedTop (capH).withTrimmedBottom (valH);
    const float d = std::min (ringArea.getWidth(), ringArea.getHeight());
    const float R = d * 0.5f - 2.0f;
    const auto c = ringArea.getCentre();

    auto polar = [&] (float angle, float r) { return juce::Point<float> (c.x + r * std::sin (angle), c.y - r * std::cos (angle)); };

    // faint fill: the vessel holds what you pour into it
    {
        const float fill = bipolar ? std::abs (pos - 0.5f) * 2.0f : pos;
        juce::ColourGradient glow (violet.withAlpha (0.05f + 0.16f * fill), c.x, c.y, violet.withAlpha (0.0f), c.x + R, c.y, true);
        g.setGradientFill (glow);
        g.fillEllipse (juce::Rectangle<float> (R * 1.6f, R * 1.6f).withCentre (c));
    }

    // drop target: a source is being dragged (faint ring) / is over this control (bright ring)
    if (const int drop = (int) s.getProperties()["drop"]; drop > 0)
    {
        const float rr = R + 3.0f;
        juce::Path ring;
        ring.addEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (c));
        if (drop == 2)
        {
            g.setColour (chlorine.withAlpha (0.14f));
            g.fillEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (c));
            g.setColour (chlorine);
            g.strokePath (ring, juce::PathStrokeType (2.0f));
        }
        else
        {
            juce::Path dashed;
            const float dashes[] { 3.0f, 4.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, ring, dashes, 2);
            g.setColour (chlorine.withAlpha (0.55f));
            g.fillPath (dashed);
        }
    }

    constexpr int N = 37;
    for (int i = 0; i < N; ++i)
    {
        const float t = (float) i / (float) (N - 1);
        const float a = start + t * (end - start);
        const bool active = bipolar ? ((t >= std::min (0.5f, pos) - 1.0e-4f) && (t <= std::max (0.5f, pos) + 1.0e-4f))
                                    : (t <= pos + 1.0e-4f);
        const bool major = (i % 9) == 0;
        float r0, r1, th;
        juce::Colour col;
        if (active) { r0 = R * 0.66f; r1 = R * 0.90f; th = 1.35f; col = violet.withAlpha (hot ? 1.0f : 0.88f); }
        else        { r0 = R * (major ? 0.70f : 0.76f); r1 = R * 0.90f; th = 1.0f; col = pewter.withAlpha (major ? 0.55f : (hot ? 0.38f : 0.26f)); }
        g.setColour (col);
        g.drawLine ({ polar (a, r0), polar (a, r1) }, th);
    }

    // live modulation: chlorine arc outside the ticks
    if (std::abs (modOff) > 0.002f)
    {
        const float p1 = juce::jlimit (0.0f, 1.0f, pos + modOff);
        const float a0 = start + pos * (end - start), a1 = start + p1 * (end - start);
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, R, R, 0.0f, std::min (a0, a1), std::max (a0, a1), true);
        g.setColour (chlorine.withAlpha (0.9f));
        g.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillEllipse (juce::Rectangle<float> (3.5f, 3.5f).withCentre (polar (a1, R)));
    }

    // blade needle
    {
        const float a = start + pos * (end - start);
        const auto tip = polar (a, R * 0.97f);
        const auto b1 = polar (a - 0.16f, R * 0.40f), b2 = polar (a + 0.16f, R * 0.40f);
        juce::Path blade;
        blade.addTriangle (tip, b1, b2);
        g.setColour (frost.withAlpha (dragging ? 1.0f : 0.92f));
        g.fillPath (blade);
        g.setColour (pewter.withAlpha (0.8f));
        g.fillPath (diamond (c, 4.0f));
    }

    // caption + value
    drawCaps (g, s.getName(), bounds.withHeight (capH), 9.5f, hot ? frost : pewter);
    g.setColour (dragging ? violet : frost);
    g.setFont (type::value (12.5f));
    g.drawText (s.getTextFromValue (s.getValue()), bounds.withTop (bounds.getBottom() - valH), juce::Justification::centred, false);
}

//==============================================================================
/** VIAL — a glass capsule with a liquid fill and a concave meniscus. */
void ReliquaryLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float, float, float,
                                             juce::Slider::SliderStyle, juce::Slider& s)
{
    using namespace ink;
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const bool hot = s.isMouseOverOrDragging();
    const bool bipolar = (bool) s.getProperties()["bipolar"];
    const float modOff = (float) s.getProperties()["mod"];
    const float pos = (float) s.valueToProportionOfLength (s.getValue());
    const juce::String shortName = s.getProperties().contains ("short") ? s.getProperties()["short"].toString() : s.getName();

    if ((bool) s.getProperties()["bar"])
    {
        // BAR — a route amount: caption · hairline track filled from the centre · value. Dimmed when unrouted.
        const bool idle = (bool) s.getProperties()["idle"];
        const auto capFont = type::caps (9.0f, 0.18f);
        const float capW = shortName.isEmpty() ? 0.0f : type::width (capFont, shortName.toUpperCase()) + 8.0f;
        const float valW = 40.0f;
        drawCaps (g, shortName, bounds.withWidth (capW), 9.0f, hot ? frost : pewter, juce::Justification::centredLeft, 0.18f);
        auto track = juce::Rectangle<float> (bounds.getX() + capW, bounds.getCentreY() - 1.0f, bounds.getWidth() - capW - valW, 2.0f);
        g.setColour (seamHi);
        g.fillRect (track);
        const juce::Colour ink = idle ? pewter.withAlpha (0.45f) : violet.withAlpha (hot ? 1.0f : 0.85f);
        const float px = track.getX() + pos * track.getWidth();
        g.setColour (ink);
        if (bipolar)
        {
            const float mid = track.getCentreX();
            g.fillRect (juce::Rectangle<float> (std::min (mid, px), track.getY() - 1.0f, std::abs (px - mid), 4.0f));
            g.setColour (pewter.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (mid - 0.5f, track.getY() - 3.0f, 1.0f, 8.0f));
        }
        else g.fillRect (juce::Rectangle<float> (track.getX(), track.getY() - 1.0f, px - track.getX(), 4.0f));
        g.setColour (idle ? pewter : frost);
        g.fillPath (diamond ({ px, track.getCentreY() }, hot ? 8.0f : 7.0f));
        g.setColour (s.isMouseButtonDown() ? violet : (idle ? pewter : frost));
        g.setFont (type::value (11.5f));
        g.drawText (s.getTextFromValue (s.getValue()), bounds.withLeft (bounds.getRight() - valW), juce::Justification::centredRight, false);
        return;
    }

    const float valH = 14.0f, capH = 14.0f;
    const float cw = std::min (13.0f, bounds.getWidth() * 0.4f);
    auto glass = juce::Rectangle<float> (cw, bounds.getHeight() - valH - capH - 6.0f)
                     .withCentre ({ bounds.getCentreX(), bounds.getY() + valH + 3.0f + (bounds.getHeight() - valH - capH - 6.0f) * 0.5f });

    juce::Path capsule;
    capsule.addRoundedRectangle (glass, cw * 0.5f);

    const float levelY = glass.getBottom() - pos * glass.getHeight();
    const float midY = glass.getCentreY();

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (capsule);
        g.setColour (basin.withAlpha (0.6f));
        g.fillPath (capsule);

        const float top = bipolar ? std::min (levelY, midY) : levelY;
        const float bot = bipolar ? std::max (levelY, midY) : glass.getBottom();
        juce::ColourGradient liquid (violet.withAlpha (hot ? 0.75f : 0.6f), 0, top, violet.withAlpha (0.12f), 0, glass.getBottom(), false);
        g.setGradientFill (liquid);
        g.fillRect (juce::Rectangle<float> (glass.getX(), top, glass.getWidth(), std::max (0.0f, bot - top)));

        // meniscus: concave surface
        juce::Path m;
        m.startNewSubPath (glass.getX(), levelY - 1.5f);
        m.quadraticTo (glass.getCentreX(), levelY + 1.8f, glass.getRight(), levelY - 1.5f);
        g.setColour (frost.withAlpha (0.95f));
        g.strokePath (m, juce::PathStrokeType (1.4f));
    }

    g.setColour (frost.withAlpha (hot ? 0.38f : 0.2f));
    g.strokePath (capsule, juce::PathStrokeType (1.0f));

    if (const int drop = (int) s.getProperties()["drop"]; drop > 0)
    {
        juce::Path halo;
        halo.addRoundedRectangle (glass.expanded (4.0f), cw * 0.5f + 4.0f);
        if (drop == 2) { g.setColour (chlorine.withAlpha (0.14f)); g.fillPath (halo); g.setColour (chlorine); g.strokePath (halo, juce::PathStrokeType (2.0f)); }
        else
        {
            juce::Path dashed;
            const float dashes[] { 3.0f, 4.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, halo, dashes, 2);
            g.setColour (chlorine.withAlpha (0.55f));
            g.fillPath (dashed);
        }
    }

    // graduations
    for (int i = 0; i <= 10; ++i)
    {
        const float gy = glass.getBottom() - (float) i / 10.0f * glass.getHeight();
        const float len = (i % 5 == 0) ? 5.0f : 3.0f;
        g.setColour (pewter.withAlpha (i % 5 == 0 ? 0.5f : 0.28f));
        g.fillRect (juce::Rectangle<float> (glass.getRight() + 3.0f, gy, len, 1.0f));
    }

    if (std::abs (modOff) > 0.002f)
    {
        const float my = glass.getBottom() - juce::jlimit (0.0f, 1.0f, pos + modOff) * glass.getHeight();
        g.setColour (chlorine);
        g.fillPath (invertedTriangle ({ glass.getX() - 5.0f, my }, 6.0f), juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, glass.getX() - 5.0f, my));
    }

    g.setColour (s.isMouseButtonDown() ? violet : frost);
    g.setFont (type::value (11.5f));
    g.drawText (s.getTextFromValue (s.getValue()), bounds.withHeight (valH), juce::Justification::centred, false);
    drawCaps (g, shortName, bounds.withTop (bounds.getBottom() - capH), 9.5f, hot ? frost : pewter);
}

//==============================================================================
/** RUNE — a diamond that fills with ultraviolet when the switch is on. */
void ReliquaryLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    using namespace ink;
    const bool on = b.getToggleState();
    const bool header = (bool) b.getProperties()["header"];
    const auto r = b.getLocalBounds().toFloat();
    const float ds = header ? 10.0f : 9.0f;
    const juce::Point<float> c { r.getX() + 8.0f, r.getCentreY() };

    if (on)
    {
        g.setColour (violet.withAlpha (0.22f));
        g.fillPath (diamond (c, ds + 7.0f));
        g.setColour (violet);
        g.fillPath (diamond (c, ds));
    }
    else
    {
        g.setColour (highlighted ? frost : pewter);
        g.strokePath (diamond (c, ds), juce::PathStrokeType (1.1f));
    }

    float th = header ? 11.5f : 10.0f;
    if (header)
    {   // narrow columns: shrink the title to fit rather than clip it
        const float tw = type::width (type::caps (th, 0.22f), b.getButtonText().toUpperCase());
        const float avail = r.getWidth() - 22.0f;
        if (tw > avail) th = std::max (8.5f, th * avail / tw);
    }
    drawCaps (g, b.getButtonText(), r.withTrimmedLeft (20.0f), th,
              on ? frost : (highlighted ? frost.withAlpha (0.8f) : pewter),
              juce::Justification::centredLeft, header ? 0.22f : 0.16f);
}

void ReliquaryLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    using namespace ink;
    if (b.getButtonText().length() <= 1) return;   // bare chevrons
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    if (down) { g.setColour (violet.withAlpha (0.15f)); g.fillRect (r); }
    g.setColour (b.getToggleState() ? violet : (highlighted ? frost.withAlpha (0.55f) : seamHi));
    g.drawRect (r, 1.0f);
}

void ReliquaryLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool)
{
    const float h = (float) b.getProperties().getWithDefault ("textHeight", 10.0f);
    const bool glyph = b.getButtonText().length() <= 1;
    if (glyph)
    {   // single-character buttons (‹ ›) render as thin chevrons
        const auto c = b.getLocalBounds().toFloat().getCentre();
        const float s = 4.0f, dir = b.getButtonText() == "<" ? 1.0f : -1.0f;
        juce::Path p;
        p.startNewSubPath (c.x + dir * s * 0.5f, c.y - s);
        p.lineTo (c.x - dir * s * 0.5f, c.y);
        p.lineTo (c.x + dir * s * 0.5f, c.y + s);
        g.setColour (highlighted ? ink::frost : ink::pewter);
        g.strokePath (p, juce::PathStrokeType (1.2f));
        return;
    }
    drawCaps (g, b.getButtonText(), b.getLocalBounds().toFloat(), h, highlighted ? ink::frost : ink::frost.withAlpha (0.85f));
}

//==============================================================================
void ReliquaryLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    using namespace ink;
    const bool hot = box.isMouseOver (true);
    const auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
    drawRule (g, r.getBottom() - 1.0f, 0.0f, (float) w, hot ? frost.withAlpha (0.45f) : seamHi);

    auto chevron = [&] (float cx, float dir)
    {
        juce::Path p;
        const float cy = r.getCentreY(), s = 3.5f;
        p.startNewSubPath (cx + dir * s * 0.5f, cy - s);
        p.lineTo (cx - dir * s * 0.5f, cy);
        p.lineTo (cx + dir * s * 0.5f, cy + s);
        g.setColour (hot ? frost : pewter.withAlpha (0.7f));
        g.strokePath (p, juce::PathStrokeType (1.1f));
    };
    chevron (6.0f, 1.0f);
    chevron ((float) w - 6.0f, -1.0f);
}

void ReliquaryLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (12, 0, box.getWidth() - 24, box.getHeight() - 1);
    label.setFont (getComboBoxFont (box));
    label.setJustificationType (juce::Justification::centred);
    label.setMinimumHorizontalScale (0.7f);
}

void ReliquaryLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (ink::basin);
    g.setColour (ink::seamHi);
    g.drawRect (0, 0, w, h, 1);
}

void ReliquaryLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                              bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                              const juce::String&, const juce::Drawable*, const juce::Colour*)
{
    using namespace ink;
    if (isSeparator) { drawRule (g, (float) area.getCentreY(), 8.0f, (float) area.getRight() - 8.0f, seam); return; }
    auto r = area.toFloat();
    if (isHighlighted && isActive) { g.setColour (violet.withAlpha (0.16f)); g.fillRect (r); }
    if (isTicked) { g.setColour (violet); g.fillPath (diamond ({ r.getX() + 12.0f, r.getCentreY() }, 7.0f)); }
    g.setColour (isActive ? (isHighlighted || isTicked ? frost : frost.withAlpha (0.82f)) : pewter.withAlpha (0.5f));
    g.setFont (getPopupMenuFont());
    g.drawText (text, r.withTrimmedLeft (24.0f).withTrimmedRight (12.0f), juce::Justification::centredLeft, true);
    if (hasSubMenu)
    {
        g.setColour (pewter);
        g.fillPath (invertedTriangle ({ r.getRight() - 12.0f, r.getCentreY() }, 6.0f),
                    juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, r.getRight() - 12.0f, r.getCentreY()));
    }
}

void ReliquaryLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& name)
{
    auto r = area.toFloat();
    drawCaps (g, name, r.withTrimmedLeft (12.0f), 9.5f, ink::violet, juce::Justification::bottomLeft, 0.24f);
}

void ReliquaryLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (ink::voidc.withAlpha (0.94f));
    g.fillRect (0, 0, w, h);
}

void ReliquaryLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (ink::violet);
    g.drawRect (0, 0, w, h, 1);
}

void ReliquaryLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
{
    g.fillAll (ink::basin);
    g.setColour (ink::seamHi);
    g.drawRect (0, 0, w, h, 1);
    g.setColour (ink::frost);
    g.setFont (type::value (13.0f));
    g.drawFittedText (text, juce::Rectangle<int> (w, h).reduced (6, 2), juce::Justification::centredLeft, 3);
}

void ReliquaryLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool over, bool dragging)
{
    using namespace ink;
    const float W = (float) w, H = (float) h;
    const auto tone = dragging ? frost : over ? violet : violet.withAlpha (0.78f);

    // a faint wedge of ink pooling into the corner
    juce::Path wedge;
    wedge.addTriangle (W, H * 0.18f, W, H, W * 0.18f, H);
    g.setGradientFill (juce::ColourGradient (violet.withAlpha (over || dragging ? 0.30f : 0.16f), W, H, violet.withAlpha (0.0f), W * 0.45f, H * 0.45f, true));
    g.fillPath (wedge);

    // the bracket: two hairlines meeting at the corner, broken at the joint
    g.setColour (tone.withAlpha (0.9f));
    g.fillRect (juce::Rectangle<float> (W * 0.30f, H - 1.6f, W * 0.52f, 1.6f));
    g.fillRect (juce::Rectangle<float> (W - 1.6f, H * 0.30f, 1.6f, H * 0.52f));

    // three diamonds stepping into the corner, growing as they go
    for (int i = 0; i < 3; ++i)
    {
        const float t = 0.30f + 0.20f * (float) i;
        const float size = 3.0f + 1.4f * (float) i;
        g.setColour (tone.withAlpha (0.45f + 0.27f * (float) i));
        g.fillPath (diamond ({ W * t + 1.0f, H * t + 1.0f }, size));
    }
    if (dragging) { g.setColour (frost.withAlpha (0.25f)); g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ W - 4.0f, H - 4.0f })); }
}
} // namespace vessel::ui
