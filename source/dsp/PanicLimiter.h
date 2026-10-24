#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <atomic>
#include <vector>

namespace LiveAudioLab
{

/**
 * Panic Limiter: emergency peak limiter protecting ears (IEMs) and speakers
 * from runaway feedback while the notch detector is still confirming.
 *
 * V3: optional lookahead. In "Lookahead" latency mode the signal is delayed by
 * a few milliseconds and the gain is computed from the undelayed signal, so the
 * limiter reaches the required reduction *before* the peak leaves the delay
 * line (no overshoot, no hard-clip distortion). In V2 the 3 ms delay sat in
 * front of the notch filters and brought no benefit, because detection itself
 * takes tens of milliseconds.
 *
 * The delay is applied even when the limiter is disabled or the host bypasses
 * the plugin, so the reported latency always matches the audio path.
 * Audio thread only (except the atomic getters/setters).
 */
class PanicLimiter
{
public:
    static constexpr float CEILING = 0.944f; // -0.5 dBFS

    PanicLimiter() = default;

    // maxLookaheadSamples: capacity reserved here (call off the audio thread).
    void prepare(double newSampleRate, int maxLookaheadSamples)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        capacity = juce::jmax(1, maxLookaheadSamples) + 1;
        delayLine.assign(static_cast<size_t>(2 * capacity), 0.0f);
        attackZeroLatency = 1.0f - std::exp(-1.0f / (0.0005f * static_cast<float>(sampleRate)));
        releaseCoeff = 1.0f - std::exp(-1.0f / (0.080f * static_cast<float>(sampleRate)));
        fadeStep = 1.0f / juce::jmax(1.0f, 0.003f * static_cast<float>(sampleRate)); // 3 ms crossfade on delay change
        reset();
    }

    void reset()
    {
        std::fill(delayLine.begin(), delayLine.end(), 0.0f);
        holdGain = 1.0f;
        holdCounter = 0;
        writeIndex = 0;
        currentGain = 1.0f;
        activeDelay = requestedDelay.load(std::memory_order_relaxed);
        fadeGain = 1.0f;
        fadingOut = false;
        gainReductionDb.store(0.0f, std::memory_order_relaxed);
    }

    void setEnabled(bool shouldEnable) noexcept { enabled.store(shouldEnable, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load(std::memory_order_relaxed); }

    void setLookaheadSamples(int samples) noexcept
    {
        requestedDelay.store(juce::jlimit(0, capacity - 1, samples), std::memory_order_relaxed);
    }
    int getLookaheadSamples() const noexcept { return requestedDelay.load(std::memory_order_relaxed); }

    float getGainReductionDb() const noexcept { return gainReductionDb.load(std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer) noexcept { run(buffer, isEnabled()); }

    // Host bypass: keep the (reported) delay but do not limit.
    void processDelayOnly(juce::AudioBuffer<float>& buffer) noexcept { run(buffer, false); }

private:
    void run(juce::AudioBuffer<float>& buffer, bool limit) noexcept
    {
        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();
        if (numChannels <= 0 || capacity <= 1) return;

        float minGain = 1.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            // Delay changes: fade out, switch, fade in (avoids the V2 sample-skip glitch).
            const int wanted = requestedDelay.load(std::memory_order_relaxed);
            if (wanted != activeDelay && ! fadingOut) fadingOut = true;
            if (fadingOut)
            {
                fadeGain -= fadeStep;
                if (fadeGain <= 0.0f)
                {
                    fadeGain = 0.0f;
                    activeDelay = wanted;
                    holdGain = 1.0f;
                    holdCounter = 0;
                    fadingOut = false;
                }
            }
            else if (fadeGain < 1.0f)
            {
                fadeGain = juce::jmin(1.0f, fadeGain + fadeStep);
            }

            float peak = 0.0f;
            float in[2] = { 0.0f, 0.0f };
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float x = buffer.getSample(ch, i);
                if (! std::isfinite(x)) x = 0.0f;
                in[ch] = x;
                peak = juce::jmax(peak, std::abs(x));
            }

            const float required = peak > CEILING ? CEILING / peak : 1.0f;
            float targetGain = required;
            if (activeDelay > 0)
            {
                // Peak-hold of the required gain for one lookahead window (O(1)
                // approximation of a sliding minimum; release is slow anyway).
                if (required <= holdGain || --holdCounter <= 0)
                {
                    holdGain = required;
                    holdCounter = activeDelay;
                }
                targetGain = holdGain;
            }

            if (limit)
            {
                if (targetGain < currentGain)
                {
                    // With lookahead: reach the target within the window (linear-ish ramp).
                    const float attack = activeDelay > 0 ? juce::jmin(1.0f, 8.0f / static_cast<float>(activeDelay)) : attackZeroLatency;
                    currentGain += attack * (targetGain - currentGain);
                }
                else
                {
                    currentGain += releaseCoeff * (targetGain - currentGain);
                }
            }
            else
            {
                currentGain = 1.0f;
            }
            minGain = juce::jmin(minGain, currentGain);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float out = in[ch];
                if (activeDelay > 0)
                {
                    const size_t base = static_cast<size_t>(ch * capacity);
                    delayLine[base + static_cast<size_t>(writeIndex)] = in[ch];
                    int readIndex = writeIndex - activeDelay;
                    if (readIndex < 0) readIndex += capacity;
                    out = delayLine[base + static_cast<size_t>(readIndex)];
                }
                out *= currentGain * fadeGain;
                if (limit) out = juce::jlimit(-1.0f, 1.0f, out); // last-resort safety
                buffer.setSample(ch, i, out);
            }
            writeIndex = (writeIndex + 1) % capacity;
        }

        gainReductionDb.store(minGain < 0.999f ? 20.0f * std::log10(minGain) : 0.0f, std::memory_order_relaxed);
    }

    double sampleRate = 44100.0;
    std::atomic<bool> enabled { true };
    std::atomic<int> requestedDelay { 0 };
    int activeDelay = 0;
    int capacity = 1;
    int writeIndex = 0;
    std::vector<float> delayLine;
    float holdGain = 1.0f;
    int holdCounter = 0;
    float currentGain = 1.0f;
    float attackZeroLatency = 0.04f;
    float releaseCoeff = 0.0003f;
    float fadeGain = 1.0f;
    float fadeStep = 0.01f;
    bool fadingOut = false;
    std::atomic<float> gainReductionDb { 0.0f };
};

} // namespace LiveAudioLab
