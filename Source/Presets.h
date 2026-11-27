#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"

/*  VESSEL II versioned preset system (schema version="2").

    Factory presets ship as one XML document (Presets/FactoryBank.xml, compiled in as binary data).
    Individual V2 presets (user presets, exports, and Stage 6 flagship presets) use:

        <VesselPreset version="2" name="..." author="..." category="..." tags="Dark|Sub|Mono"
                      description="..." favorite="0" macros="Weight|Fracture|Drift|Abyss"
                      xyX="TIMBRE" xyY="SPACE">
          <P id="flt_a_cut" v="520"/>
          ...
        </VesselPreset>

    Legacy V1 presets (<Preset ...> without version or version="1", or V1 <VESSEL> state XML)
    migrate losslessly into schema version="2": all omitted V2 parameters take their
    legacy-preserving defaults (0% organic/texture, XY center 0.5/0.5, MPE off, ±2st bend, linear vel). */
namespace vessel
{
struct FactoryPreset
{
    juce::String name, category, character, author, description;
    std::vector<std::pair<int, float>> values;   // pid → plain value (choices: index; destinations: menu index)
    juce::StringArray macroNames;                // up to eight macro captions (first 4 are Hero Macros)
    juce::String xyLabelX { "TIMBRE" };
    juce::String xyLabelY { "SPACE" };
    int schemaVersion = 2;
    bool favorite = false;
    bool isUser = false;
    juce::File sourceFile;

    /** Unique key for favorites persistence (distinguishes factory vs user presets). */
    juce::String favoriteKey() const
    {
        return (isUser ? "user:" : "factory:") + category + ":" + name;
    }

    juce::StringArray tagList() const
    {
        auto t = juce::StringArray::fromTokens (character, "|,", "");
        t.trim();
        t.removeEmptyStrings();
        return t;
    }
};

class PresetBank
{
public:
    /** The compiled-in factory bank (V1 factory bank followed by V2 flagship presets). Parsed once on first use. */
    static const PresetBank& factory();

    /** The 20 VESSEL II flagship commercial showcase presets (compiled in via V2FlagshipBank.xml). */
    static const PresetBank& v2Flagship();

    /** Anything the factory bank's parser rejected (unknown ids, bad values). Empty for a shipping bank. */
    static const juce::StringArray& factoryErrors();

    /** Parses a bank document. Unknown ids or values are reported to `errors` and skipped. */
    static PresetBank fromXml (const juce::XmlElement&, juce::StringArray* errors = nullptr);

    /** Parses a single V2 (<VesselPreset version="2">) or legacy V1 (<Preset> / <VESSEL>) XML document,
        losslessly migrating V1 presets to schema version="2". */
    static bool presetFromXml (const juce::XmlElement& element, FactoryPreset& out,
                               const juce::String& defaultAuthor = {}, juce::StringArray* errors = nullptr);

    /** Serialises a preset into a version="2" <VesselPreset> XML element. */
    static std::unique_ptr<juce::XmlElement> presetToXml (const FactoryPreset& preset);

    /** Formats a parameter plain value for XML serialisation (human-readable choices, destinations, numbers). */
    static juce::String formatParamForXml (int pid, float plainValue);

    const std::vector<FactoryPreset>& all() const noexcept { return presets; }
    int size() const noexcept { return (int) presets.size(); }
    void append (const FactoryPreset& p) { presets.push_back (p); }
    juce::StringArray categories() const;   // in bank order, unique

    /** The canonical category order of the taxonomy. */
    static const juce::StringArray& categoryOrder();

private:
    std::vector<FactoryPreset> presets;
};

/** Versioned Preset Manager (V2): unifies compiled-in factory presets, cross-platform user presets
    stored under `<userApplicationDataDirectory>/VESSEL_II/Presets/`, persisted Favorites, and search/tag filtering. */
class PresetManager
{
public:
    PresetManager();

    /** Override storage directory (used by isolated QA tests so user home dir is never touched). */
    void setStorageDirectory (const juce::File& rootDir);

    juce::File userPresetsDirectory() const;
    juce::File favoritesFile() const;

    /** Rescans user presets on disk and reloads favorites (message thread). */
    void refresh();

    /** Combined preset list: factory presets followed by user presets, with current favorite flags applied. */
    const std::vector<FactoryPreset>& allPresets() const noexcept { return combined; }
    int totalCount() const noexcept { return (int) combined.size(); }
    int factoryCount() const noexcept { return (int) PresetBank::factory().size(); }

    /** Search & filter across Name, Author, Category, Tags, and Description (case-insensitive). */
    std::vector<int> search (const juce::String& query,
                             const juce::String& categoryFilter = {},
                             bool favoritesOnly = false) const;

    /** Favorites persistence. */
    bool isFavorite (const FactoryPreset& p) const;
    void setFavorite (const FactoryPreset& p, bool fav);
    bool toggleFavorite (const FactoryPreset& p);

    /** Save a version="2" user preset (.vessel XML) to disk and refresh the catalog. */
    bool saveUserPreset (const FactoryPreset& preset, juce::File* writtenFile = nullptr);

    /** Delete a user preset file from disk and refresh the catalog. */
    bool deleteUserPreset (const juce::File& file);

    /** Load a standalone preset file (V1 or V2 XML) with lossless V1->V2 migration. */
    static bool loadPresetFile (const juce::File& file, FactoryPreset& out, juce::StringArray* errors = nullptr);

private:
    void loadFavoritesFromDisk();
    bool saveFavoritesToDisk() const;
    void rebuildCombined();

    juce::File customRootDir;
    juce::StringArray favoriteKeys;
    std::vector<FactoryPreset> userPresets;
    std::vector<FactoryPreset> combined;
};

/** Destination menu index for a parameter id (0 = none). */
int destIndexOf (int pid);
} // namespace vessel
