#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include "dsp/FeedbackDetector.h"
#include "dsp/FilterManager.h"

using namespace LiveAudioLab;
namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Result { int targetEvents = 0; int otherEvents = 0; double firstTime = -1; float error = 0; };
Result simulate(double sr, DetectionMode mode, float gain, bool howl, bool stereo, bool vibrato = false,
                bool transients = false, bool suppress = false, float toneAmplitude = 0.01f)
{
    FeedbackDetector detector;
    detector.prepare(sr);
    detector.setProfile(DetectionProfile::Music);
    detector.setDetectionMode(mode);
    FilterManager manager;
    manager.prepare(sr);
    manager.setDefaultQ(60.0f);
    constexpr float target = 2371.0f;
    std::mt19937 generator(17);
    std::uniform_real_distribution<float> noise(-0.12f, 0.12f);
    Result result;
    double musicPhase = 0, targetPhase = 0;
    constexpr int blockSize = 256;
    for (int start = 0; start < static_cast<int>(sr * 2.0); start += blockSize)
    {
        juce::AudioBuffer<float> block(stereo ? 2 : 1, blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const double time = (start + i) / sr;
            musicPhase += juce::MathConstants<double>::twoPi * 220.0 / sr;
            double music = noise(generator);
            for (int harmonic = 1; harmonic <= 6; ++harmonic)
                music += (0.16 / harmonic) * std::sin(harmonic * musicPhase);
            const double freq = target + (vibrato ? 28.0 * std::sin(juce::MathConstants<double>::twoPi * 5.0 * time) : 0.0);
            targetPhase += juce::MathConstants<double>::twoPi * freq / sr;
            float tone = 0;
            if (howl && time >= 0.5) tone = toneAmplitude * static_cast<float>(std::sin(targetPhase));
            if (vibrato) tone = 0.03f * static_cast<float>(std::sin(targetPhase));
            if (transients && std::fmod(time, 0.3) < 0.045)
                tone = 0.03f * static_cast<float>(std::sin(targetPhase));
            block.setSample(0, i, gain * (static_cast<float>(music) + tone));
            if (stereo) block.setSample(1, i, gain * (static_cast<float>(music) - tone));
        }
        if (suppress) manager.processAudio(block);
        detector.pushSamples(block.getArrayOfReadPointers(), block.getNumChannels(), blockSize);
        std::vector<DetectedFeedback> events;
        detector.processNextFrames(events);
        for (const auto& event : events)
        {
            if (std::abs(event.frequency - target) < 40.0f)
            {
                ++result.targetEvents;
                if (result.firstTime < 0) result.firstTime = (start + blockSize) / sr;
                result.error = std::abs(event.frequency - target);
            }
            else
            {
                ++result.otherEvents;
                std::cerr << "Unexpected peak: " << event.frequency << " Hz, confidence=" << event.confidence
                          << ", prominence=" << event.prominenceDb << " dB, growth=" << event.growthRate << " dB/ms\n";
            }
        }
        if (suppress) manager.handleDetections(events);
        manager.updateTimers(static_cast<float>(blockSize / sr));
    }
    if (suppress)
    {
        const auto slots = manager.getFilterSnapshots();
        require(slots.size() == 1, "residual control allocates exactly one notch");
        require(slots[0].targetGainDb > -20.0f, "successful suppression stops before maximum cut");
        std::cout << "residual cut=" << slots[0].targetGainDb << " dB, events=" << result.targetEvents << '\n';
    }
    return result;
}
// Sampled acoustic path: 256-sample propagation delay followed by a resonant
// bandpass. Output is fed back into the next microphone block, so suppression
// must stabilize the loop, rather than merely recognize an injected sine.
float closedLoop(bool suppress)
{
    constexpr double sr = 48000, frequency = 2250;
    constexpr int blockSize = 256;
    FeedbackDetector detector; detector.prepare(sr);
    detector.setProfile(DetectionProfile::Music);
    detector.setDetectionMode(DetectionMode::Advanced);
    FilterManager manager; manager.prepare(sr); manager.setDefaultQ(45);
    const double w = juce::MathConstants<double>::twoPi * frequency / sr;
    const double alpha = std::sin(w) / (2.0 * 40.0);
    const double b0 = alpha / (1.0 + alpha), b2 = -b0;
    const double a1 = -2.0 * std::cos(w) / (1.0 + alpha), a2 = (1.0 - alpha) / (1.0 + alpha);
    double s1 = 0, s2 = 0;
    std::array<float, blockSize> feedback {};
    std::mt19937 generator(29);
    std::uniform_real_distribution<float> noise(-0.02f, 0.02f);
    double finalPower = 0;
    int finalSamples = 0;
    for (int b = 0; b < 750; ++b)
    {
        juce::AudioBuffer<float> block(1, blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const double time = (b * blockSize + i) / sr;
            double music = noise(generator);
            for (int h = 1; h <= 6; ++h) music += 0.05 / h * std::sin(juce::MathConstants<double>::twoPi * 220 * h * time);
            // Smooth saturation models finite amplifier headroom in the control run.
            block.setSample(0, i, static_cast<float>(std::tanh(music + 1.08 * feedback[static_cast<size_t>(i)])));
        }
        if (suppress) manager.processAudio(block);
        for (int i = 0; i < blockSize; ++i)
        {
            const double input = block.getSample(0, i);
            const double output = b0 * input + s1;
            s1 = -a1 * output + s2;
            s2 = b2 * input - a2 * output;
            feedback[static_cast<size_t>(i)] = static_cast<float>(output);
            if (b > 562) { finalPower += output * output; ++finalSamples; }
        }
        if (suppress)
        {
            detector.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            std::vector<DetectedFeedback> events; detector.processNextFrames(events);
            manager.handleDetections(events);
            manager.updateTimers(static_cast<float>(blockSize / sr));
        }
    }
    if (suppress)
    {
        const auto slots = manager.getFilterSnapshots();
        require(!slots.empty(), "closed-loop instability creates a notch");
        bool matched = false;
        for (const auto& slot : slots) if (std::abs(slot.frequency - frequency) < 25) matched = true;
        require(matched, "closed-loop notch locates the resonant acoustic mode");
    }
    return static_cast<float>(std::sqrt(finalPower / finalSamples));
}

}
int main()
{
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (auto mode : {DetectionMode::Advanced, DetectionMode::Aggressive})
            for (float gain : {1.0f, 0.1f})
            {
                const auto result = simulate(sr, mode, gain, true, true);
                std::cout << "sr=" << sr << " mode=" << static_cast<int>(mode) << " gain=" << gain
                          << " detected=" << result.targetEvents << " other=" << result.otherEvents
                          << " latency=" << result.firstTime - 0.5 << " error=" << result.error << " Hz\n";
                require(result.targetEvents > 0, "buried antiphase feedback at each sample rate/gain");
                require(result.firstTime >= 0.5 && result.firstTime < 1.3, "weak-feedback confirmation within 800 ms");
                require(result.error < sr / FeedbackDetector::FFT_SIZE * 0.25, "sub-bin frequency accuracy");
                require(result.otherEvents == 0, "musical harmonics are protected in mixed audio");
                const auto clean = simulate(sr, mode, gain, false, false);
                require(clean.targetEvents == 0 && clean.otherEvents == 0, "clean harmonic music/noise has no events");
            }
    for (auto mode : {DetectionMode::Advanced, DetectionMode::Aggressive})
    {
        for (bool transient : {false, true})
        {
            const auto result = simulate(48000, mode, 1, false, false, !transient, transient);
            require(result.targetEvents == 0 && result.otherEvents == 0, "vibrato and brief tonal attacks are protected");
        }
        simulate(48000, mode, 1, true, false, false, false, true);
    }
    {
        FeedbackDetector detector;
        detector.prepare(48000);
        detector.setDetectionMode(DetectionMode::Advanced);
        juce::AudioBuffer<float> oversized(2, 20000);
        oversized.clear();
        detector.pushSamples(oversized.getArrayOfReadPointers(), 2, 20000);
        std::vector<DetectedFeedback> events;
        require(!detector.processNextFrames(events), "FIFO overflow safely drops a block");
        detector.reset();
        juce::AudioBuffer<float> block(1, 256);
        // V3: a constant (non-growing) tone needs the 700 ms steady-path hold, so
        // the recovery check runs 1.5 s instead of 0.75 s.
        for (int b = 0; b < 280; ++b)
        {
            for (int i = 0; i < 256; ++i)
                block.setSample(0, i, 0.02f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 1943 * (b * 256 + i) / 48000)));
            detector.pushSamples(block.getArrayOfReadPointers(), 1, 256);
            detector.processNextFrames(events);
        }
        require(!events.empty(), "detector recovers after reset/overflow");
        detector.reset();
        block.clear(); events.clear();
        for (int b = 0; b < 60; ++b) {
            detector.pushSamples(block.getArrayOfReadPointers(), 1, 256);
            detector.processNextFrames(events);
        }
        require(events.empty(), "silence after reset does not reuse phase/growth history");
    }
    {
        FeedbackDetector detector; detector.prepare(48000);
        detector.setDetectionMode(DetectionMode::Advanced);
        detector.setProfile(DetectionProfile::Music);
        const double halfBinTone = 48000.0 / FeedbackDetector::FFT_SIZE * 76.5;
        juce::AudioBuffer<float> block(1, 256);
        std::mt19937 generator(51);
        std::uniform_real_distribution<float> noise(-0.008f, 0.008f);
        bool first = false, second = false;
        for (int b = 0; b < 400; ++b) // steady tones: allow the V3 steady-path hold
        {
            for (int i = 0; i < 256; ++i)
            {
                const double time = (b * 256 + i) / 48000.0;
                block.setSample(0, i, noise(generator) + 0.012f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * halfBinTone * time))
                    + 0.01f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 3777.2 * time)));
            }
            detector.pushSamples(block.getArrayOfReadPointers(), 1, 256);
            std::vector<DetectedFeedback> events; detector.processNextFrames(events);
            for (const auto& event : events)
            {
                if (std::abs(event.frequency - halfBinTone) < 10) first = true;
                if (std::abs(event.frequency - 3777.2f) < 10) second = true;
            }
        }
        require(first && second, "multiple peaks including a noisy exact half-bin frequency are tracked");
    }
    {
        FilterManager manager;
        manager.prepare(48000);
        DetectedFeedback event; event.frequency = 2371; event.suggestedCutDb = 6;
        std::vector<DetectedFeedback> batch(50, event);
        manager.handleDetections(batch);
        auto slots = manager.getFilterSnapshots();
        require(slots.size() == 1 && slots[0].targetGainDb == -6, "batched stale events do not deepen a new notch");
        manager.updateTimers(0.16f);
        manager.handleDetections(batch);
        slots = manager.getFilterSnapshots();
        require(slots[0].targetGainDb == -9, "only one 3 dB step after settling");
        manager.setFilterLocked(slots[0].id, true);
        manager.updateTimers(1); manager.handleDetections(batch);
        require(manager.getFilterSnapshots()[0].targetGainDb == -9, "manual lock freezes depth");
        manager.setFilterLocked(slots[0].id, false);
        manager.setFilterBypassed(slots[0].id, true);
        manager.handleDetections(batch);
        require(manager.getFilterSnapshots()[0].targetGainDb == -9, "bypassed filters are not deepened");
        manager.clearAll();
        for (int i = 0; i < FilterManager::MAX_LIVE_FILTERS; ++i)
            manager.addManualFilter(300 + 100.0f * i, -6, 35, false);
        slots = manager.getFilterSnapshots();
        for (const auto& slot : slots) manager.setFilterLocked(slot.id, true);
        event.frequency = 5000; manager.handleDetections({event});
        for (const auto& slot : manager.getFilterSnapshots()) require(slot.frequency < 5000, "full locked bank cannot be stolen");
    }
    // More difficult boundary case: about -31 dB relative to total music RMS.
    // Track the measured response rather than claiming a universal fast response.
    const auto veryWeak = simulate(48000, DetectionMode::Advanced, 1, true, true, false, false, false, 0.006f);
    require(veryWeak.targetEvents > 0 && veryWeak.otherEvents == 0, "extremely weak 48 kHz feedback is detected without unrelated cuts");
    std::cout << "Very weak (-31 dB relative) response=" << veryWeak.firstTime - 0.5 << " seconds\n";
    const float uncontrolled = closedLoop(false);
    const float controlled = closedLoop(true);
    const float reductionDb = 20.0f * std::log10(controlled / uncontrolled);
    std::cout << "Closed-loop resonant energy reduction=" << reductionDb << " dB\n";
    require(reductionDb < -15.0f, "notches stabilize the simulated feedback loop by at least 15 dB");
    std::cout << "All music detection regression checks passed.\n";
}
