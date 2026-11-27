#include "Presets.h"
#include "dsp/Tables.h"
#include "BinaryData.h"

namespace vessel
{
namespace
{
bool parsePresetParamValue (int pid, const juce::String& text, float& out)
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
            if (t.isEmpty() || t.equalsIgnoreCase ("none") || t == "0") { out = 0.0f; return true; }
            const int target = pidForId (t);
            if (target >= 0)
            {
                const int idx = destIndexOf (target);
                if (idx <= 0) return false;
                out = (float) idx;
                return true;
            }
            if (t.containsOnly ("0123456789"))
            {
                const int idx = t.getIntValue();
                if (idx >= 0 && idx < (int) modDestinations().size()) { out = (float) idx; return true; }
            }
            return false;
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

juce::String sanitizeFilename (const juce::String& name)
{
    auto clean = name.trim();
    clean = clean.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-");
    clean = clean.trim();
    return clean.isEmpty() ? juce::String ("Untitled") : clean;
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
    static const juce::StringArray order { "Init", "Sub", "Bass", "Pad", "Drone", "Lead", "Keys", "Pluck", "Texture", "FX", "Cinematic", "Club" };
    return order;
}

juce::String PresetBank::formatParamForXml (int pid, float plainValue)
{
    const auto& s = spec (pid);
    if (s.type == PType::Bool)
        return plainValue > 0.5f ? "On" : "Off";
    if (s.type == PType::Choice)
    {
        if (juce::String (s.choices) == "@dest")
        {
            const int destPid = destIndexToPid (juce::roundToInt (plainValue));
            return destPid >= 0 ? juce::String (spec (destPid).id) : juce::String ("None");
        }
        const auto names = juce::String (s.choices) == "@tables" ? dsp::tableMenuNames()
                                                                 : juce::StringArray::fromTokens (s.choices, "|", "");
        const int idx = juce::jlimit (0, std::max (0, names.size() - 1), juce::roundToInt (plainValue));
        return idx < names.size() ? names[idx] : juce::String (idx);
    }
    return juce::String (plainValue, 6);
}

bool PresetBank::presetFromXml (const juce::XmlElement& element, FactoryPreset& p,
                                const juce::String& defaultAuthor, juce::StringArray* errors)
{
    const bool isPresetTag = element.hasTagName ("Preset") || element.hasTagName ("VesselPreset");
    const bool isLegacyStateTag = element.hasTagName ("VESSEL");
    if (! isPresetTag && ! isLegacyStateTag)
        return false;

    p = FactoryPreset{};
    p.schemaVersion = element.getIntAttribute ("version", isLegacyStateTag ? 1 : 2);
    p.name = element.getStringAttribute ("name", element.getStringAttribute ("presetName", "Untitled"));
    p.category = element.getStringAttribute ("category", element.getStringAttribute ("presetCategory", "User"));
    p.character = element.getStringAttribute ("tags", element.getStringAttribute ("character", element.getStringAttribute ("presetTags")));
    p.author = element.getStringAttribute ("author", element.getStringAttribute ("presetAuthor", defaultAuthor));
    p.description = element.getStringAttribute ("description", element.getStringAttribute ("presetDescription"));
    p.favorite = element.getBoolAttribute ("favorite", element.getBoolAttribute ("presetFavorite", false));
    p.xyLabelX = element.getStringAttribute ("xyX", element.getStringAttribute ("xyLabelX", "TIMBRE"));
    p.xyLabelY = element.getStringAttribute ("xyY", element.getStringAttribute ("xyLabelY", "SPACE"));

    if (element.hasAttribute ("macros"))
    {
        p.macroNames = juce::StringArray::fromTokens (element.getStringAttribute ("macros"), "|", "");
        p.macroNames.trim();
    }
    else if (const auto* macrosEl = element.getChildByName ("MACROS"))
    {
        for (int m = 0; m < kNumMacros; ++m)
            p.macroNames.add (macrosEl->getStringAttribute ("m" + juce::String (m + 1)));
    }

    if (isPresetTag)
    {
        for (auto* v : element.getChildWithTagNameIterator ("P"))
        {
            const auto id = v->getStringAttribute ("id");
            const int pid = pidForId (id);
            float value = 0.0f;
            if (pid < 0 || ! parsePresetParamValue (pid, v->getStringAttribute ("v"), value))
            {
                if (errors) errors->add (p.name + ": " + id + " = " + v->getStringAttribute ("v"));
                continue;
            }
            p.values.emplace_back (pid, value);
        }
    }
    else
    {
        // Legacy V1 <VESSEL> APVTS state XML migration: <PARAM id="..." value="..."/>
        for (auto* v : element.getChildWithTagNameIterator ("PARAM"))
        {
            const auto id = v->getStringAttribute ("id");
            const int pid = pidForId (id);
            if (pid < 0)
                continue;
            const auto& s = spec (pid);
            const float raw = (float) v->getDoubleAttribute ("value", (double) s.def);
            p.values.emplace_back (pid, juce::jlimit (s.min, s.max, raw));
        }
    }

    // All loaded presets normalize to V2 schema (missing V2 params inherit legacy-preserving defaults on apply)
    p.schemaVersion = 2;
    return true;
}

std::unique_ptr<juce::XmlElement> PresetBank::presetToXml (const FactoryPreset& preset)
{
    auto el = std::make_unique<juce::XmlElement> ("VesselPreset");
    el->setAttribute ("version", 2);
    el->setAttribute ("name", preset.name);
    el->setAttribute ("author", preset.author);
    el->setAttribute ("category", preset.category);
    el->setAttribute ("tags", preset.character);
    el->setAttribute ("description", preset.description);
    el->setAttribute ("favorite", preset.favorite ? 1 : 0);
    el->setAttribute ("xyX", preset.xyLabelX.isNotEmpty() ? preset.xyLabelX : "TIMBRE");
    el->setAttribute ("xyY", preset.xyLabelY.isNotEmpty() ? preset.xyLabelY : "SPACE");
    if (! preset.macroNames.isEmpty())
        el->setAttribute ("macros", preset.macroNames.joinIntoString ("|"));

    for (const auto& [pid, val] : preset.values)
    {
        if (pid < 0 || pid >= NUM_PARAMS)
            continue;
        auto* p = el->createNewChildElement ("P");
        p->setAttribute ("id", spec (pid).id);
        p->setAttribute ("v", formatParamForXml (pid, val));
    }
    return el;
}

PresetBank PresetBank::fromXml (const juce::XmlElement& root, juce::StringArray* errors)
{
    PresetBank bank;
    const auto defaultAuthor = root.getStringAttribute ("author");
    for (auto* e : root.getChildIterator())
    {
        if (! e->hasTagName ("Preset") && ! e->hasTagName ("VesselPreset"))
            continue;
        FactoryPreset p;
        if (presetFromXml (*e, p, defaultAuthor, errors))
            bank.presets.push_back (std::move (p));
    }
    return bank;
}

namespace
{
struct LoadedBank { PresetBank bank; PresetBank v2Bank; juce::StringArray errors; };

const LoadedBank& loadedFactory()
{
    static const LoadedBank loaded = []
    {
        LoadedBank l;
        auto xml = juce::XmlDocument::parse (juce::String::fromUTF8 (BinaryData::FactoryBank_xml, BinaryData::FactoryBank_xmlSize));
        if (xml == nullptr) l.errors.add ("FactoryBank.xml does not parse");
        else l.bank = PresetBank::fromXml (*xml, &l.errors);

        auto v2Xml = juce::XmlDocument::parse (juce::String::fromUTF8 (BinaryData::V2FlagshipBank_xml, BinaryData::V2FlagshipBank_xmlSize));
        if (v2Xml == nullptr) l.errors.add ("V2FlagshipBank.xml does not parse");
        else
        {
            l.v2Bank = PresetBank::fromXml (*v2Xml, &l.errors);
            for (const auto& p : l.v2Bank.all())
                l.bank.append (p);
        }
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
        if (b.presets.empty())
        {
            FactoryPreset init;
            init.name = "Init Vessel";
            init.category = "Init";
            init.character = "Neutral";
            init.author = "iisben";
            b.presets.push_back (std::move (init));
        }
        return b;
    }();
    return bank;
}

const PresetBank& PresetBank::v2Flagship() { return loadedFactory().v2Bank; }

const juce::StringArray& PresetBank::factoryErrors() { return loadedFactory().errors; }

juce::StringArray PresetBank::categories() const
{
    juce::StringArray c;
    for (auto& p : presets) c.addIfNotAlreadyThere (p.category);
    return c;
}

//==============================================================================
PresetManager::PresetManager()
{
    rebuildCombined();
}

void PresetManager::setStorageDirectory (const juce::File& rootDir)
{
    customRootDir = rootDir;
    refresh();
}

juce::File PresetManager::userPresetsDirectory() const
{
    const auto root = customRootDir.getFullPathName().isNotEmpty()
                        ? customRootDir
                        : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("VESSEL_II");
    return root.getChildFile ("Presets");
}

juce::File PresetManager::favoritesFile() const
{
    const auto root = customRootDir.getFullPathName().isNotEmpty()
                        ? customRootDir
                        : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("VESSEL_II");
    return root.getChildFile ("favorites.xml");
}

void PresetManager::loadFavoritesFromDisk()
{
    favoriteKeys.clear();
    const auto f = favoritesFile();
    if (! f.existsAsFile())
        return;
    if (auto xml = juce::XmlDocument::parse (f))
    {
        for (auto* child : xml->getChildWithTagNameIterator ("Fav"))
        {
            const auto key = child->getStringAttribute ("key");
            if (key.isNotEmpty())
                favoriteKeys.addIfNotAlreadyThere (key);
        }
    }
}

bool PresetManager::saveFavoritesToDisk() const
{
    const auto f = favoritesFile();
    if (! f.getParentDirectory().createDirectory())
        return false;
    juce::XmlElement root ("VesselFavorites");
    root.setAttribute ("version", 2);
    for (const auto& k : favoriteKeys)
    {
        auto* c = root.createNewChildElement ("Fav");
        c->setAttribute ("key", k);
    }
    return root.writeTo (f);
}

void PresetManager::rebuildCombined()
{
    combined.clear();
    const auto& factory = PresetBank::factory().all();
    combined.reserve (factory.size() + userPresets.size());
    for (auto p : factory)
    {
        p.isUser = false;
        p.favorite = favoriteKeys.contains (p.favoriteKey());
        combined.push_back (std::move (p));
    }
    for (auto p : userPresets)
    {
        p.isUser = true;
        p.favorite = p.favorite || favoriteKeys.contains (p.favoriteKey());
        combined.push_back (std::move (p));
    }
}

void PresetManager::refresh()
{
    loadFavoritesFromDisk();
    userPresets.clear();

    const auto dir = userPresetsDirectory();
    if (dir.isDirectory())
    {
        auto files = dir.findChildFiles (juce::File::findFiles, true, "*.vessel;*.xml");
        files.sort();
        for (const auto& file : files)
        {
            FactoryPreset p;
            if (loadPresetFile (file, p))
            {
                p.isUser = true;
                p.sourceFile = file;
                userPresets.push_back (std::move (p));
            }
        }
    }
    rebuildCombined();
}

std::vector<int> PresetManager::search (const juce::String& query,
                                        const juce::String& categoryFilter,
                                        bool favoritesOnly) const
{
    std::vector<int> matches;
    const auto q = query.trim().toLowerCase();
    const auto cat = categoryFilter.trim();

    for (size_t i = 0; i < combined.size(); ++i)
    {
        const auto& p = combined[i];
        if (favoritesOnly && ! p.favorite)
            continue;
        if (cat.isNotEmpty() && ! cat.equalsIgnoreCase ("All") && ! p.category.equalsIgnoreCase (cat))
            continue;
        if (q.isNotEmpty())
        {
            const bool hit = p.name.toLowerCase().contains (q)
                          || p.author.toLowerCase().contains (q)
                          || p.category.toLowerCase().contains (q)
                          || p.character.toLowerCase().contains (q)
                          || p.description.toLowerCase().contains (q);
            if (! hit)
                continue;
        }
        matches.push_back ((int) i);
    }
    return matches;
}

bool PresetManager::isFavorite (const FactoryPreset& p) const
{
    return favoriteKeys.contains (p.favoriteKey());
}

void PresetManager::setFavorite (const FactoryPreset& p, bool fav)
{
    const auto key = p.favoriteKey();
    if (fav) favoriteKeys.addIfNotAlreadyThere (key);
    else favoriteKeys.removeString (key);
    saveFavoritesToDisk();
    rebuildCombined();
}

bool PresetManager::toggleFavorite (const FactoryPreset& p)
{
    const bool next = ! isFavorite (p);
    setFavorite (p, next);
    return next;
}

bool PresetManager::saveUserPreset (const FactoryPreset& presetIn, juce::File* writtenFile)
{
    auto dir = userPresetsDirectory();
    if (! dir.createDirectory())
        return false;

    FactoryPreset p = presetIn;
    p.schemaVersion = 2;
    p.isUser = true;
    if (p.name.trim().isEmpty())
        p.name = "Untitled";
    if (p.category.trim().isEmpty())
        p.category = "User";

    const auto file = dir.getChildFile (sanitizeFilename (p.name) + ".vessel");
    auto xml = PresetBank::presetToXml (p);
    if (xml == nullptr || ! xml->writeTo (file))
        return false;

    if (p.favorite)
    {
        favoriteKeys.addIfNotAlreadyThere (p.favoriteKey());
        saveFavoritesToDisk();
    }
    if (writtenFile != nullptr)
        *writtenFile = file;
    refresh();
    return true;
}

bool PresetManager::deleteUserPreset (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;
    const bool ok = file.deleteFile();
    refresh();
    return ok;
}

bool PresetManager::loadPresetFile (const juce::File& file, FactoryPreset& out, juce::StringArray* errors)
{
    if (! file.existsAsFile())
        return false;
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr)
    {
        if (errors) errors->add (file.getFileName() + ": invalid XML");
        return false;
    }
    if (! PresetBank::presetFromXml (*xml, out, {}, errors))
        return false;
    out.sourceFile = file;
    return true;
}
} // namespace vessel
