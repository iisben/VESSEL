#include "Backdrop.h"
#include <algorithm>
#include <cmath>

namespace vessel::ui
{
using namespace juce::gl;

namespace
{
const char* kVertex = R"(
attribute vec2 position;
varying vec2 vUv;
void main()
{
    vUv = position * 0.5 + 0.5;
    gl_Position = vec4 (position, 0.0, 1.0);
}
)";

// Layered reactive backdrop shader inspired by glossy obsidian ferrofluid ridges + crystalline neon-violet plexus.
// Serene dark Reliquary ground at rest; transparently triggers behind the glass whenever keys are played:
//   - Key Press: expanding liquid ripple shockwave + luminous crystalline spark at the played pitch zone
//   - Long Press / Chords: glossy 3D tilted ferrofluid ridges + drifting neon-violet plasma valleys bloom transparently
//   - Poly Touch / MPE Pressure: crystalline wireframe plexus & electric violet/indigo star-flares ignite & shimmer
const char* kFragment = R"(
varying vec2 vUv;
uniform vec2  res;
uniform float time;
uniform float energy;
uniform float pulse;
uniform float pulseAge;
uniform float hold;
uniform float polyTouch;
uniform float voices;
uniform float noteX;

float hash (vec2 p) { return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453123); }

vec2 hash2 (vec2 p)
{
    vec2 k = vec2 (dot (p, vec2 (127.1, 311.7)), dot (p, vec2 (269.5, 183.3)));
    return fract (sin (k) * 43758.5453123);
}

float noise (vec2 p)
{
    vec2 i = floor (p), f = fract (p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix (mix (hash (i), hash (i + vec2 (1.0, 0.0)), u.x),
                mix (hash (i + vec2 (0.0, 1.0)), hash (i + vec2 (1.0, 1.0)), u.x), u.y);
}

float fbm4 (vec2 p)
{
    float v = 0.0, a = 0.5;
    mat2 rot = mat2 (0.80, -0.60, 0.60, 0.80);
    for (int i = 0; i < 4; ++i)
    {
        v += a * noise (p);
        p = rot * p * 2.04 + vec2 (1.7, 9.2);
        a *= 0.5;
    }
    return v;
}

float fbm3 (vec2 p)
{
    float v = 0.0, a = 0.5;
    mat2 rot = mat2 (0.80, -0.60, 0.60, 0.80);
    for (int i = 0; i < 3; ++i)
    {
        v += a * noise (p);
        p = rot * p * 2.02 + vec2 (3.1, 5.7);
        a *= 0.5;
    }
    return v;
}

// Distance from point p to line segment (a, b)
float segDist (vec2 p, vec2 a, vec2 b)
{
    vec2 pa = p - a, ba = b - a;
    float h = clamp (dot (pa, ba) / max (dot (ba, ba), 1.0e-5), 0.0, 1.0);
    return length (pa - ba * h);
}

// Animated cell vertex for the crystalline geometric plexus layer
vec2 plexusNode (vec2 cell, float t)
{
    vec2 h = hash2 (cell);
    return cell + 0.5 + 0.37 * sin (t * (0.55 + 0.65 * h) + h * 6.2831853);
}

// Crystalline wireframe plexus & glowing neon-violet/indigo star nodes (inspired by uploaded lattice layer).
// Returns vec3(wireframeIntensity, starFlareIntensity, indigoTintMix)
vec3 plexusLayer (vec2 p, float t, float energyBoost)
{
    vec2 cell = floor (p);
    vec2 cNode = plexusNode (cell, t);

    float wire = 0.0;
    float star = 0.0;
    float tintAcc = 0.0;

    // Gather 9 neighboring nodes for glowing vertices + triangulated constellation filaments
    vec2 n0 = plexusNode (cell + vec2 (-1.0, -1.0), t);
    vec2 n1 = plexusNode (cell + vec2 ( 0.0, -1.0), t);
    vec2 n2 = plexusNode (cell + vec2 ( 1.0, -1.0), t);
    vec2 n3 = plexusNode (cell + vec2 (-1.0,  0.0), t);
    vec2 n5 = plexusNode (cell + vec2 ( 1.0,  0.0), t);
    vec2 n6 = plexusNode (cell + vec2 (-1.0,  1.0), t);
    vec2 n7 = plexusNode (cell + vec2 ( 0.0,  1.0), t);
    vec2 n8 = plexusNode (cell + vec2 ( 1.0,  1.0), t);

    // Connect center node to neighbors + diagonal facets to form crisp crystalline triangles
    vec2 edgesA[12];
    vec2 edgesB[12];
    edgesA[0]  = cNode; edgesB[0]  = n1;
    edgesA[1]  = cNode; edgesB[1]  = n3;
    edgesA[2]  = cNode; edgesB[2]  = n5;
    edgesA[3]  = cNode; edgesB[3]  = n7;
    edgesA[4]  = cNode; edgesB[4]  = n0;
    edgesA[5]  = cNode; edgesB[5]  = n2;
    edgesA[6]  = cNode; edgesB[6]  = n6;
    edgesA[7]  = cNode; edgesB[7]  = n8;
    edgesA[8]  = n1;    edgesB[8]  = n3;
    edgesA[9]  = n1;    edgesB[9]  = n5;
    edgesA[10] = n7;    edgesB[10] = n3;
    edgesA[11] = n7;    edgesB[11] = n5;

    for (int i = 0; i < 12; ++i)
    {
        float len = length (edgesB[i] - edgesA[i]);
        float vis = smoothstep (1.52, 0.78, len);
        float d = segDist (p, edgesA[i], edgesB[i]);
        // Fine crystalline filament + subtle violet sheath (stays sharp, never muddy)
        float line = (smoothstep (0.017, 0.002, d) * 0.90 + smoothstep (0.060, 0.0, d) * 0.14) * vis;
        wire += line;
    }

    // Tight, jewel-sharp crystalline star-flare nodes with 4-point diamond glints
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 nb = cell + vec2 (float (x), float (y));
            vec2 pos = plexusNode (nb, t);
            vec2 diff = p - pos;
            float d = length (diff);
            float h = hash (nb + vec2 (19.3, 7.1));
            float tw = 0.55 + 0.45 * sin (t * (2.0 + 2.6 * h) + h * 19.0);
            float pinpoint = exp (-d * d * 155.0) * (0.75 + 0.45 * tw);
            float halo     = exp (-d * d * 28.0) * 0.24 * tw;
            // Razor 45-degree crystalline glint spikes
            float ax1 = exp (-abs (diff.x + diff.y) * 58.0) * exp (-d * 10.5);
            float ax2 = exp (-abs (diff.x - diff.y) * 58.0) * exp (-d * 10.5);
            float flare = (pinpoint + halo + 0.52 * (ax1 + ax2) * tw)
                        * smoothstep (0.34, 0.88, h + 0.30 * energyBoost);
            star += flare;
            tintAcc += flare * step (0.64, hash (nb + vec2 (4.1, 31.7)));
        }
    }

    float indigoMix = tintAcc / max (star, 1.0e-4);
    return vec3 (min (wire, 1.35), min (star, 1.30), clamp (indigoMix, 0.0, 1.0));
}

// Evaluate glossy obsidian ferrofluid heightfield: dense sinuously undulating black liquid ribbons
// with glowing violet plasma crevices between them (matching macro tilted ferrofluid video).
// Returns vec3(height, warp.x, warp.y)
vec3 ferroField (vec2 p, float t, float dofSoft, float ripple)
{
    vec2 q = vec2 (fbm3 (p * 0.72 + vec2 (0.0, t * 0.34)),
                   fbm3 (p * 0.72 + vec2 (5.2, 1.3) - vec2 (t * 0.24, 0.0)));
    vec2 r = vec2 (fbm3 (p * 1.05 + 1.65 * q + vec2 (1.7, 9.2) + vec2 (t * 0.22, -t * 0.18)),
                   fbm3 (p * 1.05 + 1.65 * q + vec2 (8.3, 2.8) - vec2 (t * 0.16, t * 0.22)));

    // Tight flowing ribbon frequency along tilted flow axis, with gentle organic bifurcations
    float freq = mix (13.2, 9.2, dofSoft);
    float phase = p.y * freq + 2.85 * r.x + 1.55 * q.y - t * 1.05 + ripple;
    float wave = sin (phase) + 0.28 * sin (phase * 2.0 + 1.6 * r.y) + 0.22 * sin (p.x * 4.2 + 2.2 * q.x + t * 0.45);

    // Sleek glossy obsidian ribbons occupy ~62% of the field, leaving sharp sinuous plasma crevices
    float edgeLo = mix (-0.36, -0.14, dofSoft);
    float edgeHi = mix (0.32, 0.62, dofSoft);
    float h = smoothstep (edgeLo, edgeHi, wave);
    return vec3 (h, r.x, r.y);
}

void main()
{
    vec2 uv = vUv;
    float aspect = res.x / max (res.y, 1.0);

    // 1. Serene Reliquary dark water ground (quiet & effortless legibility at rest)
    vec2 bgP = uv * vec2 (aspect, 1.0) * 1.55;
    float bgT = time * 0.025;
    vec2 bgQ = vec2 (fbm3 (bgP + vec2 (0.0, bgT)), fbm3 (bgP + vec2 (4.8, 1.7) - bgT));
    float bgF = fbm3 (bgP + 2.4 * bgQ);

    vec3 water      = vec3 (0.022, 0.025, 0.038);
    vec3 deepViolet = vec3 (0.52, 0.16, 0.98);
    vec3 neonViolet = vec3 (0.82, 0.46, 1.00);
    vec3 hotMagenta = vec3 (0.94, 0.48, 1.00);
    vec3 elecIndigo = vec3 (0.44, 0.44, 1.00);
    vec3 plasmaCore = vec3 (0.96, 0.86, 1.00);
    vec3 chlorine   = vec3 (0.14, 0.62, 0.56);

    vec3 baseCol = water;
    baseCol += deepViolet * pow (bgF, 3.2) * (0.14 + 0.12 * energy);
    baseCol += chlorine * pow (clamp (bgQ.y, 0.0, 1.0), 5.2) * 0.045;

    // 2. Reactive transparency envelope driven by key press, long press, chord mass & poly touch
    float pressAct = clamp (pulse * 0.95, 0.0, 1.0);
    float holdAct  = clamp (hold * (0.85 + 0.25 * voices), 0.0, 1.0);
    float touchAct = clamp (polyTouch, 0.0, 1.0);
    float activity = clamp (max (pressAct * 0.72, holdAct * 0.86) + touchAct * 0.36 + energy * 0.18, 0.0, 1.0);

    // Subtle transparent reveal: faint phantom obsidian when idle (~0.04), smoothly rises to 0.26..0.54 when played
    float reveal = clamp (0.04 + 0.36 * smoothstep (0.0, 1.0, activity) + 0.14 * touchAct, 0.0, 0.54);

    // 3. Tilted macro 3D perspective projection for obsidian ferrofluid
    float depth = mix (0.88, 2.75, pow (uv.y, 1.42));
    float dofSoft = smoothstep (0.46, 0.96, uv.y);
    vec2 p = vec2 ((uv.x - 0.5) * aspect * depth, depth * 1.15);
    mat2 diag = mat2 (0.87, 0.49, -0.49, 0.87);
    p = diag * p;

    // Key-press focal origin & expanding liquid shockwave
    vec2 focalUv = vec2 (mix (0.18, 0.82, noteX), 0.44);
    vec2 dUv = (uv - focalUv) * vec2 (aspect, 1.0);
    float distFocal = length (dUv);
    float waveRad = pulseAge * (0.56 + 0.24 * touchAct);
    float shockRing = exp (-abs (distFocal - waveRad) * (7.5 - 2.0 * holdAct))
                    * exp (-pulseAge * 1.45) * pressAct;
    float rippleDisp = sin (distFocal * 24.0 - pulseAge * 12.0) * shockRing * 1.35
                     + sin (distFocal * 32.0 - time * 6.5) * touchAct * 0.42 * exp (-distFocal * 1.35);

    // 4. Layer 1: Glossy obsidian ferrofluid ribbons & neon-violet plasma crevices
    float ft = time * 0.32;
    vec3 f0 = ferroField (p, ft, dofSoft, rippleDisp);
    float h = f0.x;
    vec2 warp = f0.yz;
    float eps = mix (0.015, 0.042, dofSoft);
    float hx = ferroField (p + vec2 (eps, 0.0), ft, dofSoft, rippleDisp).x;
    float hy = ferroField (p + vec2 (0.0, eps), ft, dofSoft, rippleDisp).x;
    vec3 N = normalize (vec3 ((h - hx) * (4.2 + 1.5 * touchAct),
                              (h - hy) * (4.2 + 1.5 * touchAct),
                              mix (0.24, 0.48, dofSoft)));
    vec3 V = vec3 (0.0, 0.0, 1.0);

    // Crisp luminous valleys carved between jet-black obsidian ribbons
    float valley = pow (clamp (1.0 - h, 0.0, 1.0), mix (2.4, 1.6, dofSoft));
    float pocket = fbm4 (p * 0.56 + vec2 (ft * 0.40, -ft * 0.30) + 0.75 * warp);
    float pocketMask = smoothstep (mix (0.52, 0.28, holdAct), 0.85, pocket);

    float reach = mix (0.44, 1.35, clamp (holdAct * 0.82 + voices * 0.32 + touchAct * 0.32, 0.0, 1.0));
    float focalPool = exp (-distFocal * distFocal / max (0.06, reach * reach));
    float veinPower = clamp (pocketMask * (0.36 + 0.62 * holdAct + 0.42 * touchAct)
                           + focalPool * (0.52 * pressAct + 0.42 * holdAct + 0.34 * touchAct)
                           + shockRing * 0.82, 0.0, 1.25);

    // Upper shallow-DOF out-of-focus magenta/violet bokeh sparks (top horizon of inspiration video)
    float bokehNoise = pow (fbm3 (p * 1.35 - vec2 (ft * 0.35, ft * 0.20)), 3.0);
    float bokeh = dofSoft * smoothstep (0.14, 0.52, bokehNoise) * (0.38 + 0.58 * holdAct + 0.62 * touchAct);

    vec3 L1 = normalize (vec3 ((focalUv.x - uv.x) * 1.5, 0.52 - uv.y, 0.58));
    vec3 L2 = normalize (vec3 (-0.58, 0.62, 0.48));
    vec3 H1 = normalize (L1 + V);
    vec3 H2 = normalize (L2 + V);

    // Wet specular glints tightly hugging the glossy black ribbon edges
    float spec1 = pow (max (dot (N, H1), 0.0), mix (48.0, 16.0, dofSoft));
    float spec2 = pow (max (dot (N, H2), 0.0), mix (34.0, 12.0, dofSoft));
    float fresnel = pow (1.0 - max (dot (N, V), 0.0), 3.0);
    float crestMask = smoothstep (0.24, 0.84, h);
    float lipEdge = smoothstep (0.08, 0.42, h) * smoothstep (0.92, 0.48, h);

    vec3 obsidian = vec3 (0.012, 0.011, 0.020)
                  + deepViolet * (0.14 * fresnel * (0.35 + 0.75 * veinPower)) * crestMask
                  + neonViolet * (0.22 * lipEdge * veinPower);
    float rimGlow = (spec1 * (0.90 + 0.55 * touchAct + 0.45 * pressAct) + spec2 * 0.44) * crestMask;
    vec3 specCol = mix (neonViolet, plasmaCore, clamp (spec1 * 0.75 + touchAct * 0.30, 0.0, 1.0));
    obsidian += specCol * rimGlow * (0.58 + 0.36 * holdAct);

    vec3 valleyCol = mix (deepViolet, neonViolet, smoothstep (0.12, 0.78, veinPower));
    valleyCol = mix (valleyCol, hotMagenta, smoothstep (0.48, 1.05, veinPower * (0.7 + 0.5 * touchAct)));
    valleyCol = mix (valleyCol, plasmaCore, smoothstep (0.88, 1.25, veinPower * valley + touchAct * 0.25) * 0.65);
    vec3 ferroCol = obsidian + valleyCol * valley * veinPower * 0.88;
    ferroCol += mix (deepViolet, hotMagenta, 0.55) * bokeh * 0.44;
    ferroCol += deepViolet * (pocketMask * 0.08 * (0.30 + holdAct + touchAct));

    // 5. Layer 2: Fine crystalline geometric neon-violet / electric-indigo plexus overlay
    // Drifts in subtle parallax above the ferrofluid; sparks along the key-press shockwave,
    // widens on holds/chords, and shimmers crisply with jewel-like star-flares under poly touch.
    float plexusVis = clamp (0.015 + 0.18 * pressAct + 0.20 * holdAct + 0.34 * touchAct + 0.12 * voices, 0.0, 0.50);
    vec2 plxUv = (uv - vec2 (0.5)) * vec2 (aspect, 1.0);
    vec2 plxP = plxUv * 5.8 + vec2 (time * 0.055, -time * 0.038) + (warp - 0.5) * 0.35;
    vec3 plx = plexusLayer (plxP, time * 0.44, clamp (touchAct + 0.45 * holdAct + 0.35 * pressAct, 0.0, 1.0));

    float plxField = clamp (focalPool * (0.62 + 0.48 * pressAct)
                          + shockRing * 0.95
                          + holdAct * (0.38 + 0.40 * pocketMask)
                          + touchAct * 0.68
                          + voices * 0.28, 0.0, 1.05);

    vec3 wireCol = mix (deepViolet, neonViolet, 0.50 + 0.42 * touchAct);
    vec3 nodeCol = mix (hotMagenta, elecIndigo, plx.z * 0.72);
    nodeCol = mix (nodeCol, plasmaCore, clamp (plx.y * 0.38 + touchAct * 0.22, 0.0, 0.72));

    vec3 plexusAdd = (wireCol * plx.x * (0.26 + 0.32 * touchAct + 0.18 * holdAct)
                    + nodeCol * plx.y * (0.42 + 0.46 * touchAct + 0.24 * pressAct)) * plxField * plexusVis;

    // Luminance-preserving soft compression on active layers so the neon-violet plasma stays
    // richly saturated (never greying out) while remaining translucent behind dark glass panels.
    vec3 activeLayer = ferroCol + plexusAdd * 1.15;
    float actLum = dot (activeLayer, vec3 (0.2126, 0.7152, 0.0722));
    activeLayer *= 1.0 / (1.0 + actLum * 0.92);

    vec3 col = mix (baseCol, activeLayer, reveal) + plexusAdd * 0.28;
    col += neonViolet * shockRing * 0.11;

    // Subtle vertical glass condensation streaks
    float lane = floor (uv.x * 160.0);
    float speed = 0.02 + hash (vec2 (lane, 3.0)) * 0.05;
    float streak = pow (noise (vec2 (lane * 1.37, uv.y * 2.5 + time * speed)), 14.0) * step (0.83, hash (vec2 (lane, 7.0)));
    col += vec3 (0.64, 0.56, 0.92) * streak * 0.032;

    // Vignette + bottom base glow (keeps edges grounded and controls effortlessly readable)
    float vig = smoothstep (1.35, 0.18, length ((uv - vec2 (0.5, 0.52)) * vec2 (1.12, 1.0)));
    col *= mix (0.34, 1.0, vig);
    col += neonViolet * 0.038 * smoothstep (0.32, 0.0, uv.y) * (0.45 + energy + 0.55 * holdAct + 0.45 * touchAct);

    // Anti-banding dither
    col += (hash (gl_FragCoord.xy + fract (time)) - 0.5) / 255.0;
    gl_FragColor = vec4 (clamp (col, vec3 (0.0), vec3 (1.0)), 1.0);
}
)";
} // namespace

void Backdrop::newOpenGLContextCreated()
{
    shader = std::make_unique<juce::OpenGLShaderProgram> (context);
    const bool ok = shader->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (kVertex))
                 && shader->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (kFragment))
                 && shader->link();
    if (! ok)
    {
        DBG ("VESSEL backdrop shader: " << shader->getLastError());
        shader.reset();
        return;
    }

    uRes       = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "res");
    uTime      = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "time");
    uEnergy    = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "energy");
    uPulse     = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "pulse");
    uPulseAge  = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "pulseAge");
    uHold      = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "hold");
    uPolyTouch = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "polyTouch");
    uVoices    = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "voices");
    uNoteX     = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "noteX");
    aPos = context.extensions.glGetAttribLocation (shader->getProgramID(), "position");

    const GLfloat quad[] { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    context.extensions.glGenBuffers (1, &vbo);
    context.extensions.glBindBuffer (GL_ARRAY_BUFFER, vbo);
    context.extensions.glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);

    lastFrameTime = juce::Time::getMillisecondCounterHiRes();
}

void Backdrop::renderOpenGL()
{
    const float scale = (float) context.getRenderingScale();
    auto* target = context.getTargetComponent();
    if (target == nullptr) return;
    const int w = juce::roundToInt (scale * (float) target->getWidth());
    const int h = juce::roundToInt (scale * (float) target->getHeight());
    if (w <= 0 || h <= 0) return;

    glViewport (0, 0, w, h);
    juce::OpenGLHelpers::clear (juce::Colour (0xff07080b));
    if (shader == nullptr || aPos < 0) return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float dt = std::clamp ((float) ((nowMs - lastFrameTime) * 0.001), 0.001f, 0.05f);
    lastFrameTime = nowMs;

    // Detect fresh key-press events from audio telemetry
    const uint32_t seq = inPulseSeq.load (std::memory_order_acquire);
    if (seq != lastPulseSeq)
    {
        lastPulseSeq = seq;
        const float vel = std::clamp (inPulseVel.load (std::memory_order_relaxed), 0.0f, 1.0f);
        pulseS = std::max (pulseS, 0.50f + 0.50f * vel);
        pulseAgeS = 0.0f;
    }

    const float targetEnergy = std::min (1.0f, energy.load (std::memory_order_relaxed) * 3.0f);
    const float targetHold   = std::clamp (inHold.load (std::memory_order_relaxed), 0.0f, 1.0f);
    const float targetTouch  = std::clamp (inPolyTouch.load (std::memory_order_relaxed), 0.0f, 1.0f);
    const float targetVoices = std::clamp ((float) inHeldCount.load (std::memory_order_relaxed) / 4.0f, 0.0f, 1.0f);
    const float targetNoteX  = std::clamp (inNoteNorm.load (std::memory_order_relaxed), 0.0f, 1.0f);

    auto smoothTo = [dt] (float& cur, float tgt, float attackSec, float releaseSec)
    {
        const float tau = (tgt > cur) ? attackSec : releaseSec;
        const float k = 1.0f - std::exp (-dt / std::max (0.001f, tau));
        cur += k * (tgt - cur);
    };

    smoothTo (energyS,    targetEnergy, 0.05f, 0.25f);
    smoothTo (holdS,      targetHold,   0.18f, 0.65f);
    smoothTo (polyTouchS, targetTouch,  0.04f, 0.22f);
    smoothTo (voicesS,    targetVoices, 0.08f, 0.45f);
    smoothTo (noteXS,     targetNoteX,  0.14f, 0.14f);

    pulseS *= std::exp (-dt / 0.58f);
    if (pulseS < 1.0e-4f) pulseS = 0.0f;
    pulseAgeS = std::min (8.0f, pulseAgeS + dt);

    const float flowSpeed = 0.45f + 0.95f * holdS + 1.55f * polyTouchS + 0.65f * pulseS + 0.55f * energyS;
    animTime += (double) dt * (double) flowSpeed;

    glDisable (GL_DEPTH_TEST);
    glDisable (GL_BLEND);
    shader->use();

    uRes->set ((GLfloat) w, (GLfloat) h);
    uTime->set ((GLfloat) animTime);
    uEnergy->set ((GLfloat) energyS);
    uPulse->set ((GLfloat) pulseS);
    uPulseAge->set ((GLfloat) pulseAgeS);
    uHold->set ((GLfloat) holdS);
    uPolyTouch->set ((GLfloat) polyTouchS);
    uVoices->set ((GLfloat) voicesS);
    uNoteX->set ((GLfloat) noteXS);

    context.extensions.glBindBuffer (GL_ARRAY_BUFFER, vbo);
    context.extensions.glVertexAttribPointer ((GLuint) aPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    context.extensions.glEnableVertexAttribArray ((GLuint) aPos);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    context.extensions.glDisableVertexAttribArray ((GLuint) aPos);
    context.extensions.glBindBuffer (GL_ARRAY_BUFFER, 0);

    alive.store (true, std::memory_order_relaxed);
}

void Backdrop::openGLContextClosing()
{
    if (vbo != 0) context.extensions.glDeleteBuffers (1, &vbo);
    vbo = 0;
    uRes.reset(); uTime.reset(); uEnergy.reset();
    uPulse.reset(); uPulseAge.reset(); uHold.reset();
    uPolyTouch.reset(); uVoices.reset(); uNoteX.reset();
    shader.reset();
    alive.store (false, std::memory_order_relaxed);
}
} // namespace vessel::ui
