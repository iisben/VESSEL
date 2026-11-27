// VESSEL II — Headless 1x & 2x Retina UI Snapshot QA Runner (Stage 5).
// Renders offscreen PNG snapshots of the live Editor without opening a window or audio device:
//   (a) Main View (1x + 2x Retina) with live rendered audio populating Dual Scope/Spectrum & Meters
//   (b) Preset Browser Overlay Open (1x + 2x Retina)
//   (c) Macro / Modulation Active State (1x + 2x Retina) with active Hero Macros, mod arcs, XY trail & MPE
//   (d) Additional stage views (Ruin, Shape, Space, Pressure, Save Dialog) at 2x for full visual audit.

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "dsp/Tables.h"
#include <iostream>
#include <vector>

namespace
{
void savePng (const juce::Image& img, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    if (auto os = file.createOutputStream())
        juce::PNGImageFormat().writeImageToStream (img, *os);
}

void pumpLiveAudio (VesselProcessor& proc, int blocks = 48)
{
    constexpr int kBlock = 256;
    juce::AudioBuffer<float> buf (2, kBlock);
    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0)
        {
            midi.addEvent (juce::MidiMessage::noteOn (2, 36, (uint8_t) 110), 0);   // C2 sub anchor
            midi.addEvent (juce::MidiMessage::noteOn (3, 48, (uint8_t) 104), 0);   // C3
            midi.addEvent (juce::MidiMessage::noteOn (4, 55, (uint8_t) 98),  0);   // G3
            midi.addEvent (juce::MidiMessage::noteOn (5, 63, (uint8_t) 94),  0);   // Eb4
            midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, 78), 0);      // Mod wheel
            midi.addEvent (juce::MidiMessage::channelPressureChange (3, 90), 0);   // MPE pressure
            midi.addEvent (juce::MidiMessage::controllerEvent (3, 74, 96), 0);     // MPE slide
        }
        buf.clear();
        proc.processBlock (buf, midi);
    }
}

void setPlain (VesselProcessor& proc, int pid, float v)
{
    auto* r = proc.param (pid);
    r->setValueNotifyingHost (r->convertTo0to1 (v));
}

void renderSnapPair (VesselEditor& ed, const juce::File& outDir, const juce::String& stem, bool include1x = true)
{
    ed.refreshVisualsNow();
    if (include1x)
    {
        auto img1x = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
        savePng (img1x, outDir.getChildFile (stem + "_1x.png"));
        std::cout << "  wrote " << stem << "_1x.png (" << img1x.getWidth() << "x" << img1x.getHeight() << ")\n";
    }
    auto img2x = ed.createComponentSnapshot (ed.getLocalBounds(), true, 2.0f);
    savePng (img2x, outDir.getChildFile (stem + "_2x.png"));
    std::cout << "  wrote " << stem << "_2x.png (" << img2x.getWidth() << "x" << img2x.getHeight() << ")\n";
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    vessel::dsp::setTablesFolder (juce::File (VESSEL_TABLES_DIR));

    const juce::File outDir (argc > 1 ? juce::String (argv[1])
                                      : juce::File::getCurrentWorkingDirectory().getChildFile ("build/vessel2_qa/ui_snapshots").getFullPathName());
    outDir.createDirectory();
    std::cout << "VESSEL II Headless UI Snapshot QA -> " << outDir.getFullPathName() << "\n";

    VesselProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    proc.setCurrentProgram (1);   // Rich dark patch
    setPlain (proc, vessel::v2_organic_amt, 0.32f);
    setPlain (proc, vessel::v2_organic_floor, 0.14f);
    setPlain (proc, vessel::v2_organic_mono, 1.0f);
    setPlain (proc, vessel::v2_tex_mode, 0.0f);
    setPlain (proc, vessel::v2_tex_mix, 0.24f);
    setPlain (proc, vessel::fm_on, 1.0f);
    setPlain (proc, vessel::fm_index, 0.34f);

    std::unique_ptr<juce::AudioProcessorEditor> baseEd (proc.createEditor());
    auto* ed = dynamic_cast<VesselEditor*> (baseEd.get());
    if (ed == nullptr)
    {
        std::cerr << "ERROR: VesselEditor creation failed\n";
        return 1;
    }

    // Force exact 100% logical size (1280 x 800) without touching user prefs on disk
    ed->setZoomScale (1.0f, false);
    ed->setTorch (false);

    // Populate live stereo audio telemetry, FFT spectrum, LUFS meter & sub-120 correlation
    pumpLiveAudio (proc, 64);

    // (a) Main View (1x + 2x Retina)
    ed->showStage (0);
    ed->selectModPage (0);
    ed->xyWidget().clearTrail();
    for (int k = 0; k < 12; ++k)
    {
        const float t = (float) k / 11.0f;
        ed->xyWidget().pushTrailPoint (0.32f + 0.30f * t, 0.34f + 0.32f * std::sin (t * 1.4f));
    }
    setPlain (proc, vessel::v2_xy_x, 0.62f);
    setPlain (proc, vessel::v2_xy_y, 0.66f);
    for (int r = 0; r < 6; ++r) ed->refreshVisualsNow();
    renderSnapPair (*ed, outDir, "01_main", true);

    // (b) Preset Browser Overlay Open (1x + 2x Retina)
    ed->showBrowser (true);
    renderSnapPair (*ed, outDir, "02_browser", true);
    ed->showBrowser (false);

    // (c) Macro / Modulation Active State (1x + 2x Retina)
    setPlain (proc, vessel::v2_mpe_on, 1.0f);
    setPlain (proc, vessel::macro1, 0.68f);
    setPlain (proc, vessel::macro2, 0.44f);
    setPlain (proc, vessel::macro3, 0.78f);
    setPlain (proc, vessel::macro4, 0.55f);
    setPlain (proc, vessel::res_on, 1.0f);
    setPlain (proc, vessel::res_model, 7.0f);
    ed->xyWidget().clearTrail();
    for (int k = 0; k < 18; ++k)
    {
        const float t = (float) k / 17.0f;
        ed->xyWidget().pushTrailPoint (0.22f + 0.52f * t, 0.22f + 0.56f * std::sin (t * 1.57f));
    }
    setPlain (proc, vessel::v2_xy_x, 0.74f);
    setPlain (proc, vessel::v2_xy_y, 0.78f);
    pumpLiveAudio (proc, 40);
    for (int r = 0; r < 6; ++r) ed->refreshVisualsNow();
    ed->showStage (2);       // Shape stage: Dual ZDF Ladders + Resonator modal ruler + active mod arcs
    ed->selectModPage (4);   // Macro Matrix tab
    renderSnapPair (*ed, outDir, "03_macro_mod", true);

    // (d) Additional stage audit renders (2x Retina) for complete visual verification
    ed->showStage (1);
    ed->selectModPage (3);   // 5-source Expression & MPE routing tab
    renderSnapPair (*ed, outDir, "04_stage_ruin_expr", false);

    ed->showStage (3);
    ed->selectModPage (5);   // XY Sculptor routing tab
    renderSnapPair (*ed, outDir, "05_stage_space_xy", false);

    ed->showStage (4);
    ed->selectModPage (0);
    renderSnapPair (*ed, outDir, "06_stage_pressure", false);

    ed->showSaveDialog (true);
    renderSnapPair (*ed, outDir, "07_save_dialog", false);
    ed->showSaveDialog (false);

    // Measure native 60 Hz paint cost (verify fast cached redraw)
    {
        const int n = 30;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < n; ++i)
        {
            ed->refreshVisualsNow();
            (void) ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        }
        const double ms = (juce::Time::getMillisecondCounterHiRes() - t0) / (double) n;
        std::cout << "  full 1280x800 software snapshot avg: " << juce::String (ms, 2) << " ms\n";
    }

    baseEd.reset();
    std::cout << "UI_SNAPSHOTS_OK\n";
    return 0;
}
