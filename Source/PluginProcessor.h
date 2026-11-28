#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "MidiLearn.h"
#include "Parameters.h"
#include "Presets.h"
#include "dsp/Common.h"
#include "dsp/Voice.h"
#include "dsp/Sources.h"
#include "dsp/Ruin.h"
#include "dsp/Shape.h"
#include "dsp/Space.h"
#include "dsp/Pressure.h"
#include "dsp/Modulation.h"
#include "dsp/Organic.h"
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
    std::atomic<float> modWheel { 0 }, aftertouch { 0 }, velocity { 0 }, breath { 0 }, slide { 0 };   // live expression sources
    std::atomic<float> xyX { 0.5f }, xyY { 0.5f };
    std::atomic<bool>  mpeActive { false };
    std::atomic<bool>  grainLooping { false };
    // Lock-free backdrop reactivity telemetry (audio thread -> OpenGL backdrop)
    std::atomic<uint32_t> keyPulseSeq { 0 };
    std::atomic<float> keyPulseVel { 0.0f }, keyNoteNorm { 0.5f };
    std::atomic<float> keyHold { 0.0f }, polyTouch { 0.0f };
    std::atomic<int>   heldCount { 0 };
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
#if VESSEL_HEADLESS
    bool hasEditor() const override { return false; }
#else
    bool hasEditor() const override { return true; }
#endif

    /** Read-only count of currently active synth voices (used by headless QA). */
    int activeVoiceCount() const noexcept
    {
        int count = 0;
        for (int v = 0; v < vessel::dsp::VoiceManager::kVoices; ++v)
            if (voices.voiceActive (v)) ++count;
        return count;
    }

    /** Resettable deterministic PRNG seed & sample-grid reset for headless QA invariance verification. */
    void setQaDeterministicSeed (uint32_t seed) noexcept;

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

    // ── editor-facing API (undoManager declared before apvts for destruction order)
    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;
    Telemetry telemetry;
    vessel::MidiLearnMap midiLearn;
    vessel::PresetManager presetManager;
    std::array<vessel::dsp::WarpedLfo::Table, vessel::kNumLfos> drawTables;

    juce::RangedAudioParameter* param (int pid) const noexcept { return params[(size_t) pid]; }
    void loadUserImpulse (const juce::File&);
    juce::String userImpulseName() const;
    void randomiseDrawTable (int lfo);
    void applyPreset (const vessel::FactoryPreset&);
    vessel::FactoryPreset captureCurrentPreset (const juce::String& name, const juce::String& author = "User",
                                                const juce::String& category = "User", const juce::String& tags = {},
                                                const juce::String& description = {}, bool favorite = false) const;
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
    juce::String heroMacroLabel (int m) const;
    void setMacroName (int m, const juce::String&);
    /** Bumped whenever a name changes, so the editor knows to repaint. */
    std::atomic<int> macroNamesVersion { 0 };

    // ── XY Sculptor metadata & control
    juce::String xyLabelX() const;
    juce::String xyLabelY() const;
    void setXyLabels (const juce::String& xLabel, const juce::String& yLabel);
    void setXyPosition (float x01, float y01);

    // ── Current preset metadata (V2 schema)
    juce::String currentPresetName() const;
    juce::String currentPresetAuthor() const;
    juce::String currentPresetCategory() const;
    juce::String currentPresetTags() const;
    juce::String currentPresetDescription() const;
    bool currentPresetFavorite() const;

    // ── A/B State Compare + Copy A->B (smooth click-free transition)
    int activeABSlot() const noexcept { return activeAB.load (std::memory_order_acquire); }
    void setActiveABSlot (int slot);
    void toggleAB() { setActiveABSlot (activeABSlot() == 0 ? 1 : 0); }
    void copyAToB();
    void copyBToA();
    std::atomic<int> abStateVersion { 0 };

    // ── Undo / Redo (wired to APVTS parameter edits)
    void beginUndoTransaction (const juce::String& actionName = {});
    void flushUndoState();
    bool undo();
    bool redo();
    bool canUndo() const { return undoManager.canUndo(); }
    bool canRedo() const { return undoManager.canRedo(); }
    void clearUndoHistory() { undoManager.clearUndoHistory(); }

    /** The torch UI (the panel in the dark, the mouse a flashlight): saved with the session, off by default. */
    std::atomic<bool> torchOn { false };

private:
    struct ABSnapshot
    {
        std::array<float, vessel::NUM_PARAMS> normValues {};
        std::array<juce::String, vessel::kNumMacros> macroNames {};
        juce::String xyX { "TIMBRE" }, xyY { "SPACE" };
        std::array<std::array<float, vessel::kDrawPoints>, vessel::kNumLfos> drawTables {};
        juce::String presetName, presetAuthor, presetCategory, presetTags, presetDescription;
        bool presetFavorite = false;
        bool valid = false;
    };

    struct MpeChannelState
    {
        float bendRaw = 0.0f, bendS = 0.0f;
        float slideRaw = 0.0f, slideS = 0.0f;
        float pressRaw = 0.0f, pressS = 0.0f;
    };

    struct VoiceExprState
    {
        int note = -1;
        int channel = 1;
        bool gateHeld = false;
        bool latched = false;
        uint32_t age = 0;
        float polyPressRaw = 0.0f, polyPressS = 0.0f;
        float latchedBend = 0.0f, latchedSlide = 0.0f, latchedPress = 0.0f;
    };

    void timerCallback() override;
    void refreshImpulseResponse (bool forceReload);
    void handleMidi (const juce::MidiMessage&);
    void renderChunk (juce::AudioBuffer<float>& io, int start, int n, bool isBusTick, int busSpan);
    void smoothControls (int n);
    void applyExpression();
    void applyMacros();
    void applyXy();
    void buildVoiceParams();
    void snapToCurrentState();
    void resetDrawTables();
    void resetExpressionState() noexcept;
    int  totalLatency() const;
    ABSnapshot captureSnapshot() const;
    void applySnapshotSmooth (const ABSnapshot& snap);

    /** Runs `process(bool on)` under a click-free bypass crossfade on the main x bus. */
    template <typename Fn> void faded (vessel::dsp::BypassFade&, bool on, int n, Fn&& process);

    /** Modulated plain value: smoothed base (normalised) + LFO/expression offset, mapped through the parameter's range. */
    float pv (int pid) const noexcept
    {
        return paramRanges[(size_t) pid].convertFrom0to1 (juce::jlimit (0.0f, 1.0f, base[(size_t) pid] + mod[(size_t) pid]));
    }
    float pvBus (int pid) const noexcept
    {
        return paramRanges[(size_t) pid].convertFrom0to1 (juce::jlimit (0.0f, 1.0f, busBase[(size_t) pid] + busMod[(size_t) pid]));
    }
    float pvExtra (int pid, float extra) const noexcept
    {
        return paramRanges[(size_t) pid].convertFrom0to1 (juce::jlimit (0.0f, 1.0f, base[(size_t) pid] + mod[(size_t) pid] + extra));
    }
    bool pb (int pid) const noexcept
    {
        if (ccTargetActive[(size_t) pid]) return ccTargetNorm[(size_t) pid] > 0.5f;
        return rawParams[(size_t) pid]->load (std::memory_order_relaxed) > 0.5f;
    }
    int pc (int pid) const noexcept
    {
        if (ccTargetActive[(size_t) pid])
            return juce::roundToInt (paramRanges[(size_t) pid].convertFrom0to1 (ccTargetNorm[(size_t) pid]));
        return juce::roundToInt (rawParams[(size_t) pid]->load (std::memory_order_relaxed));
    }

    static constexpr int kChunk = 64;
    static constexpr int kControlStep = vessel::dsp::VoiceManager::kControlStep;

    std::array<juce::RangedAudioParameter*, vessel::NUM_PARAMS> params {};
    std::array<std::atomic<float>*, vessel::NUM_PARAMS> rawParams {};
    std::array<juce::NormalisableRange<float>, vessel::NUM_PARAMS> paramRanges {};
    std::array<uint32_t, vessel::NUM_PARAMS> lastRawBits {};
    std::array<float, vessel::NUM_PARAMS> targetNorm {};
    std::array<float, vessel::NUM_PARAMS> mod {};
    std::array<float, vessel::NUM_PARAMS> base {};        // smoothed normalised values (audio thread only)
    std::array<float, vessel::NUM_PARAMS> busMod {};      // snapshot at fixed kChunk=64 boundary for odd-block-invariant bus FX
    std::array<float, vessel::NUM_PARAMS> busBase {};
    std::array<bool, vessel::NUM_PARAMS> smoothed {};     // continuous params get the 20 ms smoother
    std::vector<int> floatPids;                           // cached Float parameter indices for zero-virtual smoothing
    std::array<float, vessel::NUM_PARAMS> ccTargetNorm {};
    std::array<bool, vessel::NUM_PARAMS>  ccTargetActive {};
    std::array<std::atomic<float>, vessel::NUM_PARAMS> ccSyncNorm {};
    std::array<std::atomic<bool>, vessel::NUM_PARAMS>  ccSyncDirty {};
    std::atomic<bool> ccAnySyncDirty { false };
    bool baseReady = false;
    bool midiDirty = false;
    bool voiceOsIdle = true;
    bool busVoiceActive = false;
    int busLowestHeld = -1;
    uint64_t samplePos = 0;
    int ctrlRem = 0;
    int busRem = 0;
    int bodyQuietSamples = 0;
    std::atomic<bool> snapPending { true };               // set by prepare / setStateInformation, consumed on audio thread

    // expression (audio thread): raw MIDI values and their smoothed versions (global + MPE + per-voice)
    float mwRaw = 0, atRaw = 0, velRaw = 0, mwS = 0, atS = 0, velS = 0;
    float bendNormRaw = 0, bendNormS = 0;
    float breathRaw = 0, breathS = 0, slideRaw = 0, slideS = 0, activeMaxPressS = 0, keyHoldAccum = 0;
    int mpeMasterChannel = 1;
    bool sustainHeld = false;
    bool polyPressSeen = false;
    std::array<MpeChannelState, 17> mpeCh {};
    std::array<VoiceExprState, vessel::dsp::VoiceManager::kVoices> voiceExpr {};

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
    vessel::dsp::PhaseDisperser disperser;
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
    vessel::dsp::OrganicBus organicBus;
    vessel::dsp::ProceduralTextureBus textureBus;
    vessel::dsp::Svf bodyDc1[2], bodyDc2[2];
    vessel::dsp::OutputStage outStage;
    vessel::dsp::DelayLine subAlign;
    vessel::dsp::Smooth convMixS;
    vessel::dsp::Ramp inLevelR, sendR, cleanR;
    vessel::dsp::BypassFade fGran, fUnmask, fCrush, fDisp, fVhs, fFollow, fFormant, fOtt, fMono, fRes;
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
    juce::String xyNameX { "TIMBRE" }, xyNameY { "SPACE" };
    juce::String metaName { "Init Vessel" }, metaAuthor { "iisben" }, metaCategory { "Init" }, metaTags { "Clean|Static" }, metaDescription;
    bool metaFavorite = false;
    juce::CriticalSection macroNameLock;
    std::atomic<int> activeAB { 0 };
    std::array<ABSnapshot, 2> abSlots {};
    float scopeDecim = 0.0f;
    float peakHoldL = 0.0f, peakHoldR = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VesselProcessor)
};
