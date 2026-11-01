// V2.1 regression tests: click-free notch removal / bypass and NaN robustness
// (detection behaviour is verified against V2 with tests/benchmark.cpp FS_DUMP).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <tuple>
#include <limits>
#include "dsp/FeedbackDetector.h"
#include "dsp/FilterManager.h"
#include "dsp/PanicLimiter.h"

using namespace LiveAudioLab;

namespace
{
constexpr double twoPi = 6.283185307179586;
int failures = 0;

void check(bool condition, const std::string& message)
{
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", message.c_str());
    if (! condition) ++failures;
}

// Largest sample-to-sample jump of (output - steady-state model) is hard to
// define generically; instead measure the largest first difference relative to
// the largest first difference of the input sine (a click shows as a spike).
float maxStep(const juce::AudioBuffer<float>& b)
{
    float m = 0.0f;
    for (int i = 1; i < b.getNumSamples(); ++i)
        m = std::max(m, std::abs(b.getSample(0, i) - b.getSample(0, i - 1)));
    return m;
}

void testClickFreeRemoval()
{
    std::printf("[1] Click-free notch removal / bypass / deepening\n");
    const double sr = 48000;
    FilterManager manager;
    manager.prepare(sr);
    const int id = manager.addFilter(1000.0f, -20.0f, 10.0f, false, false, false);
    check(id > 0, "manual filter allocated");

    // Low-frequency sine far from the notch: removing the notch must not create a step.
    double phase = 0;
    float worst = 0.0f;
    for (int block = 0; block < 200; ++block)
    {
        juce::AudioBuffer<float> b(1, 256);
        for (int i = 0; i < 256; ++i) { phase += twoPi * 950.0 / sr; b.setSample(0, i, 0.5f * static_cast<float>(std::sin(phase))); }
        if (block == 100) manager.deleteFilter(id);
        if (block == 140) manager.addFilter(950.0f, -30.0f, 40.0f, false, false, false);
        manager.processAudio(b);
        if (block > 40) worst = std::max(worst, maxStep(b));
    }
    // The unfiltered sine's maximum step is 0.5 * 2*pi*950/48000 = 0.062.
    check(worst <= 0.0625f * 1.05f, "no step larger than the sine's own slope when deleting/adding notches (max step "
                                    + std::to_string(worst) + ")");

    // Bypass mode fades every notch out instead of switching the cascade off.
    FilterManager m2;
    m2.prepare(sr);
    m2.addFilter(1000.0f, -30.0f, 30.0f, true, true, false);
    juce::AudioBuffer<float> b(1, 4800);
    phase = 0;
    for (int i = 0; i < 4800; ++i) { phase += twoPi * 1000.0 / sr; b.setSample(0, i, 0.5f * static_cast<float>(std::sin(phase))); }
    m2.processAudio(b); // settle
    m2.setOperationMode(OperationMode::Bypass);
    juce::AudioBuffer<float> b2(1, 4800);
    for (int i = 0; i < 4800; ++i) { phase += twoPi * 1000.0 / sr; b2.setSample(0, i, 0.5f * static_cast<float>(std::sin(phase))); }
    m2.processAudio(b2);
    float firstPeak = 0.0f, lastPeak = 0.0f;
    for (int i = 0; i < 96; ++i) firstPeak = std::max(firstPeak, std::abs(b2.getSample(0, i)));
    for (int i = 4800 - 96; i < 4800; ++i) lastPeak = std::max(lastPeak, std::abs(b2.getSample(0, i)));
    check(firstPeak < 0.1f && lastPeak > 0.45f, "Bypass ramps a -30 dB notch smoothly back to unity (start "
                                               + std::to_string(firstPeak) + ", end " + std::to_string(lastPeak) + ")");
    check(maxStep(b2) <= 0.0656f * 1.05f, "Bypass transition has no click");
}

void testRobustness()
{
    std::printf("[2] NaN/Inf robustness\n");
    FilterManager manager;
    manager.prepare(48000);
    manager.addFilter(1000.0f, -12.0f, 30.0f, true, true, false);
    juce::AudioBuffer<float> b(2, 256);
    b.clear();
    b.setSample(0, 10, std::numeric_limits<float>::quiet_NaN());
    b.setSample(1, 11, std::numeric_limits<float>::infinity());
    manager.processAudio(b);
    juce::AudioBuffer<float> clean(2, 256);
    for (int i = 0; i < 256; ++i) { clean.setSample(0, i, 0.1f); clean.setSample(1, i, 0.1f); }
    manager.processAudio(clean);
    manager.processAudio(clean);
    check(std::isfinite(clean.getSample(0, 255)) && std::isfinite(clean.getSample(1, 255)),
          "notch state recovers after a NaN/Inf input block");

    PanicLimiter limiter;
    limiter.prepare(48000);
    juce::AudioBuffer<float> nanBuffer(1, 64);
    nanBuffer.clear();
    nanBuffer.setSample(0, 3, std::numeric_limits<float>::quiet_NaN());
    limiter.process(nanBuffer);
    check(std::isfinite(nanBuffer.getSample(0, 3)), "limiter output is finite for NaN input");
}
} // namespace

int main()
{
    testClickFreeRemoval();
    testRobustness();
    if (failures > 0) { std::printf("%d V2.1 check(s) FAILED\n", failures); return 1; }
    std::printf("All V2.1 checks passed.\n");
    return 0;
}
