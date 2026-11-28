#include "PluginProcessor.h"
#if ! VESSEL_HEADLESS
#include "PluginEditor.h"
#endif
#include "Presets.h"

using namespace vessel;
using namespace vessel::dsp;

namespace
{
double divisionBeats (int idx)
{
    static constexpr double beats[] { 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0, 1.5, 0.75 };
    return beats[juce::jlimit (0, 12, idx)];
}

/** Selectable velocity curve shaping (0=Linear, 1=Soft, 2=Hard, 3=S-Curve, 4=Fixed). */
float shapeVelocity (float v, int curve) noexcept
{
    v = std::clamp (v, 0.0f, 1.0f);
    switch (curve)
    {
        case 1: return std::pow (v, 0.6f);               // Soft
        case 2: return std::pow (v, 1.8f);               // Hard
        case 3: return v * v * (3.0f - 2.0f * v);        // S-Curve
        case 4: return 0.85f;                            // Fixed
        case 0:
        default: return v;
    }
}
} // namespace

//==============================================================================
VesselProcessor::VesselProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undoManager, "VESSEL", createLayout())
{
    // Build the shared wavetable bank here, on the message thread. (It used to be built lazily by the first
    // voice that sounded, i.e. on the audio thread: ~720 kB of allocation and 88 inverse FFTs mid-callback.)
    WavetableBank::get();

    floatPids.reserve (NUM_PARAMS);
    for (int i = 0; i < NUM_PARAMS; ++i)
    {
        params[(size_t) i] = apvts.getParameter (spec (i).id);
        rawParams[(size_t) i] = apvts.getRawParameterValue (spec (i).id);
        jassert (params[(size_t) i] != nullptr && rawParams[(size_t) i] != nullptr);
        paramRanges[(size_t) i] = params[(size_t) i]->getNormalisableRange();
        smoothed[(size_t) i] = spec (i).type == PType::Float && spec (i).step <= 0.0f;
        lastRawBits[(size_t) i] = 0xFFFFFFFFu;
        ccTargetNorm[(size_t) i] = 0.0f;
        ccTargetActive[(size_t) i] = false;
        ccSyncNorm[(size_t) i].store (0.0f, std::memory_order_relaxed);
        ccSyncDirty[(size_t) i].store (false, std::memory_order_relaxed);
        if (spec (i).type == PType::Float) floatPids.push_back (i);
    }

    resetDrawTables();
    PresetBank::factory();   // parse the factory bank on the message thread
    abSlots[0] = captureSnapshot();
    abSlots[1] = abSlots[0];
    undoManager.clearUndoHistory();
#if ! VESSEL_HEADLESS
    midiLearn.loadFromDisk();
    presetManager.refresh();
    startTimerHz (8);
#endif
}

VesselProcessor::~VesselProcessor()
{
    aliveFlag->store (false, std::memory_order_release);
#if ! VESSEL_HEADLESS
    stopTimer();
#endif
}

bool VesselProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    const auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

void VesselProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    if (! (sampleRate > 0.0)) sampleRate = 48000.0;
    sr = sampleRate;
    srAtomic.store (sampleRate, std::memory_order_release);
    const float fs = (float) sampleRate;

    voices.prepare (fs);
    voiceOs = std::make_unique<juce::dsp::Oversampling<float>> (3, (size_t) VoiceManager::kOsLog2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    voiceOs->initProcessing ((size_t) kChunk);
    voiceOs->reset();

    drone.prepare (fs);
    noise.prepare (fs);
    granular.prepare (fs);
    preTilt[0].reset(); preTilt[1].reset();
    ruin.prepare (fs, kChunk);
    crusher.prepare (fs);
    disperser.prepare (sampleRate);
    vhs.prepare (fs);
    tape.prepare (fs);
    follower.prepare (fs);
    unmask.prepare (fs);
    formant.prepare (fs);
    resonator.prepare (fs);
    zaxis.prepare (fs);
    bbdChorus.prepare (sampleRate);
    blackHole.prepare (fs);
    haas.prepare (fs);
    ott.prepare (fs, kChunk);
    mono.prepare (fs, kChunk);
    ducker.prepare (fs);
    organicBus.prepare (fs, kChunk);
    textureBus.prepare (fs);
    for (int c = 0; c < 2; ++c)
    {
        bodyDc1[c].setup (12.0, 0.54119610, sampleRate); bodyDc1[c].reset();
        bodyDc2[c].setup (12.0, 1.30656296, sampleRate); bodyDc2[c].reset();
    }
    outStage.prepare (fs, kChunk);
    subAlign.prepare (256);
    convMixS.prepare (fs, 30.0f); convMixS.snap (0.0f);
    inLevelR.reset(); sendR.reset(); cleanR.reset();
    resMonoNote = -1;
    resetExpressionState();
    scopeDecim = 0.0f; peakHoldL = peakHoldR = 0.0f;
    samplePos = 0; ctrlRem = 0; busRem = 0; bodyQuietSamples = 0; voiceOsIdle = true; busVoiceActive = false; busLowestHeld = -1; midiDirty = false;
    for (auto* f : { &fGran, &fUnmask, &fCrush, &fDisp, &fVhs, &fFollow, &fFormant, &fBbd, &fOtt, &fMono, &fRes }) f->prepare (fs);
    for (auto& l : lfos) l.reset();

    convolution.prepare ({ sampleRate, (juce::uint32) kChunk, 2 });
    convolution.reset();
    refreshImpulseResponse (true);   // serialized under irLock against concurrent message-thread timerCallback()

    tables.loadNow (pc (wt_table));   // the first rendered sample already uses the selected table
    tableAsked = pc (wt_table);
    voices.setTable (tables.current());
    snapToCurrentState();
    setLatencySamples (totalLatency());
    juce::ignoreUnused (samplesPerBlock);
}

int VesselProcessor::totalLatency() const
{
    return (int) std::lround (ruin.latency() + bbdChorus.latency() + outStage.latency());
}

void VesselProcessor::setQaDeterministicSeed (uint32_t seed) noexcept
{
    voices.reseed (seed);
    for (int l = 0; l < kNumLfos; ++l) lfos[(size_t) l].reseed (seed ^ (uint32_t) (11 + l * 13));
    drone.reseed (seed);
    noise.reseed (seed);
    granular.reseed (seed);
    disperser.reset();
    bbdChorus.reset();
    vhs.reseed (seed);
    resonator.reseed (seed);
    organicBus.reseed (seed);
    textureBus.reseed (seed);
    if (voiceOs) voiceOs->reset();
    for (int c = 0; c < 2; ++c) { bodyDc1[c].reset(); bodyDc2[c].reset(); }
    samplePos = 0;
    ctrlRem = 0;
    busRem = 0;
    bodyQuietSamples = 0;
    voiceOsIdle = true;
    busVoiceActive = false;
    busLowestHeld = -1;
    resetExpressionState();
}

void VesselProcessor::resetExpressionState() noexcept
{
    mwRaw = atRaw = velRaw = mwS = atS = velS = 0.0f;
    bendNormRaw = bendNormS = 0.0f;
    breathRaw = breathS = slideRaw = slideS = activeMaxPressS = 0.0f;
    keyHoldAccum = 0.0f;
    sustainHeld = false;
    polyPressSeen = false;
    for (auto& c : mpeCh) c = MpeChannelState {};
    for (auto& ve : voiceExpr) ve = VoiceExprState {};
}

/** Fresh start (prepare, state load): smoothers and bypass fades jump to the current values. */
void VesselProcessor::snapToCurrentState()
{
    snapPending.store (false, std::memory_order_relaxed);
    for (int i = 0; i < NUM_PARAMS; ++i)
    {
        const float raw = rawParams[(size_t) i]->load (std::memory_order_relaxed);
        lastRawBits[(size_t) i] = std::bit_cast<uint32_t> (raw);
        targetNorm[(size_t) i] = base[(size_t) i] = busBase[(size_t) i] = paramRanges[(size_t) i].convertTo0to1 (raw);
        ccTargetActive[(size_t) i] = false;
    }
    busMod.fill (0.0f);
    baseReady = true;
    fGran.snap (pb (gran_on)); fUnmask.snap (pb (unmask_on)); fCrush.snap (pb (crush_on)); fDisp.snap (pc (v2_disp_stages) > 0);
    fVhs.snap (pb (vhs_on));
    disperser.reset();
    fFollow.snap (pb (envf_on)); fFormant.snap (pb (form_on)); fBbd.snap (pc (v2_bbd_mode) > 0);
    bbdChorus.reset();
    fOtt.snap (pb (ott_on)); fMono.snap (pb (mono_on));
    fRes.snap (pb (res_on));
    convMixS.snap (pb (conv_on) ? pv (conv_mix) : 0.0f);
    resonator.snapModel (pc (res_model));
    voices.setMode (pc (voice_mode));   // before any note of the first block (a bounce starting on a note)
    const int lpgM = pc (v2_lpg_mode);
    voices.snapFilter (pb (flt_on) || lpgM == 1 || lpgM == 2);
    const bool tubeActive = pb (tube_on) && (pv (tube_drive) > 1.0e-5f || std::abs (pv (tube_bias)) > 1.0e-5f || std::abs (pv (tube_out)) > 1.0e-4f);
    const bool biasActive = pb (bias_on) && (pv (bias_sat) > 1.0e-5f || std::abs (pv (bias_amount)) > 1.0e-5f || pv (bias_sag) > 1.0e-5f);
    ruin.snap (tubeActive, biasActive);
    outStage.snap (pb (exc_on), pb (clip_on));
}

//==============================================================================
void VesselProcessor::handleMidi (const juce::MidiMessage& m)
{
    TapeMotor::Params tp;
    tp.mode = (TapeMotor::Mode) pc (tape_mode);

    const int vm = pc (voice_mode);   // 0 poly, 1 mono, 2 legato
    // a mode change takes effect before this note, not at the next render chunk: otherwise a note in the same block
    // as a preset change starts as a poly voice and is released silent by the switch
    voices.setMode (vm);

    // the resonator follows the voice in mono / legato: one string, re-tuned (and gliding) with the voice
    auto resFollowMono = [this, vm]
    {
        if (! voices.anyHeld()) { resonator.monoRelease(); resMonoNote = -1; return; }
        const int back = voices.lastHeld();
        if (back >= 0 && back != resMonoNote) { resonator.monoNote (back, velRaw, vm == 1); resMonoNote = back; }
    };

    const int ch = juce::jlimit (1, 16, m.getChannel());
    const bool mpeOn = pb (v2_mpe_on);
    const bool isMpeMember = mpeOn && (ch != mpeMasterChannel);

    if (m.isNoteOn())
    {
        const bool fromSilence = ! voices.anyHeld();
        const int note = m.getNoteNumber();
        const float shapedVel = shapeVelocity (m.getFloatVelocity(), pc (v2_vel_curve));
        velRaw = shapedVel;

        for (int v = 0; v < VoiceManager::kVoices; ++v)
            if (! voices.voiceActive (v)) { voiceExpr[(size_t) v].gateHeld = false; voiceExpr[(size_t) v].note = -1; }

        int targetIdx = 0;
        if (vm == 0)
        {
            targetIdx = -1;
            for (int v = 0; v < VoiceManager::kVoices; ++v)
                if (voices.voiceActive (v) && voiceExpr[(size_t) v].note == note) { targetIdx = v; break; }
            if (targetIdx < 0)
                for (int v = 0; v < VoiceManager::kVoices; ++v)
                    if (! voices.voiceActive (v)) { targetIdx = v; break; }
            if (targetIdx < 0)
            {
                uint32_t best = 0;
                for (int v = 0; v < VoiceManager::kVoices; ++v)
                {
                    const uint32_t score = voiceExpr[(size_t) v].age + (voiceExpr[(size_t) v].gateHeld ? 0u : 100000000u);
                    if (targetIdx < 0 || score > best) { targetIdx = v; best = score; }
                }
            }
        }

        voices.noteOn (note, shapedVel);

        if (targetIdx >= 0 && targetIdx < VoiceManager::kVoices)
        {
            auto& ve = voiceExpr[(size_t) targetIdx];
            ve.note = note;
            ve.channel = ch;
            ve.gateHeld = true;
            ve.latched = false;
            ve.age = 0;
            ve.polyPressRaw = 0.0f;
            ve.polyPressS = 0.0f;
            if (isMpeMember)
            {
                auto& mc = mpeCh[(size_t) ch];
                mc.bendS = mc.bendRaw;
                mc.slideS = mc.slideRaw;
                mc.pressS = mc.pressRaw;
            }
        }

        textureBus.noteOn (note, shapedVel);
        telemetry.keyPulseVel.store (shapedVel, std::memory_order_relaxed);
        telemetry.keyNoteNorm.store (std::clamp ((float) (note - 24) / 72.0f, 0.0f, 1.0f), std::memory_order_relaxed);
        telemetry.keyPulseSeq.fetch_add (1u, std::memory_order_release);
        midiDirty = true;
        if (fromSilence) tape.noteOnFromSilence (tp);
        if (pb (duck_on)) ducker.trigger();
        for (int l = 0; l < kNumLfos; ++l) lfos[(size_t) l].trigger (pc (lfoExt (l)), fromSilence);
        if (vm == 0) resonator.noteOn (note, shapedVel);
        else { resonator.monoNote (note, shapedVel, vm == 1 || fromSilence); resMonoNote = note; }
    }
    else if (m.isNoteOff())
    {
        const int note = m.getNoteNumber();
        if (! sustainHeld)
        {
            for (int v = 0; v < VoiceManager::kVoices; ++v)
            {
                auto& ve = voiceExpr[(size_t) v];
                if (ve.gateHeld && ve.note == note && (! isMpeMember || ve.channel == ch))
                {
                    const auto& mc = mpeCh[(size_t) juce::jlimit (1, 16, ve.channel)];
                    ve.latchedBend = mc.bendS;
                    ve.latchedSlide = mc.slideS;
                    ve.latchedPress = mc.pressS;
                    ve.latched = true;
                    ve.gateHeld = false;
                }
            }
        }
        voices.noteOff (note);
        if (! voices.anyHeld()) tape.allReleased (tp);
        if (vm == 0) resonator.noteOff (note);
        else resFollowMono();
    }
    else if (m.isSustainPedalOn())  { sustainHeld = true; voices.setSustain (true); resonator.setSustain (true); }
    else if (m.isSustainPedalOff())
    {
        sustainHeld = false;
        voices.setSustain (false); resonator.setSustain (false);
        if (! voices.anyHeld())
        {
            for (auto& ve : voiceExpr)
                if (ve.gateHeld)
                {
                    const auto& mc = mpeCh[(size_t) juce::jlimit (1, 16, ve.channel)];
                    ve.latchedBend = mc.bendS; ve.latchedSlide = mc.slideS; ve.latchedPress = mc.pressS;
                    ve.latched = true; ve.gateHeld = false;
                }
            tape.allReleased (tp);
        }
        if (vm != 0) resFollowMono();
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        sustainHeld = false;
        voices.allOff();
        for (auto& ve : voiceExpr) ve = VoiceExprState {};
        tape.allReleased (tp);
        resonator.allOff();
        resMonoNote = -1;
    }
    else if (m.isPitchWheel())
    {
        const float norm = juce::jlimit (-1.0f, 1.0f, ((float) m.getPitchWheelValue() - 8192.0f) / 8192.0f);
        if (isMpeMember) mpeCh[(size_t) ch].bendRaw = norm;
        else bendNormRaw = norm;
    }
    else if (m.isController())
    {
        const int cc = m.getControllerNumber();
        const float val = (float) m.getControllerValue() / 127.0f;
        const int learnedPid = midiLearn.processCCAudio (cc);
        if (learnedPid >= 0 && learnedPid < NUM_PARAMS)
        {
            ccTargetNorm[(size_t) learnedPid] = val;
            ccTargetActive[(size_t) learnedPid] = true;
            ccSyncNorm[(size_t) learnedPid].store (val, std::memory_order_relaxed);
            ccSyncDirty[(size_t) learnedPid].store (true, std::memory_order_release);
            ccAnySyncDirty.store (true, std::memory_order_release);
            midiDirty = true;
        }
        else if (cc == 1) mwRaw = val;
        else if (cc == 2) breathRaw = val;
        else if (cc == 74)
        {
            if (isMpeMember) mpeCh[(size_t) ch].slideRaw = val;
            slideRaw = val;
        }
    }
    else if (m.isChannelPressure())
    {
        const float val = (float) m.getChannelPressureValue() / 127.0f;
        if (isMpeMember) mpeCh[(size_t) ch].pressRaw = val;
        else atRaw = val;
    }
    else if (m.isAftertouch())
    {
        const int note = m.getNoteNumber();
        const float val = (float) m.getAfterTouchValue() / 127.0f;
        polyPressSeen = true;
        bool matched = false;
        for (int v = 0; v < VoiceManager::kVoices; ++v)
            if (voices.voiceActive (v) && voiceExpr[(size_t) v].note == note) { voiceExpr[(size_t) v].polyPressRaw = val; matched = true; }
        if (! matched) atRaw = val;
    }
    else if (m.isResetAllControllers()) { resetExpressionState(); }
}

void VesselProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (! baseReady || snapPending.exchange (false, std::memory_order_acq_rel))
        snapToCurrentState();

    struct ScopedAudioTable
    {
        VesselProcessor& self;
        explicit ScopedAudioTable (VesselProcessor& s) noexcept : self (s) { self.voices.setTable (self.tables.acquireAudio()); }
        ~ScopedAudioTable() noexcept
        {
            self.voices.setTable (&WavetableBank::get());
            self.tables.releaseAudio();
        }
    } tableGuard (*this);

    // transport
    playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = *b > 1.0 ? *b : 120.0;
            if (auto q = pos->getPpqPosition()) ppqBlock = *q;
            playing = pos->getIsPlaying();
        }
    granular.setTransport (playing, ppqBlock, bpm);
    for (auto& l : lfos) l.setBpm (bpm);

    // any output channels beyond the inputs must start silent
    if (n > 0)
        for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);

    auto renderSpan = [&] (int from, int to)
    {
        int p = from;
        while (p < to)
        {
            const bool isBusTick = (busRem == 0);
            if (isBusTick) busRem = kChunk;
            const int len = std::min (busRem, to - p);
            renderChunk (buffer, p, len, isBusTick, kChunk);
            busRem -= len;
            p += len;
        }
    };

    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, std::max (0, n), meta.samplePosition);
        if (pos < at) { renderSpan (pos, at); pos = at; }
        handleMidi (meta.getMessage());
    }
    if (pos < n) renderSpan (pos, n);

    if (n <= 0) return;

    // telemetry (decay scaled by block duration so 1-sample and 2048-sample buffers behave identically)
    telemetry.outPeakL = peakHoldL; telemetry.outPeakR = peakHoldR;
    const float holdDecay = (float) std::pow (0.6, (double) n / 512.0);
    peakHoldL = flushDenormal (peakHoldL * holdDecay);
    peakHoldR = flushDenormal (peakHoldR * holdDecay);
    telemetry.clipCount.fetch_add (std::exchange (outStage.clipped, 0));
    telemetry.duckGain = ducker.currentGain();
    telemetry.unmaskDb = unmask.reductionDb();
    telemetry.followerEnv = follower.lastEnv();
    telemetry.tapeSpeed = tape.speedForDisplay();
    telemetry.grainLooping = granular.isLooping();
    telemetry.modWheel = mwS; telemetry.aftertouch = std::max (atS, activeMaxPressS); telemetry.velocity = velS;
    telemetry.breath = breathS; telemetry.slide = slideS;
    telemetry.xyX = pv (v2_xy_x); telemetry.xyY = pv (v2_xy_y);
    telemetry.mpeActive = pb (v2_mpe_on);
    {
        int held = 0;
        float polyPressPeak = 0.0f;
        float polyPressEnergy = 0.0f;
        const bool mpeOnNow = pb (v2_mpe_on);
        for (int v = 0; v < VoiceManager::kVoices; ++v)
        {
            if (voices.voiceActive (v))
            {
                const auto& ve = voiceExpr[(size_t) v];
                if (ve.gateHeld) ++held;
                float vp = ve.polyPressS;
                if (mpeOnNow)
                {
                    const float mp = ve.latched ? ve.latchedPress : mpeCh[(size_t) juce::jlimit (1, 16, ve.channel)].pressS;
                    vp = std::max (vp, mp);
                }
                polyPressPeak = std::max (polyPressPeak, vp);
                polyPressEnergy += vp;
            }
        }
        if (voices.anyHeld() && held == 0) held = 1;
        const float dt = (float) n / (float) std::max (1.0, sr);
        if (held > 0)
        {
            const float chordBoost = 1.0f + 0.16f * (float) std::min (held - 1, 5);
            keyHoldAccum = std::min (1.0f, keyHoldAccum + dt * 0.42f * chordBoost);
        }
        else
        {
            keyHoldAccum = flushDenormal (keyHoldAccum * std::exp (-dt / 0.68f));
        }
        const float combinedTouch = std::clamp (std::max ({ atS, polyPressPeak, std::min (1.0f, polyPressEnergy * 0.65f) }), 0.0f, 1.0f);
        telemetry.heldCount.store (held, std::memory_order_relaxed);
        telemetry.keyHold.store (keyHoldAccum, std::memory_order_relaxed);
        telemetry.polyTouch.store (combinedTouch, std::memory_order_relaxed);
    }
    const auto og = ott.bandGainDb();
    for (int b = 0; b < 3; ++b) telemetry.ottGain[b] = og[(size_t) b];
    for (int i = 0; i < NUM_PARAMS; ++i) telemetry.modOffset[(size_t) i].store (mod[(size_t) i], std::memory_order_relaxed);
}

//==============================================================================
/** 20 ms one-pole on every continuous parameter, evaluated once per chunk in the normalised domain
    (perceptual for skewed ranges). The modules then ramp linearly across the chunk, so a host automation
    jump becomes a smooth curve with no stair-steps. Stepped params (semitones, choices, switches) are exempt. */
void VesselProcessor::smoothControls (int n)
{
    const float a = std::exp (-(float) n / (0.020f * (float) sr));
    for (const int i : floatPids)
    {
        const float raw = rawParams[(size_t) i]->load (std::memory_order_relaxed);
        const uint32_t bits = std::bit_cast<uint32_t> (raw);
        if (bits != lastRawBits[(size_t) i])
        {
            lastRawBits[(size_t) i] = bits;
            targetNorm[(size_t) i] = paramRanges[(size_t) i].convertTo0to1 (raw);
            ccTargetActive[(size_t) i] = false;
        }
        else if (ccTargetActive[(size_t) i])
        {
            targetNorm[(size_t) i] = ccTargetNorm[(size_t) i];
        }
        const float target = targetNorm[(size_t) i];
        auto& b = base[(size_t) i];
        b = smoothed[(size_t) i] ? target + a * (b - target) : target;
    }

    const float e = std::exp (-(float) n / (0.012f * (float) sr));   // expression: 12 ms (aftertouch is often 7-bit)
    mwS       = mwRaw       + e * (mwS       - mwRaw);
    atS       = atRaw       + e * (atS       - atRaw);
    velS      = velRaw      + e * (velS      - velRaw);
    bendNormS = bendNormRaw + e * (bendNormS - bendNormRaw);
    breathS   = breathRaw   + e * (breathS   - breathRaw);
    slideS    = slideRaw    + e * (slideS    - slideRaw);

    const bool mpeOn = pb (v2_mpe_on);
    float maxPress = atS;
    if (mpeOn)
    {
        for (int ch = 1; ch <= 16; ++ch)
        {
            auto& mc = mpeCh[(size_t) ch];
            mc.bendS  = mc.bendRaw  + e * (mc.bendS  - mc.bendRaw);
            mc.slideS = mc.slideRaw + e * (mc.slideS - mc.slideRaw);
            mc.pressS = mc.pressRaw + e * (mc.pressS - mc.pressRaw);
        }
    }
    if (polyPressSeen || mpeOn)
    {
        for (int v = 0; v < VoiceManager::kVoices; ++v)
        {
            auto& ve = voiceExpr[(size_t) v];
            if (polyPressSeen) ve.polyPressS = ve.polyPressRaw + e * (ve.polyPressS - ve.polyPressRaw);
            if (voices.voiceActive (v))
            {
                maxPress = std::max (maxPress, ve.polyPressS);
                if (mpeOn)
                {
                    const float mp = ve.latched ? ve.latchedPress : mpeCh[(size_t) juce::jlimit (1, 16, ve.channel)].pressS;
                    maxPress = std::max (maxPress, mp);
                }
            }
        }
    }
    activeMaxPressS = maxPress;
}

namespace
{
/** Parameters that live inside each voice: velocity routed here is applied per voice, from that voice's own
    note-on velocity. Everything else is global and follows the most recent note-on. */
bool isVoiceParam (int pid)
{
    switch (pid)
    {
        case sub1_level: case sub2_level: case sub1_shape: case sub2_shape: case sub_weight:
        case wt_level: case wt_morph: case wt_pw: case wt_pwm: case wt_pwm_rate: case wt_fine: case wt_drift:
        case flt_a_cut: case flt_a_res: case flt_b_cut: case flt_b_res: case flt_drive: case flt_env: case flt_key:
        case fm_fine: case fm_index: case fm_feedback: case fm_env: case fm_decay: case fm_key:
        case amp_a: case amp_d: case amp_s: case amp_r: case fenv_a: case fenv_d: case fenv_s: case fenv_r: case glide:
        case v2_organic_amt:
        case v2_lpg_decay: case v2_lpg_strike: case v2_fb_filter: case v2_fb_fm:
            return true;
        default: return false;
    }
}

void setVoiceField (VoiceParams& v, int pid, float x, float tuneWithoutFine)
{
    switch (pid)
    {
        case sub1_level: v.sub1Level = x; break;    case sub2_level: v.sub2Level = x; break;
        case sub1_shape: v.sub1Shape = x; break;    case sub2_shape: v.sub2Shape = x; break;
        case sub_weight: v.subWeight = x; break;    case wt_level:   v.wtLevel = x; break;
        case wt_morph:   v.morph = x; break;        case wt_pw:      v.pw = x; break;
        case wt_pwm:     v.pwm = x; break;          case wt_pwm_rate: v.pwmRate = x; break;
        case wt_fine:    v.wtTune = tuneWithoutFine + x * 0.01f; break;
        case wt_drift:   v.drift = x; break;
        case flt_a_cut:  v.cutA = x; break;         case flt_a_res:  v.resA = x; break;
        case flt_b_cut:  v.cutB = x; break;         case flt_b_res:  v.resB = x; break;
        case flt_drive:  v.fDrive = x; break;       case flt_env:    v.envAmt = x; break;
        case flt_key:    v.keyTrack = x; break;
        case amp_a: v.amp.attack = x * 0.001f; break;   case amp_d: v.amp.decay = x * 0.001f; break;
        case amp_s: v.amp.sustain = x; break;           case amp_r: v.amp.release = x * 0.001f; break;
        case fenv_a: v.filt.attack = x * 0.001f; break; case fenv_d: v.filt.decay = x * 0.001f; break;
        case fenv_s: v.filt.sustain = x; break;         case fenv_r: v.filt.release = x * 0.001f; break;
        case glide: v.glideMs = x; break;
        case fm_fine: v.fmFine = x; break;          case fm_index: v.fmIndex = x; break;
        case fm_feedback: v.fmFb = x; break;        case fm_env:   v.fmEnv = x; break;
        case fm_decay: v.fmDecay = x; break;        case fm_key:   v.fmKey = x; break;
        case v2_organic_amt: v.organicAmt = x; break;
        case v2_lpg_decay:   v.lpgDecay = x; break;
        case v2_lpg_strike:  v.lpgStrike = x; break;
        case v2_fb_filter:   v.fbFilter = x; break;
        case v2_fb_fm:       v.fbFm = x; break;
        default: break;
    }
}

constexpr int kExprDest[kExprSlots] { vel_dest1, vel_dest2, mw_dest1, mw_dest2, at_dest1, at_dest2 };
} // namespace

/** Mod wheel, aftertouch and (global) velocity → modulation offsets. Unipolar sources: offset = amount × source,
    i.e. full depth sweeps the whole range once, the same span a bipolar LFO covers at full depth. */
void VesselProcessor::applyExpression()
{
    for (int s = 0; s < kExprSlots; ++s)
    {
        const int dest = destIndexToPid (pc (kExprDest[s]));
        if (dest < 0) continue;
        const float amt = pv (kExprDest[s] + 1);
        if (s < 2)
        {
            if (! isVoiceParam (dest)) mod[(size_t) dest] += amt * velS;
        }
        else if (s < 4) mod[(size_t) dest] += amt * mwS;
        else if (! isVoiceParam (dest)) mod[(size_t) dest] += amt * std::max (atS, activeMaxPressS);
    }
    if (slideS > 1.0e-5f)
    {
        for (int s = 0; s < 2; ++s)
        {
            const int dest = destIndexToPid (pc (slideDestPid (s)));
            if (dest >= 0 && ! isVoiceParam (dest)) mod[(size_t) dest] += pv (slideAmtPid (s)) * slideS;
        }
    }
    if (breathS > 1.0e-5f)
    {
        for (int s = 0; s < 2; ++s)
        {
            const int dest = destIndexToPid (pc (breathDestPid (s)));
            if (dest >= 0) mod[(size_t) dest] += pv (breathAmtPid (s)) * breathS;
        }
    }
}

/** The eight macros (first 4 are Hero Performance Macros): amount × knob on each routed destination. Applied after
    expression and before XY & LFOs, so a macro can drive V1 targets, V2 organic/texture/FX, and LFO rate/warp. */
void VesselProcessor::applyMacros()
{
    for (int m = 0; m < kNumMacros; ++m)
    {
        const float v = pv (macroPid (m));
        if (v <= 1.0e-5f) continue;
        for (int s = 0; s < kMacroSlots; ++s)
        {
            const int dest = destIndexToPid (pc (macroDestPid (m, s)));
            if (dest >= 0) mod[(size_t) dest] += pv (macroAmtPid (m, s)) * v;
        }
    }
}

void VesselProcessor::applyXy()
{
    const float dx = (pv (v2_xy_x) - 0.5f) * 2.0f;
    const float dy = (pv (v2_xy_y) - 0.5f) * 2.0f;
    if (std::abs (dx) > 1.0e-5f)
        for (int s = 0; s < kXySlots; ++s)
            if (const int dest = destIndexToPid (pc (xyDestPid (0, s))); dest >= 0)
                mod[(size_t) dest] += pv (xyAmtPid (0, s)) * dx;
    if (std::abs (dy) > 1.0e-5f)
        for (int s = 0; s < kXySlots; ++s)
            if (const int dest = destIndexToPid (pc (xyDestPid (1, s))); dest >= 0)
                mod[(size_t) dest] += pv (xyAmtPid (1, s)) * dy;
}

juce::String VesselProcessor::macroName (int m) const
{
    const juce::ScopedLock sl (macroNameLock);
    return juce::isPositiveAndBelow (m, kNumMacros) ? macroNames[(size_t) m] : juce::String();
}

juce::String VesselProcessor::heroMacroLabel (int m) const
{
    const auto n = macroName (m);
    return n.isNotEmpty() ? n : ("MACRO " + juce::String (m + 1));
}

void VesselProcessor::setMacroName (int m, const juce::String& n)
{
    if (! juce::isPositiveAndBelow (m, kNumMacros)) return;
    {
        const juce::ScopedLock sl (macroNameLock);
        macroNames[(size_t) m] = n.trim().substring (0, 24);
    }
    ++macroNamesVersion;
}

juce::String VesselProcessor::xyLabelX() const { const juce::ScopedLock sl (macroNameLock); return xyNameX.isNotEmpty() ? xyNameX : juce::String ("TIMBRE"); }
juce::String VesselProcessor::xyLabelY() const { const juce::ScopedLock sl (macroNameLock); return xyNameY.isNotEmpty() ? xyNameY : juce::String ("SPACE"); }

void VesselProcessor::setXyLabels (const juce::String& xLabel, const juce::String& yLabel)
{
    {
        const juce::ScopedLock sl (macroNameLock);
        xyNameX = xLabel.trim().substring (0, 20);
        xyNameY = yLabel.trim().substring (0, 20);
    }
    ++macroNamesVersion;
}

void VesselProcessor::setXyPosition (float x01, float y01)
{
    if (auto* px = param (v2_xy_x)) px->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, x01));
    if (auto* py = param (v2_xy_y)) py->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, y01));
}

juce::String VesselProcessor::currentPresetName() const        { const juce::ScopedLock sl (macroNameLock); return metaName; }
juce::String VesselProcessor::currentPresetAuthor() const      { const juce::ScopedLock sl (macroNameLock); return metaAuthor; }
juce::String VesselProcessor::currentPresetCategory() const    { const juce::ScopedLock sl (macroNameLock); return metaCategory; }
juce::String VesselProcessor::currentPresetTags() const        { const juce::ScopedLock sl (macroNameLock); return metaTags; }
juce::String VesselProcessor::currentPresetDescription() const { const juce::ScopedLock sl (macroNameLock); return metaDescription; }
bool VesselProcessor::currentPresetFavorite() const            { const juce::ScopedLock sl (macroNameLock); return metaFavorite; }

void VesselProcessor::buildVoiceParams()
{
    auto& vp = vpBase;
    vp.subOn = pb (sub_on);
    vp.sub1Oct = pc (sub1_oct);
    vp.sub2Oct = pc (sub2_oct) + 1;
    vp.sub1Level = pv (sub1_level); vp.sub2Level = pv (sub2_level);
    vp.sub1Shape = pv (sub1_shape); vp.sub2Shape = pv (sub2_shape);
    vp.subWeight = pv (sub_weight);
    vp.wtOn = pb (wt_on);
    vp.wtLevel = pv (wt_level); vp.morph = pv (wt_morph); vp.pw = pv (wt_pw);
    vp.pwm = pv (wt_pwm); vp.pwmRate = pv (wt_pwm_rate);
    const float tuneCoarse = (float) ((pc (wt_oct) - 2) * 12) + pv (wt_semi);
    vp.wtTune = tuneCoarse + pv (wt_fine) * 0.01f;
    vp.drift = pv (wt_drift);
    vp.fltOn = pb (flt_on); vp.route = pc (flt_route);
    vp.cutA = pv (flt_a_cut); vp.resA = pv (flt_a_res); vp.cutB = pv (flt_b_cut); vp.resB = pv (flt_b_res);
    vp.fDrive = pv (flt_drive); vp.envAmt = pv (flt_env); vp.keyTrack = pv (flt_key);
    vp.amp  = { pv (amp_a) * 0.001f, pv (amp_d) * 0.001f, pv (amp_s), pv (amp_r) * 0.001f };
    vp.filt = { pv (fenv_a) * 0.001f, pv (fenv_d) * 0.001f, pv (fenv_s), pv (fenv_r) * 0.001f };
    vp.glideMs = pv (glide);
    vp.fmOn = pb (fm_on);
    vp.fmRatio = kFmRatios[(size_t) juce::jlimit (0, (int) std::size (kFmRatios) - 1, pc (fm_ratio))];
    vp.fmFine = pv (fm_fine); vp.fmIndex = pv (fm_index); vp.fmFb = pv (fm_feedback);
    vp.fmEnv = pv (fm_env); vp.fmDecay = pv (fm_decay); vp.fmKey = pv (fm_key);

    const bool mpeOn = pb (v2_mpe_on);
    const float globalBendSt = bendNormS * pv (v2_pb_range);
    const float mpeBendRange = pv (v2_mpe_bend);
    vp.bend = globalBendSt;
    vp.velAmp = pv (vel_amp);
    vp.organicAmt = pv (v2_organic_amt);
    vp.lpgMode = pc (v2_lpg_mode);
    vp.lpgDecay = pv (v2_lpg_decay);
    vp.lpgStrike = pv (v2_lpg_strike);
    vp.fbFilter = pv (v2_fb_filter);
    vp.fbFm = pv (v2_fb_fm);
    vp.railSag = pv (v2_rail_sag);
    vp.railTimeMs = pv (v2_rail_time);

    struct VoiceRoute { int dest = -1; float amt = 0.0f; int srcKind = 0; };
    VoiceRoute routes[6];
    int numRoutes = 0;
    for (int s = 0; s < 2; ++s)
    {
        if (const int dVel = destIndexToPid (pc (s == 0 ? vel_dest1 : vel_dest2)); dVel >= 0 && isVoiceParam (dVel))
            routes[numRoutes++] = { dVel, pv (s == 0 ? vel_amt1 : vel_amt2), 0 };
        if (const int dAt = destIndexToPid (pc (s == 0 ? at_dest1 : at_dest2)); dAt >= 0 && isVoiceParam (dAt))
            routes[numRoutes++] = { dAt, pv (s == 0 ? at_amt1 : at_amt2), 1 };
        if (const int dSl = destIndexToPid (pc (slideDestPid (s))); dSl >= 0 && isVoiceParam (dSl))
            routes[numRoutes++] = { dSl, pv (slideAmtPid (s)), 2 };
    }

    for (int v = 0; v < VoiceManager::kVoices; ++v)
    {
        auto& p = voiceParams[(size_t) v];
        p = vp;
        if (! voices.voiceActive (v)) continue;

        const auto& ve = voiceExpr[(size_t) v];
        const int vCh = juce::jlimit (1, 16, ve.channel);
        const bool memberVoice = mpeOn && (vCh != mpeMasterChannel);

        float voiceBendSt = globalBendSt;
        float voicePress = std::max (atS, ve.polyPressS);
        float voiceSlide = slideS;
        if (memberVoice)
        {
            const float bNorm = ve.latched ? ve.latchedBend  : mpeCh[(size_t) vCh].bendS;
            const float pNorm = ve.latched ? ve.latchedPress : mpeCh[(size_t) vCh].pressS;
            const float sNorm = ve.latched ? ve.latchedSlide : mpeCh[(size_t) vCh].slideS;
            voiceBendSt += bNorm * mpeBendRange;
            voicePress = std::max (voicePress, pNorm);
            voiceSlide = sNorm;
        }
        p.bend = voiceBendSt;

        if (numRoutes == 0) continue;
        const float vel = voices.voiceVelocity (v);
        int uniqueDest[6] {};
        float uniqueExtra[6] {};
        int nUnique = 0;
        for (int r = 0; r < numRoutes; ++r)
        {
            const float srcVal = routes[r].srcKind == 0 ? vel : (routes[r].srcKind == 1 ? voicePress : voiceSlide);
            const float delta = routes[r].amt * srcVal;
            int slot = -1;
            for (int u = 0; u < nUnique; ++u) if (uniqueDest[u] == routes[r].dest) { slot = u; break; }
            if (slot < 0) { uniqueDest[nUnique] = routes[r].dest; uniqueExtra[nUnique] = delta; ++nUnique; }
            else uniqueExtra[slot] += delta;
        }
        for (int u = 0; u < nUnique; ++u)
            setVoiceField (p, uniqueDest[u], pvExtra (uniqueDest[u], uniqueExtra[u]), tuneCoarse);
    }
}

template <typename Fn>
void VesselProcessor::faded (BypassFade& f, bool on, int n, Fn&& process)
{
    if (f.settled (on)) { process (on); return; }   // fully on, or fully off (the module's own off path)
    std::copy_n (xL.data(), n, dryL.data());
    std::copy_n (xR.data(), n, dryR.data());
    process (true);
    for (int i = 0; i < n; ++i)
    {
        const float g = f.next (on);
        xL[(size_t) i] = dryL[(size_t) i] + g * (xL[(size_t) i] - dryL[(size_t) i]);
        xR[(size_t) i] = dryR[(size_t) i] + g * (xR[(size_t) i] - dryR[(size_t) i]);
    }
}

void VesselProcessor::renderChunk (juce::AudioBuffer<float>& io, int start, int n, bool isBusTick, int busSpan)
{
    const float fs = (float) sr;
    voices.setMode (pc (voice_mode));
    if (const int t = pc (wt_table); t != tableAsked) { tables.request (t); tableAsked = t; }   // lock-free; loads in the background
    voices.setTable (tables.acquireAudio());

    // ── (1)(5)(11) voices oversampled in one kChunk<=64 batch, with inner kControlStep=8 fixed-grid modulation
    const bool anyVoiceStart = voices.anyActive();
    if (anyVoiceStart) { voiceOsIdle = false; bodyQuietSamples = 0; }
    const bool runVoiceOs = ! voiceOsIdle;

    float* upBodyL = nullptr; float* upBodyR = nullptr; float* upSub = nullptr;
    juce::dsp::AudioBlock<float> upBlock;
    if (runVoiceOs)
    {
        float* zc[3] { zeros.data(), zeros.data(), zeros.data() };
        juce::dsp::AudioBlock<float> zin (zc, 3, (size_t) n);
        upBlock = voiceOs->processSamplesUp (zin);
        upBlock.clear();
        upBodyL = upBlock.getChannelPointer (0);
        upBodyR = upBlock.getChannelPointer (1);
        upSub   = upBlock.getChannelPointer (2);
    }

    int subPos = 0;
    while (subPos < n)
    {
        const bool isCtrlTick = (ctrlRem == 0);
        if (isCtrlTick) ctrlRem = kControlStep;
        const int subLen = std::min (ctrlRem, n - subPos);

        if (isCtrlTick)
        {
            const double ppqNow = ppqBlock + (double) (start + subPos + kControlStep) / sr * bpm / 60.0;
            smoothControls (kControlStep);

            std::fill (mod.begin(), mod.end(), 0.0f);
            applyExpression();
            applyMacros();
            applyXy();
            for (int l = 0; l < kNumLfos; ++l)
            {
                const int b0 = lfoBase (l), e0 = lfoExt (l);
                WarpedLfo::Params lp;
                lp.rateHz = pv (b0 + 1); lp.warp = pv (b0 + 4); lp.shape = pc (b0); lp.synced = pb (b0 + 2);
                lp.beatsPerCycle = divisionBeats (pc (b0 + 3));
                lp.mode = pc (e0); lp.phaseOffset = pv (e0 + 1); lp.fadeSec = pv (e0 + 2); lp.smooth = pv (e0 + 3);
                lp.unipolar = pb (e0 + 4);
                const float v = lfos[(size_t) l].advance (kControlStep, fs, lp, drawTables[(size_t) l], playing, ppqNow);
                if (isBusTick && subPos == 0)
                {
                    telemetry.lfoValue[l].store (v, std::memory_order_relaxed);
                    telemetry.lfoPhase[l].store (lfos[(size_t) l].currentPhase(), std::memory_order_relaxed);
                }
                const float scale = lp.unipolar ? 1.0f : 0.5f;
                for (int s = 0; s < kLfoSlots; ++s)
                {
                    const int dest = destIndexToPid (pc (lfoDestPid (l, s)));
                    if (dest >= 0) mod[(size_t) dest] += pv (lfoAmtPid (l, s)) * v * scale;
                }
            }
            if (isBusTick && subPos == 0)
            {
                busBase = base;
                busMod = mod;
                busVoiceActive = voices.anyActive();
                busLowestHeld = voices.lowestHeld();
            }
            if (voices.anyActive()) buildVoiceParams();
            midiDirty = false;
        }
        else if (midiDirty)
        {
            if (voices.anyActive()) buildVoiceParams();
            midiDirty = false;
        }

        if (runVoiceOs)
        {
            if (voices.anyActive())
            {
                const int osOff = subPos * VoiceManager::kOs;
                const int osLen = subLen * VoiceManager::kOs;
                voices.renderSlice (upBodyL + osOff, upBodyR + osOff, upSub + osOff, osLen,
                                    voiceParams, pb (flt_on), pc (flt_route), isCtrlTick, kControlStep * VoiceManager::kOs);
                const uint32_t baseSamples = (uint32_t) std::max (1, subLen);
                for (int v = 0; v < VoiceManager::kVoices; ++v)
                {
                    if (voices.voiceActive (v)) voiceExpr[(size_t) v].age += baseSamples;
                    else { voiceExpr[(size_t) v].gateHeld = false; voiceExpr[(size_t) v].note = -1; }
                }
            }
            else
            {
                const int lpgM = pc (v2_lpg_mode);
                voices.snapFilter (pb (flt_on) || lpgM == 1 || lpgM == 2);
                voices.tickIdle (subLen * VoiceManager::kOs);
            }
        }

        ctrlRem -= subLen;
        samplePos += (uint64_t) subLen;
        subPos += subLen;
    }

    if (runVoiceOs)
    {
        float* oc[3] { bodyL.data(), bodyR.data(), subB.data() };
        juce::dsp::AudioBlock<float> out (oc, 3, (size_t) n);
        voiceOs->processSamplesDown (out);

        // Post-voice 64-bit 4th-order 12 Hz Butterworth DC hygiene filter (-0.003 dB @ 30 Hz, -62 dB @ 2 Hz)
        float blkPk = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float bl = bodyDc2[0].highpass (bodyDc1[0].highpass (bodyL[(size_t) i]));
            const float br = bodyDc2[1].highpass (bodyDc1[1].highpass (bodyR[(size_t) i]));
            bodyL[(size_t) i] = bl;
            bodyR[(size_t) i] = br;
            blkPk = std::max ({ blkPk, std::abs (bl), std::abs (br), std::abs (subB[(size_t) i]) });
        }

        // Only sleep voiceOs + bodyDc once voices are silent AND the 12 Hz HPF tail has decayed below -140 dBFS
        if (! voices.anyActive() && blkPk < 1.0e-7f)
        {
            bodyQuietSamples += n;
            if (bodyQuietSamples >= 256)
            {
                voiceOsIdle = true;
                voiceOs->reset();
                for (int c = 0; c < 2; ++c) { bodyDc1[c].reset(); bodyDc2[c].reset(); }
            }
        }
        else
        {
            bodyQuietSamples = 0;
        }
    }
    else
    {
        std::fill_n (bodyL.data(), n, 0.0f);
        std::fill_n (bodyR.data(), n, 0.0f);
        std::fill_n (subB.data(),  n, 0.0f);
        const int lpgM = pc (v2_lpg_mode);
        voices.snapFilter (pb (flt_on) || lpgM == 1 || lpgM == 2);
        voices.tickIdle (n * VoiceManager::kOs);
    }
    std::fill_n (texL.data(), n, 0.0f); std::fill_n (texR.data(), n, 0.0f);

    // host audio input (multi-effect use) enters the body bus; sanitize against upstream NaN/Inf
    const int inCh = std::min (getTotalNumInputChannels(), io.getNumChannels());
    if (isBusTick) inLevelR.to (pvBus (in_level), busSpan);
    if (inCh > 0 && (inLevelR.cur > 0.0f || inLevelR.inc != 0.0f))
    {
        const float* il = io.getReadPointer (0, start);
        const float* ir = io.getReadPointer (inCh > 1 ? 1 : 0, start);
        for (int i = 0; i < n; ++i)
        {
            const float g = inLevelR.next();
            const float sl = std::isfinite (il[i]) ? juce::jlimit (-16.0f, 16.0f, il[i]) : 0.0f;
            const float srIn = std::isfinite (ir[i]) ? juce::jlimit (-16.0f, 16.0f, ir[i]) : 0.0f;
            bodyL[(size_t) i] += sl * g;
            bodyR[(size_t) i] += srIn * g;
        }
    }
    else inLevelR.skip (n);

    for (int i = 0; i < n; ++i) key[(size_t) i] = 0.5f * (bodyL[(size_t) i] + bodyR[(size_t) i]) + subB[(size_t) i];

    // ── (3) drone
    DroneEngine::Params dp;
    dp.on = pb (drone_on); dp.hold = pb (drone_hold); dp.level = pvBus (drone_level); dp.tone = pvBus (drone_tone);
    dp.motion = pvBus (drone_motion); dp.spread = pvBus (drone_spread); dp.attack = pvBus (drone_attack); dp.release = pvBus (drone_release);
    dp.octave = (pc (drone_oct) - 2) * 12;
    drone.process (texL.data(), texR.data(), n, voices.lowestHeld(), dp, isBusTick, busSpan);

    // ── (4) noise injector
    NoiseInjector::Params np;
    np.on = pb (noise_on); np.hiss = pvBus (noise_hiss); np.crackle = pvBus (noise_crackle); np.hum = pvBus (noise_hum);
    np.tone = pvBus (noise_tone); np.dyn = pvBus (noise_dyn); np.mains = pc (noise_hum_freq) == 0 ? 50.0f : 60.0f;
    noise.process (texL.data(), texR.data(), key.data(), n, np, isBusTick, busSpan);

    // ── V2 procedural texture & sub-harmonic layer (0% default mix, 0 latency, 100% procedural)
    {
        ProceduralTextureBus::Params texp { pc (v2_tex_mode), pvBus (v2_tex_mix), pvBus (v2_tex_tone), pvBus (v2_tex_drive) };
        textureBus.process (texL.data(), texR.data(), key.data(), n, busVoiceActive, busLowestHeld, texp, isBusTick, busSpan);
    }

    // ── (23) ducker: sub punches through the texture layer
    ducker.process (texL.data(), texR.data(), subB.data(), n, pb (duck_on), pc (duck_mode), pvBus (duck_depth), pvBus (duck_release), isBusTick, busSpan);

    // pre-bus = body + texture
    for (int i = 0; i < n; ++i) { xL[(size_t) i] = bodyL[(size_t) i] + texL[(size_t) i]; xR[(size_t) i] = bodyR[(size_t) i] + texR[(size_t) i]; }

    // ── (2) granular micro-looper
    {
        static constexpr float divs[] { 1.0f, 0.5f, 0.25f, 0.125f, 1.0f / 3.0f, 1.0f / 6.0f };
        GranularLooper::Params gp;
        gp.freeze = pb (gran_freeze); gp.sizeMs = pvBus (gran_size); gp.density = pvBus (gran_density);
        gp.pitch = pvBus (gran_pitch); gp.stretch = pvBus (gran_stretch); gp.spray = pvBus (gran_spray); gp.reverse = pvBus (gran_reverse);
        gp.chance = pvBus (gran_chance); gp.mix = pvBus (gran_mix); gp.divBeats = divs[juce::jlimit (0, 5, pc (gran_div))];
        faded (fGran, pb (gran_on), n, [&] (bool on) { gp.on = on; granular.process (xL.data(), xR.data(), n, gp, isBusTick, busSpan); });
    }

    // ── (13) spectral unmasking keyed by the sub
    {
        Unmasker::Params up { true, pvBus (unmask_freq), pvBus (unmask_depth), pvBus (unmask_release) };
        faded (fUnmask, pb (unmask_on), n, [&] (bool on) { up.on = on; unmask.process (xL.data(), xR.data(), subB.data(), n, up, isBusTick); });
    }

    // ── (26) physical-model resonator: before RUIN, so the tube can drive it and the clean sub stays outside it
    {
        Resonator::Params rsp;
        rsp.model = pc (res_model); rsp.tune = pvBus (res_tune); rsp.decay = pvBus (res_decay); rsp.bright = pvBus (res_bright);
        rsp.stiff = pvBus (res_stiff); rsp.pos = pvBus (res_pos); rsp.damper = pvBus (res_damper); rsp.input = pvBus (res_input);
        rsp.strike = pvBus (res_strike); rsp.mix = pvBus (res_mix);
        rsp.bend = bendNormS * pvBus (v2_pb_range); rsp.glideMs = pc (voice_mode) != 0 ? pvBus (glide) : 0.0f;
        faded (fRes, pb (res_on), n, [&] (bool on) { rsp.on = on; resonator.process (xL.data(), xR.data(), n, rsp); });
    }

    // ── RUIN: sub send → (15) pre tilt → [4x: (6) tube → (15) post tilt → (10) bias] → (9) crush
    if (isBusTick)
    {
        const float ss = pvBus (sub_send);
        sendR.to (ss, busSpan);
        cleanR.to (1.0f - ss, busSpan);
    }
    for (int i = 0; i < n; ++i) { const float s = subB[(size_t) i] * sendR.next(); xL[(size_t) i] += s; xR[(size_t) i] += s; }

    const float preTiltDb = pvBus (pre_tilt);
    if (isBusTick) for (int c = 0; c < 2; ++c) preTilt[c].set (preTiltDb, pvBus (pre_pivot), fs, busSpan);
    if (std::abs (preTiltDb) > 1.0e-4f || std::abs (preTilt[0].gLo.cur - 1.0f) > 1.0e-5f || std::abs (preTilt[0].gHi.cur - 1.0f) > 1.0e-5f)
        for (int i = 0; i < n; ++i) { xL[(size_t) i] = preTilt[0].process (xL[(size_t) i]); xR[(size_t) i] = preTilt[1].process (xR[(size_t) i]); }
    else
        for (int c = 0; c < 2; ++c) { preTilt[c].gLo.skip (n); preTilt[c].gHi.skip (n); }

    RuinCore::Params rp;
    rp.tube = { pb (tube_on), pc (tube_mode), pvBus (tube_drive), pvBus (tube_bias), pvBus (tube_mix), pvBus (tube_out) };
    rp.postTiltDb = pvBus (post_tilt); rp.postPivot = pvBus (post_pivot);
    rp.bias = { pb (bias_on), pvBus (bias_amount), pvBus (bias_sag), pvBus (bias_sat), pvBus (bias_speed) };
    ruin.process (xL.data(), xR.data(), n, rp, isBusTick, busSpan);

    {
        BitCrusher::Params cp { true, pvBus (crush_bits), pvBus (crush_rate), pvBus (crush_pre), pvBus (crush_post), pvBus (crush_mix) };
        faded (fCrush, pb (crush_on), n, [&] (bool on) { cp.on = on; crusher.process (xL.data(), xR.data(), n, cp, isBusTick, busSpan); });
    }

    const float ruinLat = ruin.latency();
    const float subDelay = ruinLat + 1.0f;
    float subLevelNow = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        subAlign.push (subB[(size_t) i]);
        const float cleanSrc = ruinLat > 0.0f ? subAlign.readHermite (subDelay) : subB[(size_t) i];
        const float s = cleanSrc * cleanR.next();
        xL[(size_t) i] += s; xR[(size_t) i] += s;
        subLevelNow = std::max (subLevelNow, std::abs (s));
    }
    const float subDecay = fastExp2 (-0.07400058f * ((float) n * (1.0f / (float) kChunk)));
    telemetry.subLevel.store (flushDenormal (std::max (subLevelNow, telemetry.subLevel.load (std::memory_order_relaxed) * subDecay)),
                              std::memory_order_relaxed);

    // ── Zero-gain-change 4-to-8-stage 64-bit TPT all-pass phase dispersion (post-sub-rejoin, sub-120 Hz mono-correlated)
    {
        const int dispChoice = pc (v2_disp_stages);   // 0=Off, 1..5 -> 4..8 stages
        PhaseDisperser::Params dpp { true, dispChoice > 0 ? (dispChoice + 3) : disperser.activeStages(),
                                     pvBus (v2_disp_freq), pvBus (v2_disp_pinch) };
        faded (fDisp, dispChoice > 0, n, [&] (bool on) { dpp.on = on; disperser.process (xL.data(), xR.data(), n, dpp, isBusTick, busSpan); });
    }

    // ── (7) VHS → (8) tape motor
    {
        VhsTransport::Params vhp { true, pvBus (vhs_wow), pvBus (vhs_flutter), pvBus (vhs_drift), pvBus (vhs_rate), pvBus (vhs_age) };
        faded (fVhs, pb (vhs_on), n, [&] (bool on) { vhp.on = on; vhs.process (xL.data(), xR.data(), n, vhp, isBusTick, busSpan); });
    }
    TapeMotor::Params tmp { (TapeMotor::Mode) pc (tape_mode), pvBus (tape_start), pvBus (tape_stop) };
    tape.process (xL.data(), xR.data(), n, tmp);

    // ── SHAPE (global): (12) follower LPF → (14) formant
    {
        FollowerFilter::Params fp { true, pvBus (envf_cut), pvBus (envf_depth), pvBus (envf_res), pvBus (envf_attack), pvBus (envf_release), pvBus (envf_mix) };
        faded (fFollow, pb (envf_on), n, [&] (bool on) { fp.on = on; follower.process (xL.data(), xR.data(), n, fp, isBusTick, busSpan); });
        FormantFilter::Params ffp { true, pvBus (form_vowel), pvBus (form_body), pvBus (form_shift), pvBus (form_res), pvBus (form_mix) };
        faded (fFormant, pb (form_on), n, [&] (bool on) { ffp.on = on; formant.process (xL.data(), xR.data(), n, ffp, isBusTick, busSpan); });
    }

    // ── SPACE: (18) Z-axis → BBD Analog Chorus → (17) convolution → (16) black hole → (19) Haas
    zaxis.process (xL.data(), xR.data(), n, pb (z_on), pvBus (z_depth));

    {
        const int bbdChoice = pc (v2_bbd_mode);   // 0=Off, 1=Mode I, 2=Mode II, 3=Ensemble
        BbdChorus::Params bp { true, bbdChoice > 0 ? bbdChoice : bbdChorus.activeMode(),
                               pvBus (v2_bbd_rate), pvBus (v2_bbd_depth), pvBus (v2_bbd_tone), pvBus (v2_bbd_mix) };
        faded (fBbd, bbdChoice > 0, n, [&] (bool on) { bp.on = on; bbdChorus.process (xL.data(), xR.data(), n, bp, isBusTick, busSpan); });
    }

    convMixS.set (pb (conv_on) ? pvBus (conv_mix) : 0.0f);
    if (convMixS.target > 0.0f || convMixS.cur > 1.0e-4f)
    {
        std::copy_n (xL.data(), n, wetL.data()); std::copy_n (xR.data(), n, wetR.data());
        float* ch[2] { wetL.data(), wetR.data() };
        juce::dsp::AudioBlock<float> blk (ch, 2, (size_t) n);
        convolution.process (juce::dsp::ProcessContextReplacing<float> (blk));
        for (int i = 0; i < n; ++i)
        {
            const float m = convMixS.next();
            xL[(size_t) i] += m * (wetL[(size_t) i] - xL[(size_t) i]);
            xR[(size_t) i] += m * (wetR[(size_t) i] - xR[(size_t) i]);
        }
    }

    BlackHole::Params bh { pb (verb_on), pb (verb_inf), pvBus (verb_size), pvBus (verb_decay), pvBus (verb_damp), pvBus (verb_mod), pvBus (verb_pre), pvBus (verb_low), pvBus (verb_mix) };
    blackHole.process (xL.data(), xR.data(), n, bh, isBusTick);

    haas.process (xL.data(), xR.data(), n, pb (haas_on), pvBus (haas_time), pvBus (haas_width), pvBus (haas_xover));

    // ── PRESSURE: (21) multiband → (22) mono collapse → V2 Organic Bus → [4x: (24) exciter → output gain → (25) clipper]
    {
        MultibandOtt::Params op;
        op.depth = pvBus (ott_depth); op.time = pvBus (ott_time); op.up = pvBus (ott_up); op.down = pvBus (ott_down);
        op.lox = pvBus (ott_lox); op.hix = pvBus (ott_hix); op.lowDb = pvBus (ott_low); op.midDb = pvBus (ott_mid); op.highDb = pvBus (ott_high);
        op.inDb = pvBus (ott_in); op.outDb = pvBus (ott_out);
        faded (fOtt, pb (ott_on), n, [&] (bool on) { op.on = on; ott.process (xL.data(), xR.data(), n, op, isBusTick, busSpan); });
        const float mf = pvBus (mono_freq);
        faded (fMono, pb (mono_on), n, [&] (bool on) { mono.process (xL.data(), xR.data(), n, on, mf); });
    }

    {
        OrganicBus::Params orgp { pvBus (v2_organic_amt), pvBus (v2_organic_floor), pb (v2_organic_mono) };
        organicBus.process (xL.data(), xR.data(), n, busVoiceActive, orgp, isBusTick, busSpan);
    }

    OutputStage::Params osp { pb (exc_on), pvBus (exc_freq), pvBus (exc_amount), pvBus (exc_harm), pb (clip_on), pvBus (clip_ceiling), pvBus (out_gain) };
    outStage.process (xL.data(), xR.data(), n, osp, isBusTick, busSpan);

    // ── sanitize output + write out + telemetry
    for (int i = 0; i < n; ++i)
    {
        if (! std::isfinite (xL[(size_t) i])) xL[(size_t) i] = 0.0f;
        if (! std::isfinite (xR[(size_t) i])) xR[(size_t) i] = 0.0f;
    }

    const int outCh = io.getNumChannels();
    if (outCh >= 2)
    {
        std::copy_n (xL.data(), n, io.getWritePointer (0, start));
        std::copy_n (xR.data(), n, io.getWritePointer (1, start));
        for (int c = 2; c < outCh; ++c) io.clear (c, start, n);
    }
    else if (outCh == 1)
    {
        float* o = io.getWritePointer (0, start);
        for (int i = 0; i < n; ++i) o[i] = 0.5f * (xL[(size_t) i] + xR[(size_t) i]);
    }

    float sumSq = 0.0f;
    int w = telemetry.scopeWrite.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        peakHoldL = std::max (peakHoldL, std::abs (xL[(size_t) i]));
        peakHoldR = std::max (peakHoldR, std::abs (xR[(size_t) i]));
        sumSq += xL[(size_t) i] * xL[(size_t) i] + xR[(size_t) i] * xR[(size_t) i];
        scopeDecim += 1.0f;
        if (scopeDecim >= 2.0f)
        {
            scopeDecim = 0.0f;
            telemetry.scopeL[(size_t) w].store (xL[(size_t) i], std::memory_order_relaxed);
            telemetry.scopeR[(size_t) w].store (xR[(size_t) i], std::memory_order_relaxed);
            w = (w + 1) % Telemetry::kScope;
        }
    }
    telemetry.scopeWrite.store (w, std::memory_order_release);
    const float r = std::sqrt (sumSq / (2.0f * (float) n));
    const float rmsKeep = fastExp2 (-0.15200309f * ((float) n * (1.0f / (float) kChunk)));   // 0.9^(n/kChunk)
    telemetry.rms.store (flushDenormal (rmsKeep * telemetry.rms.load (std::memory_order_relaxed) + (1.0f - rmsKeep) * r),
                         std::memory_order_relaxed);
}

//==============================================================================
void VesselProcessor::timerCallback()
{
    if (ccAnySyncDirty.exchange (false, std::memory_order_acq_rel))
    {
        for (int i = 0; i < NUM_PARAMS; ++i)
            if (ccSyncDirty[(size_t) i].exchange (false, std::memory_order_acq_rel))
                params[(size_t) i]->setValueNotifyingHost (ccSyncNorm[(size_t) i].load (std::memory_order_relaxed));
    }
    if (midiLearn.consumeDirtyForSave())
        midiLearn.saveToDisk();

    // a table picked while the host is not processing still loads (the audio thread asks as well, at once)
    if (const int t = pc (wt_table); t != tables.currentIndex()) tables.request (t);
    refreshImpulseResponse (false);
}

void VesselProcessor::refreshImpulseResponse (bool forceReload)
{
    // (17) IR (re)generation happens off the audio thread; serialized by irLock so prepareToPlay() on a host
    // worker thread and timerCallback() on the message thread never race on loadedIr* or loadImpulseResponse().
    const juce::ScopedLock sl (irLock);
    if (forceReload) loadedIr = -1;

    const int type = pc (conv_ir);
    const float size = param (conv_size)->convertFrom0to1 (param (conv_size)->getValue());
    const double rate = std::max (8000.0, srAtomic.load (std::memory_order_acquire));

    if (type == 5)
    {
        if (! userIrDirty && loadedIr == 5 && std::abs (rate - loadedIrRate) <= 0.5) return;
        const juce::File file = userIrFile;
        userIrDirty = false;
        if (file.existsAsFile())
            convolution.loadImpulseResponse (file, juce::dsp::Convolution::Stereo::yes,
                                             juce::dsp::Convolution::Trim::yes, 0, juce::dsp::Convolution::Normalise::yes);
        else
        {
            auto silent = ImpulseFactory::make (0, 1.0f, rate);
            silent.clear();
            convolution.loadImpulseResponse (std::move (silent), rate, juce::dsp::Convolution::Stereo::yes,
                                             juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
        }
        loadedIr = 5;
        loadedIrRate = rate;
        return;
    }

    if (type != loadedIr || std::abs (size - loadedIrSize) > 0.01f || std::abs (rate - loadedIrRate) > 0.5)
    {
        auto ir = ImpulseFactory::make (type, size, rate);
        convolution.loadImpulseResponse (std::move (ir), rate, juce::dsp::Convolution::Stereo::yes,
                                         juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::yes);
        loadedIr = type; loadedIrSize = size; loadedIrRate = rate;
    }
}

void VesselProcessor::loadUserImpulse (const juce::File& f)
{
    {
        const juce::ScopedLock sl (irLock);
        userIrFile = f;
        userIrDirty = true;
    }
    if (auto* p = param (conv_ir)) p->setValueNotifyingHost (p->convertTo0to1 (5.0f));
    if (auto* p = param (conv_on)) p->setValueNotifyingHost (1.0f);
}

juce::String VesselProcessor::userImpulseName() const
{
    const juce::ScopedLock sl (irLock);
    return userIrFile.existsAsFile() ? userIrFile.getFileNameWithoutExtension() : juce::String ("No file");
}

void VesselProcessor::randomiseDrawTable (int l)
{
    juce::Random r;
    float prev = r.nextFloat() * 2.0f - 1.0f;
    for (int i = 0; i < kDrawPoints; ++i)
    {
        const float roll = r.nextFloat();
        float v = roll < 0.3f ? prev : (roll < 0.4f ? 0.0f : r.nextFloat() * 2.0f - 1.0f);
        drawTables[(size_t) l][(size_t) i] = v;
        prev = v;
    }
}

float VesselProcessor::previewLfo (int l, float phase) const
{
    // UI thread: reads the parameters directly (the smoothed/modulated values belong to the audio thread)
    const int b0 = lfoBase (l);
    auto plain = [this] (int pid) { return params[(size_t) pid]->convertFrom0to1 (params[(size_t) pid]->getValue()); };
    const float v = lfos[(size_t) l].evaluate (phase, plain (b0 + 4), juce::roundToInt (plain (b0)), drawTables[(size_t) l]);
    return plain (lfoExt (l) + 4) > 0.5f ? 0.5f * (v + 1.0f) : v;
}

//==============================================================================
int VesselProcessor::getNumPrograms() { return PresetBank::factory().size(); }

const juce::String VesselProcessor::getProgramName (int i)
{
    const auto& all = PresetBank::factory().all();
    return juce::isPositiveAndBelow (i, (int) all.size()) ? all[(size_t) i].name : juce::String();
}

void VesselProcessor::setCurrentProgram (int index)
{
    const auto& all = PresetBank::factory().all();
    if (! juce::isPositiveAndBelow (index, (int) all.size())) return;
    currentPreset.store (index, std::memory_order_relaxed);
    applyPreset (all[(size_t) index]);
}

void VesselProcessor::resetDrawTables()
{
    // factory drawn shapes: a falling breath, a two-step limp, a rising stair
    for (int i = 0; i < kDrawPoints; ++i)
    {
        const float t = (float) i / (float) kDrawPoints;
        drawTables[0][(size_t) i] = std::cos (kPi * t) * (1.0f - 0.4f * t);
        drawTables[1][(size_t) i] = (i % 8 < 3) ? 0.9f : -0.4f + 0.1f * (float) (i % 8);
        drawTables[2][(size_t) i] = -1.0f + 2.0f * std::floor (t * 6.0f) / 5.0f;
    }
}

/** Message thread. Every parameter returns to its default, then the preset's own values are applied, so a preset
    is exactly its listed sound. Changes reach the audio thread through the parameters' atomics and are smoothed
    there (20 ms), so switching presets while notes ring does not click. */
void VesselProcessor::applyPreset (const FactoryPreset& p)
{
    beginUndoTransaction ("Load Preset: " + p.name);
    for (int i = 0; i < NUM_PARAMS; ++i)
        params[(size_t) i]->setValueNotifyingHost (params[(size_t) i]->getDefaultValue());
    for (auto [pid, v] : p.values)
        if (pid >= 0 && pid < NUM_PARAMS)
            params[(size_t) pid]->setValueNotifyingHost (params[(size_t) pid]->convertTo0to1 (v));
    resetDrawTables();
    {
        const juce::ScopedLock sl (irLock);
        userIrFile = juce::File();
        userIrDirty = false;
    }
    for (int m = 0; m < kNumMacros; ++m) setMacroName (m, m < p.macroNames.size() ? p.macroNames[m] : juce::String());
    setXyLabels (p.xyLabelX, p.xyLabelY);
    {
        const juce::ScopedLock sl (macroNameLock);
        metaName = p.name; metaAuthor = p.author; metaCategory = p.category;
        metaTags = p.character; metaDescription = p.description; metaFavorite = p.favorite;
    }
    tables.loadNow (pc (wt_table));   // decoded before the next block, so the preset's first note has its table
    refreshImpulseResponse (false);
    flushUndoState();
    const int slot = activeAB.load (std::memory_order_acquire);
    abSlots[(size_t) slot] = captureSnapshot();
    if (! abSlots[(size_t) (1 - slot)].valid) abSlots[(size_t) (1 - slot)] = abSlots[(size_t) slot];
}

FactoryPreset VesselProcessor::captureCurrentPreset (const juce::String& name, const juce::String& author,
                                                     const juce::String& category, const juce::String& tags,
                                                     const juce::String& description, bool favorite) const
{
    FactoryPreset p;
    p.schemaVersion = 2;
    p.name = name.trim().isNotEmpty() ? name.trim() : currentPresetName();
    p.author = author.trim().isNotEmpty() ? author.trim() : currentPresetAuthor();
    p.category = category.trim().isNotEmpty() ? category.trim() : currentPresetCategory();
    p.character = tags.trim().isNotEmpty() ? tags.trim() : currentPresetTags();
    p.description = description.trim().isNotEmpty() ? description.trim() : currentPresetDescription();
    p.favorite = favorite;
    p.xyLabelX = xyLabelX();
    p.xyLabelY = xyLabelY();
    for (int m = 0; m < kNumMacros; ++m) p.macroNames.add (macroName (m));
    for (int i = 0; i < NUM_PARAMS; ++i)
    {
        const float plain = params[(size_t) i]->convertFrom0to1 (params[(size_t) i]->getValue());
        if (std::abs (plain - spec (i).def) > 1.0e-5f)
            p.values.emplace_back (i, plain);
    }
    return p;
}

VesselProcessor::ABSnapshot VesselProcessor::captureSnapshot() const
{
    ABSnapshot s;
    for (int i = 0; i < NUM_PARAMS; ++i) s.normValues[(size_t) i] = params[(size_t) i]->getValue();
    {
        const juce::ScopedLock sl (macroNameLock);
        s.macroNames = macroNames; s.xyX = xyNameX; s.xyY = xyNameY;
        s.presetName = metaName; s.presetAuthor = metaAuthor; s.presetCategory = metaCategory;
        s.presetTags = metaTags; s.presetDescription = metaDescription; s.presetFavorite = metaFavorite;
    }
    for (int l = 0; l < kNumLfos; ++l)
        for (int i = 0; i < kDrawPoints; ++i)
            s.drawTables[(size_t) l][(size_t) i] = drawTables[(size_t) l][(size_t) i].load (std::memory_order_relaxed);
    s.valid = true;
    return s;
}

void VesselProcessor::applySnapshotSmooth (const ABSnapshot& s)
{
    if (! s.valid) return;
    for (int i = 0; i < NUM_PARAMS; ++i)
        if (std::abs (params[(size_t) i]->getValue() - s.normValues[(size_t) i]) > 1.0e-6f)
            params[(size_t) i]->setValueNotifyingHost (s.normValues[(size_t) i]);
    {
        const juce::ScopedLock sl (macroNameLock);
        macroNames = s.macroNames; xyNameX = s.xyX; xyNameY = s.xyY;
        metaName = s.presetName; metaAuthor = s.presetAuthor; metaCategory = s.presetCategory;
        metaTags = s.presetTags; metaDescription = s.presetDescription; metaFavorite = s.presetFavorite;
    }
    for (int l = 0; l < kNumLfos; ++l)
        for (int i = 0; i < kDrawPoints; ++i)
            drawTables[(size_t) l][(size_t) i].store (s.drawTables[(size_t) l][(size_t) i], std::memory_order_relaxed);
    ++macroNamesVersion;
    tables.loadNow (pc (wt_table));
    refreshImpulseResponse (false);
}

void VesselProcessor::setActiveABSlot (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    const int cur = activeAB.load (std::memory_order_acquire);
    if (slot == cur) return;
    abSlots[(size_t) cur] = captureSnapshot();
    if (! abSlots[(size_t) slot].valid) abSlots[(size_t) slot] = abSlots[(size_t) cur];
    activeAB.store (slot, std::memory_order_release);
    beginUndoTransaction (slot == 0 ? "Switch to A" : "Switch to B");
    applySnapshotSmooth (abSlots[(size_t) slot]);
    flushUndoState();
    ++abStateVersion;
}

void VesselProcessor::copyAToB()
{
    const int cur = activeAB.load (std::memory_order_acquire);
    abSlots[(size_t) cur] = captureSnapshot();
    if (! abSlots[0].valid) abSlots[0] = abSlots[(size_t) cur];
    abSlots[1] = abSlots[0];
    if (cur == 1) { beginUndoTransaction ("Copy A to B"); applySnapshotSmooth (abSlots[1]); flushUndoState(); }
    ++abStateVersion;
}

void VesselProcessor::copyBToA()
{
    const int cur = activeAB.load (std::memory_order_acquire);
    abSlots[(size_t) cur] = captureSnapshot();
    if (! abSlots[1].valid) abSlots[1] = abSlots[(size_t) cur];
    abSlots[0] = abSlots[1];
    if (cur == 0) { beginUndoTransaction ("Copy B to A"); applySnapshotSmooth (abSlots[0]); flushUndoState(); }
    ++abStateVersion;
}

void VesselProcessor::beginUndoTransaction (const juce::String& actionName) { (void) apvts.copyState(); undoManager.beginNewTransaction (actionName); }
void VesselProcessor::flushUndoState() { (void) apvts.copyState(); }
bool VesselProcessor::undo() { flushUndoState(); return undoManager.undo(); }
bool VesselProcessor::redo() { return undoManager.redo(); }

void VesselProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("version", 2, nullptr);
    juce::ValueTree draw ("DRAW");
    for (int l = 0; l < kNumLfos; ++l)
    {
        juce::String s;
        for (int i = 0; i < kDrawPoints; ++i) s << juce::String (drawTables[(size_t) l][(size_t) i].load(), 4) << " ";
        draw.setProperty ("lfo" + juce::String (l), s.trim(), nullptr);
    }
    state.removeChild (state.getChildWithName ("DRAW"), nullptr);
    state.appendChild (draw, nullptr);
    {
        const juce::ScopedLock sl (irLock);
        state.setProperty ("userIR", userIrFile.getFullPathName(), nullptr);
    }
    state.setProperty ("preset", currentPreset.load (std::memory_order_relaxed), nullptr);
    state.setProperty ("torch", torchOn.load(), nullptr);
    state.setProperty ("activeAB", activeAB.load (std::memory_order_acquire), nullptr);
    {
        const juce::ScopedLock sl (macroNameLock);
        state.setProperty ("presetName", metaName, nullptr);
        state.setProperty ("presetAuthor", metaAuthor, nullptr);
        state.setProperty ("presetCategory", metaCategory, nullptr);
        state.setProperty ("presetTags", metaTags, nullptr);
        state.setProperty ("presetDescription", metaDescription, nullptr);
        state.setProperty ("presetFavorite", metaFavorite, nullptr);
        state.setProperty ("xyLabelX", xyNameX, nullptr);
        state.setProperty ("xyLabelY", xyNameY, nullptr);
    }
    {
        juce::ValueTree names ("MACROS");
        for (int m = 0; m < kNumMacros; ++m) names.setProperty ("m" + juce::String (m + 1), macroName (m), nullptr);
        state.removeChild (state.getChildWithName ("MACROS"), nullptr);
        state.appendChild (names, nullptr);
    }
    state.removeChild (state.getChildWithName ("MIDILEARN"), nullptr);
    state.appendChild (midiLearn.toValueTree(), nullptr);

    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void VesselProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid()) return;

    // Lossless V1 -> V2 state migration: populate any missing V2 params with legacy-preserving defaults
    std::array<bool, NUM_PARAMS> present {};
    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        const auto child = state.getChild (i);
        if (child.hasType ("PARAM"))
        {
            const int pid = pidForId (child.getProperty ("id").toString());
            if (pid >= 0 && pid < NUM_PARAMS) present[(size_t) pid] = true;
        }
    }
    for (int i = 0; i < NUM_PARAMS; ++i)
    {
        if (! present[(size_t) i])
        {
            juce::ValueTree p ("PARAM");
            p.setProperty ("id", spec (i).id, nullptr);
            p.setProperty ("value", (double) spec (i).def, nullptr);
            state.appendChild (p, nullptr);
            params[(size_t) i]->setValueNotifyingHost (params[(size_t) i]->getDefaultValue());
        }
    }
    state.setProperty ("version", 2, nullptr);

    resetDrawTables();
    if (auto draw = state.getChildWithName ("DRAW"); draw.isValid())
        for (int l = 0; l < kNumLfos; ++l)
        {
            auto tokens = juce::StringArray::fromTokens (draw.getProperty ("lfo" + juce::String (l)).toString(), " ", "");
            for (int i = 0; i < kDrawPoints && i < tokens.size(); ++i)
            {
                const float v = tokens[i].getFloatValue();
                drawTables[(size_t) l][(size_t) i] = std::isfinite (v) ? juce::jlimit (-1.0f, 1.0f, v) : 0.0f;
            }
        }

    const juce::String irPath = state.getProperty ("userIR").toString();
    {
        const juce::ScopedLock sl (irLock);
        userIrFile = irPath.isNotEmpty() ? juce::File (irPath) : juce::File();
        userIrDirty = true;
    }
    const int maxProg = std::max (0, getNumPrograms() - 1);
    currentPreset.store (juce::jlimit (0, maxProg, (int) state.getProperty ("preset", 0)), std::memory_order_relaxed);
    torchOn = (bool) state.getProperty ("torch", false);
    activeAB.store (juce::jlimit (0, 1, (int) state.getProperty ("activeAB", 0)), std::memory_order_release);
    {
        const auto names = state.getChildWithName ("MACROS");
        for (int m = 0; m < kNumMacros; ++m)
            setMacroName (m, names.isValid() ? names.getProperty ("m" + juce::String (m + 1)).toString() : juce::String());
    }
    setXyLabels (state.getProperty ("xyLabelX", "TIMBRE").toString(), state.getProperty ("xyLabelY", "SPACE").toString());
    {
        const juce::ScopedLock sl (macroNameLock);
        metaName = state.getProperty ("presetName", getProgramName (getCurrentProgram())).toString();
        metaAuthor = state.getProperty ("presetAuthor", "iisben").toString();
        metaCategory = state.getProperty ("presetCategory", "Init").toString();
        metaTags = state.getProperty ("presetTags", "").toString();
        metaDescription = state.getProperty ("presetDescription", "").toString();
        metaFavorite = (bool) state.getProperty ("presetFavorite", false);
    }
    if (auto ml = state.getChildWithName ("MIDILEARN"); ml.isValid())
        midiLearn.fromValueTree (ml, false);

    apvts.replaceState (state);
    tables.loadNow (pc (wt_table));
    refreshImpulseResponse (false);
    const int slot = activeAB.load (std::memory_order_acquire);
    abSlots[(size_t) slot] = captureSnapshot();
    abSlots[(size_t) (1 - slot)] = abSlots[(size_t) slot];
    snapPending.store (true, std::memory_order_release);
}

#if VESSEL_HEADLESS
juce::AudioProcessorEditor* VesselProcessor::createEditor() { return nullptr; }
#else
juce::AudioProcessorEditor* VesselProcessor::createEditor() { return new VesselEditor (*this); }
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VesselProcessor(); }
