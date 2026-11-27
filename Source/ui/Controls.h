#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Reliquary.h"
#include "Routing.h"

class VesselProcessor;

namespace vessel::ui
{
/** A parameter-bound slider drawn as a HALO (rotary), a VIAL (vertical) or a BAR (horizontal route amount).
    Drag to change · double-click for exact entry · alt/option-click to reset · shows live LFO modulation.
    Every modulation destination is also a drop target for a dragged LFO / expression source, and its
    right-click menu routes or un-routes any source in one click. */
class ParamSlider : public juce::Slider,
                    public juce::DragAndDropTarget
{
public:
    enum class Kind { Halo, Vial, Bar };

    ParamSlider (VesselProcessor&, int pid, Kind, const juce::String& caption = {}, const juce::String& shortName = {});
    ~ParamSlider() override;

    const int pid;
    void setModulation (float normalisedOffset);
    /** A source is being dragged and this control would accept it (drawn as a faint chlorine ring). */
    void setDropCandidate (bool);

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    static constexpr int kHaloHeight = 82;
    static constexpr int kVialHeight = 132;
    static constexpr int kBarHeight  = 16;

private:
    void openEntry();
    void closeEntry();
    void showRouteMenu();
    void route (ModSource, bool add);
    void setDropState (int);

    VesselProcessor& proc;
    juce::RangedAudioParameter& param;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    std::unique_ptr<juce::TextEditor> entry;
    float lastMod = 0.0f;
    int dropState = 0;   // 0 none, 1 candidate, 2 hovering
};

/** RUNE — diamond switch bound to a Bool parameter. */
class Rune : public juce::ToggleButton
{
public:
    Rune (VesselProcessor&, int pid, const juce::String& caption = {}, bool header = false);
    const int pid;
    static constexpr int kHeight = 24;

private:
    std::unique_ptr<juce::ButtonParameterAttachment> attachment;
};

/** SELECTOR — caption above a ‹ value › box. Clicking the chevrons steps, clicking the value opens the list.
    Compact selectors (route rows) have no caption and fill their bounds. */
class Selector : public juce::Component
{
public:
    Selector (VesselProcessor&, int pid, const juce::String& caption = {}, bool compact = false);
    void paint (juce::Graphics&) override;
    void resized() override;
    const int pid;
    static constexpr int kHeight = 40;
    static constexpr int kCompactHeight = 20;

    struct StepBox : juce::ComboBox
    {
        void mouseDown (const juce::MouseEvent& e) override;
    };
    StepBox box;

private:
    juce::String caption;
    bool compact;
    std::unique_ptr<juce::ComboBoxParameterAttachment> attachment;
};
} // namespace vessel::ui
