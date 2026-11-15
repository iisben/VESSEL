#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"

/*  VESSEL factory bank.

    Presets ship as one XML document (Presets/FactoryBank.xml, compiled in as binary data). It is a plain
    juce::ValueTree serialisation, so a preset manager can load it with ValueTree::fromXml and browse it as-is:

        <VesselPresetBank version="2" author="iisben">
          <Preset name="Undertow" category="Sub" character="Dark|Mono|Heavy" author="iisben" description="..."
                  macros="Depth|Grit|...">        eight macro captions, in order
            <P id="voice_mode" v="Legato"/>      choices by name
            <P id="flt_a_cut"  v="520"/>         continuous values in plain units (Hz, ms, dB, 0..1)
            <P id="vel_dest1"  v="flt_a_cut"/>   modulation destinations by parameter id
          </Preset>
        </VesselPresetBank>

    Every parameter not listed takes its default, so a preset is exactly its own sound with nothing inherited. */
namespace vessel
{
struct FactoryPreset
{
    juce::String name, category, character, author, description;
    std::vector<std::pair<int, float>> values;   // pid → plain value (choices: index; destinations: menu index)
    juce::StringArray macroNames;                // the eight macro captions (attribute macros="A|B|..."), may be empty
};

class PresetBank
{
public:
    /** The compiled-in factory bank. Parsed once, on first use (the processor constructor). */
    static const PresetBank& factory();

    /** Anything the factory bank's parser rejected (unknown ids, bad values). Empty for a shipping bank. */
    static const juce::StringArray& factoryErrors();

    /** Parses a bank document. Unknown ids or values are reported to `errors` and skipped. */
    static PresetBank fromXml (const juce::XmlElement&, juce::StringArray* errors = nullptr);

    const std::vector<FactoryPreset>& all() const noexcept { return presets; }
    int size() const noexcept { return (int) presets.size(); }
    juce::StringArray categories() const;   // in bank order, unique

    /** The canonical category order of the taxonomy. */
    static const juce::StringArray& categoryOrder();

private:
    std::vector<FactoryPreset> presets;
};

/** Destination menu index for a parameter id (0 = none). */
int destIndexOf (int pid);
} // namespace vessel
