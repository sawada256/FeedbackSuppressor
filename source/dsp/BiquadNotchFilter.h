#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <complex>

namespace LiveAudioLab
{

/**
 * Zero-latency RBJ peaking-cut biquad (TDF-II, double precision state).
 *
 * V3 changes:
 *  - Coefficients are recomputed once per 16-sample sub-block while parameters
 *    glide (V2 recomputed cos/sin/pow on every sample).
 *  - Gain glides in dB, frequency and Q glide in the log domain (~10 ms time
 *    constant, independent of sample rate and block size).
 *  - A filter whose gain is ~0 dB is idle: it costs nothing and its state is
 *    cleared, so a newly assigned notch starts at its own frequency instead of
 *    sweeping across the spectrum from a stale one.
 *  - Double-precision coefficients/state keep narrow low-frequency notches
 *    (pole radius ~ 1 - 1e-4) numerically stable.
 * Owned by the audio thread only; the control side passes targets through
 * FilterManager's lock-free mailbox.
 */
class BiquadNotchFilter
{
public:
    static constexpr int SUB_BLOCK = 16;

    BiquadNotchFilter() = default;

    void prepare(double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        stepCoeff = 1.0 - std::exp(-static_cast<double>(SUB_BLOCK) / (0.010 * sampleRate));
        reset();
    }

    void reset()
    {
        clearState();
        currentGainDb = targetGainDb;
        currentLogFreq = targetLogFreq;
        currentLogQ = targetLogQ;
        updateCoefficients();
    }

    void setTarget(float freqHz, float gainDb, float q) noexcept
    {
        const double f = juce::jlimit(20.0, sampleRate * 0.49, std::isfinite(freqHz) ? static_cast<double>(freqHz) : 1000.0);
        targetLogFreq = std::log(f);
        targetGainDb = juce::jlimit(-60.0, 0.0, std::isfinite(gainDb) ? static_cast<double>(gainDb) : 0.0);
        targetLogQ = std::log(juce::jlimit(0.5, 150.0, std::isfinite(q) ? static_cast<double>(q) : 35.0));
    }

    /** Jump to the current targets (state restore / tests), never used mid-signal for an active notch. */
    void snapToTarget() noexcept
    {
        currentGainDb = targetGainDb;
        currentLogFreq = targetLogFreq;
        currentLogQ = targetLogQ;
        updateCoefficients();
    }

    bool isIdle() const noexcept { return std::abs(currentGainDb) < idleDb && std::abs(targetGainDb) < idleDb; }

    float getCurrentGainDb() const noexcept { return static_cast<float>(currentGainDb); }
    float getTargetGainDb() const noexcept { return static_cast<float>(targetGainDb); }
    float getCurrentFrequency() const noexcept { return static_cast<float>(std::exp(currentLogFreq)); }

    void process(float* const* channels, int numChannels, int numSamples) noexcept
    {
        numChannels = juce::jmin(2, numChannels);
        if (isIdle())
        {
            if (! stateCleared) clearState();
            currentGainDb = 0.0;
            return;
        }

        // A notch rising from 0 dB starts directly at its own frequency/Q.
        if (std::abs(currentGainDb) < idleDb)
        {
            currentLogFreq = targetLogFreq;
            currentLogQ = targetLogQ;
            clearState();
            updateCoefficients();
        }
        stateCleared = false;

        for (int start = 0; start < numSamples; start += SUB_BLOCK)
        {
            if (std::abs(currentGainDb - targetGainDb) > 1.0e-3
                || std::abs(currentLogFreq - targetLogFreq) > 1.0e-5
                || std::abs(currentLogQ - targetLogQ) > 1.0e-4)
            {
                currentGainDb += stepCoeff * (targetGainDb - currentGainDb);
                currentLogFreq += stepCoeff * (targetLogFreq - currentLogFreq);
                currentLogQ += stepCoeff * (targetLogQ - currentLogQ);
                if (std::abs(currentGainDb - targetGainDb) < 0.01) currentGainDb = targetGainDb;
                updateCoefficients();
            }

            const int end = juce::jmin(numSamples, start + SUB_BLOCK);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* data = channels[ch];
                double z1 = s1[ch], z2 = s2[ch];
                for (int i = start; i < end; ++i)
                {
                    const double in = data[i];
                    const double out = b0 * in + z1;
                    z1 = b1 * in - a1 * out + z2;
                    z2 = b2 * in - a2 * out;
                    data[i] = static_cast<float>(out);
                }
                s1[ch] = z1; s2[ch] = z2;
            }
        }

        // Recover from non-finite state (e.g. NaN injected upstream) instead of
        // staying silent forever.
        for (int ch = 0; ch < 2; ++ch)
            if (! std::isfinite(s1[ch]) || ! std::isfinite(s2[ch])) { s1[ch] = 0.0; s2[ch] = 0.0; }
    }

    /** Linear magnitude of an RBJ peaking filter, for UI curves. */
    static float magnitudeAt(double sampleRate, float centreHz, float gainDb, float q, float atHz) noexcept
    {
        if (std::abs(gainDb) < 0.01f) return 1.0f;
        double c[5];
        computeCoefficients(sampleRate, centreHz, gainDb, q, c);
        const double w = juce::MathConstants<double>::twoPi * atHz / sampleRate;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
        const auto num = c[0] + c[1] * z1 + c[2] * z2;
        const auto den = 1.0 + c[3] * z1 + c[4] * z2;
        return static_cast<float>(std::abs(num / den));
    }

private:
    static constexpr double idleDb = 0.02;

    static void computeCoefficients(double sr, double freq, double gainDb, double q, double* c) noexcept
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double cosW0 = std::cos(w0), sinW0 = std::sin(w0);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double alpha = sinW0 / (2.0 * q);
        const double invA0 = 1.0 / (1.0 + alpha / A);
        c[0] = (1.0 + alpha * A) * invA0;
        c[1] = -2.0 * cosW0 * invA0;
        c[2] = (1.0 - alpha * A) * invA0;
        c[3] = c[1];
        c[4] = (1.0 - alpha / A) * invA0;
    }

    void updateCoefficients() noexcept
    {
        double c[5];
        computeCoefficients(sampleRate, std::exp(currentLogFreq), currentGainDb, std::exp(currentLogQ), c);
        b0 = c[0]; b1 = c[1]; b2 = c[2]; a1 = c[3]; a2 = c[4];
    }

    void clearState() noexcept
    {
        s1[0] = s1[1] = s2[0] = s2[1] = 0.0;
        stateCleared = true;
    }

    double sampleRate = 44100.0;
    double stepCoeff = 0.03;

    double targetLogFreq = std::log(1000.0), currentLogFreq = std::log(1000.0);
    double targetGainDb = 0.0, currentGainDb = 0.0;
    double targetLogQ = std::log(35.0), currentLogQ = std::log(35.0);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double s1[2] = { 0.0, 0.0 };
    double s2[2] = { 0.0, 0.0 };
    bool stateCleared = true;
};

} // namespace LiveAudioLab
