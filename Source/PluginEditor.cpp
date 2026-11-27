#include "PluginEditor.h"

using namespace vessel::ui;

namespace
{
constexpr float kMinZoom = 0.75f;
constexpr float kMaxZoom = 1.50f;
} // namespace

//==============================================================================
juce::File VesselEditor::uiPrefsFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("VESSEL_II")
        .getChildFile ("ui_prefs.xml");
}

float VesselEditor::loadPersistedZoom()
{
    const auto f = uiPrefsFile();
    if (! f.existsAsFile()) return 1.0f;
    if (auto xml = juce::XmlDocument::parse (f))
    {
        const float z = (float) xml->getDoubleAttribute ("zoom", 1.0);
        if (std::isfinite (z)) return juce::jlimit (kMinZoom, kMaxZoom, z);
    }
    return 1.0f;
}

void VesselEditor::savePersistedZoom (float scale)
{
    const auto f = uiPrefsFile();
    f.getParentDirectory().createDirectory();
    juce::XmlElement xml ("VesselUiPrefs");
    xml.setAttribute ("version", 2);
    xml.setAttribute ("zoom", (double) juce::jlimit (kMinZoom, kMaxZoom, scale));
    xml.writeTo (f);
}

//==============================================================================
VesselEditor::VesselEditor (VesselProcessor& p)
    : AudioProcessorEditor (p), proc (p)
{
    setLookAndFeel (&laf);
    setOpaque (true);
    setWantsKeyboardFocus (true);

    // Pre-render 2x Retina static anodized chassis + matte grain once at editor construction
    chassisCache = renderChassisCache (kWidth, kHeight, 2);

    header      = std::make_unique<Header> (proc);
    spine       = std::make_unique<Spine> (proc, registry);
    for (int i = 0; i < 5; ++i) pages[(size_t) i] = buildStage (i, proc, registry);
    mod         = std::make_unique<ModPanel> (proc, registry);
    scope       = std::make_unique<ScopeSpectrumWidget> (proc);
    xy          = std::make_unique<XySculptorWidget> (proc);
    masterStrip = std::make_unique<MasterStripWidget> (proc, registry);
    dock        = std::make_unique<PerformanceDock> (proc, registry);
    browser     = std::make_unique<PresetBrowser> (proc);
    saveOverlay = std::make_unique<SavePresetOverlay> (proc);

    addAndMakeVisible (canvas);
    canvas.setInterceptsMouseClicks (false, true);

    for (juce::Component* c : { (juce::Component*) header.get(), (juce::Component*) spine.get(),
                                (juce::Component*) mod.get(),    (juce::Component*) scope.get(),
                                (juce::Component*) xy.get(),     (juce::Component*) masterStrip.get(),
                                (juce::Component*) dock.get() })
        canvas.addAndMakeVisible (c);
    for (auto& pg : pages) canvas.addChildComponent (*pg);

    canvas.addChildComponent (*browser);
    canvas.addChildComponent (*saveOverlay);

    // Wire cross-panel interactions
    spine->onSelect = [this] (int s) { showStage (s); };
    dock->onMacroSelected = [this] (int m) { mod->selectMacro (m); };
    xy->onOpenXyRoutes    = [this] { mod->select (ModPanel::kXyPage); };

    browser->onClose  = [this] { showBrowser (false); };
    browser->onLoaded = [this] { header->repaint(); dock->refresh(); xy->refresh(); };

    saveOverlay->onFinished = [this] (bool saved)
    {
        showSaveDialog (false);
        if (saved) { header->repaint(); browser->open(); browser->setVisible (false); }
    };

    header->onBrowse = [this] { showBrowser (! browser->isVisible()); };
    header->onSave   = [this] { showSaveDialog (true); };
    header->onInit   = [this] { dock->refresh(); xy->refresh(); header->repaint(); };
    header->onTorch  = [this] { setTorch (! proc.torchOn.load()); };
    header->onZoom   = [this] (float scale) { setZoomScale (scale, true); };

    canvas.addAndMakeVisible (torch);
    torch.setOn (proc.torchOn.load());

    showStage (0);

    tooltips = std::make_unique<juce::TooltipWindow> (this, 650);

    setResizable (true, true);
    setResizeLimits (juce::roundToInt ((float) kWidth * kMinZoom), juce::roundToInt ((float) kHeight * kMinZoom),
                     juce::roundToInt ((float) kWidth * kMaxZoom), juce::roundToInt ((float) kHeight * kMaxZoom));
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) kWidth / (double) kHeight);

    currentZoom = loadPersistedZoom();
    setSize (juce::roundToInt ((float) kWidth * currentZoom),
             juce::roundToInt ((float) kHeight * currentZoom));
    header->setZoomPercent (juce::roundToInt (currentZoom * 100.0f));
    initializedSize = true;

    startTimerHz (60);
}

VesselEditor::~VesselEditor()
{
    stopTimer();
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void VesselEditor::setZoomScale (float scale, bool persist)
{
    currentZoom = juce::jlimit (kMinZoom, kMaxZoom, scale);
    header->setZoomPercent (juce::roundToInt (currentZoom * 100.0f));
    setSize (juce::roundToInt ((float) kWidth * currentZoom),
             juce::roundToInt ((float) kHeight * currentZoom));
    if (persist) savePersistedZoom (currentZoom);
}

void VesselEditor::showStage (int s)
{
    stage = juce::jlimit (0, 4, s);
    for (int i = 0; i < 5; ++i) pages[(size_t) i]->setVisible (i == stage);
    spine->setSelected (stage);
}

void VesselEditor::showBrowser (bool show)
{
    if (show)
    {
        saveOverlay->setVisible (false);
        browser->setVisible (true);
        browser->toFront (false);
        browser->open();
    }
    else
    {
        browser->setVisible (false);
    }
    header->setBrowserOpen (show);
}

void VesselEditor::showSaveDialog (bool show)
{
    if (show)
    {
        browser->setVisible (false);
        header->setBrowserOpen (false);
        saveOverlay->setVisible (true);
        saveOverlay->toFront (false);
        saveOverlay->open();
    }
    else
    {
        saveOverlay->setVisible (false);
    }
}

void VesselEditor::resized()
{
    const float scale = (float) getWidth() / (float) kWidth;
    if (initializedSize && std::abs (scale - currentZoom) > 0.005f)
    {
        currentZoom = juce::jlimit (kMinZoom, kMaxZoom, scale);
        header->setZoomPercent (juce::roundToInt (currentZoom * 100.0f));
        savePersistedZoom (currentZoom);
    }

    canvas.setBounds (0, 0, kWidth, kHeight);
    canvas.setTransform (juce::AffineTransform::scale (scale));

    // Strict 8px alignment grid on 1280 x 800 logical canvas:
    // Top Command Bar: y = 0..56 (56px)
    header->setBounds (0, 0, kWidth, 56);

    // Middle Workspace: y = 64..656 (592px)
    spine->setBounds (8, 64, 176, 592);
    for (auto& pg : pages) pg->setBounds (192, 64, 784, 416);
    mod->setBounds (192, 488, 784, 168);

    // Right Hero Centerpiece Column: x = 984..1272 (288px)
    scope->setBounds       (984, 64,  288, 248);
    xy->setBounds          (984, 320, 288, 240);
    masterStrip->setBounds (984, 568, 288, 88);

    // Bottom Performance Dock: y = 664..792 (128px)
    dock->setBounds (8, 664, 1264, 128);

    // Modal overlays cover everything below header
    browser->setBounds     (0, 56, kWidth, kHeight - 56);
    saveOverlay->setBounds (0, 56, kWidth, kHeight - 56);
    torch.setBounds        (0, 0, kWidth, kHeight);
}

bool VesselEditor::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (mods.isCommandDown() && (k.getKeyCode() == 'z' || k.getKeyCode() == 'Z'))
    {
        if (mods.isShiftDown()) proc.redo();
        else                    proc.undo();
        header->repaint();
        return true;
    }
    if (mods.isCommandDown() && (k.getKeyCode() == 'y' || k.getKeyCode() == 'Y'))
    {
        proc.redo();
        header->repaint();
        return true;
    }
    return false;
}

void VesselEditor::routeChanged (ModSource src, int slot, routing::Result res, int target)
{
    mod->showRoute (src, slot, res, target);
}

void VesselEditor::routesCleared (int target)
{
    mod->showMessage (juce::String (vessel::spec (target).name) + " modulation cleared");
}

void VesselEditor::setTorch (bool on)
{
    proc.torchOn = on;
    torch.setOn (on);
}

void VesselEditor::setDropPreview (bool on)
{
    for (auto* s : registry.sliders)
        s->setDropCandidate (on && routing::isDestination (s->pid));
}

void VesselEditor::dragOperationStarted (const juce::DragAndDropTarget::SourceDetails& d)
{
    ModSource src;
    if (! ModSource::fromDragId (d.description, src)) return;
    setDropPreview (true);
    mod->showMessage ("Drop " + src.displayName (proc) + " on any cyan-ringed control");
}

void VesselEditor::dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&)
{
    setDropPreview (false);
}

void VesselEditor::paint (juce::Graphics& g)
{
    // Blit pre-rendered 2x Retina static machined chassis cache (zero per-frame noise/gradient cost)
    g.drawImage (chassisCache, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
}

void VesselEditor::refreshVisualsNow()
{
    const auto& t = proc.telemetry;
    for (auto* s : registry.sliders)
    {
        s->setModulation (t.modOffset[(size_t) s->pid].load (std::memory_order_relaxed));
        s->syncMidiLearnState();
    }

    scope->refresh();
    xy->refresh();
    masterStrip->refresh();
    dock->refresh();
    header->refresh (scope->momentaryLufs(), scope->sub120Correlation(), scope->sub120HasSignal());
    spine->repaint (0, 0, spine->getWidth(), Spine::kTop + 5 * Spine::kStageH);
    pages[(size_t) stage]->refresh();
    mod->refresh();
}

void VesselEditor::timerCallback()
{
    const auto& t = proc.telemetry;
    const bool checkLearn = (++midiLearnTick & 7) == 0;
    for (auto* s : registry.sliders)
    {
        s->setModulation (t.modOffset[(size_t) s->pid].load (std::memory_order_relaxed));
        if (checkLearn) s->syncMidiLearnState();
    }

    scope->refresh();
    xy->refresh();
    masterStrip->refresh();
    dock->refresh();
    header->refresh (scope->momentaryLufs(), scope->sub120Correlation(), scope->sub120HasSignal());
    if (checkLearn)
        spine->repaint (0, 0, spine->getWidth(), Spine::kTop + 5 * Spine::kStageH);
    pages[(size_t) stage]->refresh();
    mod->refresh();
}
