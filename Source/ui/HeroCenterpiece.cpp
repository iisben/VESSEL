#include "HeroCenterpiece.h"
#include "Panels.h"
#include "../PluginProcessor.h"
#include "../dsp/Common.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace vessel::ui
{
using namespace ink;

namespace
{
float plainVal (VesselProcessor& p, int pid)
{
    auto* r = p.param (pid);
    return r->convertFrom0to1 (r->getValue());
}
} // namespace

//==============================================================================
// ScopeSpectrumWidget
//==============================================================================
ScopeSpectrumWidget::ScopeSpectrumWidget (VesselProcessor& p) : proc (p)
{
    for (int i = 0; i < kFftSize; ++i)
        hann[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * std::numbers::pi_v<float> * (float) i / (float) (kFftSize - 1));
    specEnv.fill (-80.0f);
    specPeak.fill (-80.0f);
}

void ScopeSpectrumWidget::resized()
{
    wellArea = getLocalBounds().toFloat().reduced (8.0f).withTrimmedTop (24.0f);
}

juce::Rectangle<float> ScopeSpectrumWidget::modeTabRect (int idx) const
{
    const float w = 44.0f, h = 16.0f, gap = 3.0f;
    const float startX = (float) getWidth() - 8.0f - (3.0f * w + 2.0f * gap);
    return { startX + (float) idx * (w + gap), 7.0f, w, h };
}

void ScopeSpectrumWidget::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < 3; ++i)
    {
        if (modeTabRect (i).contains (e.position))
        {
            setMode (i == 0 ? Mode::Dual : (i == 1 ? Mode::Scope : Mode::Spectrum));
            return;
        }
    }
}

void ScopeSpectrumWidget::analyzeAudio()
{
    const auto& t = proc.telemetry;
    const int writeIdx = t.scopeWrite.load (std::memory_order_acquire);

    // Pull latest kFftSize stereo samples from lock-free ring buffer (chronological order)
    double sqSum = 0.0;
    for (int i = 0; i < kFftSize; ++i)
    {
        const int idx = (writeIdx + i) % Telemetry::kScope;
        const float l = t.scopeL[(size_t) idx].load (std::memory_order_relaxed);
        const float r = t.scopeR[(size_t) idx].load (std::memory_order_relaxed);
        ringL[(size_t) i] = l;
        ringR[(size_t) i] = r;
        sqSum += (double) l * (double) l + (double) r * (double) r;
    }

    // Momentary LUFS-style loudness estimate (snap cleanly on first active signal, then smooth ballistics)
    const float rmsInst = std::sqrt ((float) (sqSum / (double) (2 * kFftSize)));
    const float rmsTelem = t.rms.load (std::memory_order_relaxed);
    const float energy = std::max (rmsInst, rmsTelem);
    const float instLufs = energy > 1.0e-5f ? juce::jlimit (-70.0f, 0.0f, -0.691f + 20.0f * std::log10 (energy) + 3.01f) : -70.0f;
    if (! lufsInitialized && instLufs > -68.0f)
    {
        lufsSmooth = instLufs;
        lufsInitialized = true;
    }
    else
    {
        const float alpha = instLufs > lufsSmooth ? 0.35f : 0.10f;
        lufsSmooth += alpha * (instLufs - lufsSmooth);
    }

    // Prepare complex interleaved Hann-windowed buffers for L and R FFTs
    std::fill (fftL.begin(), fftL.end(), 0.0f);
    std::fill (fftR.begin(), fftR.end(), 0.0f);
    for (int i = 0; i < kFftSize; ++i)
    {
        fftL[(size_t) i] = ringL[(size_t) i] * hann[(size_t) i];
        fftR[(size_t) i] = ringR[(size_t) i] * hann[(size_t) i];
    }
    fft.performRealOnlyForwardTransform (fftL.data(), true);
    fft.performRealOnlyForwardTransform (fftR.data(), true);

    // Effective scope sample rate is host SR / 2 (decimated by 2 in telemetry writer)
    const double srHost = proc.getSampleRate() > 8000.0 ? proc.getSampleRate() : 48000.0;
    const double srScope = srHost * 0.5;
    const double binHz = srScope / (double) kFftSize;

    // 1. Sub-120 Hz stereo phase correlation from complex FFT bins in [20 Hz, 120 Hz]
    const int kLo = std::max (1, (int) std::floor (20.0 / binHz));
    const int kHi = std::min (kFftSize / 2 - 1, (int) std::ceil (120.0 / binHz));
    double numLR = 0.0, powL = 0.0, powR = 0.0;
    for (int k = kLo; k <= kHi; ++k)
    {
        const double lRe = fftL[(size_t) (2 * k)], lIm = fftL[(size_t) (2 * k + 1)];
        const double rRe = fftR[(size_t) (2 * k)], rIm = fftR[(size_t) (2 * k + 1)];
        numLR += lRe * rRe + lIm * rIm;
        powL  += lRe * lRe + lIm * lIm;
        powR  += rRe * rRe + rIm * rIm;
    }
    const double denom = std::sqrt (powL * powR);
    if (denom > 1.0e-5)
    {
        const float corr = juce::jlimit (-1.0f, 1.0f, (float) (numLR / denom));
        subCorrSmooth += 0.22f * (corr - subCorrSmooth);
        subSignalPresent = true;
    }
    else
    {
        subCorrSmooth += 0.08f * (1.0f - subCorrSmooth);
        subSignalPresent = false;
    }

    // 2. Log-spaced spectral contour across [30 Hz .. 11500 Hz] (mono sum energy)
    const double fMin = 30.0, fMax = std::min (11500.0, srScope * 0.48);
    const double logRatio = std::log (fMax / fMin);
    const float invNorm = 2.0f / (float) kFftSize;

    for (int b = 0; b < kSpecBins; ++b)
    {
        const double f0 = fMin * std::exp (logRatio * (double) b / (double) kSpecBins);
        const double f1 = fMin * std::exp (logRatio * (double) (b + 1) / (double) kSpecBins);
        const int i0 = juce::jlimit (1, kFftSize / 2 - 1, (int) std::floor (f0 / binHz));
        const int i1 = juce::jlimit (i0 + 1, kFftSize / 2, (int) std::ceil (f1 / binHz));

        float pkMag = 0.0f;
        for (int k = i0; k < i1; ++k)
        {
            const float mRe = 0.5f * (fftL[(size_t) (2 * k)] + fftR[(size_t) (2 * k)]);
            const float mIm = 0.5f * (fftL[(size_t) (2 * k + 1)] + fftR[(size_t) (2 * k + 1)]);
            pkMag = std::max (pkMag, std::hypot (mRe, mIm) * invNorm);
        }
        const float db = juce::jlimit (-78.0f, 0.0f, 20.0f * std::log10 (pkMag + 1.0e-6f));
        const float prev = specEnv[(size_t) b];
        specEnv[(size_t) b] = db > prev ? prev + 0.55f * (db - prev) : prev + 0.16f * (db - prev);
        specPeak[(size_t) b] = std::max (specEnv[(size_t) b], specPeak[(size_t) b] - 0.35f);
    }
}

void ScopeSpectrumWidget::refresh()
{
    analyzeAudio();
    repaint (wellArea.toNearestInt().expanded (2));
}

void ScopeSpectrumWidget::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    drawMachinedPanel (g, b, 3.0f, true);

    drawCaps (g, "ANALYZER", { 10.0f, 6.0f, 110.0f, 18.0f }, 9.8f, titanium, juce::Justification::centredLeft, 0.18f);

    static const char* kModes[3] { "DUAL", "SCOPE", "SPEC" };
    for (int i = 0; i < 3; ++i)
    {
        const auto tr = modeTabRect (i);
        const bool sel = (i == 0 && mode == Mode::Dual) || (i == 1 && mode == Mode::Scope) || (i == 2 && mode == Mode::Spectrum);
        g.setColour (sel ? amber.withAlpha (0.16f) : well);
        g.fillRoundedRectangle (tr, 2.0f);
        g.setColour (sel ? amber : seamHi);
        g.drawRoundedRectangle (tr.reduced (0.5f), 2.0f, 0.9f);
        drawCaps (g, kModes[i], tr, 7.8f, sel ? amberHi : steel, juce::Justification::centred, 0.10f);
    }

    drawRecessedWell (g, wellArea, 2.0f);

    if (mode == Mode::Dual)
    {
        const float midY = wellArea.getY() + std::floor (wellArea.getHeight() * 0.46f);
        paintScope (g, wellArea.withBottom (midY - 2.0f));
        drawRule (g, midY, wellArea.getX() + 4.0f, wellArea.getRight() - 4.0f, seamHi);
        paintSpectrum (g, wellArea.withTop (midY + 2.0f));
    }
    else if (mode == Mode::Scope)
    {
        paintScope (g, wellArea);
    }
    else
    {
        paintSpectrum (g, wellArea);
    }
}

void ScopeSpectrumWidget::paintScope (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto a = r.reduced (6.0f, 6.0f);
    if (a.getHeight() < 16.0f) return;

    // Reticle grid
    g.setColour (seam.withAlpha (0.65f));
    for (int k = 1; k < 4; ++k)
        g.fillRect (juce::Rectangle<float> (a.getX() + a.getWidth() * (float) k * 0.25f, a.getY(), 1.0f, a.getHeight()));
    g.setColour (seamHi.withAlpha (0.75f));
    g.fillRect (juce::Rectangle<float> (a.getX(), a.getCentreY(), a.getWidth(), 1.0f));

    // Stabilized rising zero-crossing trigger in the last 1024 samples
    constexpr int kDisp = 320;
    int trig = kFftSize - kDisp - 1;
    const int searchStart = kFftSize - kDisp - 480;
    const int searchEnd   = kFftSize - kDisp - 2;
    float bestSlope = 0.0f;
    for (int i = std::max (1, searchStart); i < searchEnd; ++i)
    {
        const float m0 = 0.5f * (ringL[(size_t) (i - 1)] + ringR[(size_t) (i - 1)]);
        const float m1 = 0.5f * (ringL[(size_t) i] + ringR[(size_t) i]);
        if (m0 <= 0.0f && m1 > 0.0f && (m1 - m0) > bestSlope)
        {
            bestSlope = m1 - m0;
            trig = i;
        }
    }

    // Dynamic auto-gain ceiling so quiet signals remain legible while hot signals stay bounded
    float pk = 0.18f;
    for (int i = 0; i < kDisp; ++i)
    {
        const int idx = juce::jlimit (0, kFftSize - 1, trig + i);
        pk = std::max ({ pk, std::abs (ringL[(size_t) idx]), std::abs (ringR[(size_t) idx]) });
    }
    const float scale = (a.getHeight() * 0.44f) / std::min (1.25f, pk);

    juce::Path pathL, pathR;
    for (int i = 0; i < kDisp; ++i)
    {
        const int idx = juce::jlimit (0, kFftSize - 1, trig + i);
        const float x = a.getX() + a.getWidth() * (float) i / (float) (kDisp - 1);
        const float yL = juce::jlimit (a.getY(), a.getBottom(), a.getCentreY() - ringL[(size_t) idx] * scale);
        const float yR = juce::jlimit (a.getY(), a.getBottom(), a.getCentreY() - ringR[(size_t) idx] * scale);
        if (i == 0) { pathL.startNewSubPath (x, yL); pathR.startNewSubPath (x, yR); }
        else        { pathL.lineTo (x, yL);          pathR.lineTo (x, yR); }
    }

    // Subtle phosphor glow + crisp dual traces (R in cyan, L in amber)
    g.setColour (cyan.withAlpha (0.20f));
    g.strokePath (pathR, juce::PathStrokeType (3.0f));
    g.setColour (cyan.withAlpha (0.82f));
    g.strokePath (pathR, juce::PathStrokeType (1.15f));

    g.setColour (amber.withAlpha (0.22f));
    g.strokePath (pathL, juce::PathStrokeType (3.2f));
    g.setColour (amberHi);
    g.strokePath (pathL, juce::PathStrokeType (1.35f));

    drawCaps (g, "L", { a.getX() + 2.0f, a.getY(), 12.0f, 10.0f }, 7.5f, amber, juce::Justification::centredLeft, 0.08f);
    drawCaps (g, "R", { a.getX() + 12.0f, a.getY(), 12.0f, 10.0f }, 7.5f, cyan, juce::Justification::centredLeft, 0.08f);
    drawCaps (g, "SCOPE", { a.getRight() - 44.0f, a.getY(), 42.0f, 10.0f }, 7.5f, steel.withAlpha (0.75f), juce::Justification::centredRight, 0.12f);
}

void ScopeSpectrumWidget::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto a = r.reduced (6.0f, 5.0f).withTrimmedBottom (11.0f);
    if (a.getHeight() < 16.0f) return;

    const double srHost = proc.getSampleRate() > 8000.0 ? proc.getSampleRate() : 48000.0;
    const double fMin = 30.0, fMax = std::min (11500.0, srHost * 0.24);
    auto xOf = [&] (double hz) { return a.getX() + a.getWidth() * (float) (std::log (hz / fMin) / std::log (fMax / fMin)); };
    auto yOf = [&] (float db) { return a.getBottom() - juce::jlimit (0.0f, 1.0f, (db + 74.0f) / 72.0f) * a.getHeight(); };

    // Frequency grid & labels
    g.setFont (type::mono (8.0f));
    for (double f : { 60.0, 120.0, 250.0, 1000.0, 4000.0, 10000.0 })
    {
        if (f > fMax) continue;
        const float gx = xOf (f);
        g.setColour (f == 120.0 ? amber.withAlpha (0.28f) : seam.withAlpha (0.65f));
        g.fillRect (juce::Rectangle<float> (gx, a.getY(), 1.0f, a.getHeight()));
        g.setColour (f == 120.0 ? amber.withAlpha (0.85f) : steel.withAlpha (0.72f));
        const juce::String lbl = f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f);
        g.drawText (lbl, juce::Rectangle<float> (gx - 14.0f, a.getBottom() + 1.0f, 28.0f, 10.0f), juce::Justification::centred, false);
    }

    // Horizontal dB lines
    g.setColour (seam.withAlpha (0.45f));
    for (float db : { -18.0f, -36.0f, -54.0f })
        g.fillRect (juce::Rectangle<float> (a.getX(), yOf (db), a.getWidth(), 1.0f));

    // Build smooth contour path + filled area + peak hold line
    juce::Path envPath, fillPath, peakPath;
    for (int b = 0; b < kSpecBins; ++b)
    {
        const float x = a.getX() + a.getWidth() * (float) b / (float) (kSpecBins - 1);
        const float y = yOf (specEnv[(size_t) b]);
        const float yp = yOf (specPeak[(size_t) b]);
        if (b == 0)
        {
            envPath.startNewSubPath (x, y);
            peakPath.startNewSubPath (x, yp);
            fillPath.startNewSubPath (x, a.getBottom());
            fillPath.lineTo (x, y);
        }
        else
        {
            envPath.lineTo (x, y);
            peakPath.lineTo (x, yp);
            fillPath.lineTo (x, y);
        }
    }
    fillPath.lineTo (a.getRight(), a.getBottom());
    fillPath.closeSubPath();

    // Gradient fill under spectral contour (cyan upper -> deep amber-slate floor)
    juce::ColourGradient grad (cyan.withAlpha (0.34f), a.getX(), a.getY(),
                               amber.withAlpha (0.04f), a.getX(), a.getBottom(), false);
    grad.addColour (0.60, cyan.withAlpha (0.14f));
    g.setGradientFill (grad);
    g.fillPath (fillPath);

    g.setColour (amber.withAlpha (0.38f));
    g.strokePath (peakPath, juce::PathStrokeType (0.9f));
    g.setColour (cyanHi);
    g.strokePath (envPath, juce::PathStrokeType (1.45f));

    drawCaps (g, "SPECTRUM", { a.getRight() - 58.0f, a.getY(), 56.0f, 10.0f }, 7.5f, steel.withAlpha (0.75f), juce::Justification::centredRight, 0.12f);
}

//==============================================================================
// XySculptorWidget
//==============================================================================
XySculptorWidget::XySculptorWidget (VesselProcessor& p) : proc (p)
{
    curX = plainVal (proc, v2_xy_x);
    curY = plainVal (proc, v2_xy_y);
    pushTrailPoint (curX, curY);
    setTooltip (juce::String::fromUTF8 ("XY Atmosphere / Timbre Sculptor  \xc2\xb7  drag to morph  \xc2\xb7  Shift/Cmd/Ctrl fine  \xc2\xb7  double-click reset  \xc2\xb7  right-click route"));
}

void XySculptorWidget::resized()
{
    const auto b = getLocalBounds().toFloat();
    resetBtnArea = { b.getRight() - 48.0f, 6.0f, 40.0f, 16.0f };
    padArea = b.reduced (10.0f).withTrimmedTop (24.0f).withTrimmedBottom (18.0f);
}

juce::Point<float> XySculptorWidget::normToPad (float x01, float y01) const
{
    const auto inner = padArea.reduced (8.0f);
    return { inner.getX() + juce::jlimit (0.0f, 1.0f, x01) * inner.getWidth(),
             inner.getBottom() - juce::jlimit (0.0f, 1.0f, y01) * inner.getHeight() };
}

void XySculptorWidget::pushTrailPoint (float x01, float y01)
{
    trail[(size_t) trailHead] = { juce::jlimit (0.0f, 1.0f, x01), juce::jlimit (0.0f, 1.0f, y01), true };
    trailHead = (trailHead + 1) % kTrailLen;
}

void XySculptorWidget::clearTrail()
{
    trail.fill ({});
    trailHead = 0;
}

void XySculptorWidget::setPositionFromPoint (juce::Point<float> pt, bool fine)
{
    const auto inner = padArea.reduced (8.0f);
    float nx = curX, ny = curY;
    if (fine)
    {
        const auto d = pt - lastMouse;
        nx = juce::jlimit (0.0f, 1.0f, curX + d.x / std::max (40.0f, inner.getWidth() * 4.5f));
        ny = juce::jlimit (0.0f, 1.0f, curY - d.y / std::max (40.0f, inner.getHeight() * 4.5f));
    }
    else
    {
        nx = juce::jlimit (0.0f, 1.0f, (pt.x - inner.getX()) / std::max (1.0f, inner.getWidth()));
        ny = juce::jlimit (0.0f, 1.0f, (inner.getBottom() - pt.y) / std::max (1.0f, inner.getHeight()));
    }
    curX = nx;
    curY = ny;
    proc.setXyPosition (curX, curY);
    pushTrailPoint (curX, curY);
    repaint();
}

void XySculptorWidget::resetCenter()
{
    proc.beginUndoTransaction ("Reset XY Sculptor");
    curX = 0.5f;
    curY = 0.5f;
    proc.setXyPosition (0.5f, 0.5f);
    pushTrailPoint (0.5f, 0.5f);
    repaint();
}

void XySculptorWidget::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showContextMenu(); return; }
    if (resetBtnArea.contains (e.position)) { resetCenter(); return; }
    if (padArea.contains (e.position))
    {
        proc.beginUndoTransaction ("XY Sculptor");
        dragging = true;
        lastMouse = e.position;
        const bool fine = e.mods.isShiftDown() || e.mods.isCommandDown() || e.mods.isCtrlDown();
        setPositionFromPoint (e.position, fine);
    }
}

void XySculptorWidget::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    const bool fine = e.mods.isShiftDown() || e.mods.isCommandDown() || e.mods.isCtrlDown();
    setPositionFromPoint (e.position, fine);
    lastMouse = e.position;
}

void XySculptorWidget::mouseUp (const juce::MouseEvent&) { dragging = false; repaint(); }

void XySculptorWidget::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (padArea.contains (e.position)) resetCenter();
}

void XySculptorWidget::mouseMove (const juce::MouseEvent& e)
{
    const bool hp = padArea.contains (e.position);
    const bool hr = resetBtnArea.contains (e.position);
    if (hp != hoverPad || hr != hoverReset)
    {
        hoverPad = hp;
        hoverReset = hr;
        setMouseCursor (hp ? juce::MouseCursor::CrosshairCursor : (hr ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
        repaint();
    }
}

void XySculptorWidget::mouseExit (const juce::MouseEvent&)
{
    if (hoverPad || hoverReset) { hoverPad = hoverReset = false; repaint(); }
}

void XySculptorWidget::showContextMenu()
{
    juce::PopupMenu m;
    m.setLookAndFeel (&getLookAndFeel());
    m.addSectionHeader ("XY Sculptor");
    m.addItem (1, "Reset to Center (50% / 50%)");
    m.addItem (2, "Edit XY Destinations in Mod Deck");
    m.addSeparator();
    m.addItem (3, "MIDI Learn X Axis");
    m.addItem (4, "MIDI Learn Y Axis");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                     [safe = juce::Component::SafePointer<XySculptorWidget> (this)] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 1) safe->resetCenter();
        else if (r == 2 && safe->onOpenXyRoutes) safe->onOpenXyRoutes();
        else if (r == 3) safe->proc.midiLearn.beginLearn (v2_xy_x);
        else if (r == 4) safe->proc.midiLearn.beginLearn (v2_xy_y);
    });
}

void XySculptorWidget::refresh()
{
    const float nx = plainVal (proc, v2_xy_x);
    const float ny = plainVal (proc, v2_xy_y);
    const int ver = proc.macroNamesVersion.load (std::memory_order_relaxed);
    if (std::abs (nx - curX) > 0.002f || std::abs (ny - curY) > 0.002f)
    {
        curX = nx;
        curY = ny;
        pushTrailPoint (curX, curY);
        repaint();
    }
    else if (ver != lastLabelVer)
    {
        lastLabelVer = ver;
        repaint();
    }
}

void XySculptorWidget::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    drawMachinedPanel (g, b, 3.0f, true);

    drawCaps (g, "XY SCULPTOR", { 10.0f, 6.0f, 118.0f, 16.0f }, 9.8f, titanium, juce::Justification::centredLeft, 0.16f);

    // Live X/Y mono coordinates in header
    g.setColour (dragging ? amberHi : steel);
    g.setFont (type::mono (9.5f));
    const juce::String coord = juce::String (juce::roundToInt (curX * 100.0f)).paddedLeft (' ', 3)
                             + " / "
                             + juce::String (juce::roundToInt (curY * 100.0f)).paddedLeft (' ', 3);
    g.drawText (coord, juce::Rectangle<float> (124.0f, 6.0f, resetBtnArea.getX() - 130.0f, 16.0f), juce::Justification::centredRight, false);

    // Reset micro button
    g.setColour (hoverReset ? plate : well);
    g.fillRoundedRectangle (resetBtnArea, 2.0f);
    g.setColour (hoverReset ? amber : seamHi);
    g.drawRoundedRectangle (resetBtnArea.reduced (0.5f), 2.0f, 0.9f);
    drawCaps (g, "CTR", resetBtnArea, 7.6f, hoverReset ? amberHi : steel, juce::Justification::centred, 0.10f);

    // Recessed XY pad well
    drawRecessedWell (g, padArea, 2.0f);
    const auto inner = padArea.reduced (8.0f);

    // Subtle quadrant ambient glow around handle
    const auto handle = normToPad (curX, curY);
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (padArea.reduced (1.5f).toNearestInt());

        juce::ColourGradient aura (cyan.withAlpha (dragging ? 0.24f : 0.14f), handle.x, handle.y,
                                   amber.withAlpha (0.0f), handle.x + 88.0f, handle.y + 88.0f, true);
        aura.addColour (0.45, amber.withAlpha (dragging ? 0.10f : 0.05f));
        g.setGradientFill (aura);
        g.fillRect (padArea);

        // Precision 4x4 grid + center crosshair
        g.setColour (seam.withAlpha (0.65f));
        for (int k = 1; k < 4; ++k)
        {
            const float gx = inner.getX() + inner.getWidth() * (float) k * 0.25f;
            const float gy = inner.getY() + inner.getHeight() * (float) k * 0.25f;
            g.fillRect (juce::Rectangle<float> (gx, padArea.getY() + 2.0f, 1.0f, padArea.getHeight() - 4.0f));
            g.fillRect (juce::Rectangle<float> (padArea.getX() + 2.0f, gy, padArea.getWidth() - 4.0f, 1.0f));
        }
        g.setColour (seamHi.withAlpha (0.85f));
        g.fillRect (juce::Rectangle<float> (inner.getCentreX(), padArea.getY() + 2.0f, 1.0f, padArea.getHeight() - 4.0f));
        g.fillRect (juce::Rectangle<float> (padArea.getX() + 2.0f, inner.getCentreY(), padArea.getWidth() - 4.0f, 1.0f));

        // Live drop lines from handle to X and Y axes
        g.setColour (cyan.withAlpha (0.32f));
        g.fillRect (juce::Rectangle<float> (handle.x, padArea.getY() + 2.0f, 1.0f, padArea.getHeight() - 4.0f));
        g.fillRect (juce::Rectangle<float> (padArea.getX() + 2.0f, handle.y, padArea.getWidth() - 4.0f, 1.0f));

        // Glowing motion trail history
        juce::Point<float> prevPt {};
        bool havePrev = false;
        for (int k = 0; k < kTrailLen; ++k)
        {
            const int idx = (trailHead + k) % kTrailLen;
            const auto& tp = trail[(size_t) idx];
            if (! tp.valid) continue;
            const auto pt = normToPad (tp.x, tp.y);
            const float age = (float) (k + 1) / (float) kTrailLen;
            if (havePrev && pt.getDistanceFrom (prevPt) > 0.4f)
            {
                g.setColour (cyan.interpolatedWith (amberHi, age * 0.55f).withAlpha (age * age * 0.72f));
                g.drawLine ({ prevPt, pt }, 0.8f + age * 1.8f);
            }
            prevPt = pt;
            havePrev = true;
        }

        // Concentric machined reticle handle
        const float outerR = dragging ? 13.0f : (hoverPad ? 11.5f : 10.0f);
        g.setColour (amber.withAlpha (dragging ? 0.25f : 0.14f));
        g.fillEllipse (juce::Rectangle<float> (outerR * 2.2f, outerR * 2.2f).withCentre (handle));
        g.setColour (dragging ? amberHi : cyanHi);
        g.drawEllipse (juce::Rectangle<float> (outerR * 2.0f, outerR * 2.0f).withCentre (handle), 1.4f);
        g.setColour (well);
        g.fillPath (diamond (handle, 8.5f));
        g.setColour (dragging ? titanium : amberHi);
        g.fillPath (diamond (handle, 5.5f));
    }

    // Dynamic per-preset axis labels below pad
    const auto foot = juce::Rectangle<float> (10.0f, padArea.getBottom() + 3.0f, b.getWidth() - 20.0f, 14.0f);
    drawCaps (g, "X: " + proc.xyLabelX(), foot.withWidth (foot.getWidth() * 0.5f), 8.2f, amber, juce::Justification::centredLeft, 0.12f);
    drawCaps (g, "Y: " + proc.xyLabelY(), foot.withLeft (foot.getCentreX()), 8.2f, cyan, juce::Justification::centredRight, 0.12f);
}

//==============================================================================
// MasterStripWidget
//==============================================================================
MasterStripWidget::MasterStripWidget (VesselProcessor& p, Registry& reg) : proc (p)
{
    clipLed = std::make_unique<Rune> (p, clip_on, "CLIP", false);
    excLed  = std::make_unique<Rune> (p, exc_on,  "EXC",  false);
    monoLed = std::make_unique<Rune> (p, mono_on, "MONO", false);
    monoLed->getProperties().set ("cyan", true);

    outGain  = std::make_unique<ParamSlider> (p, out_gain,     ParamSlider::Kind::Halo, "Output");
    clipCeil = std::make_unique<ParamSlider> (p, clip_ceiling, ParamSlider::Kind::Halo, "Ceiling");
    excAmt   = std::make_unique<ParamSlider> (p, exc_amount,   ParamSlider::Kind::Halo, "Exciter");
    for (auto* s : { outGain.get(), clipCeil.get(), excAmt.get() })
    {
        s->getProperties().set ("compact", true);
        addAndMakeVisible (*s);
        reg.sliders.push_back (s);
    }
    for (auto* r : { clipLed.get(), excLed.get(), monoLed.get() })
        addAndMakeVisible (*r);
}

void MasterStripWidget::resized()
{
    const int w = getWidth();
    // Header toggles right-aligned
    monoLed->setBounds (w - 58, 4, 52, 18);
    excLed->setBounds  (w - 110, 4, 50, 18);
    clipLed->setBounds (w - 164, 4, 52, 18);

    // 3 compact rotary dials across the lower 62px
    const int kw = (w - 16) / 3;
    const int ky = 23;
    const int kh = getHeight() - ky - 3;
    outGain->setBounds  (8,          ky, kw, kh);
    clipCeil->setBounds (8 + kw,     ky, kw, kh);
    excAmt->setBounds   (8 + 2 * kw, ky, kw, kh);
}

void MasterStripWidget::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    drawMachinedPanel (g, b, 3.0f, true);
    drawCaps (g, "MASTER", { 10.0f, 5.0f, 100.0f, 16.0f }, 9.2f, titanium, juce::Justification::centredLeft, 0.16f);
    drawRule (g, 22.0f, 6.0f, b.getWidth() - 6.0f, seam);
}

void MasterStripWidget::refresh()
{
    // Sync subtle visual opacity if bypassed
    clipCeil->setAlpha (proc.param (clip_on)->getValue() > 0.5f ? 1.0f : 0.45f);
    excAmt->setAlpha   (proc.param (exc_on)->getValue() > 0.5f  ? 1.0f : 0.45f);
}

//==============================================================================
// PerformanceDock
//==============================================================================
PerformanceDock::PerformanceDock (VesselProcessor& p, Registry& reg) : proc (p)
{
    for (int m = 0; m < vessel::kHeroMacros; ++m)
    {
        heroKnobs[(size_t) m] = std::make_unique<ParamSlider> (p, vessel::macroPid (m), ParamSlider::Kind::Hero, " ");
        auto* k = heroKnobs[(size_t) m].get();
        k->onDragStart = [this, m] { selectedHero = m; if (onMacroSelected) onMacroSelected (m); repaint(); };
        addAndMakeVisible (*k);
        reg.sliders.push_back (k);
    }

    organicMono = std::make_unique<Rune> (p, v2_organic_mono, "SUB-120 LOCK", false);
    organicMono->getProperties().set ("cyan", true);
    organicDrift = std::make_unique<ParamSlider> (p, v2_organic_amt,   ParamSlider::Kind::Halo, "Drift");
    organicFloor = std::make_unique<ParamSlider> (p, v2_organic_floor, ParamSlider::Kind::Halo, "Thermal");

    texMode  = std::make_unique<Selector> (p, v2_tex_mode, "Mode", true);
    texMix   = std::make_unique<ParamSlider> (p, v2_tex_mix,   ParamSlider::Kind::Halo, "Mix");
    texTone  = std::make_unique<ParamSlider> (p, v2_tex_tone,  ParamSlider::Kind::Halo, "Tone");
    texDrive = std::make_unique<ParamSlider> (p, v2_tex_drive, ParamSlider::Kind::Halo, "Drive");
    texMix->getProperties().set ("cyan", true);

    addAndMakeVisible (*organicMono);
    addAndMakeVisible (*texMode);
    for (auto* s : { organicDrift.get(), organicFloor.get(), texMix.get(), texTone.get(), texDrive.get() })
    {
        addAndMakeVisible (*s);
        reg.sliders.push_back (s);
    }
}

PerformanceDock::~PerformanceDock()
{
    if (renameEditor != nullptr)
    {
        renameEditor->onReturnKey = nullptr;
        renameEditor->onEscapeKey = nullptr;
        renameEditor->onFocusLost = nullptr;
        renameEditor.reset();
    }
}

juce::Rectangle<float> PerformanceDock::heroBay (int m) const
{
    const float w = macrosPanel.getWidth() / (float) vessel::kHeroMacros;
    return { macrosPanel.getX() + (float) m * w, macrosPanel.getY(), w, macrosPanel.getHeight() };
}

juce::Rectangle<float> PerformanceDock::heroHeader (int m) const
{
    const auto bay = heroBay (m);
    return { bay.getX() + 6.0f, bay.getY() + 6.0f, bay.getWidth() - 12.0f, 20.0f };
}

juce::Rectangle<float> PerformanceDock::heroDragHandle (int m) const
{
    const auto hdr = heroHeader (m);
    return { hdr.getX(), hdr.getY(), 18.0f, hdr.getHeight() };
}

void PerformanceDock::resized()
{
    const float w = (float) getWidth(), h = (float) getHeight();
    // Strict 8px guttered 3-bay hardware performance dock
    macrosPanel  = { 0.0f, 0.0f, 592.0f, h };
    organicPanel = { 600.0f, 0.0f, 280.0f, h };
    texturePanel = { 888.0f, 0.0f, w - 888.0f, h };

    for (int m = 0; m < vessel::kHeroMacros; ++m)
    {
        const auto bay = heroBay (m).toNearestInt();
        heroKnobs[(size_t) m]->setBounds (bay.getX() + 8, bay.getY() + 28, bay.getWidth() - 16, bay.getHeight() - 32);
    }

    // Organic bay
    const auto org = organicPanel.toNearestInt();
    organicMono->setBounds (org.getRight() - 114, org.getY() + 5, 108, 20);
    const int ow = (org.getWidth() - 24) / 2;
    organicDrift->setBounds (org.getX() + 12,      org.getY() + 34, ow, org.getHeight() - 40);
    organicFloor->setBounds (org.getX() + 12 + ow, org.getY() + 34, ow, org.getHeight() - 40);

    // Texture bay
    const auto tex = texturePanel.toNearestInt();
    texMode->setBounds (tex.getRight() - 134, tex.getY() + 6, 126, 20);
    const int tw = (tex.getWidth() - 24) / 3;
    texMix->setBounds   (tex.getX() + 12,          tex.getY() + 34, tw, tex.getHeight() - 40);
    texTone->setBounds  (tex.getX() + 12 + tw,     tex.getY() + 34, tw, tex.getHeight() - 40);
    texDrive->setBounds (tex.getX() + 12 + 2 * tw, tex.getY() + 34, tw, tex.getHeight() - 40);
}

void PerformanceDock::refresh()
{
    const int ver = proc.macroNamesVersion.load (std::memory_order_relaxed);
    if (ver != lastNamesVer)
    {
        lastNamesVer = ver;
        repaint (macrosPanel.toNearestInt());
    }

    // Show true outer cyan modulation ring only when the Hero Macro itself is modulated OR actively driving routed targets
    for (int m = 0; m < vessel::kHeroMacros; ++m)
    {
        const float macroVal = proc.param (vessel::macroPid (m))->getValue();
        const float selfMod = proc.telemetry.modOffset[(size_t) vessel::macroPid (m)].load (std::memory_order_relaxed);
        if (std::abs (selfMod) > 0.004f)
        {
            heroKnobs[(size_t) m]->setModulation (selfMod);
        }
        else if (macroVal > 0.015f)
        {
            float peakRouteActivity = 0.0f;
            for (int s = 0; s < vessel::kMacroSlots; ++s)
            {
                const int destIdx = juce::roundToInt (plainVal (proc, vessel::macroDestPid (m, s)));
                if (destIdx > 0)
                {
                    const float amt = plainVal (proc, vessel::macroAmtPid (m, s));
                    const float eff = amt * macroVal;
                    if (std::abs (eff) > std::abs (peakRouteActivity)) peakRouteActivity = eff;
                }
            }
            heroKnobs[(size_t) m]->setModulation (juce::jlimit (-0.28f, 0.28f, peakRouteActivity * 0.45f));
        }
        else
        {
            heroKnobs[(size_t) m]->setModulation (0.0f);
        }
    }

    const bool texActive = plainVal (proc, v2_tex_mix) > 0.001f;
    texTone->setAlpha  (texActive ? 1.0f : 0.55f);
    texDrive->setAlpha (texActive ? 1.0f : 0.55f);
}

void PerformanceDock::beginRename (int m)
{
    selectedHero = m;
    renameEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *renameEditor;
    addAndMakeVisible (ed);
    ed.setBounds (heroHeader (m).withTrimmedLeft (20.0f).toNearestInt());
    ed.setFont (type::caps (10.0f, 0.10f));
    ed.setInputRestrictions (18);
    ed.setIndents (2, 1);
    ed.setText (proc.heroMacroLabel (m), false);
    ed.selectAll();
    auto commit = [this, m] (bool keep)
    {
        if (renameEditor == nullptr) return;
        if (keep) proc.setMacroName (m, renameEditor->getText().trim());
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PerformanceDock> (this)]
        {
            if (safe != nullptr) { safe->renameEditor.reset(); safe->repaint(); }
        });
    };
    ed.onReturnKey = [commit] { commit (true); };
    ed.onFocusLost = [commit] { commit (true); };
    ed.onEscapeKey = [commit] { commit (false); };
    ed.grabKeyboardFocus();
}

void PerformanceDock::mouseDown (const juce::MouseEvent& e)
{
    pressPos = e.position;
    dragArmed = false;
    dragMacroIdx = -1;
    for (int m = 0; m < vessel::kHeroMacros; ++m)
    {
        if (heroDragHandle (m).expanded (4.0f, 2.0f).contains (e.position))
        {
            dragArmed = true;
            dragMacroIdx = m;
            selectedHero = m;
            if (onMacroSelected) onMacroSelected (m);
            repaint();
            return;
        }
        if (heroHeader (m).contains (e.position))
        {
            selectedHero = m;
            if (onMacroSelected) onMacroSelected (m);
            repaint();
            return;
        }
    }
}

void PerformanceDock::mouseDrag (const juce::MouseEvent& e)
{
    if (dragArmed && dragMacroIdx >= 0 && e.position.getDistanceFrom (pressPos) > 4.0f)
    {
        dragArmed = false;
        const ModSource src { ModSource::Macro, dragMacroIdx };
        if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
            if (! c->isDragAndDropActive())
                c->startDragging (src.dragId(), this, juce::ScaledImage (makeDragImage (src.displayName (proc)), 2.0), false);
    }
}

void PerformanceDock::mouseUp (const juce::MouseEvent&) { dragArmed = false; dragMacroIdx = -1; }

void PerformanceDock::mouseDoubleClick (const juce::MouseEvent& e)
{
    for (int m = 0; m < vessel::kHeroMacros; ++m)
        if (heroHeader (m).contains (e.position)) { beginRename (m); return; }
}

void PerformanceDock::mouseMove (const juce::MouseEvent& e)
{
    bool overHandle = false;
    for (int m = 0; m < vessel::kHeroMacros; ++m)
        if (heroDragHandle (m).expanded (4.0f, 2.0f).contains (e.position)) overHandle = true;
    setMouseCursor (overHandle ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void PerformanceDock::paint (juce::Graphics& g)
{
    drawMachinedPanel (g, macrosPanel,  3.0f, true);
    drawMachinedPanel (g, organicPanel, 3.0f, true);
    drawMachinedPanel (g, texturePanel, 3.0f, true);

    // ── Bay 1: 4 Hero Macro Knobs
    for (int m = 0; m < vessel::kHeroMacros; ++m)
    {
        const auto bay = heroBay (m);
        const auto hdr = heroHeader (m);
        if (m > 0) drawSeam (g, bay.getX(), bay.getY() + 8.0f, bay.getBottom() - 8.0f, seam);

        if (m == selectedHero)
        {
            g.setColour (amber.withAlpha (0.045f));
            g.fillRect (bay.reduced (2.0f));
            g.setColour (amber);
            g.fillRect (juce::Rectangle<float> (bay.getX() + 10.0f, bay.getY() + 1.0f, bay.getWidth() - 20.0f, 2.0f));
        }

        // Draggable cryo-cyan routing diamond
        const juce::Point<float> dc { hdr.getX() + 7.0f, hdr.getCentreY() };
        g.setColour (cyan);
        g.strokePath (diamond (dc, 9.0f), juce::PathStrokeType (1.2f));
        g.fillPath (diamond (dc, 4.0f));

        // Dynamic Hero Macro label
        const auto lbl = proc.heroMacroLabel (m);
        drawCaps (g, lbl, hdr.withTrimmedLeft (17.0f).withTrimmedRight (24.0f), 9.5f,
                  m == selectedHero ? amberHi : titanium, juce::Justification::centredLeft, 0.12f);

        // Index badge + active route count pips
        drawCaps (g, "M" + juce::String (m + 1), hdr.withLeft (hdr.getRight() - 22.0f), 8.2f, steel, juce::Justification::centredRight, 0.08f);

        int routed = 0;
        for (int s = 0; s < vessel::kMacroSlots; ++s)
            if (juce::roundToInt (plainVal (proc, vessel::macroDestPid (m, s))) != 0) ++routed;
        for (int p = 0; p < vessel::kMacroSlots; ++p)
        {
            g.setColour (p < routed ? cyan : seamHi.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (bay.getX() + 23.0f + (float) p * 7.0f, hdr.getBottom() + 1.0f, 4.0f, 2.0f));
        }
    }

    // ── Bay 2: Organic Analog Layer
    drawCaps (g, "ORGANIC ANALOG", { organicPanel.getX() + 10.0f, organicPanel.getY() + 7.0f, 150.0f, 16.0f },
              9.2f, titanium, juce::Justification::centredLeft, 0.15f);
    drawRule (g, organicPanel.getY() + 28.0f, organicPanel.getX() + 8.0f, organicPanel.getRight() - 8.0f, seam);

    // ── Bay 3: Procedural Texture Bus
    drawCaps (g, "PROCEDURAL TEXTURE", { texturePanel.getX() + 10.0f, texturePanel.getY() + 7.0f, 210.0f, 16.0f },
              9.2f, titanium, juce::Justification::centredLeft, 0.15f);
    drawRule (g, texturePanel.getY() + 28.0f, texturePanel.getX() + 8.0f, texturePanel.getRight() - 8.0f, seam);
}
} // namespace vessel::ui
