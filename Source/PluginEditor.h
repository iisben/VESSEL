#pragma once

#include "PluginProcessor.h"
#include "ui/Reliquary.h"
#include "ui/Backdrop.h"
#include "ui/Torch.h"
#include "ui/Panels.h"

/** VESSEL editor. Logical canvas 1240 x 780, scaled uniformly when the window is resized.
    An OpenGL context renders the ink backdrop and composites every component on top of it.
    It is the drag-and-drop container for mapping (an LFO tab dragged onto a control routes it). */
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

    static constexpr int kWidth = 1240, kHeight = 780;
    void showStage (int);
    void showBrowser (bool);
    void selectModPage (int page) { mod->select (page); }
    /** Rings every modulation destination, as during a drag (also used by the snapshot tool). */
    void setDropPreview (bool);
    /** The torch (the mouse as a flashlight in a dark panel); for snapshots it can be held at a point. */
    void setTorch (bool);
    void aimTorch (std::optional<juce::Point<float>> p) { torch.aim (p); }

    // RouteListener
    void routeChanged (vessel::ui::ModSource, int slot, vessel::ui::routing::Result, int targetPid) override;
    void routesCleared (int targetPid) override;

private:
    void timerCallback() override;
    void dragOperationStarted (const juce::DragAndDropTarget::SourceDetails&) override;
    void dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&) override;

    VesselProcessor& proc;
    vessel::ui::ReliquaryLookAndFeel laf;
    juce::OpenGLContext gl;
    vessel::ui::Backdrop backdrop { gl };
    vessel::ui::Registry registry;

    juce::Component canvas;
    std::unique_ptr<vessel::ui::Header> header;
    std::unique_ptr<vessel::ui::Spine> spine;
    std::array<std::unique_ptr<vessel::ui::StagePage>, 5> pages;
    std::unique_ptr<vessel::ui::MasterRail> rail;
    std::unique_ptr<vessel::ui::ModPanel> mod;
    std::unique_ptr<vessel::ui::PresetBrowser> browser;
    std::unique_ptr<juce::TooltipWindow> tooltips;
    vessel::ui::Torch torch;

    int stage = 0;
    bool glWasAlive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VesselEditor)
};
