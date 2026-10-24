// V3 regression tests: real-time behaviour of the filter bank, click-free
// parameter changes, recurrence memory, decimated analysis, robustness, and
// measurable detection metrics (detection time, false positives, frequency error).
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

void testRecurrenceAndBandwidth()
{
    std::printf("[2] Recurrence memory, low-frequency bandwidth floor, release\n");
    FilterManager manager;
    manager.prepare(48000);
    manager.setLiftDuration(5.0f);
    DetectedFeedback event;
    event.frequency = 2000.0f;
    event.suggestedCutDb = 6.0f;
    manager.handleDetections({ event });
    for (int i = 0; i < 4; ++i) { manager.updateTimers(0.2f); manager.handleDetections({ event }); }
    const float deepest = manager.getFilterSnapshots()[0].targetGainDb;
    check(deepest <= -15.0f, "residual feedback deepens the notch in 3 dB steps (" + std::to_string(deepest) + " dB)");

    for (int i = 0; i < 200 && ! manager.getFilterSnapshots().empty(); ++i)
        manager.updateTimers(0.1f);
    check(manager.getFilterSnapshots().empty(), "Live notch is released and freed after the lift timer");

    manager.handleDetections({ event });
    const float restart = manager.getFilterSnapshots()[0].targetGainDb;
    check(restart <= deepest + 3.5f, "a recurring frequency restarts close to its previous depth ("
                                     + std::to_string(restart) + " dB)");

    event.frequency = 100.0f;
    manager.handleDetections({ event });
    float lowQ = 0.0f;
    for (const auto& s : manager.getFilterSnapshots()) if (std::abs(s.frequency - 100.0f) < 1.0f) lowQ = s.q;
    check(lowQ > 0.0f && lowQ <= 25.0f + 1e-3f, "100 Hz notch is at least 4 Hz wide (Q " + std::to_string(lowQ) + ")");
}

void testRobustness()
{
    std::printf("[3] NaN/Inf robustness and decimated analysis\n");
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
    limiter.prepare(48000, 480);
    juce::AudioBuffer<float> nanBuffer(1, 64);
    nanBuffer.clear();
    nanBuffer.setSample(0, 3, std::numeric_limits<float>::quiet_NaN());
    limiter.process(nanBuffer);
    check(std::isfinite(nanBuffer.getSample(0, 3)), "limiter output is finite for NaN input");

    for (double sr : { 88200.0, 96000.0, 192000.0 })
    {
        FeedbackDetector detector;
        detector.prepare(sr);
        detector.setDetectionMode(DetectionMode::Advanced);
        detector.setProfile(DetectionProfile::Music);
        std::mt19937 rng(3);
        std::normal_distribution<float> noise(0.0f, 0.002f);
        double ph = 0;
        float found = -1.0f;
        juce::AudioBuffer<float> block(1, 512);
        std::vector<DetectedFeedback> events;
        for (int start = 0; start < static_cast<int>(sr * 2.0) && found < 0; start += 512)
        {
            for (int i = 0; i < 512; ++i)
            {
                const double t = (start + i) / sr;
                ph += twoPi * 3333.3 / sr;
                const double level = std::min(0.3, 0.001 * std::pow(10.0, 1.5 * t)); // +30 dB/s
                block.setSample(0, i, static_cast<float>(level * std::sin(ph)) + noise(rng));
            }
            detector.pushSamples(block.getArrayOfReadPointers(), 1, 512);
            events.clear();
            detector.processNextFrames(events);
            for (const auto& e : events) if (std::abs(e.frequency - 3333.3f) < 20.0f) found = e.frequency;
        }
        check(detector.getSampleRate() <= 48000.0 && found > 0 && std::abs(found - 3333.3f) < 0.5f,
              "host " + std::to_string(static_cast<int>(sr)) + " Hz analysed at " + std::to_string(static_cast<int>(detector.getSampleRate()))
              + " Hz, tone found at " + std::to_string(found) + " Hz");
    }
}

// Synthetic voice (harmonics through vowel formants, vibrato, slow drift).
struct Singer
{
    double sr, phase = 0, drift = 0;
    std::mt19937 rng;
    std::normal_distribution<double> g { 0.0, 1.0 };
    Singer(double s, unsigned seed) : sr(s), rng(seed) {}
    double next(double t)
    {
        static const double notes[] = { 220.0, 246.9, 277.2, 293.7, 329.6, 293.7, 277.2, 246.9 };
        const double noteTime = std::fmod(t, 0.8);
        const double f0 = notes[static_cast<int>(t / 0.8) % 8]
                          * std::pow(2.0, (noteTime > 0.3 ? 40.0 * std::sin(twoPi * 5.5 * (noteTime - 0.3)) : 0.0) / 1200.0);
        drift = 0.9999 * drift + 0.00005 * g(rng);
        const double f = f0 * (1.0 + drift);
        phase += twoPi * f / sr;
        double out = 0;
        for (int h = 1; h * f < 8000.0; ++h)
        {
            double env = 0.02;
            for (auto [fc, bw, gain] : { std::tuple { 700.0, 110.0, 1.0 }, { 1220.0, 120.0, 0.5 }, { 2600.0, 160.0, 0.25 } })
            {
                const double x = (h * f - fc) / bw;
                env += gain / (1.0 + x * x);
            }
            out += env / std::sqrt(static_cast<double>(h)) * std::sin(h * phase);
        }
        const double attack = std::min(1.0, noteTime / 0.04), release = std::min(1.0, (0.8 - noteTime) / 0.06);
        return 0.15 * std::max(0.0, std::min(attack, release)) * out;
    }
};

void testDetectionMetrics()
{
    std::printf("[4] Detection metrics (Advanced, 48 kHz)\n");
    const double sr = 48000;
    // (a) false positives on 30 s of sung vibrato phrase
    {
        FeedbackDetector detector;
        detector.prepare(sr);
        detector.setDetectionMode(DetectionMode::Advanced);
        Singer singer(sr, 3);
        std::mt19937 rng(5);
        std::normal_distribution<float> noise(0.0f, 0.002f);
        juce::AudioBuffer<float> block(1, 256);
        std::vector<DetectedFeedback> events;
        int count = 0;
        for (int start = 0; start < static_cast<int>(sr * 30.0); start += 256)
        {
            for (int i = 0; i < 256; ++i) block.setSample(0, i, static_cast<float>(singer.next((start + i) / sr)) + noise(rng));
            detector.pushSamples(block.getArrayOfReadPointers(), 1, 256);
            events.clear();
            detector.processNextFrames(events);
            count += static_cast<int>(events.size());
        }
        const double perMinute = count * 2.0;
        check(perMinute <= 20.0, "sung vibrato phrase: " + std::to_string(perMinute) + " events/min (V2: 26, limit 20)");
    }
    // (b) detection time and frequency error of a 100 dB/s build-up over the singer
    for (double target : { 1253.1, 4111.4 })
    {
        FeedbackDetector detector;
        detector.prepare(sr);
        detector.setDetectionMode(DetectionMode::Advanced);
        Singer singer(sr, 7);
        juce::AudioBuffer<float> block(1, 256);
        std::vector<DetectedFeedback> events;
        double fbPhase = 0, first = -1;
        float freq = 0;
        for (int start = 0; start < static_cast<int>(sr * 3.0) && first < 0; start += 256)
        {
            for (int i = 0; i < 256; ++i)
            {
                const double t = (start + i) / sr;
                double fb = 0;
                if (t >= 1.5)
                {
                    fbPhase += twoPi * target / sr;
                    fb = std::pow(10.0, std::min(-6.0, -60.0 + 100.0 * (t - 1.5)) / 20.0) * std::sin(fbPhase);
                }
                block.setSample(0, i, static_cast<float>(singer.next(t) + fb));
            }
            detector.pushSamples(block.getArrayOfReadPointers(), 1, 256);
            events.clear();
            detector.processNextFrames(events);
            for (const auto& e : events)
                if (std::abs(e.frequency - target) < 12.0 && first < 0) { first = (start + 256) / sr - 1.5; freq = e.frequency; }
        }
        check(first > 0 && first < 0.35, "100 dB/s build-up at " + std::to_string(target) + " Hz detected after "
                                         + std::to_string(first * 1000.0) + " ms (V2 mean: 324 ms, limit 350 ms)");
        check(std::abs(freq - target) < 1.0, "first-event frequency error " + std::to_string(std::abs(freq - target)) + " Hz (< 1 Hz)");
    }
}
} // namespace

int main()
{
    testClickFreeRemoval();
    testRecurrenceAndBandwidth();
    testRobustness();
    testDetectionMetrics();
    if (failures > 0)
    {
        std::printf("%d V3 check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("All V3 checks passed.\n");
    return 0;
}
