#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <atomic>

namespace LiveAudioLab
{

/**
 * Panic Limiter: Fast-acting emergency soft limiter.
 * Protects performers' ears (IEMs) and sound systems from runaway acoustic feedback.
 */
class PanicLimiter
{
public:
    PanicLimiter() = default;

    void prepare(double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        reset();
    }

    void reset()
    {
        currentGain = 1.0f;
        gainReductionDb.store(0.0f, std::memory_order_relaxed);
    }

    void setEnabled(bool shouldEnable) noexcept { enabled.store(shouldEnable, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load(std::memory_order_relaxed); }

    float getGainReductionDb() const noexcept
    {
        return gainReductionDb.load(std::memory_order_relaxed);
    }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! isEnabled())
        {
            gainReductionDb.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        const float ceilingLinear = 0.95f; // approx -0.45 dBFS
        const float attackCoeff = 1.0f - std::exp(-1.0f / (0.0005f * static_cast<float>(sampleRate))); // 0.5 ms attack
        const float releaseCoeff = 1.0f - std::exp(-1.0f / (0.080f * static_cast<float>(sampleRate)));  // 80 ms release

        float maxPeakInBlock = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
            {
                // V2.1: NaN/Inf input is replaced by silence instead of propagating.
                if (! std::isfinite(buffer.getSample(ch, i)))
                    buffer.setSample(ch, i, 0.0f);
                peak = juce::jmax(peak, std::abs(buffer.getSample(ch, i)));
            }
            }

            float targetGain = 1.0f;
            if (peak > ceilingLinear)
            {
                targetGain = ceilingLinear / (peak + 1e-6f);
            }

            if (targetGain < currentGain)
                currentGain += attackCoeff * (targetGain - currentGain);
            else
                currentGain += releaseCoeff * (targetGain - currentGain);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float val = buffer.getSample(ch, i) * currentGain;
                // Hard ceiling clamp as safety net
                val = juce::jlimit(-1.0f, 1.0f, val);
                buffer.setSample(ch, i, val);
            }

            maxPeakInBlock = juce::jmax(maxPeakInBlock, peak);
        }

        float grDb = (currentGain < 0.999f) ? 20.0f * std::log10(currentGain) : 0.0f;
        gainReductionDb.store(grDb, std::memory_order_relaxed);
    }

private:
    double sampleRate = 44100.0;
    std::atomic<bool> enabled { true };
    float currentGain = 1.0f;
    std::atomic<float> gainReductionDb { 0.0f };
};

} // namespace LiveAudioLab
