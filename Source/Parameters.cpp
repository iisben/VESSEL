#include "Parameters.h"
#include "dsp/Tables.h"

namespace vessel
{
namespace
{
const ParamSpec kSpecs[NUM_PARAMS] =
{
   #define VESSEL_SPEC(id, name, type, mn, mx, df, ctr, stp, unit, ch) \
        { #id, name, PType::type, (float) (mn), (float) (mx), (float) (df), (float) (ctr), (float) (stp), Unit::unit, ch },
    VESSEL_PARAMS (VESSEL_SPEC)
   #undef VESSEL_SPEC
};

juce::StringArray splitChoices (const char* s)
{
    if (juce::String (s) == "@dest")
    {
        juce::StringArray names { "None" };
        for (size_t i = 1; i < modDestinations().size(); ++i)
            names.add (spec (modDestinations()[i]).name);
        return names;
    }
    if (juce::String (s) == "@tables") return dsp::tableMenuNames();
    return juce::StringArray::fromTokens (s, "|", "");
}

const char* kVowels[] = { "A", "E", "I", "O", "U" };
} // namespace

const ParamSpec& spec (int pid) noexcept { return kSpecs[pid]; }

Stage stageOf (int pid) noexcept
{
    if (pid >= v2_tex_mode && pid <= v2_tex_drive) return Stage::Source;
    if (pid >= v2_organic_amt && pid <= v2_organic_mono) return Stage::Voice;
    if (isResonatorParam (pid)) return Stage::Shape;   // appended after the mod block, but lives in SHAPE
    if (pid == wt_table || isFmParam (pid)) return Stage::Source;
    if (pid >= lfo1_shape) return Stage::Mod;   // LFOs and the expression routing
    if (pid >= ott_on)     return Stage::Pressure;
    if (pid >= z_on)       return Stage::Space;
    if (pid >= flt_on)     return Stage::Shape;
    if (pid >= pre_tilt)   return Stage::Ruin;
    if (pid >= voice_mode) return Stage::Voice;
    return Stage::Source;
}

const char* stageName (Stage s) noexcept
{
    switch (s)
    {
        case Stage::Source:   return "Source";
        case Stage::Voice:    return "Voice";
        case Stage::Ruin:     return "Ruin";
        case Stage::Shape:    return "Shape";
        case Stage::Space:    return "Space";
        case Stage::Pressure: return "Pressure";
        case Stage::Mod:      return "Mod";
    }
    return "";
}

const std::vector<int>& modDestinations()
{
    static const std::vector<int> list = []
    {
        std::vector<int> v { -1 };
        for (int i = 0; i < lfo1_shape; ++i)
            if (kSpecs[i].type == PType::Float)
                v.push_back (i);
        // LFO rates and warps are destinations too (cross-modulation); expression routing is not
        for (int i = lfo1_shape; i <= lfo3_amt2; ++i)
            if (kSpecs[i].type == PType::Float && juce::String (kSpecs[i].id).contains ("_amt") == false)
                v.push_back (i);
        // appended later: the resonator. (Saved states store the menu index, so this list only ever grows.)
        for (int i = res_on; i <= res_mix; ++i)
            if (kSpecs[i].type == PType::Float)
                v.push_back (i);
        // then FM
        for (int i = fm_on; i <= fm_key; ++i)
            if (kSpecs[i].type == PType::Float)
                v.push_back (i);
        // then V2 Organic & Procedural Texture parameters (append-only)
        for (int i = v2_organic_amt; i <= v2_tex_drive; ++i)
            if (kSpecs[i].type == PType::Float)
                v.push_back (i);
        return v;
    }();
    return list;
}

int destIndexToPid (int index) noexcept
{
    const auto& d = modDestinations();
    return (index > 0 && index < (int) d.size()) ? d[(size_t) index] : -1;
}

juce::String formatValue (int pid, float v, int)
{
    const auto& s = kSpecs[pid];
    if (s.type == PType::Bool) return v > 0.5f ? "On" : "Off";
    if (s.type == PType::Choice)
    {
        auto c = splitChoices (s.choices);
        return c[juce::jlimit (0, c.size() - 1, juce::roundToInt (v))];
    }

    switch (s.unit)
    {
        case Unit::Pct: return juce::String (juce::roundToInt (v * 100.0f)) + "%";
        case Unit::Bip:
        {
            const int p = juce::roundToInt (v * 100.0f);
            return (p > 0 ? "+" : "") + juce::String (p) + "%";
        }
        case Unit::Hz:
            if (v >= 1000.0f) return juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz";
            if (v < 10.0f)    return juce::String (v, 2) + " Hz";
            if (v < 100.0f)   return juce::String (v, 1) + " Hz";
            return juce::String (juce::roundToInt (v)) + " Hz";
        case Unit::Ms:
            if (v >= 1000.0f) return juce::String (v / 1000.0f, 2) + " s";
            if (v < 10.0f)    return juce::String (v, 1) + " ms";
            return juce::String (juce::roundToInt (v)) + " ms";
        case Unit::Sec:  return juce::String (v, v < 10.0f ? 2 : 1) + " s";
        case Unit::Db:   return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB";
        case Unit::St:
            if (s.step >= 1.0f) return (v > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + " st";
            return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " st";
        case Unit::Ct:   return (v > 0.5f ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct";
        case Unit::X:    return juce::String (v, 2) + "x";
        case Unit::Vowel:
        {
            const int a = juce::jlimit (0, 4, (int) std::floor (v));
            const float f = v - (float) a;
            if (f < 0.12f || a == 4) return kVowels[a];
            if (f > 0.88f)           return kVowels[juce::jmin (4, a + 1)];
            return juce::String (kVowels[a]) + juce::String::fromUTF8 (" \xc2\xb7 ") + kVowels[a + 1];
        }
        case Unit::None:
        default:
            if (pid == crush_bits) return juce::String (v, 1) + " bit";
            return juce::String (v, 2);
    }
}

float parseValue (int pid, const juce::String& textIn)
{
    const auto& s = kSpecs[pid];
    auto t = textIn.trim().toLowerCase();

    if (s.type == PType::Bool)
        return (t == "on" || t == "1" || t == "true") ? 1.0f : 0.0f;
    if (s.type == PType::Choice)
    {
        auto c = splitChoices (s.choices);
        for (int i = 0; i < c.size(); ++i)
            if (c[i].equalsIgnoreCase (t)) return (float) i;
        return (float) t.getIntValue();
    }
    if (s.unit == Unit::Vowel)
    {
        for (int i = 0; i < 5; ++i)
            if (t == juce::String (kVowels[i]).toLowerCase()) return (float) i;
    }

    float v = t.retainCharacters ("0123456789.-+").getFloatValue();
    switch (s.unit)
    {
        case Unit::Pct:
        case Unit::Bip: v /= 100.0f; break;
        case Unit::Hz:  if (t.contains ("k")) v *= 1000.0f; break;
        case Unit::Ms:  if (t.endsWith ("s") && ! t.endsWith ("ms")) v *= 1000.0f; break;
        case Unit::None: case Unit::Sec: case Unit::Db: case Unit::St: case Unit::Ct: case Unit::X: case Unit::Vowel:
        default: break;
    }
    return juce::jlimit (s.min, s.max, v);
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 0; i < NUM_PARAMS; ++i)
    {
        const auto& s = kSpecs[i];
        const juce::ParameterID id { s.id, 1 };

        if (s.type == PType::Bool)
        {
            layout.add (std::make_unique<juce::AudioParameterBool> (id, s.name, s.def > 0.5f));
        }
        else if (s.type == PType::Choice)
        {
            layout.add (std::make_unique<juce::AudioParameterChoice> (id, s.name, splitChoices (s.choices), (int) s.def));
        }
        else
        {
            juce::NormalisableRange<float> range (s.min, s.max, s.step);
            if (s.centre > 0.0f && s.centre > s.min && s.centre < s.max)
                range.setSkewForCentre (s.centre);

            const int pid = i;
            auto attrs = juce::AudioParameterFloatAttributes()
                             .withStringFromValueFunction ([pid] (float v, int n) { return formatValue (pid, v, n); })
                             .withValueFromStringFunction ([pid] (const juce::String& t) { return parseValue (pid, t); });

            layout.add (std::make_unique<juce::AudioParameterFloat> (id, s.name, range, s.def, attrs));
        }
    }
    return layout;
}
} // namespace vessel
