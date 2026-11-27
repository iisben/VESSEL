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
} // namespace

//==============================================================================
VesselProcessor::VesselProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VESSEL", createLayout())
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
        if (spec (i).type == PType::Float) floatPids.push_back (i);
    }

    resetDrawTables();
    PresetBank::factory();   // parse the factory bank on the message thread
#if ! VESSEL_HEADLESS
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
    vhs.prepare (fs);
    tape.prepare (fs);
    follower.prepare (fs);
    unmask.prepare (fs);
    formant.prepare (fs);
    resonator.prepare (fs);
    zaxis.prepare (fs);
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
    mwRaw = atRaw = velRaw = mwS = atS = velS = bendRaw = bendS = 0.0f;
    scopeDecim = 0.0f; peakHoldL = peakHoldR = 0.0f;
    samplePos = 0; ctrlRem = 0; busRem = 0; bodyQuietSamples = 0; voiceOsIdle = true; busVoiceActive = false; busLowestHeld = -1; midiDirty = false;
    for (auto* f : { &fGran, &fUnmask, &fCrush, &fVhs, &fFollow, &fFormant, &fOtt, &fMono, &fRes }) f->prepare (fs);
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
    return (int) std::lround (ruin.latency() + outStage.latency());
}

void VesselProcessor::setQaDeterministicSeed (uint32_t seed) noexcept
{
    voices.reseed (seed);
    for (int l = 0; l < kNumLfos; ++l) lfos[(size_t) l].reseed (seed ^ (uint32_t) (11 + l * 13));
    drone.reseed (seed);
    noise.reseed (seed);
    granular.reseed (seed);
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
    }
    busMod.fill (0.0f);
    baseReady = true;
    fGran.snap (pb (gran_on)); fUnmask.snap (pb (unmask_on)); fCrush.snap (pb (crush_on)); fVhs.snap (pb (vhs_on));
    fFollow.snap (pb (envf_on)); fFormant.snap (pb (form_on)); fOtt.snap (pb (ott_on)); fMono.snap (pb (mono_on));
    fRes.snap (pb (res_on));
    convMixS.snap (pb (conv_on) ? pv (conv_mix) : 0.0f);
    resonator.snapModel (pc (res_model));
    voices.setMode (pc (voice_mode));   // before any note of the first block (a bounce starting on a note)
    voices.snapFilter (pb (flt_on));
    ruin.snap (pb (tube_on), pb (bias_on));
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

    if (m.isNoteOn())
    {
        const bool fromSilence = ! voices.anyHeld();
        const int note = m.getNoteNumber();
        velRaw = m.getFloatVelocity();
        voices.noteOn (note, velRaw);
        textureBus.noteOn (note, velRaw);
        midiDirty = true;
        if (fromSilence) tape.noteOnFromSilence (tp);
        if (pb (duck_on)) ducker.trigger();
        for (int l = 0; l < kNumLfos; ++l) lfos[(size_t) l].trigger (pc (lfoExt (l)), fromSilence);
        if (vm == 0) resonator.noteOn (note, velRaw);
        else { resonator.monoNote (note, velRaw, vm == 1 || fromSilence); resMonoNote = note; }
    }
    else if (m.isNoteOff())
    {
        voices.noteOff (m.getNoteNumber());
        if (! voices.anyHeld()) tape.allReleased (tp);
        if (vm == 0) resonator.noteOff (m.getNoteNumber());
        else resFollowMono();
    }
    else if (m.isSustainPedalOn())  { voices.setSustain (true); resonator.setSustain (true); }
    else if (m.isSustainPedalOff())
    {
        voices.setSustain (false); resonator.setSustain (false);
        if (! voices.anyHeld()) tape.allReleased (tp);
        if (vm != 0) resFollowMono();
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff()) { voices.allOff(); tape.allReleased (tp); resonator.allOff(); resMonoNote = -1; }
    else if (m.isPitchWheel()) bendRaw = ((float) m.getPitchWheelValue() - 8192.0f) / 8192.0f * 2.0f;
    else if (m.isControllerOfType (1)) mwRaw = (float) m.getControllerValue() / 127.0f;
    else if (m.isChannelPressure()) atRaw = (float) m.getChannelPressureValue() / 127.0f;
    else if (m.isAftertouch()) atRaw = (float) m.getAfterTouchValue() / 127.0f;   // poly pressure drives the same bus
    else if (m.isResetAllControllers()) { mwRaw = atRaw = bendRaw = 0.0f; }
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
    telemetry.modWheel = mwS; telemetry.aftertouch = atS; telemetry.velocity = velS;
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
        }
        const float target = targetNorm[(size_t) i];
        auto& b = base[(size_t) i];
        b = smoothed[(size_t) i] ? target + a * (b - target) : target;
    }

    const float e = std::exp (-(float) n / (0.012f * (float) sr));   // expression: 12 ms (aftertouch is often 7-bit)
    mwS  = mwRaw  + e * (mwS  - mwRaw);
    atS  = atRaw  + e * (atS  - atRaw);
    velS = velRaw + e * (velS - velRaw);
    bendS = bendRaw + e * (bendS - bendRaw);
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
            if (! isVoiceParam (dest)) mod[(size_t) dest] += amt * velS;   // per-voice velocity handled in buildVoiceParams
        }
        else mod[(size_t) dest] += amt * (s < 4 ? mwS : atS);
    }
}

/** The eight macros: amount × knob on each routed destination. Applied after expression (so nothing here depends
    on it) and before the LFOs, so a macro can also set an LFO's rate or warp. */
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

juce::String VesselProcessor::macroName (int m) const
{
    const juce::ScopedLock sl (macroNameLock);
    return juce::isPositiveAndBelow (m, kNumMacros) ? macroNames[(size_t) m] : juce::String();
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
    vp.bend = bendS;
    vp.velAmp = pv (vel_amp);
    vp.organicAmt = pv (v2_organic_amt);

    int vDest[2] { -1, -1 };
    float vAmt[2] { 0.0f, 0.0f };
    for (int s = 0; s < 2; ++s)
    {
        const int d = destIndexToPid (pc (s == 0 ? vel_dest1 : vel_dest2));
        if (d >= 0 && isVoiceParam (d)) { vDest[s] = d; vAmt[s] = pv (s == 0 ? vel_amt1 : vel_amt2); }
    }

    for (int v = 0; v < VoiceManager::kVoices; ++v)
    {
        auto& p = voiceParams[(size_t) v];
        p = vp;
        if (! voices.voiceActive (v)) continue;
        const float vel = voices.voiceVelocity (v);
        for (int s = 0; s < 2; ++s)
        {
            if (vDest[s] < 0) continue;
            float extra = vAmt[s] * vel;
            if (vDest[0] == vDest[1] && s == 1) continue;                    // same target twice: sum once
            if (vDest[0] == vDest[1]) extra = (vAmt[0] + vAmt[1]) * vel;
            setVoiceField (p, vDest[s], pvExtra (vDest[s], extra), tuneCoarse);
        }
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
            }
            else
            {
                voices.snapFilter (pb (flt_on));
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
        voices.snapFilter (pb (flt_on));
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
        rsp.bend = bendS; rsp.glideMs = pc (voice_mode) != 0 ? pvBus (glide) : 0.0f;
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

    // ── SPACE: (18) Z-axis → (17) convolution → (16) black hole → (19) Haas
    zaxis.process (xL.data(), xR.data(), n, pb (z_on), pvBus (z_depth));

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
    for (int i = 0; i < NUM_PARAMS; ++i)
        params[(size_t) i]->setValueNotifyingHost (params[(size_t) i]->getDefaultValue());
    for (auto [pid, v] : p.values)
        params[(size_t) pid]->setValueNotifyingHost (params[(size_t) pid]->convertTo0to1 (v));
    resetDrawTables();
    {
        const juce::ScopedLock sl (irLock);
        userIrFile = juce::File();
        userIrDirty = false;
    }
    for (int m = 0; m < kNumMacros; ++m) setMacroName (m, p.macroNames[m]);
    tables.loadNow (pc (wt_table));   // decoded before the next block, so the preset's first note has its table
    refreshImpulseResponse (false);
}

void VesselProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
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
    {
        juce::ValueTree names ("MACROS");
        for (int m = 0; m < kNumMacros; ++m) names.setProperty ("m" + juce::String (m + 1), macroName (m), nullptr);
        state.removeChild (state.getChildWithName ("MACROS"), nullptr);
        state.appendChild (names, nullptr);
    }

    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void VesselProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid()) return;

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
    {
        const auto names = state.getChildWithName ("MACROS");   // absent in states saved before the macros existed
        for (int m = 0; m < kNumMacros; ++m)
            setMacroName (m, names.isValid() ? names.getProperty ("m" + juce::String (m + 1)).toString() : juce::String());
    }

    apvts.replaceState (state);
    tables.loadNow (pc (wt_table));
    refreshImpulseResponse (false);
    snapPending.store (true, std::memory_order_release);
}

#if VESSEL_HEADLESS
juce::AudioProcessorEditor* VesselProcessor::createEditor() { return nullptr; }
#else
juce::AudioProcessorEditor* VesselProcessor::createEditor() { return new VesselEditor (*this); }
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VesselProcessor(); }
