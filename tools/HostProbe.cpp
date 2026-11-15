// Loads a built VST3 bundle the way a host would, and exercises it:
// scan, instantiate, bus layout, params, state round-trip, render with MIDI, editor open/close.
//   HostProbe <path/to/Plugin.vst3>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>
#include <iostream>

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (! ok) ++failures;
}

float renderPeak (juce::AudioPluginInstance& p, double sr, int block, int seconds, bool& finite)
{
    juce::AudioBuffer<float> buf (juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), block);
    juce::MidiBuffer midi;
    float peak = 0.0f;
    finite = true;
    const int blocks = (int) (sr * seconds) / block;
    for (int b = 0; b < blocks; ++b)
    {
        buf.clear();
        midi.clear();
        if (b == 0)          midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
        if (b == 1)          midi.addEvent (juce::MidiMessage::noteOn (1, 43, (juce::uint8) 90), 7);
        if (b == blocks / 2) { midi.addEvent (juce::MidiMessage::noteOff (1, 36), 0); midi.addEvent (juce::MidiMessage::noteOff (1, 43), 0); }
        p.processBlock (buf, midi);
        for (int c = 0; c < p.getTotalNumOutputChannels(); ++c)
            for (int i = 0; i < block; ++i)
            {
                const float s = buf.getSample (c, i);
                if (! std::isfinite (s)) finite = false;
                peak = juce::jmax (peak, std::abs (s));
            }
    }
    return peak;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: HostProbe <Plugin.vst3>\n"; return 2; }
    juce::ScopedJuceInitialiser_GUI gui;

    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> found;
    fmt.findAllTypesForFile (found, argv[1]);
    check (found.size() == 1, "scan found " + juce::String (found.size()) + " plug-in(s)");
    if (found.isEmpty()) return 1;

    const auto& d = *found[0];
    std::cout << "  " << d.name << " by " << d.manufacturerName << ", " << d.category
              << (d.isInstrument ? ", instrument" : ", effect") << ", " << d.numInputChannels << " in / " << d.numOutputChannels << " out\n";

    juce::String err;
    auto inst = fmt.createInstanceFromDescription (d, 48000.0, 512, err);
    check (inst != nullptr, "instantiate" + (err.isEmpty() ? juce::String() : ": " + err));
    if (inst == nullptr) return 1;

    check (inst->getTotalNumOutputChannels() == 2, "stereo output");
    check (inst->acceptsMidi(), "accepts MIDI");
    const int nParams = inst->getParameters().size();
    check (nParams > 100, juce::String (nParams) + " parameters exposed");

    inst->setPlayConfigDetails (inst->getTotalNumInputChannels(), 2, 48000.0, 512);
    inst->prepareToPlay (48000.0, 512);
    std::cout << "  latency " << inst->getLatencySamples() << " samples\n";

    bool finite = false;
    const float peak = renderPeak (*inst, 48000.0, 512, 4, finite);
    check (finite, "render finite at 48 kHz / 512");
    check (peak > 1.0e-3f && peak <= 1.0f, "render peak " + juce::String (juce::Decibels::gainToDecibels (peak), 1) + " dBFS");
    inst->releaseResources();

    // odd block size + high rate
    inst->prepareToPlay (96000.0, 137);
    const float peak2 = renderPeak (*inst, 96000.0, 137, 2, finite);
    check (finite && peak2 <= 1.0f, "render finite at 96 kHz / 137");
    inst->releaseResources();

    // state round trip: change a param, save, reset, restore
    auto* p = inst->getParameters()[5];
    const float orig = p->getValue();
    p->setValueNotifyingHost (orig < 0.5f ? 0.8f : 0.2f);
    const float changed = p->getValue();
    juce::MemoryBlock state;
    inst->getStateInformation (state);
    p->setValueNotifyingHost (orig);
    inst->setStateInformation (state.getData(), (int) state.getSize());
    check (std::abs (p->getValue() - changed) < 1.0e-3f, "state round-trip (" + p->getName (32) + ")");

    // programs
    check (inst->getNumPrograms() >= 6, juce::String (inst->getNumPrograms()) + " factory programs");
    for (int i = 0; i < inst->getNumPrograms(); ++i) inst->setCurrentProgram (i);
    inst->setCurrentProgram (0);

    // optional: HostProbe <plugin> "<program name>" prints a render signature for that program, e.g. to A/B a bundle
    // with and without its Resources/Wavetables folder (identical signatures = the table never loaded)
    if (argc > 2)
    {
        int index = -1;
        for (int i = 0; i < inst->getNumPrograms(); ++i) if (inst->getProgramName (i) == juce::String (argv[2])) index = i;
        check (index >= 0, "program \"" + juce::String (argv[2]) + "\" found");
        if (index >= 0)
        {
            inst->setCurrentProgram (index);
            inst->prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> buf (2, 512);
            double energy = 0.0;
            for (int b = 0; b < 188; ++b)
            {
                juce::MidiBuffer midi;
                buf.clear();
                if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
                if (b == 120) midi.addEvent (juce::MidiMessage::noteOff (1, 48), 0);
                inst->processBlock (buf, midi);
                for (int i = 0; i < 512; ++i) energy += (double) buf.getSample (0, i) * buf.getSample (0, i);
            }
            std::cout << "  signature of \"" << argv[2] << "\": " << juce::String (10.0 * std::log10 (energy + 1e-30), 6) << " dB\n";
        }
    }

    // editor + rapid open/close while audio renders + multi-instance + torture edge cases
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
        check (ed != nullptr, "editor created" + (ed ? " (" + juce::String (ed->getWidth()) + "x" + juce::String (ed->getHeight()) + ")" : juce::String()));
        if (ed != nullptr)
        {
            ed->setSize (ed->getWidth() * 3 / 4, ed->getHeight() * 3 / 4);
            auto snap = ed->createComponentSnapshot (ed->getLocalBounds());
            check (snap.isValid(), "editor paints");
            inst->editorBeingDeleted (ed.get());
        }
    }

    // 1) Corrupted / empty / legacy state resilience + post-prepare snap
    {
        inst->prepareToPlay (48000.0, 256);
        inst->setStateInformation (nullptr, 0);
        const char garbage[] = "\x00\xff<not-xml>corrupted";
        inst->setStateInformation (garbage, (int) sizeof (garbage));
        juce::XmlElement wrongRoot ("OTHER_PLUGIN");
        juce::MemoryBlock wrongBlob;
        juce::AudioProcessor::copyXmlToBinary (wrongRoot, wrongBlob);
        inst->setStateInformation (wrongBlob.getData(), (int) wrongBlob.getSize());
        check (true, "corrupted and empty state blobs rejected safely");
        inst->releaseResources();
    }

    // 2) Low sample-rate (22.05 kHz) + 44.1 kHz + 96 kHz + extreme 0%/100% parameters + NaN input sanitization
    {
        // Load a heavy preset with OTT, MonoCollapse, BlackHole, Resonator, FM, etc., then push extreme corners
        if (inst->getNumPrograms() > 2) inst->setCurrentProgram (2);
        for (auto* param : inst->getParameters()) param->setValueNotifyingHost (1.0f);
        inst->prepareToPlay (22050.0, 128);
        bool fin22 = false;
        renderPeak (*inst, 22050.0, 128, 1, fin22);
        check (fin22, "render finite at 22.05 kHz with all parameters at 100%");
        for (auto* param : inst->getParameters()) param->setValueNotifyingHost (0.0f);
        bool fin0 = false;
        renderPeak (*inst, 22050.0, 128, 1, fin0);
        check (fin0, "render finite at 22.05 kHz with all parameters at 0%");
        inst->releaseResources();

        inst->setCurrentProgram (1 % juce::jmax (1, inst->getNumPrograms()));
        inst->prepareToPlay (44100.0, 256);
        juce::AudioBuffer<float> nanBuf (2, 256);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i)
                nanBuf.setSample (c, i, (i % 2 == 0) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity());
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 127, (juce::uint8) 127), 0);
        inst->processBlock (nanBuf, midi);
        bool nanClean = true;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i)
                if (! std::isfinite (nanBuf.getSample (c, i))) nanClean = false;
        check (nanClean, "NaN/Inf upstream audio input sanitized at 44.1 kHz");
        inst->releaseResources();
    }

    // 3) Variable & 1-sample / 0-sample buffer torture (FL Studio style) + rapid editor open/close + 2nd instance
    {
        auto inst2 = fmt.createInstanceFromDescription (d, 48000.0, 64, err);
        check (inst2 != nullptr, "second concurrent instance created");
        inst->setCurrentProgram (0);
        inst->prepareToPlay (48000.0, 512);
        if (inst2 != nullptr) { inst2->setCurrentProgram (1 % juce::jmax (1, inst2->getNumPrograms())); inst2->prepareToPlay (96000.0, 64); }

        const int sizes[] { 1, 1, 0, 13, 64, 1, 513, 7, 1 };
        bool varFinite = true;
        for (int cycle = 0; cycle < 8; ++cycle)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
            for (int idx = 0; idx < 9; ++idx)
            {
                const int bs = sizes[idx];
                juce::AudioBuffer<float> b1 (2, juce::jmax (1, bs));
                b1.clear();
                juce::MidiBuffer m1;
                if (cycle == 0 && idx == 0) m1.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 110), 0);
                juce::AudioBuffer<float> view (b1.getArrayOfWritePointers(), 2, bs);
                inst->processBlock (view, m1);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < bs; ++i)
                        if (! std::isfinite (view.getSample (c, i))) varFinite = false;
            }
            if (inst2 != nullptr)
            {
                juce::AudioBuffer<float> b2 (2, 64);
                b2.clear();
                juce::MidiBuffer m2;
                if (cycle == 0) m2.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                inst2->processBlock (b2, m2);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 64; ++i)
                        if (! std::isfinite (b2.getSample (c, i))) varFinite = false;
            }
            if (ed != nullptr) inst->editorBeingDeleted (ed.get());
        }
        check (varFinite, "variable 0/1/13/64/513-sample buffers + multi-instance + 8x rapid editor open/close");
        inst->releaseResources();
        if (inst2 != nullptr) inst2->releaseResources();
    }

    inst.reset();
    std::cout << (failures == 0 ? "VST3 PROBE PASSED\n" : "VST3 PROBE FAILED\n");
    return failures == 0 ? 0 : 1;
}
