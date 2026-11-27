#pragma once

#include "Controls.h"
#include "../Parameters.h"
#include "../Presets.h"
#include <functional>
#include <vector>

class VesselProcessor;

namespace vessel::ui
{
/** Every parameter-bound slider registers here so the editor can paint live modulation and MIDI Learn states. */
struct Registry
{
    std::vector<ParamSlider*> sliders;
    std::vector<juce::Component*> live;
};

//==============================================================================
/** A machined vertical module column: hardware LED header + index, rows of controls,
    and an optional recessed instrument display at the bottom. */
class ModuleColumn : public juce::Component
{
public:
    ModuleColumn (VesselProcessor&, Registry&, const juce::String& title, const juce::String& featureNo, int togglePid = -1);

    ParamSlider& halo (int pid, const juce::String& caption = {});
    ParamSlider& vial (int pid, const juce::String& caption, const juce::String& shortName);
    Rune& rune (int pid, const juce::String& caption = {});
    Selector& selector (int pid, const juce::String& caption = {});
    juce::TextButton& button (const juce::String& text);

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
/** Left signal spine: the five synthesis stages as machined tabs + the Voice & Expression block. */
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

    static constexpr int kStageH = 50, kTop = 8;

private:
    int stageAt (juce::Point<int>) const;
    VesselProcessor& proc;
    int selected = 0, hover = -1;
    std::unique_ptr<ModuleColumn> voice;
};

//==============================================================================
/** Top Command Bar:
    - VESSEL II wordmark & emblem
    - Preset Bar (Prev/Next, Name/Category/Favorite/Browser trigger, Save, Init)
    - A/B Compare + Copy A->B, Undo/Redo, MPE Indicator/Toggle
    - Zoom Selector (75% / 100% / 125% / 150%)
    - Stereo Output Peak/LUFS-style Meter + Sub-120 Hz Mono-Correlation Indicator */
class Header : public juce::Component
{
public:
    explicit Header (VesselProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void refresh (float lufsMomentary, float sub120Corr, bool subHasSignal);

    std::function<void()> onBrowse;
    std::function<void()> onSave;
    std::function<void()> onInit;
    std::function<void()> onTorch;
    std::function<void (float)> onZoom;

    void setBrowserOpen (bool o) { browserOpen = o; repaint(); }
    void setZoomPercent (int pct);
    juce::Rectangle<int> lightsArea() const { return { 0, 0, 0, 0 }; }

private:
    void stepPreset (int delta);
    juce::Rectangle<int> presetBarArea() const;
    juce::Rectangle<int> presetClickArea() const;
    juce::Rectangle<int> favStarArea() const;
    juce::Rectangle<float> meterArea() const;
    juce::Rectangle<float> corrArea() const;

    VesselProcessor& proc;
    bool browserOpen = false;
    bool hoverPreset = false, hoverStar = false;
    int activeZoomPct = 100;
    int lastAbVer = -1;

    float peakHoldL = 0.0f, peakHoldR = 0.0f;
    float dispPeakL = 0.0f, dispPeakR = 0.0f;
    float clipFlash = 0.0f;
    float curLufs = -70.0f;
    float curSubCorr = 1.0f;
    bool curSubActive = false;

    juce::TextButton prevBtn { "<" }, nextBtn { ">" };
    juce::TextButton saveBtn { "SAVE" }, initBtn { "INIT" };
    juce::TextButton abBtnA { "A" }, abBtnB { "B" }, abCopyBtn { "A>B" };
    juce::TextButton undoBtn { "UNDO" }, redoBtn { "REDO" };
    juce::TextButton mpeBtn { "MPE" };
    std::array<juce::TextButton, 4> zoomBtns;
    std::unique_ptr<juce::ButtonParameterAttachment> mpeAttach;
};

//==============================================================================
/** Modulation Matrix Deck (below the active stage):
    6 vertical tabs: LFO 1, LFO 2, LFO 3, EXPR (Vel/MW/AT/Slide/Breath), MACRO (1..8), XY (X/Y routes). */
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
    void select (int page);   // 0..2 LFO, 3 Expr, 4 Macro, 5 XY
    void selectMacro (int m);
    void refresh();

    void showRoute (ModSource, int slot, routing::Result, int targetPid);
    void showMessage (const juce::String&);

    static constexpr int kExprPage = 3, kMacroPage = 4, kXyPage = 5, kPages = 6;

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
        // 0=Vel, 1=MW, 2=AT, 3=Slide(CC74), 4=Breath(CC2)
        std::array<std::array<Selector*, 2>, 5> dest {};
        std::array<std::array<ParamSlider*, 2>, 5> amt {};
        ParamSlider* velAmp = nullptr;
        std::vector<juce::Component*> all;
    };
    struct MacroPage
    {
        std::array<ParamSlider*, kNumMacros> knob {};
        std::array<std::array<Selector*, kMacroSlots>, kNumMacros> dest {};
        std::array<std::array<ParamSlider*, kMacroSlots>, kNumMacros> amt {};
    };
    struct XyPage
    {
        std::array<std::array<Selector*, kXySlots>, 2> dest {};
        std::array<std::array<ParamSlider*, kXySlots>, 2> amt {};
        std::vector<juce::Component*> all;
    };

    void drawAt (juce::Point<float>, bool interpolateFromLast);
    void paintTabs (juce::Graphics&);
    void paintLfo (juce::Graphics&);
    void paintExpr (juce::Graphics&);
    void paintMacros (juce::Graphics&);
    void paintXy (juce::Graphics&);
    void beginRename (int m);
    static int pageAtTab (int tab) noexcept { return tab; }
    static int tabOfPage (int page) noexcept { return page; }
    juce::Rectangle<float> tabRect (int i) const;
    juce::Rectangle<float> macroChip (int m) const;
    juce::Rectangle<float> exprHead (int src) const;
    juce::Rectangle<float> routeRow (int page, int slot) const;
    bool sourceAt (juce::Point<float>, ModSource&) const;
    static void setIdle (ParamSlider*, bool);

    VesselProcessor& proc;
    int current = kMacroPage;
    juce::OwnedArray<juce::Component> owned;
    std::array<LfoPage, kNumLfos> lfo;
    ExprPage expr;
    MacroPage mac;
    XyPage xy;
    int macroSel = 0, namesSeen = -1;
    std::unique_ptr<juce::TextEditor> rename;
    juce::TextButton scatter { "SCATTER" };
    juce::Rectangle<float> canvas, tabs;
    juce::Point<float> lastDraw, pressAt;
    bool drawing = false, dragArmed = false;
    ModSource armed;

    juce::String message;
    double messageUntil = 0.0, flashUntil = 0.0;
    int flashPage = -1, flashSlot = -1;
};

//==============================================================================
/** Searchable, Categorized, Tagged & Favorites-Enabled V2 Preset Browser Overlay. */
class PresetBrowser : public juce::Component
{
public:
    explicit PresetBrowser (VesselProcessor&);
    ~PresetBrowser() override;

    std::function<void()> onClose, onLoaded;

    void open();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    void setSearchQuery (const juce::String& q);
    void setFavoritesOnly (bool fav);

private:
    void rebuildCategories();
    void rebuildItems();
    void load (int combinedIndex, bool close);
    int currentCombinedIndex() const;
    juce::Rectangle<float> starRectForItem (const juce::Rectangle<float>& itemRect) const;

    VesselProcessor& proc;
    juce::TextEditor searchBox;
    juce::TextButton favBtn { "FAVORITES" };
    juce::TextButton clearBtn { "CLEAR" };
    juce::TextButton closeBtn { "CLOSE" };

    juce::StringArray cats;   // index 0 = "All"
    juce::String selectedCat { "All" };
    bool favoritesOnly = false;

    juce::Rectangle<float> panel, listArea, gridArea, infoArea;
    std::vector<juce::Rectangle<float>> catRects, itemRects;
    std::vector<int> items;   // indices into proc.presetManager.allPresets()
    int hoverItem = -1, hoverCat = -1, hoverStar = -1;
    int cols = 3;
    float scrollY = 0.0f, maxScrollY = 0.0f;
};

//==============================================================================
/** Compact brutalist inline overlay for saving a version="2" User Preset. */
class SavePresetOverlay : public juce::Component
{
public:
    explicit SavePresetOverlay (VesselProcessor&);
    std::function<void (bool saved)> onFinished;

    void open();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void commitSave();

    VesselProcessor& proc;
    juce::Rectangle<float> dialog;
    juce::TextEditor nameEd, catEd, tagsEd, descEd;
    juce::TextButton saveBtn { "SAVE PRESET" }, cancelBtn { "CANCEL" };
};

/** Builds stage page 0..4 (Source, Ruin, Shape, Space, Pressure). */
std::unique_ptr<StagePage> buildStage (int index, VesselProcessor&, Registry&);
} // namespace vessel::ui
