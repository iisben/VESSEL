#include "Presets.h"
#include "dsp/Tables.h"
#include "BinaryData.h"

namespace vessel
{
namespace
{
int pidForId (const juce::String& id)
{
    for (int i = 0; i < NUM_PARAMS; ++i)
        if (id == spec (i).id) return i;
    return -1;
}

bool parseValue (int pid, const juce::String& text, float& out)
{
    const auto& s = spec (pid);
    const auto t = text.trim();

    if (s.type == PType::Bool)
    {
        if (t.equalsIgnoreCase ("on") || t == "1" || t.equalsIgnoreCase ("true"))  { out = 1.0f; return true; }
        if (t.equalsIgnoreCase ("off") || t == "0" || t.equalsIgnoreCase ("false")) { out = 0.0f; return true; }
        return false;
    }
    if (s.type == PType::Choice)
    {
        if (juce::String (s.choices) == "@dest")
        {
            if (t.isEmpty() || t.equalsIgnoreCase ("none")) { out = 0.0f; return true; }
            const int target = pidForId (t);
            const int idx = target >= 0 ? destIndexOf (target) : 0;
            if (idx <= 0) return false;
            out = (float) idx;
            return true;
        }
        // wavetables by "Category - Name" (or the built-in "Vessel")
        const auto names = juce::String (s.choices) == "@tables" ? dsp::tableMenuNames()
                                                                 : juce::StringArray::fromTokens (s.choices, "|", "");
        for (int i = 0; i < names.size(); ++i)
            if (names[i].equalsIgnoreCase (t)) { out = (float) i; return true; }
        if (t.containsOnly ("0123456789")) { out = (float) juce::jlimit (0, names.size() - 1, t.getIntValue()); return true; }
        return false;
    }
    if (! t.containsOnly ("0123456789.-+eE")) return false;
    out = juce::jlimit (s.min, s.max, t.getFloatValue());
    return true;
}
} // namespace

int destIndexOf (int pid)
{
    const auto& d = modDestinations();
    for (size_t i = 1; i < d.size(); ++i) if (d[i] == pid) return (int) i;
    return 0;
}

const juce::StringArray& PresetBank::categoryOrder()
{
    static const juce::StringArray order { "Init", "Sub", "Bass", "Pad", "Drone", "Lead", "Keys", "Pluck", "Texture", "FX", "Cinematic" };
    return order;
}

PresetBank PresetBank::fromXml (const juce::XmlElement& root, juce::StringArray* errors)
{
    PresetBank bank;
    for (auto* e : root.getChildWithTagNameIterator ("Preset"))
    {
        FactoryPreset p;
        p.name = e->getStringAttribute ("name");
        p.category = e->getStringAttribute ("category");
        p.character = e->getStringAttribute ("character");
        p.author = e->getStringAttribute ("author", root.getStringAttribute ("author"));
        p.description = e->getStringAttribute ("description");
        p.macroNames = juce::StringArray::fromTokens (e->getStringAttribute ("macros"), "|", "");
        p.macroNames.trim();

        for (auto* v : e->getChildWithTagNameIterator ("P"))
        {
            const auto id = v->getStringAttribute ("id");
            const int pid = pidForId (id);
            float value = 0.0f;
            if (pid < 0 || ! parseValue (pid, v->getStringAttribute ("v"), value))
            {
                if (errors) errors->add (p.name + ": " + id + " = " + v->getStringAttribute ("v"));
                continue;
            }
            p.values.emplace_back (pid, value);
        }
        bank.presets.push_back (std::move (p));
    }
    return bank;
}

namespace
{
struct LoadedBank { PresetBank bank; juce::StringArray errors; };

const LoadedBank& loadedFactory()
{
    static const LoadedBank loaded = []
    {
        LoadedBank l;
        auto xml = juce::XmlDocument::parse (juce::String::fromUTF8 (BinaryData::FactoryBank_xml, BinaryData::FactoryBank_xmlSize));
        if (xml == nullptr) l.errors.add ("FactoryBank.xml does not parse");
        else l.bank = PresetBank::fromXml (*xml, &l.errors);
        jassert (l.errors.isEmpty());
        return l;
    }();
    return loaded;
}
} // namespace

const PresetBank& PresetBank::factory()
{
    static const PresetBank bank = []
    {
        auto b = loadedFactory().bank;
        if (b.presets.empty()) b.presets.push_back ({ "Init Vessel", "Init", "Neutral", "iisben", {}, {}, {} });
        return b;
    }();
    return bank;
}

const juce::StringArray& PresetBank::factoryErrors() { return loadedFactory().errors; }

juce::StringArray PresetBank::categories() const
{
    juce::StringArray c;
    for (auto& p : presets) c.addIfNotAlreadyThere (p.category);
    return c;
}
} // namespace vessel
