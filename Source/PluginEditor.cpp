#include "PluginEditor.h"

using namespace vessel::ui;

VesselEditor::VesselEditor (VesselProcessor& p)
    : AudioProcessorEditor (p), proc (p)
{
    setLookAndFeel (&laf);
    setOpaque (true);

    header = std::make_unique<Header> (proc);
    spine  = std::make_unique<Spine> (proc, registry);
    for (int i = 0; i < 5; ++i) pages[(size_t) i] = buildStage (i, proc, registry);
    rail = std::make_unique<MasterRail> (proc, registry);
    mod  = std::make_unique<ModPanel> (proc, registry);

    addAndMakeVisible (canvas);
    canvas.setInterceptsMouseClicks (false, true);
    for (juce::Component* c : { (juce::Component*) header.get(), (juce::Component*) spine.get(), (juce::Component*) rail.get(), (juce::Component*) mod.get() })
        canvas.addAndMakeVisible (c);
    for (auto& pg : pages) canvas.addChildComponent (*pg);

    browser = std::make_unique<PresetBrowser> (proc);
    canvas.addChildComponent (*browser);
    browser->onClose  = [this] { showBrowser (false); };
    browser->onLoaded = [this] { header->repaint(); };
    header->onBrowse  = [this] { showBrowser (! browser->isVisible()); };
    header->onTorch   = [this] { setTorch (! proc.torchOn.load()); };

    canvas.addAndMakeVisible (torch);   // always on top of every page, the browser included
    torch.setOn (proc.torchOn.load());

    spine->onSelect = [this] (int s) { showStage (s); };
    showStage (0);

    tooltips = std::make_unique<juce::TooltipWindow> (this, 900);

    gl.setRenderer (&backdrop);
    gl.setComponentPaintingEnabled (true);
    gl.setContinuousRepainting (true);
    gl.attachTo (*this);

    setResizable (true, true);
    setResizeLimits (kWidth * 3 / 4, kHeight * 3 / 4, kWidth * 3 / 2, kHeight * 3 / 2);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) kWidth / (double) kHeight);
    setSize (kWidth, kHeight);

    startTimerHz (30);
}

VesselEditor::~VesselEditor()
{
    stopTimer();
    juce::PopupMenu::dismissAllActiveMenus();
    gl.detach();
    gl.setRenderer (nullptr);
    setLookAndFeel (nullptr);
}

void VesselEditor::showStage (int s)
{
    stage = juce::jlimit (0, 4, s);
    for (int i = 0; i < 5; ++i) pages[(size_t) i]->setVisible (i == stage);
    spine->setSelected (stage);
}

void VesselEditor::resized()
{
    const float scale = (float) getWidth() / (float) kWidth;
    canvas.setBounds (0, 0, kWidth, kHeight);
    canvas.setTransform (juce::AffineTransform::scale (scale));

    header->setBounds (0, 0, kWidth, 60);
    spine->setBounds (0, 60, 200, kHeight - 60);
    for (auto& pg : pages) pg->setBounds (200, 60, 800, 500);
    mod->setBounds (200, 560, 800, kHeight - 560);
    rail->setBounds (1000, 60, kWidth - 1000, kHeight - 60);
    browser->setBounds (0, 60, kWidth, kHeight - 60);
    torch.setBounds (0, 0, kWidth, kHeight);
    torch.setClear (header->lightsArea().translated (header->getX(), header->getY()));
}

void VesselEditor::showBrowser (bool show)
{
    if (show) { browser->setVisible (true); browser->toFront (false); browser->open(); }
    else browser->setVisible (false);
    header->setBrowserOpen (show);
}

void VesselEditor::routeChanged (vessel::ui::ModSource src, int slot, vessel::ui::routing::Result res, int target)
{
    mod->showRoute (src, slot, res, target);
}

void VesselEditor::routesCleared (int target)
{
    mod->showMessage (juce::String (vessel::spec (target).name) + " has no modulation now");
}

void VesselEditor::setTorch (bool on)
{
    proc.torchOn = on;
    torch.setOn (on);
    header->repaint();
}

void VesselEditor::setDropPreview (bool on)
{
    for (auto* s : registry.sliders) s->setDropCandidate (on && vessel::ui::routing::isDestination (s->pid));
}

void VesselEditor::dragOperationStarted (const juce::DragAndDropTarget::SourceDetails& d)
{
    vessel::ui::ModSource src;
    if (! vessel::ui::ModSource::fromDragId (d.description, src)) return;
    setDropPreview (true);
    mod->showMessage ("Drop " + src.displayName (proc) + " on any ringed control");
}

void VesselEditor::dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&)
{
    setDropPreview (false);
}

void VesselEditor::paint (juce::Graphics& g)
{
    // When OpenGL is running, the shader has already drawn the ground; draw nothing so it shows through.
    if (backdrop.isAlive()) return;

    // Software fallback: a static rendition of the ink
    g.fillAll (ink::voidc);
    const auto b = getLocalBounds().toFloat();
    juce::ColourGradient pool (ink::violet.withAlpha (0.10f), b.getWidth() * 0.35f, b.getHeight() * 0.8f,
                               ink::violet.withAlpha (0.0f), b.getWidth() * 0.9f, b.getHeight() * 0.1f, true);
    g.setGradientFill (pool);
    g.fillAll();
}

void VesselEditor::timerCallback()
{
    const auto& t = proc.telemetry;
    backdrop.setEnergy (t.rms.load());

    const bool alive = backdrop.isAlive();
    if (alive != glWasAlive) { glWasAlive = alive; setOpaque (! alive); repaint(); }

    for (auto* s : registry.sliders)
        s->setModulation (t.modOffset[(size_t) s->pid].load (std::memory_order_relaxed));

    header->repaint (header->getWidth() - 320, 0, 320, header->getHeight());
    spine->repaint (0, 0, spine->getWidth(), Spine::kTop + 5 * Spine::kStageH);
    pages[(size_t) stage]->refresh();
    rail->refresh();
    mod->refresh();
}
