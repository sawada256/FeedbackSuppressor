#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <complex>

namespace LiveAudioLab
{

/**
 * High-precision parametric IIR Biquad Notch / Peaking Filter
 * Provides 0-sample algorithmic latency using direct form II transposed structure.
 * Includes smooth parameter interpolation to prevent zipper noise and clicks.
 *
 * V2.1 (behaviour-preserving changes only):
 *  - Same per-sample parameter glide and the same float arithmetic as V2, so the
 *    notch response is bit-identical while active.
 *  - When no glide is in progress, each channel runs in a tight loop without
 *    re-checking the smoothing condition per sample (same results, less CPU).
 *  - Bypass no longer drops the filter from the signal path instantly: the gain
 *    glides to 0 dB with the normal ~10 ms smoothing and only then stops (no click).
 *  - A filter whose gain is ~0 dB starts a new notch directly at its own
 *    frequency/Q instead of sweeping from the previous one (V2 swept from 1 kHz).
 *  - Non-finite state (NaN/Inf input) is reset instead of silencing the channel.
 */
class BiquadNotchFilter
{
public:
    BiquadNotchFilter() = default;

    void prepare(double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        smoothingFactor = static_cast<float>(1.0 - std::exp(-1.0 / (0.01 * sampleRate)));
        reset();
        updateCoefficients(true);
    }

    void reset()
    {
        s1[0] = s2[0] = 0.0f;
        s1[1] = s2[1] = 0.0f;
        currentGainDb = effectiveTargetGain();
        currentFreq = targetFreq;
        currentQ = targetQ;
    }

    void setParameters(float freqHz, float gainDb, float q, bool immediate = false)
    {
        targetFreq = juce::jlimit(20.0f, static_cast<float>(sampleRate * 0.49), freqHz);
        targetGainDb = juce::jlimit(-60.0f, 0.0f, gainDb);
        targetQ = juce::jlimit(1.0f, 150.0f, q);

        if (immediate)
        {
            currentFreq = targetFreq;
            currentGainDb = effectiveTargetGain();
            currentQ = targetQ;
            updateCoefficients(true);
        }
    }

    /** Bypass glides the gain to 0 dB (click-free) and then stops processing. */
    void setBypass(bool shouldBypass) { bypassed = shouldBypass; }
    bool isBypassed() const noexcept { return bypassed; }

    float getTargetFrequency() const noexcept { return targetFreq; }
    float getCurrentGainDb() const noexcept { return currentGainDb; }
    float getTargetGainDb() const noexcept { return targetGainDb; }
    float getTargetQ() const noexcept { return targetQ; }

    void process(juce::AudioBuffer<float>& buffer)
    {
        process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
    }

    /**
     * Process a stereo or mono sample block.
     * Algorithmic latency is strictly 0 samples.
     */
    void process(float* const* channels, int numChannelsIn, int numSamples) noexcept
    {
        const float gainTarget = effectiveTargetGain();
        if (bypassed && std::abs(currentGainDb) < 0.05f)
        {
            if (! stateCleared)
            {
                // Fully faded out: settle at 0 dB and drop the decayed state.
                currentGainDb = 0.0f;
                s1[0] = s2[0] = s1[1] = s2[1] = 0.0f;
                stateCleared = true;
            }
            return;
        }

        // A notch rising from ~0 dB goes straight to its own frequency/Q
        // (V2 glided across the spectrum from the previous/placeholder frequency).
        if (std::abs(currentGainDb) < 0.05f && std::abs(gainTarget) >= 0.05f
            && (currentFreq != targetFreq || currentQ != targetQ))
        {
            currentFreq = targetFreq;
            currentQ = targetQ;
            updateCoefficients(false);
        }
        stateCleared = false;

        const int numChannels = juce::jmin(2, numChannelsIn);
        const float smoothFactor = smoothingFactor;

        int i = 0;
        while (i < numSamples)
        {
            if (needsSmoothing(gainTarget))
            {
                // Identical to V2: smooth, recompute, then filter this sample.
                currentGainDb += smoothFactor * (gainTarget - currentGainDb);
                currentFreq += smoothFactor * (targetFreq - currentFreq);
                currentQ += smoothFactor * (targetQ - currentQ);
                updateCoefficients(false);
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    float* channelData = channels[ch];
                    const float in = channelData[i];
                    const float out = b0 * in + s1[ch];
                    s1[ch] = b1 * in - a1 * out + s2[ch];
                    s2[ch] = b2 * in - a2 * out;
                    channelData[i] = out;
                }
                ++i;
                continue;
            }

            // Steady state: coefficients are fixed for the rest of the block.
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* channelData = channels[ch];
                float z1 = s1[ch], z2 = s2[ch];
                for (int j = i; j < numSamples; ++j)
                {
                    const float in = channelData[j];
                    const float out = b0 * in + z1;
                    z1 = b1 * in - a1 * out + z2;
                    z2 = b2 * in - a2 * out;
                    channelData[j] = out;
                }
                s1[ch] = z1;
                s2[ch] = z2;
            }
            break;
        }

        for (int ch = 0; ch < 2; ++ch)
            if (! std::isfinite(s1[ch]) || ! std::isfinite(s2[ch]))
                s1[ch] = s2[ch] = 0.0f;
    }

    /**
     * Calculate magnitude response at a given frequency for UI curve display
     */
    float getMagnitudeForFrequency(float freqHz) const
    {
        if (std::abs(currentGainDb) < 0.01f)
            return 1.0f;

        const double w = 2.0 * juce::MathConstants<double>::pi * static_cast<double>(freqHz) / sampleRate;
        const std::complex<double> z1 = std::polar(1.0, -w);
        const std::complex<double> z2 = std::polar(1.0, -2.0 * w);

        const std::complex<double> num = static_cast<double>(b0) + static_cast<double>(b1) * z1 + static_cast<double>(b2) * z2;
        const std::complex<double> den = 1.0 + static_cast<double>(a1) * z1 + static_cast<double>(a2) * z2;

        return static_cast<float>(std::abs(num / den));
    }

private:
    float effectiveTargetGain() const noexcept { return bypassed ? 0.0f : targetGainDb; }

    bool needsSmoothing(float gainTarget) const noexcept
    {
        return std::abs(currentGainDb - gainTarget) > 0.01f
            || std::abs(currentFreq - targetFreq) > 0.5f
            || std::abs(currentQ - targetQ) > 0.1f;
    }

    void updateCoefficients(bool force)
    {
        if (!force && std::abs(currentGainDb) < 0.001f)
        {
            b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
            a1 = 0.0f; a2 = 0.0f;
            return;
        }

        // Robert Bristow-Johnson (RBJ) Peaking EQ / Notch Formula
        const double w0 = 2.0 * juce::MathConstants<double>::pi * static_cast<double>(currentFreq) / sampleRate;
        const double cosW0 = std::cos(w0);
        const double sinW0 = std::sin(w0);
        const double A = std::pow(10.0, static_cast<double>(currentGainDb) / 40.0);
        const double alpha = sinW0 / (2.0 * static_cast<double>(currentQ));

        const double b0_un = 1.0 + alpha * A;
        const double b1_un = -2.0 * cosW0;
        const double b2_un = 1.0 - alpha * A;
        const double a0_un = 1.0 + alpha / A;
        const double a1_un = -2.0 * cosW0;
        const double a2_un = 1.0 - alpha / A;

        const double invA0 = 1.0 / a0_un;

        b0 = static_cast<float>(b0_un * invA0);
        b1 = static_cast<float>(b1_un * invA0);
        b2 = static_cast<float>(b2_un * invA0);
        a1 = static_cast<float>(a1_un * invA0);
        a2 = static_cast<float>(a2_un * invA0);
    }

    double sampleRate = 44100.0;
    float smoothingFactor = 0.002265f;
    bool bypassed = false;
    bool stateCleared = true;

    float targetFreq = 1000.0f;
    float currentFreq = 1000.0f;

    float targetGainDb = 0.0f;
    float currentGainDb = 0.0f;

    float targetQ = 35.0f;
    float currentQ = 35.0f;

    // Filter coefficients
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;

    // State variables per channel (stereo)
    float s1[2] = { 0.0f, 0.0f };
    float s2[2] = { 0.0f, 0.0f };
};

} // namespace LiveAudioLab
