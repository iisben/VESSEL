#include "Backdrop.h"

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

const char* kFragment = R"(
varying vec2 vUv;
uniform vec2 res;
uniform float time;
uniform float energy;

float hash (vec2 p) { return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453); }

float noise (vec2 p)
{
    vec2 i = floor (p), f = fract (p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix (mix (hash (i), hash (i + vec2 (1.0, 0.0)), u.x),
                mix (hash (i + vec2 (0.0, 1.0)), hash (i + vec2 (1.0, 1.0)), u.x), u.y);
}

float fbm (vec2 p)
{
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { v += a * noise (p); p = p * 2.03 + vec2 (1.7, 9.2); a *= 0.5; }
    return v;
}

void main()
{
    vec2 uv = vUv;
    vec2 p = uv * vec2 (res.x / res.y, 1.0) * 1.7;
    float t = time * (0.018 + 0.05 * energy);

    // domain-warped ink
    vec2 q = vec2 (fbm (p + vec2 (0.0, t)), fbm (p + vec2 (5.2, 1.3) - t));
    vec2 r = vec2 (fbm (p + 3.2 * q + vec2 (1.7, 9.2) + 0.6 * t), fbm (p + 3.2 * q + vec2 (8.3, 2.8) - 0.4 * t));
    float f = fbm (p + 3.0 * r);

    vec3 water   = vec3 (0.026, 0.030, 0.042);
    vec3 violet  = vec3 (0.46, 0.27, 0.88);
    vec3 chlorine= vec3 (0.12, 0.58, 0.54);

    vec3 col = water;
    col += violet * pow (f, 3.2) * (0.20 + 0.25 * energy);
    col += chlorine * pow (clamp (r.y, 0.0, 1.0), 5.0) * 0.07;

    // condensation: thin vertical streaks sliding down the glass
    float lane = floor (uv.x * 160.0);
    float speed = 0.02 + hash (vec2 (lane, 3.0)) * 0.05;
    float streak = pow (noise (vec2 (lane * 1.37, uv.y * 2.5 + time * speed)), 14.0) * step (0.82, hash (vec2 (lane, 7.0)));
    col += vec3 (0.55, 0.6, 0.8) * streak * 0.05;

    // vignette + floor glow (the vessel's base)
    float v = smoothstep (1.3, 0.2, length ((uv - vec2 (0.5, 0.55)) * vec2 (1.15, 1.0)));
    col *= mix (0.32, 1.0, v);
    col += violet * 0.035 * smoothstep (0.35, 0.0, uv.y) * (0.5 + energy);

    // dither against banding
    col += (hash (gl_FragCoord.xy + fract (time)) - 0.5) / 255.0;
    gl_FragColor = vec4 (col, 1.0);
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

    uRes    = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "res");
    uTime   = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "time");
    uEnergy = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "energy");
    aPos = context.extensions.glGetAttribLocation (shader->getProgramID(), "position");

    const GLfloat quad[] { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    context.extensions.glGenBuffers (1, &vbo);
    context.extensions.glBindBuffer (GL_ARRAY_BUFFER, vbo);
    context.extensions.glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);
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

    glDisable (GL_DEPTH_TEST);
    glDisable (GL_BLEND);
    shader->use();

    energySmoothed += 0.05f * (std::min (1.0f, energy.load (std::memory_order_relaxed) * 3.0f) - energySmoothed);
    uRes->set ((GLfloat) w, (GLfloat) h);
    uTime->set ((GLfloat) ((juce::Time::getMillisecondCounterHiRes() - startTime) * 0.001));
    uEnergy->set ((GLfloat) energySmoothed);

    context.extensions.glBindBuffer (GL_ARRAY_BUFFER, vbo);
    context.extensions.glVertexAttribPointer ((GLuint) aPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    context.extensions.glEnableVertexAttribArray ((GLuint) aPos);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    context.extensions.glDisableVertexAttribArray ((GLuint) aPos);
    context.extensions.glBindBuffer (GL_ARRAY_BUFFER, 0);

    alive = true;
}

void Backdrop::openGLContextClosing()
{
    if (vbo != 0) context.extensions.glDeleteBuffers (1, &vbo);
    vbo = 0;
    uRes.reset(); uTime.reset(); uEnergy.reset();
    shader.reset();
    alive = false;
}
} // namespace vessel::ui
