#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Reliquary.h"
#include "Routing.h"

class VesselProcessor;

namespace vessel::ui
{
/** A parameter-bound hardware control: MACHINED DIAL (Halo/Hero), VERTICAL FADER (Vial), or ROUTE BAR (Bar).
    - Drag to adjust · Shift/Cmd/Ctrl for fine precision drag
    - Double-click (or Alt/Option-click) resets to default (undoable)
    - Hover/drag shows illuminated exact value readout
    - Right-click opens Modulation Routing, MIDI CC Learn, Type Exact Value, and Reset
    - Shows live modulation arcs (cryo-cyan) and MIDI Learn active state */
class ParamSlider : public juce::Slider,
                    public juce::DragAndDropTarget
{
public:
    enum class Kind { Halo, Vial, Bar, Hero };

    ParamSlider (VesselProcessor&, int pid, Kind, const juce::String& caption = {}, const juce::String& shortName = {});
    ~ParamSlider() override;

    const int pid;
    void setModulation (float normalisedOffset);
    /** Syncs MIDI Learn active/bound visual indicator (called on UI refresh timer). */
    void syncMidiLearnState();
    /** A source is being dragged and this control would accept it (drawn as a dashed cyan ring). */
    void setDropCandidate (bool);

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    static constexpr int kHaloHeight = 72;
    static constexpr int kVialHeight = 112;
    static constexpr int kBarHeight  = 15;
    static constexpr int kHeroHeight = 84;

private:
    void openEntry();
    void closeEntry();
    void resetToDefault();
    void showRouteMenu();
    void route (ModSource, bool add);
    void setDropState (int);

    VesselProcessor& proc;
    juce::RangedAudioParameter& param;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    std::unique_ptr<juce::TextEditor> entry;
    float lastMod = 0.0f;
    int dropState = 0;   // 0 none, 1 candidate, 2 hovering
    bool lastLearning = false;
    int lastLearnedCc = -1;
};

/** Machined hardware LED toggle bound to a Bool parameter. */
class Rune : public juce::ToggleButton
{
public:
    Rune (VesselProcessor&, int pid, const juce::String& caption = {}, bool header = false);
    const int pid;
    static constexpr int kHeight = 22;

private:
    std::unique_ptr<juce::ButtonParameterAttachment> attachment;
};

/** SELECTOR — caption above a recessed ‹ value › box. Clicking chevrons steps; clicking centre opens the list. */
class Selector : public juce::Component
{
public:
    Selector (VesselProcessor&, int pid, const juce::String& caption = {}, bool compact = false);
    void paint (juce::Graphics&) override;
    void resized() override;
    const int pid;
    static constexpr int kHeight = 36;
    static constexpr int kCompactHeight = 18;

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
