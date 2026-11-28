#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include "dsp/TableList.h"

/*  VESSEL parameter table.

    One X-macro row per parameter:
        F (id, display name, type, min, max, default, skew centre (0 = linear), step, unit, choices)

    The enum PID and the spec table are both generated from this list, so indices can never drift apart.
    Every Float parameter is automatically a modulation destination for the warped LFOs.            */

namespace vessel
{
enum class PType { Float, Bool, Choice };
enum class Unit  { None, Pct, Bip, Hz, Ms, Sec, Db, St, Ct, X, Vowel };

#define VESSEL_PARAMS(F) \
    /* ── I · SOURCE ── Sub-harmonic generators (1) */ \
    F (sub_on,        "Sub",            Bool,   0, 1, 1, 0, 0, None, "") \
    F (sub1_oct,      "Sub 1 Octave",   Choice, 0, 2, 1, 0, 1, None, "0|-1|-2") \
    F (sub2_oct,      "Sub 2 Octave",   Choice, 0, 2, 0, 0, 1, None, "-1|-2|-3") \
    F (sub1_level,    "Sub 1 Level",    Float,  0, 1, 0.8f, 0, 0, Pct, "") \
    F (sub2_level,    "Sub 2 Level",    Float,  0, 1, 0.45f, 0, 0, Pct, "") \
    F (sub1_shape,    "Sub 1 Shape",    Float,  0, 1, 0.0f, 0, 0, Pct, "") \
    F (sub2_shape,    "Sub 2 Shape",    Float,  0, 1, 0.25f, 0, 0, Pct, "") \
    F (sub_weight,    "Sub Weight",     Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    F (sub_send,      "Sub to Ruin",    Float,  0, 1, 0.15f, 0, 0, Pct, "") \
    /* Analog wavetable oscillator (5) */ \
    F (wt_on,         "Wavetable",      Bool,   0, 1, 1, 0, 0, None, "") \
    F (wt_level,      "WT Level",       Float,  0, 1, 0.55f, 0, 0, Pct, "") \
    F (wt_morph,      "WT Morph",       Float,  0, 1, 0.35f, 0, 0, Pct, "") \
    F (wt_pw,         "WT Pulse Width", Float,  0.05f, 0.95f, 0.5f, 0, 0, Pct, "") \
    F (wt_pwm,        "WT PWM Depth",   Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    F (wt_pwm_rate,   "WT PWM Rate",    Float,  0.02f, 12, 0.35f, 1, 0, Hz, "") \
    F (wt_oct,        "WT Octave",      Choice, 0, 4, 2, 0, 1, None, "-2|-1|0|+1|+2") \
    F (wt_semi,       "WT Semitone",    Float,  -12, 12, 0, 0, 1, St, "") \
    F (wt_fine,       "WT Fine",        Float,  -100, 100, 0, 0, 0, Ct, "") \
    F (wt_drift,      "WT Drift",       Float,  0, 1, 0.25f, 0, 0, Pct, "") \
    /* Cinematic drone engine (3) */ \
    F (drone_on,      "Drone",          Bool,   0, 1, 1, 0, 0, None, "") \
    F (drone_level,   "Drone Level",    Float,  0, 1, 0.35f, 0, 0, Pct, "") \
    F (drone_oct,     "Drone Octave",   Choice, 0, 2, 1, 0, 1, None, "-2|-1|0") \
    F (drone_tone,    "Drone Tone",     Float,  0, 1, 0.4f, 0, 0, Pct, "") \
    F (drone_motion,  "Drone Motion",   Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    F (drone_spread,  "Drone Spread",   Float,  0, 1, 0.6f, 0, 0, Pct, "") \
    F (drone_attack,  "Drone Attack",   Float,  0.05f, 20, 3, 2, 0, Sec, "") \
    F (drone_release, "Drone Release",  Float,  0.05f, 30, 6, 3, 0, Sec, "") \
    F (drone_hold,    "Drone Hold",     Bool,   0, 1, 0, 0, 0, None, "") \
    /* Dust & vinyl noise injector (4) */ \
    F (noise_on,      "Noise",          Bool,   0, 1, 1, 0, 0, None, "") \
    F (noise_hiss,    "Tape Hiss",      Float,  0, 1, 0.12f, 0, 0, Pct, "") \
    F (noise_crackle, "Crackle",        Float,  0, 1, 0.15f, 0, 0, Pct, "") \
    F (noise_hum,     "Hum",            Float,  0, 1, 0.0f, 0, 0, Pct, "") \
    F (noise_hum_freq,"Hum Mains",      Choice, 0, 1, 0, 0, 1, None, "50 Hz|60 Hz") \
    F (noise_tone,    "Noise Tone",     Float,  0, 1, 0.55f, 0, 0, Pct, "") \
    F (noise_dyn,     "Noise Dynamic",  Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    /* Granular micro-looper (2) */ \
    F (gran_on,       "Granular",       Bool,   0, 1, 0, 0, 0, None, "") \
    F (gran_freeze,   "Grain Freeze",   Bool,   0, 1, 0, 0, 0, None, "") \
    F (gran_size,     "Grain Size",     Float,  10, 500, 90, 80, 0, Ms, "") \
    F (gran_density,  "Grain Density",  Float,  1, 80, 18, 15, 0, Hz, "") \
    F (gran_pitch,    "Grain Pitch",    Float,  -24, 24, 0, 0, 0, St, "") \
    F (gran_stretch,  "Grain Stretch",  Float,  0, 2, 1, 0, 0, X, "") \
    F (gran_spray,    "Grain Spray",    Float,  0, 1, 0.15f, 0, 0, Pct, "") \
    F (gran_reverse,  "Grain Reverse",  Float,  0, 1, 0.0f, 0, 0, Pct, "") \
    F (gran_div,      "Stutter Div",    Choice, 0, 5, 2, 0, 1, None, "1/4|1/8|1/16|1/32|1/8T|1/16T") \
    F (gran_chance,   "Stutter Chance", Float,  0, 1, 0.0f, 0, 0, Pct, "") \
    F (gran_mix,      "Grain Mix",      Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    /* ── Voice ── */ \
    F (voice_mode,    "Voice Mode",     Choice, 0, 2, 0, 0, 1, None, "Poly|Mono|Legato") \
    F (glide,         "Glide",          Float,  0, 2000, 0, 200, 0, Ms, "") \
    F (amp_a,         "Amp Attack",     Float,  0.5f, 10000, 5, 300, 0, Ms, "") \
    F (amp_d,         "Amp Decay",      Float,  1, 10000, 400, 500, 0, Ms, "") \
    F (amp_s,         "Amp Sustain",    Float,  0, 1, 0.8f, 0, 0, Pct, "") \
    F (amp_r,         "Amp Release",    Float,  5, 15000, 600, 800, 0, Ms, "") \
    /* ── II · RUIN ── Pre/post distortion EQ (15) */ \
    F (pre_tilt,      "Pre Tilt",       Float,  -12, 12, 0, 0, 0, Db, "") \
    F (pre_pivot,     "Pre Pivot",      Float,  100, 8000, 800, 800, 0, Hz, "") \
    F (post_tilt,     "Post Tilt",      Float,  -12, 12, 0, 0, 0, Db, "") \
    F (post_pivot,    "Post Pivot",     Float,  100, 8000, 1200, 800, 0, Hz, "") \
    /* Asymmetric tube drive (6) */ \
    F (tube_on,       "Tube",           Bool,   0, 1, 1, 0, 0, None, "") \
    F (tube_mode,     "Tube Mode",      Choice, 0, 2, 0, 0, 1, None, "Triode|Pentode|Starved") \
    F (tube_drive,    "Tube Drive",     Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    F (tube_bias,     "Tube Bias",      Float,  -1, 1, 0.15f, 0, 0, Bip, "") \
    F (tube_mix,      "Tube Mix",       Float,  0, 1, 1, 0, 0, Pct, "") \
    F (tube_out,      "Tube Output",    Float,  -24, 12, 0, 0, 0, Db, "") \
    /* Analog bias modulator (10) */ \
    F (bias_on,       "Bias",           Bool,   0, 1, 0, 0, 0, None, "") \
    F (bias_amount,   "Bias Offset",    Float,  -1, 1, 0.2f, 0, 0, Bip, "") \
    F (bias_sag,      "Bias Sag",       Float,  0, 1, 0.35f, 0, 0, Pct, "") \
    F (bias_sat,      "Tape Saturation",Float,  0, 1, 0.4f, 0, 0, Pct, "") \
    F (bias_speed,    "Sag Recovery",   Float,  5, 500, 80, 60, 0, Ms, "") \
    /* Bit crusher / SRR (9) */ \
    F (crush_on,      "Crush",          Bool,   0, 1, 0, 0, 0, None, "") \
    F (crush_bits,    "Bit Depth",      Float,  1, 16, 12, 0, 0, None, "") \
    F (crush_rate,    "Sample Rate",    Float,  250, 48000, 22050, 6000, 0, Hz, "") \
    F (crush_pre,     "Crush Pre Filter",  Float, 0, 1, 0.0f, 0, 0, Pct, "") \
    F (crush_post,    "Crush Post Filter", Float, 0, 1, 0.3f, 0, 0, Pct, "") \
    F (crush_mix,     "Crush Mix",      Float,  0, 1, 1, 0, 0, Pct, "") \
    /* VHS wow & flutter (7) */ \
    F (vhs_on,        "VHS",            Bool,   0, 1, 1, 0, 0, None, "") \
    F (vhs_wow,       "Wow",            Float,  0, 1, 0.2f, 0, 0, Pct, "") \
    F (vhs_flutter,   "Flutter",        Float,  0, 1, 0.15f, 0, 0, Pct, "") \
    F (vhs_drift,     "Drift",          Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    F (vhs_rate,      "Wow Rate",       Float,  0.1f, 4, 0.6f, 0.8f, 0, Hz, "") \
    F (vhs_age,       "Tape Age",       Float,  0, 1, 0.25f, 0, 0, Pct, "") \
    /* Tape start/stop (8) */ \
    F (tape_mode,     "Tape Motor",     Choice, 0, 3, 0, 0, 1, None, "Off|Start|Stop|Start+Stop") \
    F (tape_start,    "Start Time",     Float,  20, 3000, 350, 400, 0, Ms, "") \
    F (tape_stop,     "Stop Time",      Float,  50, 6000, 900, 800, 0, Ms, "") \
    /* ── III · SHAPE ── Dual ladder filters (11) */ \
    F (flt_on,        "Ladders",        Bool,   0, 1, 1, 0, 0, None, "") \
    F (flt_route,     "Ladder Routing", Choice, 0, 2, 0, 0, 1, None, "Serial|Parallel|A Only") \
    F (flt_a_cut,     "Ladder A Cutoff",Float,  20, 20000, 1800, 1000, 0, Hz, "") \
    F (flt_a_res,     "Ladder A Res",   Float,  0, 1, 0.35f, 0, 0, Pct, "") \
    F (flt_b_cut,     "Ladder B Cutoff",Float,  20, 20000, 5000, 1000, 0, Hz, "") \
    F (flt_b_res,     "Ladder B Res",   Float,  0, 1, 0.2f, 0, 0, Pct, "") \
    F (flt_drive,     "Ladder Drive",   Float,  0, 1, 0.25f, 0, 0, Pct, "") \
    F (flt_env,       "Ladder Env Amt", Float,  -1, 1, 0.35f, 0, 0, Bip, "") \
    F (flt_key,       "Key Track",      Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    F (fenv_a,        "Filter Attack",  Float,  0.5f, 10000, 8, 300, 0, Ms, "") \
    F (fenv_d,        "Filter Decay",   Float,  1, 10000, 600, 500, 0, Ms, "") \
    F (fenv_s,        "Filter Sustain", Float,  0, 1, 0.2f, 0, 0, Pct, "") \
    F (fenv_r,        "Filter Release", Float,  5, 15000, 800, 800, 0, Ms, "") \
    /* Envelope follower LPF (12) */ \
    F (envf_on,       "Follower LPF",   Bool,   0, 1, 0, 0, 0, None, "") \
    F (envf_cut,      "Follower Base",  Float,  20, 20000, 300, 1000, 0, Hz, "") \
    F (envf_depth,    "Follower Depth", Float,  -1, 1, 0.6f, 0, 0, Bip, "") \
    F (envf_res,      "Follower Res",   Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    F (envf_attack,   "Follower Attack",Float,  0.1f, 200, 5, 20, 0, Ms, "") \
    F (envf_release,  "Follower Release",Float, 5, 2000, 180, 200, 0, Ms, "") \
    F (envf_mix,      "Follower Mix",   Float,  0, 1, 1, 0, 0, Pct, "") \
    /* Spectral unmasking (13) */ \
    F (unmask_on,     "Unmask",         Bool,   0, 1, 1, 0, 0, None, "") \
    F (unmask_freq,   "Unmask Freq",    Float,  30, 300, 110, 100, 0, Hz, "") \
    F (unmask_depth,  "Unmask Depth",   Float,  0, 24, 9, 0, 0, Db, "") \
    F (unmask_release,"Unmask Release", Float,  10, 600, 120, 120, 0, Ms, "") \
    /* Formant filter (14) */ \
    F (form_on,       "Formant",        Bool,   0, 1, 0, 0, 0, None, "") \
    F (form_vowel,    "Vowel",          Float,  0, 4, 0, 0, 0, Vowel, "") \
    F (form_body,     "Cello Body",     Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    F (form_shift,    "Formant Shift",  Float,  -12, 12, 0, 0, 0, St, "") \
    F (form_res,      "Formant Res",    Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    F (form_mix,      "Formant Mix",    Float,  0, 1, 0.6f, 0, 0, Pct, "") \
    /* ── IV · SPACE ── Z-axis micro-delay (18) */ \
    F (z_on,          "Z-Axis",         Bool,   0, 1, 1, 0, 0, None, "") \
    F (z_depth,       "Z Depth",        Float,  -1, 1, 0, 0, 0, Bip, "") \
    /* Impulse convolution (17) */ \
    F (conv_on,       "Convolution",    Bool,   0, 1, 0, 0, 0, None, "") \
    F (conv_ir,       "Space",          Choice, 0, 5, 0, 0, 1, None, "Concrete Pipe|Old TV Speaker|Stairwell|Water Tank|Cassette Shell|User IR") \
    F (conv_size,     "Space Size",     Float,  0.5f, 2, 1, 1, 0, X, "") \
    F (conv_mix,      "Space Mix",      Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    /* Black hole reverb (16) */ \
    F (verb_on,       "Black Hole",     Bool,   0, 1, 1, 0, 0, None, "") \
    F (verb_size,     "Hole Size",      Float,  0, 1, 0.7f, 0, 0, Pct, "") \
    F (verb_decay,    "Hole Decay",     Float,  0.3f, 60, 8, 6, 0, Sec, "") \
    F (verb_inf,      "Infinite",       Bool,   0, 1, 0, 0, 0, None, "") \
    F (verb_damp,     "Hole Damping",   Float,  0, 1, 0.45f, 0, 0, Pct, "") \
    F (verb_mod,      "Hole Motion",    Float,  0, 1, 0.4f, 0, 0, Pct, "") \
    F (verb_pre,      "Pre-Delay",      Float,  0, 500, 30, 80, 0, Ms, "") \
    F (verb_low,      "Hole Low Cut",   Float,  20, 1000, 120, 150, 0, Hz, "") \
    F (verb_mix,      "Hole Mix",       Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    /* Haas widener (19) */ \
    F (haas_on,       "Haas",           Bool,   0, 1, 0, 0, 0, None, "") \
    F (haas_time,     "Haas Time",      Float,  0, 35, 12, 0, 0, Ms, "") \
    F (haas_width,    "Haas Width",     Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    F (haas_xover,    "Haas Crossover", Float,  80, 1500, 300, 300, 0, Hz, "") \
    /* ── V · PRESSURE ── Multiband up/down compression (21) */ \
    F (ott_on,        "Multiband",      Bool,   0, 1, 0, 0, 0, None, "") \
    F (ott_depth,     "MB Depth",       Float,  0, 1, 0.4f, 0, 0, Pct, "") \
    F (ott_time,      "MB Time",        Float,  0.1f, 10, 1, 1, 0, X, "") \
    F (ott_up,        "MB Upward",      Float,  0, 1, 0.6f, 0, 0, Pct, "") \
    F (ott_down,      "MB Downward",    Float,  0, 1, 0.8f, 0, 0, Pct, "") \
    F (ott_lox,       "MB Low X-Over",  Float,  40, 600, 120, 150, 0, Hz, "") \
    F (ott_hix,       "MB High X-Over", Float,  1000, 12000, 2500, 3000, 0, Hz, "") \
    F (ott_low,       "MB Low Gain",    Float,  -12, 12, 0, 0, 0, Db, "") \
    F (ott_mid,       "MB Mid Gain",    Float,  -12, 12, 0, 0, 0, Db, "") \
    F (ott_high,      "MB High Gain",   Float,  -12, 12, 0, 0, 0, Db, "") \
    F (ott_in,        "MB Input",       Float,  -12, 12, 0, 0, 0, Db, "") \
    F (ott_out,       "MB Output",      Float,  -12, 12, 0, 0, 0, Db, "") \
    /* M/S spatial collapse (22) */ \
    F (mono_on,       "Mono Collapse",  Bool,   0, 1, 1, 0, 0, None, "") \
    F (mono_freq,     "Mono Below",     Float,  20, 500, 120, 120, 0, Hz, "") \
    /* Sidechain ducker (23) */ \
    F (duck_on,       "Ducker",         Bool,   0, 1, 1, 0, 0, None, "") \
    F (duck_mode,     "Duck Mode",      Choice, 0, 1, 0, 0, 1, None, "Follow|Pump") \
    F (duck_depth,    "Duck Depth",     Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    F (duck_release,  "Duck Release",   Float,  20, 1500, 180, 200, 0, Ms, "") \
    /* Harmonic exciter (24) */ \
    F (exc_on,        "Exciter",        Bool,   0, 1, 0, 0, 0, None, "") \
    F (exc_freq,      "Exciter Freq",   Float,  1500, 16000, 6000, 5000, 0, Hz, "") \
    F (exc_amount,    "Exciter Amount", Float,  0, 1, 0.25f, 0, 0, Pct, "") \
    F (exc_harm,      "Exciter Harmonics", Float, 0, 1, 0.4f, 0, 0, Pct, "") \
    /* Brickwall hard clipper (25) + master */ \
    F (clip_on,       "Clipper",        Bool,   0, 1, 1, 0, 0, None, "") \
    F (clip_ceiling,  "Ceiling",        Float,  -12, 0, -0.3f, 0, 0, Db, "") \
    F (out_gain,      "Output",         Float,  -24, 12, 0, 0, 0, Db, "") \
    F (in_level,      "Input",          Float,  0, 1, 0, 0, 0, Pct, "") \
    /* ── MOD ── Warped LFOs (20) */ \
    F (lfo1_shape,    "LFO 1 Shape",    Choice, 0, 10, 0, 0, 1, None, "Sine|Triangle|Saw|Square|S&H|Glitch|Drawn|Ramp Down|Wander|Pump|Stairs") \
    F (lfo1_rate,     "LFO 1 Rate",     Float,  0.01f, 40, 0.5f, 1, 0, Hz, "") \
    F (lfo1_sync,     "LFO 1 Sync",     Bool,   0, 1, 0, 0, 0, None, "") \
    F (lfo1_div,      "LFO 1 Division", Choice, 0, 12, 4, 0, 1, None, "4 bar|2 bar|1 bar|1/2|1/4|1/8|1/16|1/32|1/4T|1/8T|1/16T|1/4D|1/8D") \
    F (lfo1_warp,     "LFO 1 Warp",     Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo1_dest1,    "LFO 1 Dest A",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo1_amt1,     "LFO 1 Amt A",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo1_dest2,    "LFO 1 Dest B",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo1_amt2,     "LFO 1 Amt B",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo2_shape,    "LFO 2 Shape",    Choice, 0, 10, 5, 0, 1, None, "Sine|Triangle|Saw|Square|S&H|Glitch|Drawn|Ramp Down|Wander|Pump|Stairs") \
    F (lfo2_rate,     "LFO 2 Rate",     Float,  0.01f, 40, 2.0f, 1, 0, Hz, "") \
    F (lfo2_sync,     "LFO 2 Sync",     Bool,   0, 1, 1, 0, 0, None, "") \
    F (lfo2_div,      "LFO 2 Division", Choice, 0, 12, 6, 0, 1, None, "4 bar|2 bar|1 bar|1/2|1/4|1/8|1/16|1/32|1/4T|1/8T|1/16T|1/4D|1/8D") \
    F (lfo2_warp,     "LFO 2 Warp",     Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo2_dest1,    "LFO 2 Dest A",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo2_amt1,     "LFO 2 Amt A",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo2_dest2,    "LFO 2 Dest B",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo2_amt2,     "LFO 2 Amt B",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo3_shape,    "LFO 3 Shape",    Choice, 0, 10, 6, 0, 1, None, "Sine|Triangle|Saw|Square|S&H|Glitch|Drawn|Ramp Down|Wander|Pump|Stairs") \
    F (lfo3_rate,     "LFO 3 Rate",     Float,  0.01f, 40, 0.12f, 1, 0, Hz, "") \
    F (lfo3_sync,     "LFO 3 Sync",     Bool,   0, 1, 0, 0, 0, None, "") \
    F (lfo3_div,      "LFO 3 Division", Choice, 0, 12, 2, 0, 1, None, "4 bar|2 bar|1 bar|1/2|1/4|1/8|1/16|1/32|1/4T|1/8T|1/16T|1/4D|1/8D") \
    F (lfo3_warp,     "LFO 3 Warp",     Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo3_dest1,    "LFO 3 Dest A",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo3_amt1,     "LFO 3 Amt A",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo3_dest2,    "LFO 3 Dest B",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo3_amt2,     "LFO 3 Amt B",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    /* ── EXPRESSION ── per-preset performance routing (no panel: set by presets, automatable from the host) */ \
    F (vel_amp,       "Velocity to Amp",Float,  0, 1, 0.65f, 0, 0, Pct, "") \
    F (vel_dest1,     "Velocity Dest A",Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (vel_amt1,      "Velocity Amt A", Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (vel_dest2,     "Velocity Dest B",Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (vel_amt2,      "Velocity Amt B", Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mw_dest1,      "Mod Wheel Dest A",Choice,0, 1, 0, 0, 1, None, "@dest") \
    F (mw_amt1,       "Mod Wheel Amt A",Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mw_dest2,      "Mod Wheel Dest B",Choice,0, 1, 0, 0, 1, None, "@dest") \
    F (mw_amt2,       "Mod Wheel Amt B",Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (at_dest1,      "Aftertouch Dest A",Choice,0, 1, 0, 0, 1, None, "@dest") \
    F (at_amt1,       "Aftertouch Amt A",Float, -1, 1, 0, 0, 0, Bip, "") \
    F (at_dest2,      "Aftertouch Dest B",Choice,0, 1, 0, 0, 1, None, "@dest") \
    F (at_amt2,       "Aftertouch Amt B",Float, -1, 1, 0, 0, 0, Bip, "") \
    /* ── VII · MOD (appended) ── LFO trigger, phase, fade, smoothing, polarity and routes C/D (20) */ \
    F (lfo1_mode,     "LFO 1 Trigger",  Choice, 0, 2, 0, 0, 1, None, "Free|Retrig|One-shot") \
    F (lfo1_phase,    "LFO 1 Phase",    Float,  0, 1, 0, 0, 0, Pct, "") \
    F (lfo1_fade,     "LFO 1 Fade In",  Float,  0, 10, 0, 1, 0, Sec, "") \
    F (lfo1_smooth,   "LFO 1 Smooth",   Float,  0, 1, 0, 0, 0, Pct, "") \
    F (lfo1_uni,      "LFO 1 Unipolar", Bool,   0, 1, 0, 0, 0, None, "") \
    F (lfo1_dest3,    "LFO 1 Dest C",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo1_amt3,     "LFO 1 Amt C",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo1_dest4,    "LFO 1 Dest D",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo1_amt4,     "LFO 1 Amt D",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo2_mode,     "LFO 2 Trigger",  Choice, 0, 2, 0, 0, 1, None, "Free|Retrig|One-shot") \
    F (lfo2_phase,    "LFO 2 Phase",    Float,  0, 1, 0, 0, 0, Pct, "") \
    F (lfo2_fade,     "LFO 2 Fade In",  Float,  0, 10, 0, 1, 0, Sec, "") \
    F (lfo2_smooth,   "LFO 2 Smooth",   Float,  0, 1, 0, 0, 0, Pct, "") \
    F (lfo2_uni,      "LFO 2 Unipolar", Bool,   0, 1, 0, 0, 0, None, "") \
    F (lfo2_dest3,    "LFO 2 Dest C",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo2_amt3,     "LFO 2 Amt C",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo2_dest4,    "LFO 2 Dest D",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo2_amt4,     "LFO 2 Amt D",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo3_mode,     "LFO 3 Trigger",  Choice, 0, 2, 0, 0, 1, None, "Free|Retrig|One-shot") \
    F (lfo3_phase,    "LFO 3 Phase",    Float,  0, 1, 0, 0, 0, Pct, "") \
    F (lfo3_fade,     "LFO 3 Fade In",  Float,  0, 10, 0, 1, 0, Sec, "") \
    F (lfo3_smooth,   "LFO 3 Smooth",   Float,  0, 1, 0, 0, 0, Pct, "") \
    F (lfo3_uni,      "LFO 3 Unipolar", Bool,   0, 1, 0, 0, 0, None, "") \
    F (lfo3_dest3,    "LFO 3 Dest C",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo3_amt3,     "LFO 3 Amt C",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (lfo3_dest4,    "LFO 3 Dest D",   Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (lfo3_amt4,     "LFO 3 Amt D",    Float,  -1, 1, 0, 0, 0, Bip, "") \
    /* ── IV · SHAPE (appended) ── Physical-model resonator (26) */ \
    F (res_on,        "Resonator",      Bool,   0, 1, 0, 0, 0, None, "") \
    F (res_model,     "Res Model",      Choice, 0, 7, 0, 0, 1, None, "String|Tube|Bell|Bar|Membrane|Plate|Bowl|Gong") \
    F (res_tune,      "Res Tune",       Float,  -24, 24, 0, 0, 0, St, "") \
    F (res_decay,     "Res Decay",      Float,  0.05f, 20, 1.5f, 1.5f, 0, Sec, "") \
    F (res_bright,    "Res Brightness", Float,  0, 1, 0.55f, 0, 0, Pct, "") \
    F (res_stiff,     "Res Stiffness",  Float,  0, 1, 0.15f, 0, 0, Pct, "") \
    F (res_pos,       "Res Position",   Float,  0.02f, 0.5f, 0.18f, 0, 0, Pct, "") \
    F (res_damper,    "Res Damper",     Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    F (res_input,     "Res Input",      Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    F (res_strike,    "Res Strike",     Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    F (res_mix,       "Res Mix",        Float,  0, 1, 0.5f, 0, 0, Pct, "") \
    /* ── I · SOURCE (appended) ── Wavetable library: 0 = the built-in table, then the imported ones */ \
    F (wt_table,      "WT Table",       Choice, 0, dsp::kNumTables, 0, 0, 1, None, "@tables") \
    /* FM operator (27): a sine modulator phase-modulating the wavetable oscillator */ \
    F (fm_on,         "FM",             Bool,   0, 1, 0, 0, 0, None, "") \
    F (fm_ratio,      "FM Ratio",       Choice, 0, 16, 3, 0, 1, None, "1/4|1/2|3/4|1|1.5|2|2.5|3|3.5|4|5|6|7|8|9|11|14") \
    F (fm_fine,       "FM Fine",        Float,  -100, 100, 0, 0, 0, Ct, "") \
    F (fm_index,      "FM Index",       Float,  0, 1, 0.3f, 0, 0, Pct, "") \
    F (fm_feedback,   "FM Feedback",    Float,  0, 1, 0, 0, 0, Pct, "") \
    F (fm_env,        "FM Env",         Float,  0, 1, 0, 0, 0, Pct, "") \
    F (fm_decay,      "FM Decay",       Float,  0.005f, 8, 0.4f, 0.5f, 0, Sec, "") \
    F (fm_key,        "FM Key Track",   Float,  0, 1, 0, 0, 0, Pct, "") \
    /* ── MACROS (appended) ── eight named performance macros, six routes each (amount × macro, like expression) */ \
    F (macro1,        "Macro 1",        Float,  0, 1, 0, 0, 0, Pct, "") \
    F (macro2,        "Macro 2",        Float,  0, 1, 0, 0, 0, Pct, "") \
    F (macro3,        "Macro 3",        Float,  0, 1, 0, 0, 0, Pct, "") \
    F (macro4,        "Macro 4",        Float,  0, 1, 0, 0, 0, Pct, "") \
    F (macro5,        "Macro 5",        Float,  0, 1, 0, 0, 0, Pct, "") \
    F (macro6,        "Macro 6",        Float,  0, 1, 0, 0, 0, Pct, "") \
    F (macro7,        "Macro 7",        Float,  0, 1, 0, 0, 0, Pct, "") \
    F (macro8,        "Macro 8",        Float,  0, 1, 0, 0, 0, Pct, "") \
    F (mac1_dest1,    "Macro 1 Dest A", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac1_amt1,     "Macro 1 Amt A",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac1_dest2,    "Macro 1 Dest B", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac1_amt2,     "Macro 1 Amt B",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac1_dest3,    "Macro 1 Dest C", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac1_amt3,     "Macro 1 Amt C",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac1_dest4,    "Macro 1 Dest D", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac1_amt4,     "Macro 1 Amt D",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac1_dest5,    "Macro 1 Dest E", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac1_amt5,     "Macro 1 Amt E",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac1_dest6,    "Macro 1 Dest F", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac1_amt6,     "Macro 1 Amt F",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac2_dest1,    "Macro 2 Dest A", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac2_amt1,     "Macro 2 Amt A",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac2_dest2,    "Macro 2 Dest B", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac2_amt2,     "Macro 2 Amt B",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac2_dest3,    "Macro 2 Dest C", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac2_amt3,     "Macro 2 Amt C",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac2_dest4,    "Macro 2 Dest D", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac2_amt4,     "Macro 2 Amt D",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac2_dest5,    "Macro 2 Dest E", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac2_amt5,     "Macro 2 Amt E",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac2_dest6,    "Macro 2 Dest F", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac2_amt6,     "Macro 2 Amt F",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac3_dest1,    "Macro 3 Dest A", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac3_amt1,     "Macro 3 Amt A",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac3_dest2,    "Macro 3 Dest B", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac3_amt2,     "Macro 3 Amt B",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac3_dest3,    "Macro 3 Dest C", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac3_amt3,     "Macro 3 Amt C",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac3_dest4,    "Macro 3 Dest D", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac3_amt4,     "Macro 3 Amt D",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac3_dest5,    "Macro 3 Dest E", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac3_amt5,     "Macro 3 Amt E",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac3_dest6,    "Macro 3 Dest F", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac3_amt6,     "Macro 3 Amt F",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac4_dest1,    "Macro 4 Dest A", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac4_amt1,     "Macro 4 Amt A",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac4_dest2,    "Macro 4 Dest B", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac4_amt2,     "Macro 4 Amt B",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac4_dest3,    "Macro 4 Dest C", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac4_amt3,     "Macro 4 Amt C",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac4_dest4,    "Macro 4 Dest D", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac4_amt4,     "Macro 4 Amt D",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac4_dest5,    "Macro 4 Dest E", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac4_amt5,     "Macro 4 Amt E",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac4_dest6,    "Macro 4 Dest F", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac4_amt6,     "Macro 4 Amt F",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac5_dest1,    "Macro 5 Dest A", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac5_amt1,     "Macro 5 Amt A",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac5_dest2,    "Macro 5 Dest B", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac5_amt2,     "Macro 5 Amt B",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac5_dest3,    "Macro 5 Dest C", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac5_amt3,     "Macro 5 Amt C",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac5_dest4,    "Macro 5 Dest D", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac5_amt4,     "Macro 5 Amt D",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac5_dest5,    "Macro 5 Dest E", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac5_amt5,     "Macro 5 Amt E",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac5_dest6,    "Macro 5 Dest F", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac5_amt6,     "Macro 5 Amt F",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac6_dest1,    "Macro 6 Dest A", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac6_amt1,     "Macro 6 Amt A",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac6_dest2,    "Macro 6 Dest B", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac6_amt2,     "Macro 6 Amt B",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac6_dest3,    "Macro 6 Dest C", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac6_amt3,     "Macro 6 Amt C",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac6_dest4,    "Macro 6 Dest D", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac6_amt4,     "Macro 6 Amt D",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac6_dest5,    "Macro 6 Dest E", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac6_amt5,     "Macro 6 Amt E",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac6_dest6,    "Macro 6 Dest F", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac6_amt6,     "Macro 6 Amt F",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac7_dest1,    "Macro 7 Dest A", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac7_amt1,     "Macro 7 Amt A",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac7_dest2,    "Macro 7 Dest B", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac7_amt2,     "Macro 7 Amt B",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac7_dest3,    "Macro 7 Dest C", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac7_amt3,     "Macro 7 Amt C",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac7_dest4,    "Macro 7 Dest D", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac7_amt4,     "Macro 7 Amt D",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac7_dest5,    "Macro 7 Dest E", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac7_amt5,     "Macro 7 Amt E",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac7_dest6,    "Macro 7 Dest F", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac7_amt6,     "Macro 7 Amt F",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac8_dest1,    "Macro 8 Dest A", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac8_amt1,     "Macro 8 Amt A",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac8_dest2,    "Macro 8 Dest B", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac8_amt2,     "Macro 8 Amt B",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac8_dest3,    "Macro 8 Dest C", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac8_amt3,     "Macro 8 Amt C",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac8_dest4,    "Macro 8 Dest D", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac8_amt4,     "Macro 8 Amt D",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac8_dest5,    "Macro 8 Dest E", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac8_amt5,     "Macro 8 Amt E",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    F (mac8_dest6,    "Macro 8 Dest F", Choice, 0, 1, 0, 0, 1, None, "@dest") \
    F (mac8_amt6,     "Macro 8 Amt F",  Float,  -1, 1, 0, 0, 0, Bip, "") \
    /* ── V2 Dark Analog Organic Layer & Procedural Texture Bus (append-only, 0% default) ── */ \
    F (v2_organic_amt,   "Organic Drift",  Float,  0, 1, 0.0f,  0, 0, Pct, "") \
    F (v2_organic_floor, "Thermal Floor",  Float,  0, 1, 0.0f,  0, 0, Pct, "") \
    F (v2_organic_mono,  "Sub-120 Anchor", Bool,   0, 1, 0,     0, 0, None, "") \
    F (v2_tex_mode,      "Texture Mode",   Choice, 0, 2, 0,     0, 1, None, "Tape Air|Sub Weight|Metal Exciter") \
    F (v2_tex_mix,       "Texture Mix",    Float,  0, 1, 0.0f,  0, 0, Pct, "") \
    F (v2_tex_tone,      "Texture Tone",   Float,  0, 1, 0.5f,  0, 0, Pct, "") \
    F (v2_tex_drive,     "Texture Drive",  Float,  0, 1, 0.35f, 0, 0, Pct, "") \
    /* ── V2 Control Plane: XY Sculptor, MPE, Velocity Curve, Slide & Breath (append-only, legacy-preserving defaults) ── */ \
    F (v2_xy_x,          "XY X",           Float,  0, 1, 0.5f,  0, 0, Pct, "") \
    F (v2_xy_y,          "XY Y",           Float,  0, 1, 0.5f,  0, 0, Pct, "") \
    F (v2_xy_x_dest1,    "XY X Dest A",    Choice, 0, 1, 0,     0, 1, None, "@dest") \
    F (v2_xy_x_amt1,     "XY X Amt A",     Float, -1, 1, 0.0f,  0, 0, Bip, "") \
    F (v2_xy_x_dest2,    "XY X Dest B",    Choice, 0, 1, 0,     0, 1, None, "@dest") \
    F (v2_xy_x_amt2,     "XY X Amt B",     Float, -1, 1, 0.0f,  0, 0, Bip, "") \
    F (v2_xy_y_dest1,    "XY Y Dest A",    Choice, 0, 1, 0,     0, 1, None, "@dest") \
    F (v2_xy_y_amt1,     "XY Y Amt A",     Float, -1, 1, 0.0f,  0, 0, Bip, "") \
    F (v2_xy_y_dest2,    "XY Y Dest B",    Choice, 0, 1, 0,     0, 1, None, "@dest") \
    F (v2_xy_y_amt2,     "XY Y Amt B",     Float, -1, 1, 0.0f,  0, 0, Bip, "") \
    F (v2_mpe_on,        "MPE Mode",       Bool,   0, 1, 0,     0, 0, None, "") \
    F (v2_mpe_bend,      "MPE Bend Range", Float,  1, 48, 48,   0, 1, St, "") \
    F (v2_pb_range,      "Pitch Bend Range", Float, 1, 48, 2,   0, 1, St, "") \
    F (v2_vel_curve,     "Velocity Curve", Choice, 0, 4, 0,     0, 1, None, "Linear|Soft|Hard|S-Curve|Fixed") \
    F (v2_slide_dest1,   "Slide Dest A",   Choice, 0, 1, 0,     0, 1, None, "@dest") \
    F (v2_slide_amt1,    "Slide Amt A",    Float, -1, 1, 0.0f,  0, 0, Bip, "") \
    F (v2_slide_dest2,   "Slide Dest B",   Choice, 0, 1, 0,     0, 1, None, "@dest") \
    F (v2_slide_amt2,    "Slide Amt B",    Float, -1, 1, 0.0f,  0, 0, Bip, "") \
    F (v2_breath_dest1,  "Breath Dest A",  Choice, 0, 1, 0,     0, 1, None, "@dest") \
    F (v2_breath_amt1,   "Breath Amt A",   Float, -1, 1, 0.0f,  0, 0, Bip, "") \
    F (v2_breath_dest2,  "Breath Dest B",  Choice, 0, 1, 0,     0, 1, None, "@dest") \
    F (v2_breath_amt2,   "Breath Amt B",   Float, -1, 1, 0.0f,  0, 0, Bip, "") \
    /* ── V2 Boutique Analog Realism: Vactrol LPG & Audio-Rate Feedback (append-only, Off/0% default) ── */ \
    F (v2_lpg_mode,      "Vactrol LPG",    Choice, 0, 3, 0,     0, 1, None, "Off|Both|LPF|VCA") \
    F (v2_lpg_decay,     "LPG Ring",       Float,  0, 1, 0.45f, 0, 0, Pct, "") \
    F (v2_lpg_strike,    "LPG Strike",     Float,  0, 1, 0.65f, 0, 0, Pct, "") \
    F (v2_fb_filter,     "Filter Feedback",Float, -1, 1, 0.0f,  0, 0, Bip, "") \
    F (v2_fb_fm,         "Filter to FM",   Float,  0, 1, 0.0f,  0, 0, Pct, "") \
    /* ── V2 Boutique Analog Realism: Polyphonic Power-Rail Sag & Phase Dispersion (append-only, Off/0% default) ── */ \
    F (v2_rail_sag,      "Power Rail Sag", Float,  0, 1, 0.0f,    0, 0, Pct, "") \
    F (v2_rail_time,     "Rail Recovery",  Float, 20, 80, 45.0f, 45, 0, Ms,  "") \
    F (v2_disp_stages,   "Phase Disperse", Choice, 0, 5, 0,       0, 1, None, "Off|4 Stg|5 Stg|6 Stg|7 Stg|8 Stg") \
    F (v2_disp_freq,     "Disperse Freq",  Float, 40, 2000, 180.0f, 220, 0, Hz, "") \
    F (v2_disp_pinch,    "Disperse Pinch", Float,  0, 1, 0.50f,   0, 0, Pct, "")

enum PID : int
{
   #define VESSEL_ENUM(id, ...) id,
    VESSEL_PARAMS (VESSEL_ENUM)
   #undef VESSEL_ENUM
    NUM_PARAMS
};

struct ParamSpec
{
    const char* id;
    const char* name;
    PType type;
    float min, max, def, centre, step;
    Unit unit;
    const char* choices;
};

const ParamSpec& spec (int pid) noexcept;

/** Stages, used for grouping (UI pages, destination menus). */
enum class Stage { Source, Voice, Ruin, Shape, Space, Pressure, Mod };
Stage stageOf (int pid) noexcept;
const char* stageName (Stage) noexcept;

/** Modulation destinations: index 0 is "none", then every Float parameter of the original table (in order),
    the LFO rates and warps, then Float parameters appended later (the resonator, then FM). Indices are saved in user
    state, so new destinations are only ever appended. */
const std::vector<int>& modDestinations();
int destIndexToPid (int index) noexcept;   // -1 for none

juce::String formatValue (int pid, float value, int maxChars = 12);
float parseValue (int pid, const juce::String& text);

juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

constexpr int kNumLfos = 3;
constexpr int kLfoSlots = 4;   // destinations per LFO (A, B in the original block; C, D appended)

/** LFO parameter addressing. The original block (shape..amt B) and the appended block (trigger..amt D) each
    repeat with a fixed stride, so LFO n's parameters are found by offset. */
constexpr int lfoBase (int l) noexcept { return lfo1_shape + l * (lfo2_shape - lfo1_shape); }   // +0 shape +1 rate +2 sync +3 div +4 warp
constexpr int lfoExt  (int l) noexcept { return lfo1_mode + l * (lfo2_mode - lfo1_mode); }      // +0 trigger +1 phase +2 fade +3 smooth +4 unipolar
constexpr int lfoDestPid (int l, int slot) noexcept { return slot < 2 ? lfoBase (l) + 5 + slot * 2 : lfoExt (l) + 5 + (slot - 2) * 2; }
constexpr int lfoAmtPid  (int l, int slot) noexcept { return lfoDestPid (l, slot) + 1; }
static_assert (lfo3_amt2 == lfoAmtPid (2, 1) && lfo3_amt4 == lfoAmtPid (2, 3) && lfo2_uni == lfoExt (1) + 4, "LFO parameter stride");

/** Expression routes: source 0 velocity, 1 mod wheel, 2 aftertouch; two slots each. */
constexpr int exprDestPid (int src, int slot) noexcept { return (src == 0 ? vel_dest1 : src == 1 ? mw_dest1 : at_dest1) + slot * 2; }
constexpr int exprAmtPid  (int src, int slot) noexcept { return exprDestPid (src, slot) + 1; }
static_assert (exprAmtPid (2, 1) == at_amt2 && exprDestPid (0, 1) == vel_dest2, "expression parameter layout");

inline bool isResonatorParam (int pid) noexcept { return pid >= res_on && pid <= res_mix; }
inline bool isFmParam (int pid) noexcept        { return pid >= fm_on && pid <= fm_key; }
inline bool isMacroParam (int pid) noexcept     { return pid >= macro1 && pid <= mac8_amt6; }
inline bool isV2DspParam (int pid) noexcept     { return pid >= v2_organic_amt && pid <= v2_tex_drive; }
inline bool isV2ControlParam (int pid) noexcept { return pid >= v2_xy_x && pid <= v2_breath_amt2; }

/** Hero Macros (first 4 of the 8 performance macros, NAMM performance dock workhorses). */
constexpr int kHeroMacros = 4;
constexpr int kXySlots = 2;
constexpr int xyDestPid (int axis, int slot) noexcept { return (axis == 0 ? v2_xy_x_dest1 : v2_xy_y_dest1) + slot * 2; }
constexpr int xyAmtPid  (int axis, int slot) noexcept { return xyDestPid (axis, slot) + 1; }
static_assert (xyAmtPid (1, 1) == v2_xy_y_amt2 && xyDestPid (0, 1) == v2_xy_x_dest2, "XY parameter layout");

/** Extended V2 expression slots: Slide (CC74) and Breath (CC2), 2 destinations each. */
constexpr int slideDestPid  (int slot) noexcept { return v2_slide_dest1 + slot * 2; }
constexpr int slideAmtPid   (int slot) noexcept { return slideDestPid (slot) + 1; }
constexpr int breathDestPid (int slot) noexcept { return v2_breath_dest1 + slot * 2; }
constexpr int breathAmtPid  (int slot) noexcept { return breathDestPid (slot) + 1; }
static_assert (slideAmtPid (1) == v2_slide_amt2 && breathAmtPid (1) == v2_breath_amt2, "V2 expression layout");

/** Look up a PID by string ID, or -1 if unknown. */
int pidForId (const juce::String& id) noexcept;

/** Macros: eight knobs, six routes each. A macro's offset on a destination is amount × knob (normalised), summed
    with the LFOs and expression, so a macro at 0 leaves the preset exactly as written. */
constexpr int kNumMacros = 8;
constexpr int kMacroSlots = 6;
constexpr int macroPid (int m) noexcept { return macro1 + m; }
constexpr int macroDestPid (int m, int slot) noexcept { return mac1_dest1 + (m * kMacroSlots + slot) * 2; }
constexpr int macroAmtPid  (int m, int slot) noexcept { return macroDestPid (m, slot) + 1; }
static_assert (macroAmtPid (7, 5) == mac8_amt6 && macroDestPid (3, 2) == mac4_dest3, "macro parameter layout");

/** FM ratio choices (fm_ratio) as numbers. */
constexpr float kFmRatios[] { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 11.0f, 14.0f };
/** Performance sources routed by the vel_/mw_/at_ parameters (two destinations each). */
enum class ExprSource { Velocity, ModWheel, Aftertouch };
constexpr int kExprSlots = 6;   // 3 sources x 2 destinations
constexpr int kDrawPoints = 32;
} // namespace vessel
