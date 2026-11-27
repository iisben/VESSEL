#include "Reliquary.h"
#include "BinaryData.h"
#include <cmath>
#include <cstdint>
#include <numbers>

namespace vessel::ui
{
//==============================================================================
namespace type
{
namespace
{
struct EmbeddedFonts
{
    juce::Typeface::Ptr sansBold;
    juce::Typeface::Ptr sansMedium;
    juce::Typeface::Ptr monoMedium;

    EmbeddedFonts()
    {
        sansBold   = juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceGroteskBold_ttf,
                                                              BinaryData::SpaceGroteskBold_ttfSize);
        sansMedium = juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceGroteskMedium_ttf,
                                                              BinaryData::SpaceGroteskMedium_ttfSize);
        monoMedium = juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoMedium_ttf,
                                                              BinaryData::JetBrainsMonoMedium_ttfSize);
    }

    static const EmbeddedFonts& get()
    {
        static const EmbeddedFonts instance;
        return instance;
    }
};

juce::Font makeFont (const juce::Typeface::Ptr& tf, float h, bool fallbackMono, bool fallbackBold)
{
    if (tf != nullptr)
        return juce::Font (juce::FontOptions (tf).withHeight (h));
    auto opts = juce::FontOptions (fallbackMono ? juce::Font::getDefaultMonospacedFontName()
                                                : juce::Font::getDefaultSansSerifFontName(),
                                   h,
                                   fallbackBold ? juce::Font::bold : juce::Font::plain);
    return juce::Font (opts);
}
} // namespace

juce::Font display (float h)
{
    return makeFont (EmbeddedFonts::get().sansBold, h, false, true);
}

juce::Font sans (float h, bool bold)
{
    const auto& f = EmbeddedFonts::get();
    return makeFont (bold ? f.sansBold : f.sansMedium, h, false, bold);
}

juce::Font caps (float h, float tracking)
{
    const float sz = h < 11.0f ? h + 0.5f : h;
    return makeFont (EmbeddedFonts::get().sansBold, sz, false, true)
        .withExtraKerningFactor (h < 10.0f ? tracking * 0.72f : tracking);
}

juce::Font mono (float h)
{
    return makeFont (EmbeddedFonts::get().monoMedium, h, true, false);
}

juce::Font value (float h)
{
    return mono (h);
}

float width (const juce::Font& f, const juce::String& s)
{
    return juce::GlyphArrangement::getStringWidth (f, s);
}
} // namespace type

//==============================================================================
void drawCaps (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, float h, juce::Colour c,
               juce::Justification just, float tracking)
{
    g.setColour (c);
    const auto font = type::caps (h, tracking);
    g.setFont (font);
    if (just.testFlags (juce::Justification::horizontallyCentred))
        r = r.translated (font.getExtraKerningFactor() * font.getHeight() * 0.45f, 0.0f);
    g.drawText (text.toUpperCase(), r, just, true);
}

void drawMachinedPanel (juce::Graphics& g, juce::Rectangle<float> r, float cornerRadius, bool elevated)
{
    using namespace ink;
    const auto topCol = elevated ? juce::Colour (0xff151922) : juce::Colour (0xff0f1218);
    const auto botCol = elevated ? juce::Colour (0xff10131a) : juce::Colour (0xff0b0d12);
    g.setGradientFill (juce::ColourGradient (topCol, r.getX(), r.getY(), botCol, r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle (r, cornerRadius);

    // 1px top anodized specular bevel
    g.setColour (bevelHi);
    g.fillRect (juce::Rectangle<float> (r.getX() + cornerRadius, r.getY() + 1.0f,
                                        std::max (0.0f, r.getWidth() - cornerRadius * 2.0f), 1.0f));
    // crisp outer hairline border
    g.setColour (seamHi.withAlpha (elevated ? 0.85f : 0.65f));
    g.drawRoundedRectangle (r.reduced (0.5f), cornerRadius, 1.0f);
}

void drawRecessedWell (juce::Graphics& g, juce::Rectangle<float> r, float cornerRadius)
{
    using namespace ink;
    g.setColour (well);
    g.fillRoundedRectangle (r, cornerRadius);
    // subtle top inner shadow
    g.setGradientFill (juce::ColourGradient (juce::Colour (0x70000000), r.getX(), r.getY(),
                                             juce::Colour (0x00000000), r.getX(), r.getY() + 7.0f, false));
    g.fillRoundedRectangle (r.withHeight (std::min (8.0f, r.getHeight())), cornerRadius);
    // lower rim highlight + hairline perimeter
    g.setColour (seamHi.withAlpha (0.72f));
    g.drawRoundedRectangle (r.reduced (0.5f), cornerRadius, 1.0f);
    g.setColour (juce::Colour (0x10ffffff));
    g.fillRect (juce::Rectangle<float> (r.getX() + cornerRadius, r.getBottom() - 1.0f,
                                        std::max (0.0f, r.getWidth() - cornerRadius * 2.0f), 1.0f));
}

juce::Image renderChassisCache (int logicalW, int logicalH, int scaleFactor)
{
    using namespace ink;
    const int s = juce::jlimit (1, 2, scaleFactor);
    const int pw = logicalW * s, ph = logicalH * s;
    juce::Image img (juce::Image::RGB, pw, ph, true);

    {
        juce::Graphics g (img);
        g.addTransform (juce::AffineTransform::scale ((float) s));

        // Base brushed dark obsidian chassis gradient
        juce::ColourGradient baseGrad (juce::Colour (0xff0b0d12), 0.0f, 0.0f,
                                       juce::Colour (0xff07080b), 0.0f, (float) logicalH, false);
        baseGrad.addColour (0.08, juce::Colour (0xff0e1117));
        baseGrad.addColour (0.85, juce::Colour (0xff090b0f));
        g.setGradientFill (baseGrad);
        g.fillAll();

        // Subtle architectural vertical sheen across chassis
        juce::ColourGradient sheen (juce::Colour (0x0a9eb0cc), (float) logicalW * 0.25f, 0.0f,
                                    juce::Colour (0x00000000), (float) logicalW * 0.85f, (float) logicalH, true);
        g.setGradientFill (sheen);
        g.fillAll();

        // Subtle 8px structural registration ticks along outer bezel
        g.setColour (seam.withAlpha (0.55f));
        for (int x = 16; x < logicalW; x += 64)
            g.fillRect ((float) x, (float) logicalH - 4.0f, 1.0f, 3.0f);

        // Outer machined frame hairline + inner 1px specular edge
        g.setColour (seamHi.withAlpha (0.75f));
        g.drawRect (0.5f, 0.5f, (float) logicalW - 1.0f, (float) logicalH - 1.0f, 1.0f);
        g.setColour (bevelHi);
        g.fillRect (1.0f, 1.0f, (float) logicalW - 2.0f, 1.0f);
    }

    // Deterministic fine matte anodized grain baked directly into the static cache (zero per-frame cost)
    {
        juce::Image::BitmapData bmp (img, juce::Image::BitmapData::readWrite);
        uint32_t rng = 0x56455353u; // "VESS"
        for (int y = 0; y < ph; ++y)
        {
            // Subtle horizontal brushed-aluminum micro-band every few rows
            const int brush = ((y & 3) == 0) ? 1 : 0;
            uint8_t* row = bmp.getLinePointer (y);
            for (int x = 0; x < pw; ++x)
            {
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                const int n = (int) (rng & 3u) - 1 + brush; // -1..+2 LSB micro-grain
                uint8_t* px = row + x * bmp.pixelStride;
                px[0] = (uint8_t) juce::jlimit (0, 255, (int) px[0] + n);
                px[1] = (uint8_t) juce::jlimit (0, 255, (int) px[1] + n);
                px[2] = (uint8_t) juce::jlimit (0, 255, (int) px[2] + n);
            }
        }
    }

    return img;
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
    g.setColour (c.withAlpha (0.65f));
    g.fillRect (juce::Rectangle<float> (std::floor (x), y0, 1.0f, y1 - y0));
}

void drawRule (juce::Graphics& g, float y, float x0, float x1, juce::Colour c)
{
    g.setColour (c.withAlpha (0.65f));
    g.fillRect (juce::Rectangle<float> (x0, std::floor (y), x1 - x0, 1.0f));
}

//==============================================================================
ReliquaryLookAndFeel::ReliquaryLookAndFeel()
{
    using namespace ink;
    setColour (juce::ResizableWindow::backgroundColourId, chassis);
    setColour (juce::Label::textColourId, titanium);
    setColour (juce::ComboBox::textColourId, titanium);
    setColour (juce::ComboBox::backgroundColourId, well);
    setColour (juce::ComboBox::outlineColourId, seamHi);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff10131b));
    setColour (juce::PopupMenu::textColourId, titanium);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, amber.withAlpha (0.20f));
    setColour (juce::PopupMenu::highlightedTextColourId, titanium);
    setColour (juce::TextEditor::textColourId, titanium);
    setColour (juce::TextEditor::highlightColourId, amber.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, titanium);
    setColour (juce::CaretComponent::caretColourId, amberHi);
    setColour (juce::TextButton::textColourOffId, titanium);
    setColour (juce::TextButton::textColourOnId, titanium);
    setColour (juce::TooltipWindow::textColourId, titanium);
}

juce::Slider::SliderLayout ReliquaryLookAndFeel::getSliderLayout (juce::Slider& s)
{
    juce::Slider::SliderLayout l;
    l.sliderBounds = s.getLocalBounds();
    return l;
}

//==============================================================================
/** MACHINED ROTARY DIAL — brushed aluminum cap, recessed arc track, phosphor-amber value arc,
    outer cryo-cyan live modulation arc, and hover/drag precision readout pill. */
void ReliquaryLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float, float start, float end, juce::Slider& s)
{
    using namespace ink;
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const bool hot = s.isMouseOverOrDragging();
    const bool dragging = s.isMouseButtonDown();
    const bool bipolar = (bool) s.getProperties()["bipolar"];
    const bool hero = (bool) s.getProperties()["hero"];
    const bool compact = (bool) s.getProperties()["compact"];
    const bool accentCyan = (bool) s.getProperties()["cyan"];
    const bool learning = (bool) s.getProperties()["learn"];
    const int learnedCc = (int) s.getProperties().getWithDefault ("cc", -1);
    const float modOff = (float) s.getProperties()["mod"];
    const float pos = (float) s.valueToProportionOfLength (s.getValue());

    const juce::Colour activeCol = accentCyan ? cyan : amber;
    const juce::Colour activeHi  = accentCyan ? cyanHi : amberHi;

    const float capH = hero ? 0.0f : (compact ? 11.0f : 12.0f);
    const float valH = hero ? 16.0f : (compact ? 13.0f : 14.0f);
    auto ringArea = bounds.withTrimmedTop (capH).withTrimmedBottom (valH);
    const float d = std::min (ringArea.getWidth(), ringArea.getHeight());
    const float R = std::max (10.0f, d * 0.5f - (hero ? 4.0f : 3.0f));
    const auto c = ringArea.getCentre();

    auto polar = [&] (float angle, float r) { return juce::Point<float> (c.x + r * std::sin (angle), c.y - r * std::cos (angle)); };

    // Drop target highlight (dragging LFO / Macro / Expression onto this parameter)
    if (const int drop = (int) s.getProperties()["drop"]; drop > 0)
    {
        const float rr = R + 2.5f;
        juce::Path ring;
        ring.addEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (c));
        if (drop == 2)
        {
            g.setColour (cyan.withAlpha (0.16f));
            g.fillEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (c));
            g.setColour (cyanHi);
            g.strokePath (ring, juce::PathStrokeType (1.8f));
        }
        else
        {
            juce::Path dashed;
            const float dashes[] { 3.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, ring, dashes, 2);
            g.setColour (cyan.withAlpha (0.62f));
            g.fillPath (dashed);
        }
    }

    // MIDI Learn active pulse ring
    if (learning)
    {
        juce::Path ring;
        ring.addEllipse (juce::Rectangle<float> ((R + 3.0f) * 2.0f, (R + 3.0f) * 2.0f).withCentre (c));
        g.setColour (cyan.withAlpha (0.18f));
        g.fillPath (ring);
        g.setColour (cyanHi);
        g.strokePath (ring, juce::PathStrokeType (1.8f));
    }

    // Outer engraved scale ticks for hero knobs (and subtle major ticks on standard dials)
    const float trackR = hero ? R - 4.5f : R - 1.5f;
    const float trackW = hero ? 3.2f : 2.3f;
    if (hero)
    {
        constexpr int kTicks = 25;
        for (int i = 0; i < kTicks; ++i)
        {
            const float t = (float) i / (float) (kTicks - 1);
            const float a = start + t * (end - start);
            const bool maj = (i % 6) == 0;
            const bool active = bipolar ? ((t >= std::min (0.5f, pos) - 1e-4f) && (t <= std::max (0.5f, pos) + 1e-4f))
                                        : (t <= pos + 1e-4f);
            g.setColour (active ? activeCol.withAlpha (0.85f) : steel.withAlpha (maj ? 0.55f : 0.25f));
            g.drawLine ({ polar (a, R - 1.2f), polar (a, R + (maj ? 2.4f : 1.0f)) }, maj ? 1.2f : 0.85f);
        }
    }
    else
    {
        // Minimal 3-point cardinal ticks (min, mid, max) for brutalist precision
        for (float t : { 0.0f, 0.5f, 1.0f })
        {
            const float a = start + t * (end - start);
            g.setColour (steel.withAlpha (0.40f));
            g.drawLine ({ polar (a, trackR + 2.0f), polar (a, trackR + 3.6f) }, 1.0f);
        }
    }

    // Recessed background arc track
    {
        juce::Path bgArc;
        bgArc.addCentredArc (c.x, c.y, trackR, trackR, 0.0f, start, end, true);
        g.setColour (well);
        g.strokePath (bgArc, juce::PathStrokeType (trackW + 1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        g.setColour (seamHi.withAlpha (0.85f));
        g.strokePath (bgArc, juce::PathStrokeType (trackW, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }

    // Active phosphor-amber (or cryo-cyan) value arc
    const float aVal = start + pos * (end - start);
    const float aRef = bipolar ? (start + 0.5f * (end - start)) : start;
    if (std::abs (aVal - aRef) > 0.01f)
    {
        juce::Path valArc;
        valArc.addCentredArc (c.x, c.y, trackR, trackR, 0.0f, std::min (aRef, aVal), std::max (aRef, aVal), true);
        if (hot || hero)
        {
            g.setColour (activeCol.withAlpha (0.18f));
            g.strokePath (valArc, juce::PathStrokeType (trackW + 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        }
        g.setColour (dragging ? activeHi : activeCol);
        g.strokePath (valArc, juce::PathStrokeType (trackW, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }

    // Live modulation ring overlay (cryo-cyan arc outside the value track)
    if (std::abs (modOff) > 0.002f)
    {
        const float p1 = juce::jlimit (0.0f, 1.0f, pos + modOff);
        const float a1 = start + p1 * (end - start);
        const float modR = trackR + (hero ? 3.5f : 2.6f);
        juce::Path modArc;
        modArc.addCentredArc (c.x, c.y, modR, modR, 0.0f, std::min (aVal, a1), std::max (aVal, a1), true);
        g.setColour (cyan.withAlpha (0.92f));
        g.strokePath (modArc, juce::PathStrokeType (hero ? 2.2f : 1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (cyanHi);
        g.fillEllipse (juce::Rectangle<float> (3.2f, 3.2f).withCentre (polar (a1, modR)));
    }

    // Machined anodized knob body (cap)
    const float capR = trackR * (hero ? 0.72f : 0.70f);
    {
        const auto capBounds = juce::Rectangle<float> (capR * 2.0f, capR * 2.0f).withCentre (c);
        // Subtle drop shadow into chassis
        g.setColour (juce::Colour (0x88000000));
        g.fillEllipse (capBounds.translated (0.0f, 1.4f));

        // Machined dark metal cap gradient
        juce::ColourGradient capGrad (hot ? juce::Colour (0xff262c3a) : juce::Colour (0xff1f2430),
                                      c.x, c.y - capR,
                                      juce::Colour (0xff10131a),
                                      c.x, c.y + capR, false);
        g.setGradientFill (capGrad);
        g.fillEllipse (capBounds);

        // Specular bevel ring + dark recessed inner face
        g.setColour (hot ? titanium.withAlpha (0.28f) : juce::Colour (0x1cffffff));
        g.drawEllipse (capBounds.reduced (0.5f), 1.0f);

        if (hero)
        {
            const auto inner = capBounds.reduced (3.5f);
            g.setColour (juce::Colour (0xff131720));
            g.fillEllipse (inner);
            g.setColour (seamHi.withAlpha (0.7f));
            g.drawEllipse (inner, 0.8f);
        }

        // Precision machined pointer needle
        const auto p0 = polar (aVal, capR * (hero ? 0.22f : 0.25f));
        const auto p1 = polar (aVal, capR * 0.92f);
        g.setColour (dragging ? activeHi : titanium);
        g.drawLine ({ p0, p1 }, hero ? 2.2f : 1.7f);
    }

    // Caption (top)
    if (! hero && s.getName().trim().isNotEmpty())
    {
        drawCaps (g, s.getName(), bounds.withHeight (capH), compact ? 8.2f : 8.8f,
                  hot ? titanium : steel, juce::Justification::centred, 0.11f);
    }

    // Value readout (bottom) — illuminates into a crisp precision pill on hover/drag
    const auto valRect = bounds.withTop (bounds.getBottom() - valH);
    const auto valText = learning ? juce::String ("LEARN")
                                  : (hot && learnedCc >= 0 && ! dragging ? "CC " + juce::String (learnedCc)
                                                                         : s.getTextFromValue (s.getValue()));
    if (hot || dragging || learning)
    {
        const auto pill = valRect.reduced (2.0f, 0.5f);
        g.setColour (well.withAlpha (0.95f));
        g.fillRoundedRectangle (pill, 2.0f);
        g.setColour (learning ? cyan : (dragging ? activeHi : activeCol.withAlpha (0.65f)));
        g.drawRoundedRectangle (pill, 2.0f, 0.9f);
    }
    g.setColour (learning ? cyanHi : (dragging ? activeHi : (hot ? titanium : titanium.withAlpha (0.90f))));
    g.setFont (type::mono (hero ? 11.0f : (compact ? 9.5f : 10.0f)));
    g.drawText (valText, valRect, juce::Justification::centred, false);
}

//==============================================================================
/** MACHINED VERTICAL FADER (VIAL) & HORIZONTAL ROUTE BAR */
void ReliquaryLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float, float, float,
                                             juce::Slider::SliderStyle, juce::Slider& s)
{
    using namespace ink;
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const bool hot = s.isMouseOverOrDragging();
    const bool dragging = s.isMouseButtonDown();
    const bool bipolar = (bool) s.getProperties()["bipolar"];
    const bool learning = (bool) s.getProperties()["learn"];
    const float modOff = (float) s.getProperties()["mod"];
    const float pos = (float) s.valueToProportionOfLength (s.getValue());
    const juce::String shortName = s.getProperties().contains ("short") ? s.getProperties()["short"].toString() : s.getName();

    if ((bool) s.getProperties()["bar"])
    {
        // HORIZONTAL ROUTE BAR — compact precision modulation depth strip
        const bool idle = (bool) s.getProperties()["idle"];
        const bool accentCyan = (bool) s.getProperties()["cyan"];
        const juce::Colour fillCol = idle ? steel.withAlpha (0.35f)
                                          : (accentCyan ? cyan : amber).withAlpha (hot ? 1.0f : 0.88f);
        const auto capFont = type::caps (8.5f, 0.12f);
        const float capW = shortName.isEmpty() ? 0.0f : std::min (44.0f, type::width (capFont, shortName.toUpperCase()) + 7.0f);
        const float valW = 36.0f;

        if (capW > 0.0f)
            drawCaps (g, shortName, bounds.withWidth (capW), 8.5f, hot ? titanium : steel, juce::Justification::centredLeft, 0.12f);

        auto track = juce::Rectangle<float> (bounds.getX() + capW, bounds.getCentreY() - 2.0f,
                                             std::max (12.0f, bounds.getWidth() - capW - valW - 2.0f), 4.0f);
        g.setColour (well);
        g.fillRoundedRectangle (track, 1.5f);
        g.setColour (seamHi.withAlpha (0.75f));
        g.drawRoundedRectangle (track, 1.5f, 0.7f);

        const float px = track.getX() + pos * track.getWidth();
        g.setColour (fillCol);
        if (bipolar)
        {
            const float mid = track.getCentreX();
            g.fillRect (juce::Rectangle<float> (std::min (mid, px), track.getY() + 0.5f, std::abs (px - mid), track.getHeight() - 1.0f));
            g.setColour (steel.withAlpha (0.75f));
            g.fillRect (juce::Rectangle<float> (mid - 0.5f, track.getY() - 2.0f, 1.0f, track.getHeight() + 4.0f));
        }
        else
        {
            g.fillRect (juce::Rectangle<float> (track.getX(), track.getY() + 0.5f, px - track.getX(), track.getHeight() - 1.0f));
        }

        // Machined vertical handle pip
        g.setColour (idle ? steel : (dragging ? amberHi : titanium));
        g.fillRect (juce::Rectangle<float> (px - 1.5f, track.getY() - 2.0f, 3.0f, track.getHeight() + 4.0f));

        g.setColour (dragging ? amberHi : (idle ? steel.withAlpha (0.65f) : titanium.withAlpha (0.90f)));
        g.setFont (type::mono (9.5f));
        g.drawText (s.getTextFromValue (s.getValue()), bounds.withLeft (bounds.getRight() - valW), juce::Justification::centredRight, false);
        return;
    }

    // VERTICAL MACHINED FADER (ADSR)
    const float valH = 13.0f, capH = 13.0f;
    const float cw = std::min (11.0f, bounds.getWidth() * 0.34f);
    auto slot = juce::Rectangle<float> (cw, std::max (20.0f, bounds.getHeight() - valH - capH - 6.0f))
                    .withCentre ({ bounds.getCentreX(), bounds.getY() + valH + 3.0f + (bounds.getHeight() - valH - capH - 6.0f) * 0.5f });

    drawRecessedWell (g, slot, 2.0f);

    const float levelY = slot.getBottom() - pos * slot.getHeight();
    const float midY = slot.getCentreY();
    const float top = bipolar ? std::min (levelY, midY) : levelY;
    const float bot = bipolar ? std::max (levelY, midY) : slot.getBottom() - 1.0f;

    // Active phosphor-amber bar fill
    if (bot > top + 0.5f)
    {
        juce::ColourGradient fill (amber.withAlpha (hot ? 0.92f : 0.80f), 0.0f, top,
                                   amber.withAlpha (0.28f), 0.0f, slot.getBottom(), false);
        g.setGradientFill (fill);
        g.fillRect (juce::Rectangle<float> (slot.getX() + 1.5f, top, slot.getWidth() - 3.0f, bot - top));
    }

    // Machined slider cap / meniscus bar
    g.setColour (dragging ? amberHi : titanium);
    g.fillRect (juce::Rectangle<float> (slot.getX() - 2.5f, levelY - 1.5f, slot.getWidth() + 5.0f, 3.0f));

    // Drop target / MIDI learn highlight
    if (const int drop = (int) s.getProperties()["drop"]; drop > 0 || learning)
    {
        const auto halo = slot.expanded (4.0f, 2.0f);
        g.setColour (cyan.withAlpha (drop == 2 || learning ? 0.95f : 0.55f));
        g.drawRoundedRectangle (halo, 2.5f, drop == 2 || learning ? 1.6f : 1.0f);
    }

    // Precision side scale graduations
    for (int i = 0; i <= 8; ++i)
    {
        const float gy = slot.getBottom() - (float) i / 8.0f * slot.getHeight();
        const bool maj = (i % 4 == 0);
        g.setColour (steel.withAlpha (maj ? 0.50f : 0.24f));
        g.fillRect (juce::Rectangle<float> (slot.getRight() + 2.5f, gy, maj ? 4.5f : 2.5f, 1.0f));
    }

    // Live modulation marker
    if (std::abs (modOff) > 0.002f)
    {
        const float my = slot.getBottom() - juce::jlimit (0.0f, 1.0f, pos + modOff) * slot.getHeight();
        g.setColour (cyanHi);
        g.fillPath (invertedTriangle ({ slot.getX() - 4.5f, my }, 5.5f),
                    juce::AffineTransform::rotation (-std::numbers::pi_v<float> * 0.5f, slot.getX() - 4.5f, my));
    }

    // Value readout (top) — compact formatting when column width is tight (< 46px) so adjacent ADSR faders never touch
    juce::String valStr = s.getTextFromValue (s.getValue());
    const bool tight = bounds.getWidth() < 46.0f;
    if (tight) valStr = valStr.replace (" ms", "ms").replace (" Hz", "Hz");
    g.setColour (dragging ? amberHi : titanium);
    g.setFont (type::mono (tight ? 8.3f : 9.5f));
    g.drawText (valStr, bounds.withHeight (valH).reduced (1.0f, 0.0f), juce::Justification::centred, false);
    drawCaps (g, shortName, bounds.withTop (bounds.getBottom() - capH), 8.8f, hot ? titanium : steel, juce::Justification::centred, 0.12f);
}

//==============================================================================
/** BRUTALIST HARDWARE LED SWITCH */
void ReliquaryLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    using namespace ink;
    const bool on = b.getToggleState();
    const bool header = (bool) b.getProperties()["header"];
    const bool accentCyan = (bool) b.getProperties()["cyan"];
    const auto ledCol = accentCyan ? cyan : amber;
    const auto r = b.getLocalBounds().toFloat();

    const float ledW = header ? 7.0f : 6.0f;
    const float ledH = header ? 7.0f : 6.0f;
    const auto led = juce::Rectangle<float> (r.getX() + 4.0f, r.getCentreY() - ledH * 0.5f, ledW, ledH);

    if (on)
    {
        g.setColour (ledCol.withAlpha (0.25f));
        g.fillRoundedRectangle (led.expanded (2.5f), 2.0f);
        g.setColour (ledCol);
        g.fillRoundedRectangle (led, 1.2f);
    }
    else
    {
        g.setColour (well);
        g.fillRoundedRectangle (led, 1.2f);
        g.setColour (highlighted ? titanium : seamHi);
        g.drawRoundedRectangle (led, 1.2f, 1.0f);
    }

    float th = header ? 10.5f : 9.0f;
    if (header)
    {
        const float tw = type::width (type::caps (th, 0.16f), b.getButtonText().toUpperCase());
        const float avail = std::max (20.0f, r.getWidth() - 18.0f);
        if (tw > avail) th = std::max (8.0f, th * avail / tw);
    }
    drawCaps (g, b.getButtonText(), r.withTrimmedLeft (16.0f), th,
              on ? titanium : (highlighted ? titanium.withAlpha (0.85f) : steel),
              juce::Justification::centredLeft, header ? 0.16f : 0.12f);
}

void ReliquaryLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    using namespace ink;
    const bool on = b.getToggleState();
    const bool accentCyan = (bool) b.getProperties()["cyan"];
    const auto col = accentCyan ? cyan : amber;
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);

    if (b.getButtonText().length() <= 1 && (b.getButtonText() == "<" || b.getButtonText() == ">"))
    {
        if (highlighted || down)
        {
            g.setColour (down ? col.withAlpha (0.22f) : plate);
            g.fillRoundedRectangle (r, 2.0f);
        }
        return;
    }

    g.setColour (down ? col.withAlpha (0.22f) : (on ? col.withAlpha (0.14f) : (highlighted ? plate : well.withAlpha (0.85f))));
    g.fillRoundedRectangle (r, 2.5f);
    g.setColour (on ? col : (highlighted ? titanium.withAlpha (0.55f) : seamHi));
    g.drawRoundedRectangle (r, 2.5f, 1.0f);
}

void ReliquaryLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool)
{
    using namespace ink;
    const float h = (float) b.getProperties().getWithDefault ("textHeight", 9.2f);
    const bool on = b.getToggleState();
    const bool accentCyan = (bool) b.getProperties()["cyan"];
    const auto activeCol = accentCyan ? cyanHi : amberHi;

    if (b.getButtonText() == "<" || b.getButtonText() == ">")
    {
        const auto c = b.getLocalBounds().toFloat().getCentre();
        const float s = 3.8f, dir = b.getButtonText() == "<" ? 1.0f : -1.0f;
        juce::Path p;
        p.startNewSubPath (c.x + dir * s * 0.5f, c.y - s);
        p.lineTo (c.x - dir * s * 0.5f, c.y);
        p.lineTo (c.x + dir * s * 0.5f, c.y + s);
        g.setColour (highlighted ? titanium : steel);
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
        return;
    }
    const auto col = ! b.isEnabled() ? steel.withAlpha (0.35f)
                   : on ? activeCol
                   : highlighted ? titanium : titanium.withAlpha (0.85f);
    drawCaps (g, b.getButtonText(), b.getLocalBounds().toFloat(), h, col, juce::Justification::centred, 0.12f);
}

//==============================================================================
void ReliquaryLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool down, int, int, int, int, juce::ComboBox& box)
{
    using namespace ink;
    const bool hot = box.isMouseOver (true) || down;
    const auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f);
    g.setColour (well);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (down ? amber : (hot ? titanium.withAlpha (0.45f) : seamHi));
    g.drawRoundedRectangle (r, 2.0f, 1.0f);

    auto chevron = [&] (float cx, float dir)
    {
        juce::Path p;
        const float cy = r.getCentreY(), s = 3.0f;
        p.startNewSubPath (cx + dir * s * 0.5f, cy - s);
        p.lineTo (cx - dir * s * 0.5f, cy);
        p.lineTo (cx + dir * s * 0.5f, cy + s);
        g.setColour (hot ? titanium : steel.withAlpha (0.7f));
        g.strokePath (p, juce::PathStrokeType (1.15f));
    };
    chevron (7.0f, 1.0f);
    chevron ((float) w - 7.0f, -1.0f);
}

void ReliquaryLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (13, 0, std::max (10, box.getWidth() - 26), box.getHeight());
    label.setFont (getComboBoxFont (box));
    label.setJustificationType (juce::Justification::centred);
    label.setMinimumHorizontalScale (0.72f);
}

void ReliquaryLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (juce::Colour (0xff0e1118));
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
    if (isHighlighted && isActive)
    {
        g.setColour (amber.withAlpha (0.18f));
        g.fillRect (r.reduced (2.0f, 0.5f));
    }
    if (isTicked)
    {
        g.setColour (amber);
        g.fillRect (juce::Rectangle<float> (r.getX() + 8.0f, r.getCentreY() - 3.0f, 6.0f, 6.0f));
    }
    g.setColour (isActive ? (isHighlighted || isTicked ? titanium : titanium.withAlpha (0.85f)) : steel.withAlpha (0.45f));
    g.setFont (getPopupMenuFont());
    g.drawText (text, r.withTrimmedLeft (22.0f).withTrimmedRight (14.0f), juce::Justification::centredLeft, true);
    if (hasSubMenu)
    {
        g.setColour (steel);
        g.fillPath (invertedTriangle ({ r.getRight() - 10.0f, r.getCentreY() }, 5.5f),
                    juce::AffineTransform::rotation (-std::numbers::pi_v<float> * 0.5f, r.getRight() - 10.0f, r.getCentreY()));
    }
}

void ReliquaryLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& name)
{
    auto r = area.toFloat();
    drawCaps (g, name, r.withTrimmedLeft (10.0f), 8.8f, ink::amber, juce::Justification::bottomLeft, 0.20f);
}

void ReliquaryLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (ink::well);
    g.fillRect (0, 0, w, h);
}

void ReliquaryLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& ed)
{
    g.setColour (ed.hasKeyboardFocus (true) ? ink::amber : ink::seamHi);
    g.drawRect (0, 0, w, h, 1);
}

void ReliquaryLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
{
    g.fillAll (juce::Colour (0xff0e1118));
    g.setColour (ink::amber.withAlpha (0.7f));
    g.fillRect (0, 0, 2, h);
    g.setColour (ink::seamHi);
    g.drawRect (0, 0, w, h, 1);
    g.setColour (ink::titanium);
    g.setFont (type::sans (12.0f, false));
    g.drawFittedText (text, juce::Rectangle<int> (w, h).reduced (8, 3), juce::Justification::centredLeft, 3);
}

void ReliquaryLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool over, bool dragging)
{
    using namespace ink;
    const float W = (float) w, H = (float) h;
    const auto tone = dragging ? amberHi : (over ? amber : steel.withAlpha (0.65f));
    g.setColour (tone);
    for (int i = 0; i < 3; ++i)
    {
        const float off = 4.0f + (float) i * 4.0f;
        g.drawLine (W - off, H - 2.0f, W - 2.0f, H - off, 1.3f);
    }
}
} // namespace vessel::ui
