#pragma once

#include <juce_opengl/juce_opengl.h>
#include <atomic>
#include <cstdint>

namespace vessel::ui
{
/** The OpenGL layer under the whole editor: nocturnal Reliquary dark water that transparently awakens
    glossy obsidian ferrofluid ridges & neon-violet plasma veins whenever keys are pressed.
    Reactive to:
      - Key Press (expanding liquid ripple & luminous focal flash scaled by velocity & pitch)
      - Long Press / Chords (progressive translucent depth-of-field ferrofluid reveal & drifting glow pools)
      - Polyphonic Aftertouch / MPE Pressure (incandescent lavender-violet plasma veins & shimmering specular rims). */
class Backdrop : public juce::OpenGLRenderer
{
public:
    explicit Backdrop (juce::OpenGLContext& c) : context (c) {}

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    void setEnergy (float e) noexcept { energy.store (e, std::memory_order_relaxed); }
    void setPerformanceState (uint32_t pulseSeq, float pulseVel, float noteNorm,
                              float hold, int heldCount, float polyTouch) noexcept
    {
        inPulseSeq.store (pulseSeq, std::memory_order_release);
        inPulseVel.store (pulseVel, std::memory_order_relaxed);
        inNoteNorm.store (noteNorm, std::memory_order_relaxed);
        inHold.store (hold, std::memory_order_relaxed);
        inHeldCount.store (heldCount, std::memory_order_relaxed);
        inPolyTouch.store (polyTouch, std::memory_order_relaxed);
    }

    bool isAlive() const noexcept { return alive.load (std::memory_order_relaxed); }

private:
    juce::OpenGLContext& context;
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> uRes, uTime, uEnergy;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> uPulse, uPulseAge, uHold, uPolyTouch, uVoices, uNoteX;
    ::GLuint vbo = 0;
    ::GLint aPos = -1;

    double startTime = juce::Time::getMillisecondCounterHiRes();
    double lastFrameTime = juce::Time::getMillisecondCounterHiRes();
    double animTime = 0.0;

    // GL-thread smoothed animation states
    uint32_t lastPulseSeq = 0;
    float energyS = 0.0f;
    float pulseS = 0.0f;
    float pulseAgeS = 8.0f;
    float holdS = 0.0f;
    float polyTouchS = 0.0f;
    float voicesS = 0.0f;
    float noteXS = 0.5f;

    // Lock-free inputs written from editor timer
    std::atomic<float> energy { 0.0f };
    std::atomic<uint32_t> inPulseSeq { 0 };
    std::atomic<float> inPulseVel { 0.0f };
    std::atomic<float> inNoteNorm { 0.5f };
    std::atomic<float> inHold { 0.0f };
    std::atomic<int> inHeldCount { 0 };
    std::atomic<float> inPolyTouch { 0.0f };
    std::atomic<bool> alive { false };
};
} // namespace vessel::ui
