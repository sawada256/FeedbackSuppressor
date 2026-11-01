#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace LiveAudioLab
{

/**
 * Optional Lookahead Delay line for main audio path.
 * When enabled (e.g. 1ms ~ 5ms), it delays audio slightly so notch filters
 * have time to engage before sudden loud feedback peaks reach the output.
 * When disabled (0ms), it operates at strictly 0 samples delay.
 */
class LookaheadDelay
{
public:
    LookaheadDelay() = default;

    void prepare(double newSampleRate, int maxDelaySamples = 1024)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        bufferSize = juce::jmax(maxDelaySamples, static_cast<int>(sampleRate * 0.02)); // 20ms buffer
        delayBuffer.setSize(2, bufferSize);
        delayBuffer.clear();
        writeIndex = 0;
        currentDelay = targetDelay = 0;
    }

    void reset()
    {
        delayBuffer.clear();
        writeIndex = 0;
    }

    void setLookaheadMs(float ms)
    {
        targetDelay = juce::jlimit(0, bufferSize - 1, static_cast<int>(std::round(ms * 0.001f * static_cast<float>(sampleRate))));
    }

    int getActualDelaySamples() const noexcept { return currentDelay; }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (targetDelay == 0 && currentDelay == 0)
            return; // 0ms strictly zero delay path

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            if (currentDelay < targetDelay)
                ++currentDelay;
            else if (currentDelay > targetDelay)
                --currentDelay;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float in = buffer.getSample(ch, i);

                // Write to ring buffer
                delayBuffer.setSample(ch, writeIndex, in);

                // Read delayed sample
                int readIndex = writeIndex - currentDelay;
                if (readIndex < 0)
                    readIndex += bufferSize;

                float out = delayBuffer.getSample(ch, readIndex);
                buffer.setSample(ch, i, out);
            }

            writeIndex = (writeIndex + 1) % bufferSize;
        }
    }

private:
    double sampleRate = 44100.0;
    int bufferSize = 1024;
    int targetDelay = 0;
    int currentDelay = 0;
    int writeIndex = 0;
    juce::AudioBuffer<float> delayBuffer;
};

} // namespace LiveAudioLab
