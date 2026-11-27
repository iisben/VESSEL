#pragma once

#include <juce_opengl/juce_opengl.h>
#include <atomic>

namespace vessel::ui
{
/** The OpenGL layer under the whole editor: slow domain-warped ultraviolet ink in black water, with
    condensation streaks and a vignette. The output level stirs it (faster flow, brighter ink).
    Components are composited on top by the same OpenGL context. */
class Backdrop : public juce::OpenGLRenderer
{
public:
    explicit Backdrop (juce::OpenGLContext& c) : context (c) {}

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    void setEnergy (float e) noexcept { energy.store (e, std::memory_order_relaxed); }
    bool isAlive() const noexcept { return alive.load(); }

private:
    juce::OpenGLContext& context;
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> uRes, uTime, uEnergy;
    ::GLuint vbo = 0;
    ::GLint aPos = -1;
    double startTime = juce::Time::getMillisecondCounterHiRes();
    float energySmoothed = 0.0f;
    std::atomic<float> energy { 0.0f };
    std::atomic<bool> alive { false };
};
} // namespace vessel::ui
