#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "Parameters.h"
#include "dsp/Common.h"
#include "dsp/Voice.h"
#include "dsp/Sources.h"
#include "dsp/Ruin.h"
#include "dsp/Shape.h"
#include "dsp/Space.h"
#include "dsp/Pressure.h"
#include "dsp/Modulation.h"
#include "dsp/Resonator.h"
#include "dsp/Tables.h"

namespace vessel { struct FactoryPreset; }

/** Lock-free telemetry from the audio thread to the editor. */
struct Telemetry
{
    std::atomic<float> outPeakL { 0 }, outPeakR { 0 }, subLevel { 0 }, duckGain { 1 }, unmaskDb { 0 };
    std::atomic<float> followerEnv { 0 }, tapeSpeed { 1 }, rms { 0 };
    std::atomic<float> ottGain[3] { { 0 }, { 0 }, { 0 } };
    std::atomic<float> lfoValue[vessel::kNumLfos] { { 0 }, { 0 }, { 0 } };
    std::atomic<float> lfoPhase[vessel::kNumLfos] { { 0 }, { 0 }, { 0 } };
    std::atomic<int>   clipCount { 0 };
    std::atomic<float> modWheel { 0 }, aftertouch { 0 }, velocity { 0 };   // live expression sources
    std::atomic<bool>  grainLooping { false };
    std::array<std::atomic<float>, vessel::NUM_PARAMS> modOffset {};   // live normalised modulation per parameter

    // stereo scope ring (written by audio, read by UI). Relaxed atomics: a plain store on arm64/x86, but no data race.
    static constexpr int kScope = 2048;
    std::array<std::atomic<float>, kScope> scopeL {}, scopeR {};
    std::atomic<int> scopeWrite { 0 };
};

class VesselProcessor : public juce::AudioProcessor,
                        private juce::Timer
{
public:
    VesselProcessor();
    ~VesselProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "VESSEL"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentPreset.load (std::memory_order_relaxed); }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ── editor-facing API
    juce::AudioProcessorValueTreeState apvts;
    Telemetry telemetry;
    std::array<vessel::dsp::WarpedLfo::Table, vessel::kNumLfos> drawTables;

    juce::RangedAudioParameter* param (int pid) const noexcept { return params[(size_t) pid]; }
    void loadUserImpulse (const juce::File&);
    juce::String userImpulseName() const;
    void randomiseDrawTable (int lfo);
    void applyPreset (const vessel::FactoryPreset&);
    /** Evaluates an LFO's shape for the UI preview. */
    float previewLfo (int lfo, float phase) const;
    /** The wavetable the voices play now (the built-in one until a selected table has finished loading). */
    const vessel::dsp::WavetableBank& currentTable() const noexcept { return *tables.current(); }
    /** UI-safe ref-counted handle to the active wavetable bank so background eviction cannot free it mid-paint. */
    std::shared_ptr<const vessel::dsp::WavetableBank> currentTableShared() const { return tables.currentShared(); }
    int currentTableIndex() const noexcept { return tables.currentIndex(); }
    /** Shared lifetime token for asynchronous UI callbacks (e.g. native FileChooser). */
    std::shared_ptr<std::atomic<bool>> aliveToken() const noexcept { return aliveFlag; }

    /** Macro names (message thread; saved with the state and set by presets). An empty name shows "Macro n". */
    juce::String macroName (int m) const;
    void setMacroName (int m, const juce::String&);
    /** Bumped whenever a name changes, so the editor knows to repaint. */
    std::atomic<int> macroNamesVersion { 0 };

private:
    void timerCallback() override;
    void refreshImpulseResponse (bool forceReload);
    void handleMidi (const juce::MidiMessage&);
    void renderChunk (juce::AudioBuffer<float>& io, int start, int n);
    void smoothControls (int n);
    void applyExpression();
    void applyMacros();
    void buildVoiceParams();
    void snapToCurrentState();
    void resetDrawTables();
    int  totalLatency() const;

    /** Runs `process(bool on)` under a click-free bypass crossfade on the main x bus. */
    template <typename Fn> void faded (vessel::dsp::BypassFade&, bool on, int n, Fn&& process);

    /** Modulated plain value: smoothed base (normalised) + LFO/expression offset, mapped through the parameter's range. */
    float pv (int pid) const noexcept
    {
        return params[(size_t) pid]->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, base[(size_t) pid] + mod[(size_t) pid]));
    }
    float pvExtra (int pid, float extra) const noexcept
    {
        return params[(size_t) pid]->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, base[(size_t) pid] + mod[(size_t) pid] + extra));
    }
    bool pb (int pid) const noexcept { return params[(size_t) pid]->getValue() > 0.5f; }
    int  pc (int pid) const noexcept { return juce::roundToInt (params[(size_t) pid]->convertFrom0to1 (params[(size_t) pid]->getValue())); }

    static constexpr int kChunk = 64;

    std::array<juce::RangedAudioParameter*, vessel::NUM_PARAMS> params {};
    std::array<float, vessel::NUM_PARAMS> mod {};
    std::array<float, vessel::NUM_PARAMS> base {};        // smoothed normalised values (audio thread only)
    std::array<bool, vessel::NUM_PARAMS> smoothed {};     // continuous params get the 20 ms smoother
    bool baseReady = false;
    std::atomic<bool> snapPending { true };               // set by prepare / setStateInformation, consumed on audio thread

    // expression (audio thread): raw MIDI values and their smoothed versions
    float mwRaw = 0, atRaw = 0, velRaw = 0, mwS = 0, atS = 0, velS = 0, bendRaw = 0, bendS = 0;

    double sr = 48000.0;
    std::atomic<double> srAtomic { 48000.0 };

    // DSP
    vessel::dsp::TableStore tables;        // imported wavetables; declared before the voices that point into it
    int tableAsked = -1;                   // audio thread: the wt_table value last requested
    vessel::dsp::VoiceManager voices;
    vessel::dsp::DroneEngine drone;
    vessel::dsp::NoiseInjector noise;
    vessel::dsp::GranularLooper granular;
    vessel::dsp::Tilt preTilt[2];
    vessel::dsp::RuinCore ruin;
    vessel::dsp::BitCrusher crusher;
    vessel::dsp::VhsTransport vhs;
    vessel::dsp::TapeMotor tape;
    vessel::dsp::FollowerFilter follower;
    vessel::dsp::Unmasker unmask;
    vessel::dsp::FormantFilter formant;
    vessel::dsp::Resonator resonator;
    vessel::dsp::ZAxis zaxis;
    juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 512 } };
    vessel::dsp::BlackHole blackHole;
    vessel::dsp::HaasWidener haas;
    vessel::dsp::MultibandOtt ott;
    vessel::dsp::MonoCollapse mono;
    vessel::dsp::Ducker ducker;
    vessel::dsp::OutputStage outStage;
    vessel::dsp::DelayLine subAlign;
    vessel::dsp::Smooth convMixS;
    vessel::dsp::Ramp inLevelR, sendR, cleanR;
    vessel::dsp::BypassFade fGran, fUnmask, fCrush, fVhs, fFollow, fFormant, fOtt, fMono, fRes;
    int resMonoNote = -1;   // the note the resonator follows in mono / legato voice modes

    // 4x voice bus: rendered oversampled, decimated once for body L/R and sub together
    std::unique_ptr<juce::dsp::Oversampling<float>> voiceOs;
    std::array<vessel::dsp::VoiceParams, vessel::dsp::VoiceManager::kVoices> voiceParams;
    vessel::dsp::VoiceParams vpBase;
    std::array<vessel::dsp::WarpedLfo, vessel::kNumLfos> lfos { vessel::dsp::WarpedLfo (11u), vessel::dsp::WarpedLfo (23u), vessel::dsp::WarpedLfo (37u) };

    // chunk scratch
    std::array<float, kChunk> bodyL {}, bodyR {}, subB {}, texL {}, texR {}, key {}, xL {}, xR {}, wetL {}, wetR {}, dryL {}, dryR {};
    std::array<float, kChunk> zeros {};

    // transport
    double bpm = 120.0, ppqBlock = 0.0;
    bool playing = false;

    // convolution state (non-RT: message timer + prepareToPlay; serialized by irLock, never touched by audio)
    int loadedIr = -1;
    float loadedIrSize = -1.0f;
    double loadedIrRate = 0.0;
    juce::File userIrFile;
    bool userIrDirty = false;
    juce::CriticalSection irLock;

    std::atomic<int> currentPreset { 0 };
    std::shared_ptr<std::atomic<bool>> aliveFlag = std::make_shared<std::atomic<bool>> (true);
    std::array<juce::String, vessel::kNumMacros> macroNames;
    juce::CriticalSection macroNameLock;
    float scopeDecim = 0.0f;
    float peakHoldL = 0.0f, peakHoldR = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VesselProcessor)
};
