#include <iostream>
#include <vector>
#include <cmath>
#include "../source/dsp/FeedbackDetector.h"

using namespace LiveAudioLab;

int main()
{
    const double sampleRate = 44100.0;
    const int blockSize = 512;

    FeedbackDetector detector;
    detector.prepare(sampleRate);
    detector.setSensitivity(0.0f);

    std::cout << "--- Diagnosing Sine Wave Detection ---" << std::endl;

    // Test a sine wave at 1000 Hz (which is not an exact FFT bin center: 1000 / 21.533 = 46.44)
    // and at 2500 Hz (2500 / 21.533 = 116.10)
    // Gradually ramp amplitude from -40 dBFS (0.01) to 0 dBFS (1.0) over 3 seconds
    const float sineFreq = 1000.0f;
    const int totalBlocks = static_cast<int>(sampleRate * 3.0 / blockSize);

    float phase = 0.0f;
    const float phaseInc = 2.0f * juce::MathConstants<float>::pi * sineFreq / static_cast<float>(sampleRate);

    std::vector<DetectedFeedback> detections;
    bool anyDetected = false;

    for (int b = 0; b < totalBlocks; ++b)
    {
        float timeSec = static_cast<float>(b * blockSize) / static_cast<float>(sampleRate);
        // Gradually increase volume from 0.01 (-40 dBFS) to 1.0 (0 dBFS)
        float amp = 0.01f + (1.0f - 0.01f) * (timeSec / 3.0f);

        juce::AudioBuffer<float> block(1, blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            // Simulated audio source (moderate music/noise) + increasing sine wave
            float source = ((rand() % 1000) / 500.0f - 1.0f) * 0.05f; // -26dB background audio
            float sine = amp * std::sin(phase);
            phase += phaseInc;
            block.setSample(0, i, source + sine);
        }

        detector.pushSamples(block.getArrayOfReadPointers(), 1, blockSize);
        detections.clear();
        if (detector.processNextFrames(detections))
        {
            if (!detections.empty())
            {
                anyDetected = true;
                std::cout << "DETECTION at time " << timeSec << "s, amp = " << amp
                          << " (" << 20.0f * std::log10(amp) << " dBFS), freq = "
                          << detections[0].frequency << " Hz" << std::endl;
            }
        }
    }

    std::cout << "Final result: " << (anyDetected ? "DETECTED" : "FAILED (NOT DETECTED)") << std::endl;
    return 0;
}
