#pragma once

#include "Controls.h"
#include "../Parameters.h"
#include <functional>

class VesselProcessor;

namespace vessel::ui
{
/** Every parameter-bound slider registers here so the editor can paint live modulation onto it. */
struct Registry
{
    std::vector<ParamSlider*> sliders;
    std::vector<juce::Component*> live;   // components with telemetry to repaint each frame
};

//==============================================================================
/** A vertical rank: rune header (or plain title) + feature number, rows of controls,
    and an optional "instrument" area at the bottom that visualises the module's state. */
class ModuleColumn : public juce::Component
{
public:
    ModuleColumn (VesselProcessor&, Registry&, const juce::String& title, const juce::String& featureNo, int togglePid = -1);

    ParamSlider& halo (int pid, const juce::String& caption = {});
    ParamSlider& vial (int pid, const juce::String& caption, const juce::String& shortName);
    Rune& rune (int pid, const juce::String& caption = {});
    Selector& selector (int pid, const juce::String& caption = {});
    juce::TextButton& button (const juce::String& text);

    /** Adds the most recently created N components as one row (laid out left to right, equal widths, or in
        proportion to `weights` when given). */
    void row (std::initializer_list<juce::Component*>, int height, std::initializer_list<float> weights = {});

    std::function<void (juce::Graphics&, juce::Rectangle<float>)> instrument;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    struct Row { std::vector<juce::Component*> items; int height; std::vector<float> weights; };

    VesselProcessor& proc;
    Registry& reg;
    juce::String title, number;
    int togglePid;
    std::unique_ptr<Rune> header;
    juce::OwnedArray<juce::Component> owned;
    std::vector<Row> rows;
    juce::Rectangle<float> instrumentArea;
    bool lastOn = true, showNumber = true;
};

//==============================================================================
class StagePage : public juce::Component
{
public:
    ModuleColumn& add (std::unique_ptr<ModuleColumn>, float weight);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh() { for (auto* c : columns) c->refresh(); }

private:
    juce::OwnedArray<ModuleColumn> columns;
    std::vector<float> weights;
};

//==============================================================================
/** Left rail: the five stages as a signal column, plus the voice block. */
class Spine : public juce::Component
{
public:
    Spine (VesselProcessor&, Registry&);
    std::function<void (int)> onSelect;
    void setSelected (int s) { selected = s; repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }

    static constexpr int kStageH = 64, kTop = 14;

private:
    int stageAt (juce::Point<int>) const;
    VesselProcessor& proc;
    int selected = 0, hover = -1;
    std::unique_ptr<ModuleColumn> voice;
};

//==============================================================================
class Header : public juce::Component
{
public:
    explicit Header (VesselProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    std::function<void()> onBrowse;   // the preset name was clicked
    std::function<void()> onTorch;    // the Lights Out button was clicked
    std::function<void()> onKeys;     // the QWERTY keyboard piano pill was clicked
    /** The Lights Out button, top right (header coordinates). */
    juce::Rectangle<int> lightsArea() const { return { getWidth() - 132, getHeight() / 2 - 14, 114, 28 }; }
    /** Computer keyboard piano toggle pill, left of Lights Out. */
    juce::Rectangle<int> keysArea() const   { return { lightsArea().getX() - 90, getHeight() / 2 - 14, 80, 28 }; }
    void setBrowserOpen (bool o) { browserOpen = o; repaint(); }
    void setQwertyState (bool on, int octave) { qwertyOn = on; qwertyOct = octave; repaint(); }

private:
    bool browserOpen = false;
    bool qwertyOn = false;
    int qwertyOct = 3;
    juce::Rectangle<int> presetArea() const;
    VesselProcessor& proc;
    juce::TextButton prev { "<" }, next { ">" };
};

//==============================================================================
/** Right rail: the vessel meter + phase scope, then clipper, exciter and mono collapse. */
class MasterRail : public juce::Component
{
public:
    MasterRail (VesselProcessor&, Registry&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    void paintMeter (juce::Graphics&, juce::Rectangle<float>);
    void paintScope (juce::Graphics&, juce::Rectangle<float>);
    void paintTelemetry (juce::Graphics&, juce::Rectangle<float>);

    VesselProcessor& proc;
    std::unique_ptr<ModuleColumn> clip, exciter, mono;
    juce::Rectangle<float> meterArea, scopeArea, telemetryArea;
    float clipFlash = 0.0f;
    int lastClipCount = 0;
};

//==============================================================================
/** Bottom: the modulation deck. The first tab is MACROS (eight named knobs, six routes each), then the three
    warped LFOs (motion controls, shape canvas, four routes each), then EXPRESSION (velocity, mod wheel and
    aftertouch, two routes each).
    Mapping: drag an LFO tab, a macro's diamond or an expression header onto any control, or right-click the control. */
class ModPanel : public juce::Component
{
public:
    ModPanel (VesselProcessor&, Registry&);
    ~ModPanel() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void select (int page);   // 0..2 LFO, 3 expression, 4 macros
    void selectMacro (int m);
    void refresh();

    /** Brings the source's page forward, points at the route and says what happened. */
    void showRoute (ModSource, int slot, routing::Result, int targetPid);
    void showMessage (const juce::String&);

    static constexpr int kExprPage = 3, kMacroPage = 4, kPages = 5;

private:
    struct LfoPage
    {
        Selector* shape = nullptr, *div = nullptr, *trig = nullptr;
        Rune* sync = nullptr, *uni = nullptr;
        ParamSlider* rate = nullptr, *warp = nullptr, *phase = nullptr, *fade = nullptr, *smooth = nullptr;
        std::array<Selector*, kLfoSlots> dest {};
        std::array<ParamSlider*, kLfoSlots> amt {};
        std::vector<juce::Component*> all;
    };
    struct ExprPage
    {
        std::array<std::array<Selector*, 2>, 3> dest {};
        std::array<std::array<ParamSlider*, 2>, 3> amt {};
        ParamSlider* velAmp = nullptr;
        std::vector<juce::Component*> all;
    };

    struct MacroPage
    {
        std::array<ParamSlider*, kNumMacros> knob {};
        std::array<std::array<Selector*, kMacroSlots>, kNumMacros> dest {};
        std::array<std::array<ParamSlider*, kMacroSlots>, kNumMacros> amt {};
    };

    void drawAt (juce::Point<float>, bool interpolateFromLast);
    void paintMacros (juce::Graphics&);
    juce::Rectangle<float> macroHead (int m) const;
    void beginRename (int m);
    static int pageAtTab (int tab) noexcept { return tab == 0 ? kMacroPage : tab - 1; }
    static int tabOfPage (int page) noexcept { return page == kMacroPage ? 0 : page + 1; }
    void paintTabs (juce::Graphics&);
    void paintLfo (juce::Graphics&);
    void paintExpr (juce::Graphics&);
    juce::Rectangle<float> tabRect (int i) const;
    juce::Rectangle<float> exprHead (int src) const;
    juce::Rectangle<float> routeRow (int page, int slot) const;
    /** The drag source under p: an LFO tab or an expression header. */
    bool sourceAt (juce::Point<float>, ModSource&) const;
    static void setIdle (ParamSlider*, bool);

    VesselProcessor& proc;
    int current = 0;
    juce::OwnedArray<juce::Component> owned;
    std::array<LfoPage, kNumLfos> lfo;
    ExprPage expr;
    MacroPage mac;
    int macroSel = 0, namesSeen = -1;
    std::unique_ptr<juce::TextEditor> rename;
    juce::TextButton scatter { "Scatter" };
    juce::Rectangle<float> canvas, tabs;
    juce::Point<float> lastDraw, pressAt;
    bool drawing = false, dragArmed = false;
    ModSource armed;

    juce::String message;
    double messageUntil = 0.0, flashUntil = 0.0;
    int flashPage = -1, flashSlot = -1;
};

//==============================================================================
/** The preset browser: drops down from the header's preset name over the stage, in the Reliquary style.
    Categories (with counts) on the left, the category's presets as a grid of names with their character
    tags, and an info strip for the preset under the mouse (or the current one).
    Click loads and closes · arrow keys audition · page up / down change category · esc or a click outside closes. */
class PresetBrowser : public juce::Component
{
public:
    explicit PresetBrowser (VesselProcessor&);
    std::function<void()> onClose, onLoaded;

    void open();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void setCategory (int);
    void load (int presetIndex, bool close);
    std::vector<int> itemsOf (int cat) const;

    VesselProcessor& proc;
    juce::StringArray cats;
    juce::Rectangle<float> panel, list, grid, info;
    std::vector<juce::Rectangle<float>> catRects, itemRects;
    std::vector<int> items;
    int cat = 0, hoverItem = -1, hoverCat = -1, cols = 3;
};

/** Builds stage page 0..4 (Source, Ruin, Shape, Space, Pressure). */
std::unique_ptr<StagePage> buildStage (int index, VesselProcessor&, Registry&);
} // namespace vessel::ui
