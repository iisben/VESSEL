#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "Controls.h"
#include "Reliquary.h"
#include "Routing.h"
#include "../Parameters.h"
#include <array>
#include <functional>

class VesselProcessor;

namespace vessel::ui
{
struct Registry;

//==============================================================================
/** Real-time 60 Hz lock-free Dual Oscilloscope & Logarithmic Spectral-Contour Visualizer.
    Reads strictly from `proc.telemetry.scopeL/scopeR/scopeWrite` (zero audio-thread allocations/locks).
    Also computes Sub-120 Hz stereo phase correlation and momentary LUFS-style level on the UI thread
    for the Top Bar meters without adding any audio-thread CPU overhead. */
class ScopeSpectrumWidget : public juce::Component
{
public:
    enum class Mode { Dual, Scope, Spectrum };

    explicit ScopeSpectrumWidget (VesselProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void refresh();

    void setMode (Mode m) { mode = m; repaint(); }
    Mode currentMode() const noexcept { return mode; }

    /** Sub-120 Hz stereo phase correlation in [-1, +1] computed from latest FFT frame on UI thread. */
    float sub120Correlation() const noexcept { return subCorrSmooth; }
    bool sub120HasSignal() const noexcept { return subSignalPresent; }
    /** Momentary LUFS-style loudness estimate in dB (clamped [-70, 0]). */
    float momentaryLufs() const noexcept { return lufsSmooth; }

private:
    static constexpr int kFftOrder = 11;
    static constexpr int kFftSize  = 1 << kFftOrder;   // 2048 samples @ sr/2 (~85 ms @ 48 kHz -> 11.7 Hz/bin)
    static constexpr int kSpecBins = 96;

    void analyzeAudio();
    void paintScope (juce::Graphics&, juce::Rectangle<float>);
    void paintSpectrum (juce::Graphics&, juce::Rectangle<float>);
    juce::Rectangle<float> modeTabRect (int idx) const;

    VesselProcessor& proc;
    Mode mode = Mode::Dual;
    juce::dsp::FFT fft { kFftOrder };

    std::array<float, kFftSize> ringL {}, ringR {}, hann {};
    std::array<float, kFftSize * 2> fftL {}, fftR {};
    std::array<float, kSpecBins> specEnv {}, specPeak {};

    float subCorrSmooth = 1.0f;
    bool subSignalPresent = false;
    float lufsSmooth = -70.0f;
    bool lufsInitialized = false;
    juce::Rectangle<float> wellArea;
};

//==============================================================================
/** Interactive draggable XY Atmosphere / Timbre Sculptor bound to `v2_xy_x` and `v2_xy_y`.
    Features glowing motion trail history, precision reticle cursor, dynamic per-preset X/Y axis labels,
    Shift/Cmd/Ctrl fine drag, double-click center reset, and right-click destination quick-routing. */
class XySculptorWidget : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    explicit XySculptorWidget (VesselProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void refresh();

    /** Seeds a subtle visual trajectory trail (also useful for headless UI showcase snapshots). */
    void pushTrailPoint (float x01, float y01);
    void clearTrail();

    std::function<void()> onOpenXyRoutes;

private:
    void setPositionFromPoint (juce::Point<float>, bool fine);
    void resetCenter();
    void showContextMenu();
    juce::Point<float> normToPad (float x01, float y01) const;

    VesselProcessor& proc;
    juce::Rectangle<float> padArea, resetBtnArea;
    bool dragging = false, hoverPad = false, hoverReset = false;
    juce::Point<float> lastMouse {};
    float curX = 0.5f, curY = 0.5f;
    int lastLabelVer = -1;

    static constexpr int kTrailLen = 36;
    struct TrailPt { float x = 0.5f, y = 0.5f; bool valid = false; };
    std::array<TrailPt, kTrailLen> trail {};
    int trailHead = 0;
};

//==============================================================================
/** Always-accessible compact Master Output Plate (bottom of Right Hero Column). */
class MasterStripWidget : public juce::Component
{
public:
    MasterStripWidget (VesselProcessor&, Registry&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    VesselProcessor& proc;
    std::unique_ptr<Rune> clipLed, excLed, monoLed;
    std::unique_ptr<ParamSlider> outGain, clipCeil, excAmt;
};

//==============================================================================
/** Bottom Performance Dock:
    - 4 Hero Performance Macro Knobs (M1..M4) with dynamic per-preset labels, live activity rings,
      drag-to-route handles, and double-click inline rename
    - V2 Organic Analog Layer (Organic Drift, Thermal Floor, Sub-120 Anchor toggle)
    - V2 Procedural Texture Bus (Mode selector, Mix, Tone, Drive) */
class PerformanceDock : public juce::Component
{
public:
    PerformanceDock (VesselProcessor&, Registry&);
    ~PerformanceDock() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void refresh();

    std::function<void (int)> onMacroSelected;

private:
    juce::Rectangle<float> heroBay (int m) const;
    juce::Rectangle<float> heroHeader (int m) const;
    juce::Rectangle<float> heroDragHandle (int m) const;
    void beginRename (int m);

    VesselProcessor& proc;
    std::array<std::unique_ptr<ParamSlider>, vessel::kHeroMacros> heroKnobs;
    std::unique_ptr<Rune> organicMono;
    std::unique_ptr<ParamSlider> organicDrift, organicFloor;
    std::unique_ptr<Selector> texMode;
    std::unique_ptr<ParamSlider> texMix, texTone, texDrive;
    std::unique_ptr<juce::TextEditor> renameEditor;

    juce::Rectangle<float> macrosPanel, organicPanel, texturePanel;
    int selectedHero = 0;
    int lastNamesVer = -1;
    bool dragArmed = false;
    int dragMacroIdx = -1;
    juce::Point<float> pressPos {};
};
} // namespace vessel::ui
