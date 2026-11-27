#pragma once

#include "PluginProcessor.h"
#include "ui/Reliquary.h"
#include "ui/HeroCenterpiece.h"
#include "ui/Torch.h"
#include "ui/Panels.h"

/** VESSEL II Flagship Editor (NAMM 2027).
    - Strict 8px alignment grid on a 1280 x 800 logical canvas
    - Native JUCE 2D hardware-accelerated Graphics with pre-rendered 2x Retina static chassis cache
      (zero OpenGL context dependency for rock-solid macOS/Windows/Linux DAW stability)
    - Steady 60 Hz lock-free updates for Dual Scope / Spectral-Contour, XY Sculptor, Meters, and Mod Rings
    - Persisted cross-platform window zoom (75% / 100% / 125% / 150% + aspect-locked drag resize) */
class VesselEditor : public juce::AudioProcessorEditor,
                     public juce::DragAndDropContainer,
                     public vessel::ui::RouteListener,
                     private juce::Timer
{
public:
    explicit VesselEditor (VesselProcessor&);
    ~VesselEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    static constexpr int kWidth = 1280, kHeight = 800;

    void showStage (int);
    void showBrowser (bool);
    void showSaveDialog (bool);
    void selectModPage (int page) { mod->select (page); }
    void setZoomScale (float scale, bool persist = true);

    /** Rings every modulation destination (during drag or QA snapshots). */
    void setDropPreview (bool);
    /** Optional torch overlay preserved for V1/VesselCheck compatibility (default OFF). */
    void setTorch (bool);
    void aimTorch (std::optional<juce::Point<float>> p) { torch.aim (p); }

    /** Force an immediate telemetry/analyzer UI refresh (used by headless Retina snapshot QA). */
    void refreshVisualsNow();
    vessel::ui::XySculptorWidget& xyWidget() noexcept { return *xy; }
    vessel::ui::PresetBrowser& browserWidget() noexcept { return *browser; }

    // RouteListener
    void routeChanged (vessel::ui::ModSource, int slot, vessel::ui::routing::Result, int targetPid) override;
    void routesCleared (int targetPid) override;

private:
    void timerCallback() override;
    void dragOperationStarted (const juce::DragAndDropTarget::SourceDetails&) override;
    void dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&) override;
    static juce::File uiPrefsFile();
    static float loadPersistedZoom();
    static void savePersistedZoom (float scale);

    VesselProcessor& proc;
    vessel::ui::ReliquaryLookAndFeel laf;
    vessel::ui::Registry registry;

    /** Pre-rendered 2x Retina static anodized chassis + matte grain cache. */
    juce::Image chassisCache;

    juce::Component canvas;
    std::unique_ptr<vessel::ui::Header> header;
    std::unique_ptr<vessel::ui::Spine> spine;
    std::array<std::unique_ptr<vessel::ui::StagePage>, 5> pages;
    std::unique_ptr<vessel::ui::ModPanel> mod;
    std::unique_ptr<vessel::ui::ScopeSpectrumWidget> scope;
    std::unique_ptr<vessel::ui::XySculptorWidget> xy;
    std::unique_ptr<vessel::ui::MasterStripWidget> masterStrip;
    std::unique_ptr<vessel::ui::PerformanceDock> dock;
    std::unique_ptr<vessel::ui::PresetBrowser> browser;
    std::unique_ptr<vessel::ui::SavePresetOverlay> saveOverlay;
    std::unique_ptr<juce::TooltipWindow> tooltips;
    vessel::ui::Torch torch;

    int stage = 0;
    int midiLearnTick = 0;
    float currentZoom = 1.0f;
    bool initializedSize = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VesselEditor)
};
