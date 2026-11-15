#include "Controls.h"
#include "../PluginProcessor.h"

namespace vessel::ui
{
//==============================================================================
ParamSlider::ParamSlider (VesselProcessor& p, int id, Kind kind, const juce::String& caption, const juce::String& shortName)
    : pid (id), proc (p), param (*p.param (id))
{
    const auto& s = vessel::spec (pid);
    setName (caption.isNotEmpty() ? caption : juce::String (s.name));
    if (shortName.isNotEmpty()) getProperties().set ("short", shortName);
    getProperties().set ("bipolar", s.min < 0.0f && std::abs (s.min + s.max) < 1.0e-4f);
    getProperties().set ("mod", 0.0f);
    getProperties().set ("drop", 0);
    if (kind == Kind::Bar) getProperties().set ("bar", true);

    setSliderStyle (kind == Kind::Halo ? juce::Slider::RotaryHorizontalVerticalDrag
                  : kind == Kind::Vial ? juce::Slider::LinearVertical
                                       : juce::Slider::LinearHorizontal);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    setMouseDragSensitivity (220);
    setVelocityBasedMode (false);
    if (kind == Kind::Bar) setSliderSnapsToMousePosition (false);   // small target: drag relative, never jump
    setPopupDisplayEnabled (false, false, nullptr);

    const bool dest = routing::isDestination (pid);
    setTooltip (juce::String (s.name) + juce::String::fromUTF8 ("  \xc2\xb7  double-click to type, option-click to reset")
                + (dest ? juce::String::fromUTF8 ("  \xc2\xb7  right-click or drop an LFO here to modulate") : juce::String()));

    attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this, nullptr);
}

ParamSlider::~ParamSlider()
{
    if (entry != nullptr)
    {
        entry->onReturnKey = nullptr;
        entry->onEscapeKey = nullptr;
        entry->onFocusLost = nullptr;
        entry.reset();
    }
}

void ParamSlider::setModulation (float off)
{
    if (std::abs (off - lastMod) < 0.0015f) return;
    lastMod = off;
    getProperties().set ("mod", off);
    repaint();
}

void ParamSlider::setDropState (int s)
{
    if (s == dropState) return;
    dropState = s;
    getProperties().set ("drop", s);
    repaint();
}

void ParamSlider::setDropCandidate (bool c) { setDropState (c ? 1 : 0); }

void ParamSlider::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showRouteMenu(); return; }
    if (e.mods.isAltDown())
    {
        param.beginChangeGesture();
        param.setValueNotifyingHost (param.getDefaultValue());
        param.endChangeGesture();
        return;
    }
    juce::Slider::mouseDown (e);
}

void ParamSlider::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    openEntry();
}

void ParamSlider::openEntry()
{
    entry = std::make_unique<juce::TextEditor>();
    auto& ed = *entry;
    addAndMakeVisible (ed);
    const int h = 19;
    const auto style = getSliderStyle();
    ed.setBounds (style == juce::Slider::LinearVertical   ? getLocalBounds().withHeight (h)
                : style == juce::Slider::LinearHorizontal ? getLocalBounds().withTrimmedLeft (getWidth() / 2)
                                                          : getLocalBounds().withTop (getHeight() - h));
    ed.setFont (type::value (13.0f));
    ed.setJustification (juce::Justification::centred);
    ed.setIndents (2, 2);
    ed.setText (getTextFromValue (getValue()), false);
    ed.selectAll();
    ed.onReturnKey = [this]
    {
        if (entry == nullptr) return;
        const auto text = entry->getText();
        param.beginChangeGesture();
        setValue (getValueFromText (text), juce::sendNotificationSync);
        param.endChangeGesture();
        closeEntry();
    };
    ed.onEscapeKey = [this] { closeEntry(); };
    ed.onFocusLost = [this] { closeEntry(); };
    ed.grabKeyboardFocus();
}

void ParamSlider::closeEntry()
{
    // defer: never delete the editor from inside its own callback
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ParamSlider> (this)]
    {
        if (safe != nullptr) { safe->entry.reset(); safe->repaint(); }
    });
}

//------------------------------------------------------------------------------
// one-click routing

void ParamSlider::route (ModSource src, bool add)
{
    int slot = -1;
    const auto res = add ? routing::add (proc, src, pid, slot) : routing::remove (proc, src, pid);
    if (! add) slot = -1;
    if (auto* l = findParentComponentOfClass<RouteListener>()) l->routeChanged (src, slot, res, pid);
}

void ParamSlider::showRouteMenu()
{
    const bool dest = routing::isDestination (pid);
    const auto sources = routing::allSources();

    juce::PopupMenu m;
    m.setLookAndFeel (&getLookAndFeel());   // a parentless menu would fall back to the default look
    m.addSectionHeader (vessel::spec (pid).name);

    if (dest)
    {
        bool any = false;
        for (size_t i = 0; i < sources.size(); ++i)
        {
            const auto& src = sources[i];
            if (i == 3 || i == 6) m.addSeparator();
            const int slot = routing::slotFor (proc, src, pid);
            juce::String text = src.displayName (proc);
            if (slot >= 0)
            {
                any = true;
                const float amt = proc.param (src.amtPid (slot))->convertFrom0to1 (proc.param (src.amtPid (slot))->getValue());
                text << juce::String::fromUTF8 ("   \xc2\xb7  ") << (amt >= 0.0f ? "+" : "") << juce::roundToInt (amt * 100.0f) << "%"
                     << "  (route " << juce::String::charToString ((juce::juce_wchar) ('A' + slot)) << ")";
            }
            else if (routing::freeSlot (proc, src) < 0) text << juce::String::fromUTF8 ("   \xc2\xb7  routes full");
            m.addItem ((int) i + 1, text, slot >= 0 || routing::freeSlot (proc, src) >= 0, slot >= 0);
        }
        m.addSeparator();
        m.addItem (90, "Clear modulation", any);
    }
    m.addItem (98, juce::String::fromUTF8 ("Type a value\xe2\x80\xa6"));
    m.addItem (99, "Reset to default");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                     [safe = juce::Component::SafePointer<ParamSlider> (this), sources] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& self = *safe;
        if (r >= 1 && r <= (int) sources.size())
        {
            const auto src = sources[(size_t) r - 1];
            self.route (src, routing::slotFor (self.proc, src, self.pid) < 0);
        }
        else if (r == 90)
        {
            for (auto& src : sources) routing::remove (self.proc, src, self.pid);
            if (auto* l = self.findParentComponentOfClass<RouteListener>()) l->routesCleared (self.pid);
        }
        else if (r == 98) self.openEntry();
        else if (r == 99)
        {
            self.param.beginChangeGesture();
            self.param.setValueNotifyingHost (self.param.getDefaultValue());
            self.param.endChangeGesture();
        }
    });
}

bool ParamSlider::isInterestedInDragSource (const SourceDetails& d)
{
    ModSource src;
    return ModSource::fromDragId (d.description, src) && routing::isDestination (pid);
}

void ParamSlider::itemDragEnter (const SourceDetails&) { setDropState (2); }
void ParamSlider::itemDragExit (const SourceDetails&)  { setDropState (1); }

void ParamSlider::itemDropped (const SourceDetails& d)
{
    setDropState (1);
    ModSource src;
    if (ModSource::fromDragId (d.description, src)) route (src, true);
}

//==============================================================================
Rune::Rune (VesselProcessor& p, int id, const juce::String& caption, bool header) : pid (id)
{
    setButtonText (caption.isNotEmpty() ? caption : juce::String (vessel::spec (pid).name));
    getProperties().set ("header", header);
    attachment = std::make_unique<juce::ButtonParameterAttachment> (*p.param (pid), *this, nullptr);
}

//==============================================================================
Selector::Selector (VesselProcessor& p, int id, const juce::String& cap, bool compactIn)
    : pid (id), caption (cap.isNotEmpty() ? cap : juce::String (vessel::spec (id).name)), compact (compactIn)
{
    auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p.param (pid));
    jassert (choice != nullptr);

    const bool isDest = juce::String (vessel::spec (pid).choices) == "@dest";
    if (isDest)
    {
        // item order must equal the choice order (the attachment maps by index); headings mark the stages,
        // and the resonator (appended to the list later) gets a heading of its own
        const auto& dests = vessel::modDestinations();
        box.addItem ("None", 1);
        juce::String lastHeading;
        for (size_t i = 1; i < dests.size(); ++i)
        {
            const juce::String heading = vessel::isResonatorParam (dests[i]) ? juce::String ("Resonator")
                                       : vessel::isFmParam (dests[i])        ? juce::String ("FM")
                                                                             : juce::String (vessel::stageName (vessel::stageOf (dests[i])));
            if (heading != lastHeading) { box.addSectionHeading (heading); lastHeading = heading; }
            box.addItem (vessel::spec (dests[i]).name, (int) i + 1);
        }
    }
    else if (juce::String (vessel::spec (pid).choices) == "@tables")
    {
        // one submenu per category, short names inside; the combo box walks submenus in order, so item index i
        // is still choice i (the tables are grouped by category in the list)
        jassert (choice->choices.size() == vessel::dsp::kNumTables + 1);
        box.addItem ("Vessel", 1);
        auto* root = box.getRootMenu();
        juce::PopupMenu sub;
        juce::String category;
        for (int i = 0; i < vessel::dsp::kNumTables; ++i)
        {
            const auto& t = vessel::dsp::kTables[i];
            if (category != t.category)
            {
                if (sub.getNumItems() > 0) root->addSubMenu (category, sub);
                sub = {};
                category = t.category;
            }
            sub.addItem (i + 2, juce::String::fromUTF8 (t.name));
        }
        if (sub.getNumItems() > 0) root->addSubMenu (category, sub);
        box.setTooltip ("Wavetable: the Echo Sound Works Core Tables (CC0), by category");
    }
    else
    {
        box.addItemList (choice->choices, 1);
    }

    if (compact) box.setTooltip (caption);
    addAndMakeVisible (box);
    attachment = std::make_unique<juce::ComboBoxParameterAttachment> (*choice, box, nullptr);
}

void Selector::paint (juce::Graphics& g)
{
    if (compact) return;
    drawCaps (g, caption, getLocalBounds().toFloat().withHeight (14.0f), 9.5f, isMouseOver (true) ? ink::frost : ink::pewter);
}

void Selector::resized() { box.setBounds (compact ? getLocalBounds() : getLocalBounds().withTrimmedTop (15)); }

void Selector::StepBox::mouseDown (const juce::MouseEvent& e)
{
    const int n = getNumItems();
    if (e.x < 14 && n > 0)          { setSelectedItemIndex ((getSelectedItemIndex() + n - 1) % n); return; }
    if (e.x > getWidth() - 14 && n > 0) { setSelectedItemIndex ((getSelectedItemIndex() + 1) % n); return; }
    juce::ComboBox::mouseDown (e);
}
} // namespace vessel::ui
