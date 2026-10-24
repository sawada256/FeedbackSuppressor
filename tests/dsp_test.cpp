#include <iostream>
#include <vector>
#include <cmath>
#include "../source/dsp/FeedbackDetector.h"
#include "../source/dsp/FilterManager.h"
#include "../source/dsp/PanicLimiter.h"

// Keep verification assertions active in Release, while JUCE keeps its own
// configured build mode. Undefine only after the JUCE headers have been included.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace LiveAudioLab;

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "  Feedback Suppressor Comprehensive DSP " << std::endl;
    std::cout << "========================================" << std::endl;

    const double sampleRate = 44100.0;
    const int blockSize = 256;

    // 1. Latency Verification
    {
        std::cout << "\n[Test 1] 0-Sample Algorithmic Latency Test..." << std::endl;
        BiquadNotchFilter filter;
        filter.prepare(sampleRate);
        filter.setTarget(1000.0f, -12.0f, 35.0f);
        filter.snapToTarget();

        juce::AudioBuffer<float> testBuf(1, 16);
        testBuf.clear();
        testBuf.setSample(0, 0, 1.0f); // Dirac delta impulse at index 0

        filter.process(testBuf.getArrayOfWritePointers(), 1, testBuf.getNumSamples());

        // First sample must be non-zero (immediate minimum-phase response at index 0)
        float firstSample = testBuf.getSample(0, 0);
        std::cout << "  Impulse response at t=0: " << firstSample << std::endl;
        assert(std::abs(firstSample) > 0.1f && "Must respond immediately at sample 0");
        std::cout << "  --> PASS: Strictly 0-sample algorithmic delay!" << std::endl;
    }

    // 2. Silence / Disconnected Mic Immunity Test (No False Positives)
    {
        std::cout << "\n[Test 2] Silence & Line Noise Immunity Test (No Mic)..." << std::endl;
        FeedbackDetector detector;
        detector.prepare(sampleRate);
        detector.setSensitivity(0.0f); // Default sensitivity

        FilterManager manager;
        manager.prepare(sampleRate);
        manager.setOperationMode(OperationMode::LiveAuto);

        std::vector<DetectedFeedback> detections;

        // Run 1 second of preamp line noise (-50 dBFS) and quiet hum
        const int totalBlocks = static_cast<int>((sampleRate * 1.0) / blockSize);
        for (int b = 0; b < totalBlocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                float noise = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 0.002f; // ~ -54 dBFS
                block.setSample(0, i, noise);
            }

            detector.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            detections.clear();
            if (detector.processNextFrames(detections))
            {
                manager.handleDetections(detections);
            }
        }

        auto snapshots = manager.getFilterSnapshots();
        std::cout << "  Filters allocated during silence/noise: " << snapshots.size() << std::endl;
        assert(snapshots.empty() && "No filters should ever be allocated on silence or quiet noise!");
        std::cout << "  --> PASS: Zero false positives during silence and line noise!" << std::endl;
    }

    // 3. Singing Voice / Harmonic Sound Rejection Test
    {
        std::cout << "\n[Test 3] Singing Voice / Harmonic Sound Rejection Test..." << std::endl;
        FeedbackDetector detector;
        detector.prepare(sampleRate);
        detector.setSensitivity(0.0f);
        detector.setProfile(DetectionProfile::Speech);

        FilterManager manager;
        manager.prepare(sampleRate);
        manager.setOperationMode(OperationMode::LiveAuto);

        std::vector<DetectedFeedback> detections;

        // Simulate vocal sound: fundamental at 440 Hz (A4) + 2nd harmonic at 880 Hz + 3rd harmonic at 1320 Hz
        const int totalBlocks = static_cast<int>((sampleRate * 1.0) / blockSize);
        float p1 = 0.0f, p2 = 0.0f, p3 = 0.0f;
        const float inc1 = 2.0f * juce::MathConstants<float>::pi * 440.0f / static_cast<float>(sampleRate);
        const float inc2 = 2.0f * juce::MathConstants<float>::pi * 880.0f / static_cast<float>(sampleRate);
        const float inc3 = 2.0f * juce::MathConstants<float>::pi * 1320.0f / static_cast<float>(sampleRate);

        for (int b = 0; b < totalBlocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                float voice = 0.15f * std::sin(p1) + 0.10f * std::sin(p2) + 0.05f * std::sin(p3);
                p1 += inc1; p2 += inc2; p3 += inc3;
                block.setSample(0, i, voice);
            }

            detector.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            detections.clear();
            if (detector.processNextFrames(detections))
            {
                manager.handleDetections(detections);
            }
        }

        auto snapshots = manager.getFilterSnapshots();
        std::cout << "  Filters allocated during singing voice: " << snapshots.size() << std::endl;
        assert(snapshots.empty() && "Singing voice with harmonics must not be falsely notched!");
        std::cout << "  --> PASS: Natural harmonic voice is protected from false notching!" << std::endl;
    }

    // 4. Acoustic Feedback Howling Detection & Dynamic Notch Test
    {
        std::cout << "\n[Test 4] Real Acoustic Feedback Howling Detection & Dynamic Notch..." << std::endl;
        FeedbackDetector detector;
        detector.prepare(sampleRate);
        detector.setSensitivity(0.0f);

        FilterManager manager;
        manager.prepare(sampleRate);
        manager.setOperationMode(OperationMode::LiveAuto);
        manager.setDefaultQ(35.0f);
        manager.setMaxCutDepth(-24.0f);
        manager.setLiftDuration(15.0f);

        const float targetFeedbackFreq = 2200.0f; // 2.2 kHz feedback howling
        std::cout << "  Simulating 2.2 kHz feedback howling buildup (exponential loop)..." << std::endl;

        std::vector<DetectedFeedback> detections;
        bool detected = false;
        float detectedFreq = 0.0f;

        const int totalBlocks = static_cast<int>((sampleRate * 2.0) / blockSize);
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * targetFeedbackFreq / static_cast<float>(sampleRate);

        for (int b = 0; b < totalBlocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            float timeSec = static_cast<float>(b * blockSize) / static_cast<float>(sampleRate);

            for (int i = 0; i < blockSize; ++i)
            {
                float noise = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 0.001f;
                float sine = 0.0f;

                if (timeSec > 0.3f)
                {
                    // Exponential rise (runaway howling loop)
                    float growth = std::min(0.6f, 0.01f * std::exp((timeSec - 0.3f) * 7.0f));
                    sine = growth * std::sin(phase);
                    phase += phaseInc;
                }

                block.setSample(0, i, noise + sine);
            }

            detector.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);

            detections.clear();
            if (detector.processNextFrames(detections))
            {
                if (!detections.empty())
                {
                    detected = true;
                    detectedFreq = detections[0].frequency;
                    manager.handleDetections(detections);
                }
            }

            manager.processAudio(block);
            manager.updateTimers(static_cast<float>(blockSize) / static_cast<float>(sampleRate));
        }

        std::cout << "  Detection result: " << (detected ? "DETECTED" : "NOT DETECTED") << std::endl;
        assert(detected && "Acoustic feedback howling must be detected!");

        std::cout << "  Detected frequency: " << detectedFreq << " Hz (Target: " << targetFeedbackFreq << " Hz)" << std::endl;
        float freqError = std::abs(detectedFreq - targetFeedbackFreq) / targetFeedbackFreq * 100.0f;
        std::cout << "  Frequency accuracy error: " << freqError << "%" << std::endl;
        assert(freqError < 2.0f && "Frequency error must be under 2%");

        auto snapshots = manager.getFilterSnapshots();
        std::cout << "  Active notch filters assigned: " << snapshots.size() << std::endl;
        assert(!snapshots.empty() && "FilterManager must have allocated a filter slot");
        std::cout << "  Assigned notch gain: " << snapshots[0].currentGainDb << " dB" << std::endl;
        assert(snapshots[0].currentGainDb <= -6.0f && "Filter must attenuate feedback");
        std::cout << "  --> PASS: Feedback detected and suppressed accurately!" << std::endl;
    }

    // 5. Lookahead Limiter Delay Test (V3: the 3 ms delay belongs to the panic limiter)
    {
        std::cout << "\n[Test 5] Lookahead Limiter Delay Test..." << std::endl;
        PanicLimiter limiter;
        limiter.prepare(sampleRate, static_cast<int>(0.01 * sampleRate));

        // Zero latency mode
        limiter.setLookaheadSamples(0);
        limiter.reset();
        juce::AudioBuffer<float> buf0(1, 16);
        buf0.clear();
        buf0.setSample(0, 0, 0.5f);
        limiter.process(buf0);
        assert(buf0.getSample(0, 0) == 0.5f && "Zero latency mode must have 0 delay");
        std::cout << "  Zero latency mode: delay = 0 samples" << std::endl;

        // 3 ms lookahead
        const int expectedDelay = static_cast<int>(std::round(0.003 * sampleRate)); // 132 samples
        limiter.setLookaheadSamples(expectedDelay);
        limiter.reset();
        juce::AudioBuffer<float> bufLookahead(1, expectedDelay * 3);
        bufLookahead.clear();
        bufLookahead.setSample(0, 0, 0.5f);
        limiter.process(bufLookahead);
        int peakSampleIdx = -1;
        for (int i = 0; i < bufLookahead.getNumSamples(); ++i)
            if (std::abs(bufLookahead.getSample(0, i)) > 0.25f) { peakSampleIdx = i; break; }
        std::cout << "  Impulse emerged at sample: " << peakSampleIdx << " (expected " << expectedDelay << ")" << std::endl;
        assert(peakSampleIdx == expectedDelay && "Impulse must be delayed by exactly the reported 3 ms");

        // Lookahead: a sudden +12 dB step must never exceed the ceiling (no overshoot, no clipping needed)
        limiter.reset();
        juce::AudioBuffer<float> step(1, 2048);
        for (int i = 0; i < 2048; ++i) step.setSample(0, i, i < 1000 ? 0.1f : 4.0f);
        limiter.process(step);
        float maxPeak = 0.0f;
        for (int i = 0; i < 2048; ++i) maxPeak = std::max(maxPeak, std::abs(step.getSample(0, i)));
        std::cout << "  Lookahead step response peak: " << maxPeak << std::endl;
        assert(maxPeak <= PanicLimiter::CEILING + 0.01f && "Lookahead limiter must not overshoot the ceiling");
        std::cout << "  --> PASS: Lookahead limiter delays by exactly 3 ms and catches steps without overshoot!" << std::endl;
    }

    // 6. Panic Limiter Overload Clamping Test
    {
        std::cout << "\n[Test 6] Panic Limiter Overload Clamping Test..." << std::endl;
        PanicLimiter limiter;
        limiter.prepare(sampleRate, 512);
        limiter.setEnabled(true);

        juce::AudioBuffer<float> loudBuffer(1, 256);
        for (int i = 0; i < 256; ++i)
            loudBuffer.setSample(0, i, 4.0f); // +12 dBFS overload

        limiter.process(loudBuffer);

        float maxPeak = 0.0f;
        for (int i = 0; i < 256; ++i)
            maxPeak = std::max(maxPeak, std::abs(loudBuffer.getSample(0, i)));

        std::cout << "  Input peak: +12 dBFS (4.0)" << std::endl;
        std::cout << "  Output peak after limiter: " << maxPeak << " (Ceiling <= 1.0)" << std::endl;
        std::cout << "  Gain reduction reported: " << limiter.getGainReductionDb() << " dB" << std::endl;
        assert(maxPeak <= 1.0001f && "Limiter must prevent clipping above 1.0");
        assert(limiter.getGainReductionDb() < -6.0f && "Limiter must report significant gain reduction");
        std::cout << "  --> PASS: Panic limiter safely clamps explosive levels!" << std::endl;
    }

    // 7. Gradual Sine Wave Ramp Added to Audio Source (User Simulation)
    {
        std::cout << "\n[Test 7] Gradual Sine Wave Ramp Added to Audio Source (User Simulation)..." << std::endl;
        FeedbackDetector detector;
        detector.prepare(sampleRate);
        detector.setSensitivity(0.0f);

        FilterManager manager;
        manager.prepare(sampleRate);
        manager.setOperationMode(OperationMode::LiveAuto);

        const float sineFreq = 1000.0f;
        const int totalBlocks = static_cast<int>(sampleRate * 2.5 / blockSize);

        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * sineFreq / static_cast<float>(sampleRate);

        std::vector<DetectedFeedback> detections;
        bool detected = false;
        float detectedFreq = 0.0f;

        for (int b = 0; b < totalBlocks; ++b)
        {
            float timeSec = static_cast<float>(b * blockSize) / static_cast<float>(sampleRate);
            // Gradually increase amplitude from -30 dBFS (0.03) to 0 dBFS (1.0) over 2.5s
            float amp = 0.03f + (1.0f - 0.03f) * (timeSec / 2.5f);

            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                float bgMusic = ((rand() % 1000) / 500.0f - 1.0f) * 0.04f; // background audio
                float sine = amp * std::sin(phase);
                phase += phaseInc;
                block.setSample(0, i, bgMusic + sine);
            }

            detector.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            detections.clear();
            if (detector.processNextFrames(detections))
            {
                if (!detections.empty())
                {
                    detected = true;
                    detectedFreq = detections[0].frequency;
                    manager.handleDetections(detections);
                }
            }
        }

        std::cout << "  Gradual sine ramp detection: " << (detected ? "DETECTED" : "NOT DETECTED") << std::endl;
        if (detected)
        {
            std::cout << "  Detected frequency: " << detectedFreq << " Hz (Target: " << sineFreq << " Hz)" << std::endl;
            float err = std::abs(detectedFreq - sineFreq) / sineFreq * 100.0f;
            std::cout << "  Accuracy error: " << err << "%" << std::endl;
            assert(err < 1.0f && "Frequency error must be under 1%");
        }
        assert(detected && "Gradual sine wave ramp must be detected!");
        std::cout << "  --> PASS: Gradual sine wave added to audio source is accurately detected!" << std::endl;
    }

    // 8. -40 dB Sensitivity Range & Ultra-Conservative Mode Test
    {
        std::cout << "\n[Test 8] -40 dB Sensitivity Range & Conservative Thresholds Test..." << std::endl;
        FeedbackDetector detector;
        detector.prepare(sampleRate);
        detector.setSensitivity(-40.0f); // Ultra-conservative -40 dB sensitivity

        FilterManager manager;
        manager.prepare(sampleRate);
        manager.setOperationMode(OperationMode::LiveAuto);

        std::vector<DetectedFeedback> detections;

        // Moderate sine wave at -20 dBFS (should NOT trigger at -40 dB sensitivity)
        const float testFreq = 1500.0f;
        const int blocks = static_cast<int>(sampleRate * 0.5 / blockSize);
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * testFreq / static_cast<float>(sampleRate);

        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                block.setSample(0, i, 0.1f * std::sin(phase)); // -20 dBFS
                phase += phaseInc;
            }
            detector.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            detections.clear();
            if (detector.processNextFrames(detections))
                manager.handleDetections(detections);
        }

        auto snapshots = manager.getFilterSnapshots();
        std::cout << "  Filters allocated at -20 dBFS with -40 dB sens: " << snapshots.size() << " (Expected: 0)" << std::endl;
        assert(snapshots.empty() && "Moderate sound must NOT trigger at -40 dB sensitivity!");

        // Severe runaway feedback at -6 dBFS (0.50 linear): MUST trigger even at -40 dB sensitivity!
        bool triggeredOnSevere = false;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                block.setSample(0, i, 0.5f * std::sin(phase)); // -6 dBFS (loud feedback)
                phase += phaseInc;
            }
            detector.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            detections.clear();
            if (detector.processNextFrames(detections))
            {
                if (!detections.empty())
                {
                    triggeredOnSevere = true;
                    manager.handleDetections(detections);
                }
            }
        }

        std::cout << "  Severe feedback (-6 dBFS) detected: " << (triggeredOnSevere ? "YES" : "NO") << std::endl;
        assert(triggeredOnSevere && "Dangerous feedback must trigger even at -40 dB sensitivity!");
        std::cout << "  --> PASS: -40 dB sensitivity cleanly ignores moderate signals and catches severe runaway feedback!" << std::endl;
    }

    // 9. High Focus Profile: Lower Baseline & Enhanced High-Frequency Sensitivity Test
    {
        std::cout << "\n[Test 9] High Focus Profile Test (Lower Baseline & Earlier Squeal Suppression)..." << std::endl;
        FeedbackDetector detSpeech;
        detSpeech.prepare(sampleRate);
        detSpeech.setProfile(DetectionProfile::Speech);

        FeedbackDetector detHigh;
        detHigh.prepare(sampleRate);
        detHigh.setProfile(DetectionProfile::HighFocus);

        // Feed same pink noise / ambient signal to both detectors
        const int blocks = static_cast<int>((sampleRate * 0.5) / blockSize);
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                float s = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 0.05f;
                block.setSample(0, i, s);
            }
            detSpeech.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            detHigh.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);

            std::vector<DetectedFeedback> d;
            detSpeech.processNextFrames(d);
            detHigh.processNextFrames(d);
        }

        std::vector<float> speechMags, speechBase;
        std::vector<float> highMags, highBase;
        detSpeech.copySpectrumTo(speechMags, speechBase);
        detHigh.copySpectrumTo(highMags, highBase);

        // Compare baseline at 4 kHz (bin index for 4000 Hz)
        int bin4k = static_cast<int>(std::round(4000.0f * static_cast<float>(FeedbackDetector::FFT_SIZE) / static_cast<float>(sampleRate)));
        float baseSpeech4k = speechBase[static_cast<size_t>(bin4k)];
        float baseHigh4k = highBase[static_cast<size_t>(bin4k)];

        std::cout << "  Speech baseline at 4 kHz: " << baseSpeech4k << " dBFS" << std::endl;
        std::cout << "  High Focus baseline at 4 kHz: " << baseHigh4k << " dBFS" << std::endl;
        assert(baseHigh4k < baseSpeech4k - 2.0f && "High Focus baseline at 4 kHz must be significantly lower than Speech baseline!");
        std::cout << "  --> Baseline at 4 kHz is " << (baseSpeech4k - baseHigh4k) << " dB lower in High Focus profile!" << std::endl;

        // Verify that High Focus detects 4 kHz feedback at lower gain
        FilterManager mgrHigh;
        mgrHigh.prepare(sampleRate);
        mgrHigh.setOperationMode(OperationMode::LiveAuto);

        bool highDetected = false;
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * 4000.0f / static_cast<float>(sampleRate);

        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                // Soft 4 kHz squeal at -30 dBFS (amplitude 0.0316)
                block.setSample(0, i, 0.0316f * std::sin(phase));
                phase += phaseInc;
            }
            detHigh.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            std::vector<DetectedFeedback> detections;
            if (detHigh.processNextFrames(detections))
            {
                if (!detections.empty())
                {
                    highDetected = true;
                    mgrHigh.handleDetections(detections);
                    break;
                }
            }
        }

        std::cout << "  High-frequency squeal detected in High Focus mode: " << (highDetected ? "YES" : "NO") << std::endl;
        assert(highDetected && "High Focus profile must detect 4 kHz feedback even at lower gain!");
        std::cout << "  --> PASS: High Focus profile successfully lowers baseline and catches squeal at low gain!" << std::endl;
    }

    // 10. Low Focus Profile: Lower Baseline & Enhanced Low-Frequency Resonance Sensitivity Test
    {
        std::cout << "\n[Test 10] Low Focus Profile Test (Lower Baseline & Earlier Boom Suppression)..." << std::endl;
        FeedbackDetector detSpeech;
        detSpeech.prepare(sampleRate);
        detSpeech.setProfile(DetectionProfile::Speech);

        FeedbackDetector detLow;
        detLow.prepare(sampleRate);
        detLow.setProfile(DetectionProfile::LowFocus);

        const int blocks = static_cast<int>((sampleRate * 0.5) / blockSize);
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                float s = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 0.05f;
                block.setSample(0, i, s);
            }
            detSpeech.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            detLow.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);

            std::vector<DetectedFeedback> d;
            detSpeech.processNextFrames(d);
            detLow.processNextFrames(d);
        }

        std::vector<float> speechMags, speechBase;
        std::vector<float> lowMags, lowBase;
        detSpeech.copySpectrumTo(speechMags, speechBase);
        detLow.copySpectrumTo(lowMags, lowBase);

        // Compare baseline at 300 Hz
        int bin300 = static_cast<int>(std::round(300.0f * static_cast<float>(FeedbackDetector::FFT_SIZE) / static_cast<float>(sampleRate)));
        float baseSpeech300 = speechBase[static_cast<size_t>(bin300)];
        float baseLow300 = lowBase[static_cast<size_t>(bin300)];

        std::cout << "  Speech baseline at 300 Hz: " << baseSpeech300 << " dBFS" << std::endl;
        std::cout << "  Low Focus baseline at 300 Hz: " << baseLow300 << " dBFS" << std::endl;
        assert(baseLow300 < baseSpeech300 - 2.0f && "Low Focus baseline at 300 Hz must be significantly lower than Speech baseline!");
        std::cout << "  --> Baseline at 300 Hz is " << (baseSpeech300 - baseLow300) << " dB lower in Low Focus profile!" << std::endl;

        // Verify detection of 300 Hz booming resonance
        FilterManager mgrLow;
        mgrLow.prepare(sampleRate);
        mgrLow.setOperationMode(OperationMode::LiveAuto);

        bool lowDetected = false;
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * 300.0f / static_cast<float>(sampleRate);

        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                // 300 Hz feedback at -28 dBFS
                block.setSample(0, i, 0.04f * std::sin(phase));
                phase += phaseInc;
            }
            detLow.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            std::vector<DetectedFeedback> detections;
            if (detLow.processNextFrames(detections))
            {
                if (!detections.empty())
                {
                    lowDetected = true;
                    mgrLow.handleDetections(detections);
                    break;
                }
            }
        }

        std::cout << "  Low-frequency resonance detected in Low Focus mode: " << (lowDetected ? "YES" : "NO") << std::endl;
        assert(lowDetected && "Low Focus profile must detect 300 Hz boom feedback!");
        std::cout << "  --> PASS: Low Focus profile successfully lowers baseline and catches boom at low gain!" << std::endl;
    }

    // 11. Proposal 1-3 Verification: Weak Feedback Buried Inside Music Mix (Negative SNR Test)
    {
        std::cout << "\n[Test 11] Weak Feedback Buried in Music Mix Test (Aggressive Phase-Lock Mode)..." << std::endl;

        FeedbackDetector detStandard;
        detStandard.prepare(sampleRate);
        detStandard.setDetectionMode(DetectionMode::Standard); // Safe conservative mode

        FeedbackDetector detAggressive;
        detAggressive.prepare(sampleRate);
        detAggressive.setDetectionMode(DetectionMode::Aggressive); // Mix Hunter mode

        // Simulate rich music ambience (ambient noise floor ~ -26 dBFS)
        // plus a weak feedback squeal at 2200 Hz (buried close to background level)
        const int blocks = static_cast<int>((sampleRate * 0.8) / blockSize);

        float pSqueal = 0.0f;
        const float incSqueal = 2.0f * juce::MathConstants<float>::pi * 2200.0f / static_cast<float>(sampleRate);

        bool standardDetected = false;
        bool aggressiveDetected = false;

        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                // Ambient background noise/mix (~ -26 dBFS)
                float noise = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 0.08f;
                // Weak squeal buried in noise (~ -32 dBFS, amp 0.025)
                float squeal = 0.025f * std::sin(pSqueal);

                block.setSample(0, i, noise + squeal);
                pSqueal += incSqueal;
            }

            detStandard.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            detAggressive.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);

            std::vector<DetectedFeedback> dStd;
            if (detStandard.processNextFrames(dStd))
            {
                for (const auto& df : dStd)
                {
                    if (std::abs(df.frequency - 2200.0f) < 50.0f)
                        standardDetected = true;
                }
            }

            std::vector<DetectedFeedback> dAgg;
            if (detAggressive.processNextFrames(dAgg))
            {
                for (const auto& df : dAgg)
                {
                    if (std::abs(df.frequency - 2200.0f) < 50.0f)
                        aggressiveDetected = true;
                }
            }
        }

        std::cout << "  Standard (Safe) mode triggered on buried tone: " << (standardDetected ? "YES" : "NO (Safely Ignored)") << std::endl;
        std::cout << "  Aggressive (Phase-Lock) mode triggered on buried tone: " << (aggressiveDetected ? "YES (Detected)" : "NO") << std::endl;
        assert(!standardDetected && "Standard mode MUST safely protect music without false-positive triggers!");
        assert(aggressiveDetected && "Aggressive mode with Phase-Lock MUST catch buried feedback!");
        std::cout << "  --> PASS: Aggressive Phase-Lock mode cleanly hunts buried feedback without corrupting Standard mode!" << std::endl;
    }

    // 12. -60 dB Ultra-Conservative Sensitivity Test
    {
        std::cout << "\n[Test 12] -60 dB Ultra-Conservative Sensitivity Range Test..." << std::endl;
        FeedbackDetector detUltraLow;
        detUltraLow.prepare(sampleRate);
        detUltraLow.setSensitivity(-60.0f); // Minimum sensitivity

        FilterManager mgr;
        mgr.prepare(sampleRate);
        mgr.setOperationMode(OperationMode::LiveAuto);

        // Run loud 1 kHz signal at -12 dBFS (amp 0.25)
        const int blocks = static_cast<int>((sampleRate * 0.5) / blockSize);
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * 1000.0f / static_cast<float>(sampleRate);

        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                block.setSample(0, i, 0.25f * std::sin(phase)); // -12 dBFS loud sound
                phase += phaseInc;
            }
            detUltraLow.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            std::vector<DetectedFeedback> d;
            if (detUltraLow.processNextFrames(d))
                mgr.handleDetections(d);
        }

        auto snaps = mgr.getFilterSnapshots();
        std::cout << "  Filters allocated at -12 dBFS with -60 dB sens: " << snaps.size() << " (Expected: 0)" << std::endl;
        assert(snaps.empty() && "-60 dB sensitivity must completely ignore signals at -12 dBFS!");

        // Near full scale runaway feedback (-2.0 dBFS, amp 0.79)
        bool triggeredOnRunaway = false;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                block.setSample(0, i, 0.79f * std::sin(phase)); // -2.0 dBFS runaway overload
                phase += phaseInc;
            }
            detUltraLow.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
            std::vector<DetectedFeedback> d;
            if (detUltraLow.processNextFrames(d) && !d.empty())
            {
                triggeredOnRunaway = true;
                break;
            }
        }

        std::cout << "  Extreme runaway feedback (-2 dBFS) detected at -60 dB sens: " << (triggeredOnRunaway ? "YES" : "NO") << std::endl;
        assert(triggeredOnRunaway && "Extreme runaway feedback must trigger even at -60 dB sensitivity!");
        std::cout << "  --> PASS: -60 dB sensitivity successfully silences all moderate signals and catches true runaway blasts!" << std::endl;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "  ALL 12 COMPREHENSIVE TESTS PASSED!    " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
