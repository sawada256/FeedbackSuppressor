#include "FeedbackDetector.h"
#include <algorithm>
#include <cmath>

namespace LiveAudioLab
{

FeedbackDetector::FeedbackDetector()
{
    for (auto& channel : fifoBuffer)
        channel.resize(FIFO_SIZE, 0.0f);
    clearAnalysisHistory();

    uiSpectrum.resize(FFT_SIZE / 2, -120.0f);
    uiBaseline.resize(FFT_SIZE / 2, -120.0f);
}

void FeedbackDetector::prepare(double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    reset();
}

void FeedbackDetector::reset()
{
    for (auto& channel : fifoBuffer)
        std::fill(channel.begin(), channel.end(), 0.0f);
    fifoWritePos.store(0, std::memory_order_relaxed);
    fifoReadPos.store(0, std::memory_order_relaxed);
    droppedBlocks.store(0, std::memory_order_relaxed);
    lastDroppedBlocks = 0;
    clearAnalysisHistory();
}

void FeedbackDetector::clearAnalysisHistory()
{
    // Silent hops call this every hop; once cleared nothing changes until the
    // next analysed frame, so repeating the ~0.5 MB fill is pure waste.
    if (historyCleared)
        return;
    historyCleared = true;
    fftBuffer.fill(0.0f);
    rightFftBuffer.fill(0.0f);
    currentMagnitudesDb.fill(-120.0f);
    averagedPower.fill(0.0f);
    rawMagnitudesDb.fill(-120.0f);
    baselineEnvelopeDb.fill(-120.0f);
    for (auto& channel : prevPhaseRad) channel.fill(0.0f);
    phaseCoherence.fill(0.0f);
    phaseSumCos.fill(0.0f);
    phaseSumSin.fill(0.0f);
    for (auto& channel : hasValidPhase) channel.fill(false);
    for (auto& channel : validPhaseFrames) channel.fill(0);
    for (auto& hist : historyMagnitudesDb) hist.fill(-120.0f);
    // An error of 0 has cos 1 / sin 0 (same as V2's zero-filled error history).
    for (auto& channel : phaseCosHistory)
        for (auto& hist : channel) hist.fill(1.0f);
    for (auto& channel : phaseSinHistory)
        for (auto& hist : channel) hist.fill(0.0f);
    baselineStamp.fill(0);
    candidatePersistence.fill(0);
    trackedFrequency.fill(0.0f);
    trackedDurationMs.fill(0.0f);
    strongPhaseDurationMs.fill(0.0f);
    eventCooldownMs.fill(0.0f);
    historyIndex = phaseHistIndex = validHistoryFrames = 0;
}

void FeedbackDetector::pushSamples(const float* const* channelData, int numChannels, int numSamples)
{
    if (numChannels <= 0 || numSamples <= 0)
        return;

    int writePos = fifoWritePos.load(std::memory_order_relaxed);
    const int readPos = fifoReadPos.load(std::memory_order_acquire);
    const int freeSpace = (readPos - writePos - 1 + FIFO_SIZE) % FIFO_SIZE;
    // SPSC ownership: never overwrite samples still being read by the worker.
    if (numSamples > freeSpace)
    {
        droppedBlocks.fetch_add(1, std::memory_order_release);
        return;
    }
    for (int i = 0; i < numSamples; ++i)
    {
        const float left = channelData[0] != nullptr ? channelData[0][i] : 0.0f;
        const float right = numChannels > 1 && channelData[1] != nullptr ? channelData[1][i] : left;
        fifoBuffer[0][static_cast<size_t>(writePos)] = std::isfinite(left) ? left : 0.0f;
        fifoBuffer[1][static_cast<size_t>(writePos)] = std::isfinite(right) ? right : 0.0f;
        writePos = (writePos + 1) % FIFO_SIZE;
    }
    fifoWritePos.store(writePos, std::memory_order_release);
}

bool FeedbackDetector::processNextFrames(std::vector<DetectedFeedback>& detectedList)
{
    const auto drops = droppedBlocks.load(std::memory_order_acquire);
    const auto mode = currentDetectionMode.load();
    const auto profile = currentProfile.load();
    if (drops != lastDroppedBlocks || mode != analyzedMode || profile != analyzedProfile)
    {
        clearAnalysisHistory();
        // Discard the pre-gap tail; do not stitch discontinuous audio into phase history.
        if (drops != lastDroppedBlocks)
            fifoReadPos.store(fifoWritePos.load(std::memory_order_acquire), std::memory_order_release);
        lastDroppedBlocks = drops;
        analyzedMode = mode;
        analyzedProfile = profile;
    }
    int readPos = fifoReadPos.load(std::memory_order_relaxed);
    int writePos = fifoWritePos.load(std::memory_order_acquire);
    int available = writePos - readPos;
    if (available < 0)
        available += FIFO_SIZE;

    bool foundAny = false;

    while (available >= FFT_SIZE)
    {
        // 1. Read FFT_SIZE samples and compute frame RMS
        float sumSquares = 0.0f;
        for (int i = 0; i < FFT_SIZE; ++i)
        {
            int idx = (readPos + i) % FIFO_SIZE;
            float s = fifoBuffer[0][static_cast<size_t>(idx)];
            float r = fifoBuffer[1][static_cast<size_t>(idx)];
            rightFftBuffer[static_cast<size_t>(i)] = r;
            fftBuffer[static_cast<size_t>(i)] = s;
            sumSquares += juce::jmax(s * s, r * r);
        }
        std::fill(fftBuffer.begin() + FFT_SIZE, fftBuffer.end(), 0.0f);
        std::fill(rightFftBuffer.begin() + FFT_SIZE, rightFftBuffer.end(), 0.0f);

        float frameRms = std::sqrt(sumSquares / static_cast<float>(FFT_SIZE));
        float frameRmsDb = 20.0f * std::log10(frameRms + 1e-6f);

        // Absolute guard against numerical noise; musical modes use a lower floor.
        if (frameRmsDb < (mode == DetectionMode::Standard ? -55.0f : -80.0f))
        {
            clearAnalysisHistory();
            lastHopAnalysedMusical = false;
            readPos = (readPos + HOP_SIZE) % FIFO_SIZE;
            fifoReadPos.store(readPos, std::memory_order_release);
            available -= HOP_SIZE;
            continue;
        }

        historyCleared = false;
        ++frameCounter;
        lastHopAnalysedMusical = mode != DetectionMode::Standard;

        // 2. Apply Hanning window
        window.multiplyWithWindowingTable(fftBuffer.data(), FFT_SIZE);
        window.multiplyWithWindowingTable(rightFftBuffer.data(), FFT_SIZE);

        // 3. Perform FFT with real & imaginary phase preservation
        fft.performRealOnlyForwardTransform(fftBuffer.data());
        fft.performRealOnlyForwardTransform(rightFftBuffer.data());

        // 4. Convert complex spectrum to magnitude (dB) and calculate instantaneous phase
        const int numBins = FFT_SIZE / 2;
        const float norm = 2.0f / static_cast<float>(FFT_SIZE);
        const int phaseFrames = juce::jlimit(8, PHASE_HIST_FRAMES, static_cast<int>(std::ceil(0.085 * sampleRate / HOP_SIZE)));
        const float spectralAlpha = static_cast<float>(1.0 - std::exp(-HOP_SIZE / (0.035 * sampleRate)));

        for (int k = 0; k < numBins; ++k)
        {
            float re = fftBuffer[static_cast<size_t>(2 * k)];
            float im = fftBuffer[static_cast<size_t>(2 * k + 1)];
            const float rr = rightFftBuffer[static_cast<size_t>(2 * k)];
            const float ri = rightFftBuffer[static_cast<size_t>(2 * k + 1)];
            const int channel = rr * rr + ri * ri > re * re + im * im ? 1 : 0;
            const float power = juce::jmax(re * re + im * im, rr * rr + ri * ri);
            float normalizedPower = power * norm * norm;
            rawMagnitudesDb[static_cast<size_t>(k)] = 20.0f * std::log10(std::sqrt(normalizedPower) + 1.0e-6f);
            if (mode != DetectionMode::Standard)
            {
                auto& average = averagedPower[static_cast<size_t>(k)];
                if (validHistoryFrames == 0) average = normalizedPower;
                else average += spectralAlpha * (normalizedPower - average);
                normalizedPower = average;
            }
            const float mag = std::sqrt(normalizedPower);
            currentMagnitudesDb[static_cast<size_t>(k)] = 20.0f * std::log10(mag + 1.0e-6f);

            if (mode == DetectionMode::Standard)
                continue; // the legacy detector never reads phase; history is cleared on mode change

            // Track phase independently for each channel. Choosing the louder spectrum
            // must not create artificial phase discontinuities when channels alternate.
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto ci = static_cast<size_t>(ch);
                const auto ki = static_cast<size_t>(k);
                const float phase = ch == 0 ? std::atan2(im, re) : std::atan2(ri, rr);
                float plv = 0.0f;
                float sumCos = 0.0f, sumSin = 0.0f;
                if (hasValidPhase[ci][ki])
                {
                    const float expected = juce::MathConstants<float>::twoPi
                        * static_cast<float>((k * HOP_SIZE) % FFT_SIZE) / FFT_SIZE;
                    const float error = std::remainder(phase - prevPhaseRad[ci][ki] - expected,
                        juce::MathConstants<float>::twoPi);
                    phaseCosHistory[ci][static_cast<size_t>(phaseHistIndex)][ki] = std::cos(error);
                    phaseSinHistory[ci][static_cast<size_t>(phaseHistIndex)][ki] = std::sin(error);
                    for (int h = 0; h < phaseFrames; ++h)
                    {
                        const auto hi = static_cast<size_t>((phaseHistIndex - h + PHASE_HIST_FRAMES) % PHASE_HIST_FRAMES);
                        sumCos += phaseCosHistory[ci][hi][ki];
                        sumSin += phaseSinHistory[ci][hi][ki];
                    }
                    validPhaseFrames[ci][ki] = juce::jmin(PHASE_HIST_FRAMES, validPhaseFrames[ci][ki] + 1);
                    if (validPhaseFrames[ci][ki] >= phaseFrames)
                        plv = std::sqrt(sumCos * sumCos + sumSin * sumSin) / static_cast<float>(phaseFrames);
                }
                else
                {
                    validPhaseFrames[ci][ki] = 0;
                    hasValidPhase[ci][ki] = true;
                }
                prevPhaseRad[ci][ki] = phase;
                if (ch == channel)
                {
                    phaseCoherence[ki] = plv;
                    // The phase-vocoder frequency itself is only needed for peak
                    // bins; keep the sums and evaluate it lazily (same formula).
                    phaseSumCos[ki] = sumCos;
                    phaseSumSin[ki] = sumSin;
                }
            }
        }

        phaseHistIndex = (phaseHistIndex + 1) % PHASE_HIST_FRAMES;

        // 5. Calculate smooth baseline and 1/f curve. The music-aware modes only
        // need it at candidate peaks: computed lazily there (same formula), and
        // for the whole spectrum once per call for the UI.
        if (mode == DetectionMode::Standard)
            calculateBaselineEnvelope();

        // 6. Analyze current frame for howling candidates
        const auto previousCount = detectedList.size();
        if (mode == DetectionMode::Standard) analyzeFrame(detectedList);
        else analyzeMusicalFrame(detectedList);
        if (detectedList.size() > previousCount)
            foundAny = true;

        // 7. Update history
        historyMagnitudesDb[static_cast<size_t>(historyIndex)] = rawMagnitudesDb;
        historyIndex = (historyIndex + 1) % HISTORY_FRAMES;
        validHistoryFrames = juce::jmin(validHistoryFrames + 1, HISTORY_FRAMES);

        // Advance read pointer by HOP_SIZE
        readPos = (readPos + HOP_SIZE) % FIFO_SIZE;
        fifoReadPos.store(readPos, std::memory_order_release);
        available -= HOP_SIZE;
    }

    // Thread-safe copy for UI (full music-aware baseline only for the last frame)
    if (lastHopAnalysedMusical)
    {
        for (int k = 0; k < FFT_SIZE / 2; ++k)
            musicalBaselineAt(k);
        lastHopAnalysedMusical = false;
    }
    {
        std::lock_guard<std::mutex> lock(spectrumMutex);
        std::copy(currentMagnitudesDb.begin(), currentMagnitudesDb.end(), uiSpectrum.begin());
        std::copy(baselineEnvelopeDb.begin(), baselineEnvelopeDb.end(), uiBaseline.begin());
    }

    return foundAny;
}

void FeedbackDetector::calculateBaselineEnvelope()
{
    const int numBins = FFT_SIZE / 2;
    const int windowRadius = 14;

    for (int k = 0; k < numBins; ++k)
    {
        float freqHz = (static_cast<float>(k) * static_cast<float>(sampleRate)) / static_cast<float>(FFT_SIZE);
        float refOffset = getReferenceCurveDb(freqHz);

        int startK = juce::jmax(0, k - windowRadius);
        int endK = juce::jmin(numBins - 1, k + windowRadius);

        // Compute local average excluding immediate peak bins (k-2 to k+2)
        float sum = 0.0f;
        int count = 0;
        for (int j = startK; j <= endK; ++j)
        {
            if (std::abs(j - k) > 2)
            {
                sum += currentMagnitudesDb[static_cast<size_t>(j)];
                ++count;
            }
        }
        float localAvg = count > 0 ? (sum / static_cast<float>(count)) : currentMagnitudesDb[static_cast<size_t>(k)];

        // For HighFocus profile, lower the local average floor in high frequencies
        // so background energy does not artificially elevate the baseline.
        if (analyzedProfile == DetectionProfile::HighFocus && freqHz >= 2000.0f)
        {
            float octavesFrom2k = std::log2(freqHz / 2000.0f);
            float reduction = juce::jlimit(2.0f, 6.0f, 2.5f + 1.5f * octavesFrom2k);
            localAvg -= reduction;
        }
        else if (analyzedProfile == DetectionProfile::LowFocus && freqHz >= 80.0f && freqHz <= 1200.0f)
        {
            localAvg -= 3.5f;
        }

        if (analyzedMode != DetectionMode::Standard)
        {
            // Trim the upper quartile of shoulder bins, then average LINEAR power.
            // A single neighboring musical partial must not dominate the background.
            std::array<float, 28> shoulders {};
            int n = 0;
            for (int j = startK; j <= endK; ++j)
                if (std::abs(j - k) > 2)
                    shoulders[static_cast<size_t>(n++)] = currentMagnitudesDb[static_cast<size_t>(j)];
            std::sort(shoulders.begin(), shoulders.begin() + n);
            double power = 0.0;
            const int keep = juce::jmax(1, n * 3 / 4);
            for (int j = 0; j < keep; ++j)
                power += std::pow(10.0, shoulders[static_cast<size_t>(j)] / 10.0);
            baselineEnvelopeDb[static_cast<size_t>(k)] = static_cast<float>(10.0 * std::log10(power / keep + 1.0e-12));
            continue;
        }

        // Baseline is the higher of local average or reference tilt
        baselineEnvelopeDb[static_cast<size_t>(k)] = juce::jmax(localAvg, refOffset);
    }
}

float FeedbackDetector::musicalBaselineAt(int k) noexcept
{
    // Same computation as the music-aware branch of calculateBaselineEnvelope()
    // (identical values), evaluated only for bins that are actually queried.
    const auto ki = static_cast<size_t>(k);
    if (baselineStamp[ki] == frameCounter)
        return baselineEnvelopeDb[ki];
    const int startK = juce::jmax(0, k - 14);
    const int endK = juce::jmin(FFT_SIZE / 2 - 1, k + 14);
    std::array<float, 28> shoulders {};
    int n = 0;
    for (int j = startK; j <= endK; ++j)
        if (std::abs(j - k) > 2)
            shoulders[static_cast<size_t>(n++)] = currentMagnitudesDb[static_cast<size_t>(j)];
    std::sort(shoulders.begin(), shoulders.begin() + n);
    double power = 0.0;
    const int keep = juce::jmax(1, n * 3 / 4);
    for (int j = 0; j < keep; ++j)
        power += std::pow(10.0, shoulders[static_cast<size_t>(j)] / 10.0);
    baselineEnvelopeDb[ki] = static_cast<float>(10.0 * std::log10(power / keep + 1.0e-12));
    baselineStamp[ki] = frameCounter;
    return baselineEnvelopeDb[ki];
}

float FeedbackDetector::getReferenceCurveDb(float freqHz) const
{
    if (freqHz < 50.0f)
        return -80.0f;

    // Pink noise slope (-3dB/octave or -10dB/decade) from 1kHz reference at -42 dBFS
    float octavesFrom1k = std::log2(freqHz / 1000.0f);
    float baseCurve = -3.0f * octavesFrom1k - 42.0f;

    if (analyzedProfile == DetectionProfile::Speech)
    {
        // Speech profile: vocal presence zone (300Hz ~ 3.5kHz) has higher tolerance,
        // steep falloff outside vocal range to focus feedback detection where mic feedback occurs.
        if (freqHz >= 300.0f && freqHz <= 3500.0f)
            baseCurve += 5.0f;
        else if (freqHz > 4500.0f)
            baseCurve -= 4.0f * std::log2(freqHz / 4500.0f);
        else if (freqHz < 200.0f)
            baseCurve -= 6.0f;
    }
    else if (analyzedProfile == DetectionProfile::Music)
    {
        baseCurve -= 1.0f;
    }
    else if (analyzedProfile == DetectionProfile::HighFocus)
    {
        // High Focus (Anti-Squeal): Lowers baseline curve above 2 kHz.
        // Higher frequencies (squealing feedback) have a significantly lower reference baseline
        // (-5dB at 2kHz to -10dB at 8kHz), making even weak or developing high-frequency feedback
        // stand out prominently and trigger suppression at lower input gains.
        if (freqHz >= 2000.0f)
        {
            float octavesFrom2k = std::log2(freqHz / 2000.0f);
            baseCurve -= (5.0f + 2.5f * octavesFrom2k);
        }
    }
    else if (analyzedProfile == DetectionProfile::LowFocus)
    {
        // Low Focus (Anti-Boom): Lowers baseline curve in the 80 Hz ~ 1200 Hz region.
        // Bass howling, mic handling resonance, and stage booming are detected and suppressed
        // at earlier stages and lower gains.
        if (freqHz >= 80.0f && freqHz <= 1200.0f)
        {
            baseCurve -= 5.0f;
        }
    }

    return baseCurve;
}

void FeedbackDetector::analyzeFrame(std::vector<DetectedFeedback>& detectedList)
{
    const int numBins = FFT_SIZE / 2;
    const float binWidth = static_cast<float>(sampleRate) / static_cast<float>(FFT_SIZE);

    const int minBin = juce::jlimit(2, numBins - 5, static_cast<int>(minDetectionHz / binWidth));
    const int maxBin = juce::jlimit(minBin + 1, numBins - 5, static_cast<int>(maxDetectionHz / binWidth));

    // 1. Minimum peak absolute level gate
    // Default (0 dB): -36 dBFS. At -18 dB: -26.1 dBFS. At -60 dB: capped at -4.0 dBFS.
    const float rawMinPeak = -36.0f - (sensitivityOffsetDb * 0.55f);
    const float minPeakLevelDb = juce::jmin(-4.0f, rawMinPeak);

    // 2. Prominence threshold above background baseline
    // Default (0 dB): 9.0 dB. At -18 dB: 18 dB. At -60 dB: capped at 28 dB.
    const float rawProminence = 9.0f - (sensitivityOffsetDb * 0.5f);
    const float baseProminenceThresholdDb = juce::jmin(28.0f, rawProminence);

    // 3. Growth rate thresholds (dB rise over ~23ms)
    float minGrowthDb = 2.0f; // Medium
    if (growthSensitivityLevel == 0) minGrowthDb = 4.0f;      // Low sensitivity
    else if (growthSensitivityLevel == 2) minGrowthDb = 0.8f; // High sensitivity

    const int prevFrameIdx = (historyIndex + HISTORY_FRAMES - 2) % HISTORY_FRAMES;

    for (int k = minBin; k <= maxBin; ++k)
    {
        float mag = currentMagnitudesDb[static_cast<size_t>(k)];
        float freqHz = static_cast<float>(k) * binWidth;

        // Gate 1: Absolute peak level
        float effectiveMinPeak = minPeakLevelDb;
        if (analyzedProfile == DetectionProfile::HighFocus && freqHz >= 2000.0f)
            effectiveMinPeak -= 5.0f; // Lower gate allows softer high-frequency squeals to be caught
        else if (analyzedProfile == DetectionProfile::LowFocus && freqHz <= 1200.0f)
            effectiveMinPeak -= 4.0f; // Lower gate allows softer low-frequency boom to be caught

        if (mag < effectiveMinPeak)
        {
            candidatePersistence[static_cast<size_t>(k)] = 0;
            continue;
        }

        // Gate 2: Local maximum check
        if (mag <= currentMagnitudesDb[static_cast<size_t>(k - 1)] ||
            mag < currentMagnitudesDb[static_cast<size_t>(k + 1)])
        {
            candidatePersistence[static_cast<size_t>(k)] = 0;
            continue;
        }

        // Gate 3: Prominence check
        float baseline = baselineEnvelopeDb[static_cast<size_t>(k)];
        float prominence = mag - baseline;
        float effectiveProminenceThresh = baseProminenceThresholdDb;

        if (prominence < effectiveProminenceThresh)
        {
            candidatePersistence[static_cast<size_t>(k)] = 0;
            continue;
        }

        // Gate 4: Spectral Purity (Main Lobe Skirt Drop-off)
        // Sub-bin frequencies leak to adjacent bin, but at +-2 and +-3 bins away,
        // pure sinusoidal resonance drops steeply (>= 6dB at +-2, >= 12dB at +-3).
        float dropLeft2 = mag - currentMagnitudesDb[static_cast<size_t>(k - 2)];
        float dropRight2 = mag - currentMagnitudesDb[static_cast<size_t>(k + 2)];
        float dropLeft3 = mag - currentMagnitudesDb[static_cast<size_t>(k - 3)];
        float dropRight3 = mag - currentMagnitudesDb[static_cast<size_t>(k + 3)];

        float purityThresh2 = 6.0f;
        float purityThresh3 = 12.0f;
        bool isPureTone = (dropLeft2 >= purityThresh2 && dropRight2 >= purityThresh2 &&
                           dropLeft3 >= purityThresh3 && dropRight3 >= purityThresh3);
        if (!isPureTone)
        {
            candidatePersistence[static_cast<size_t>(k)] = 0;
            continue;
        }

        // Gate 5: Harmonic Rejection (Singing voice has harmonic series: f0, 2f0, 3f0)
        bool hasStrongHarmonics = false;
        if (analyzedProfile != DetectionProfile::Flat1OverF)
        {
            // Check 2nd harmonic (around 2k)
            int target2k = static_cast<int>(std::round(2.0f * static_cast<float>(k)));
            for (int offset = -1; offset <= 1; ++offset)
            {
                int h = target2k + offset;
                if (h > 1 && h < numBins - 1)
                {
                    float hMag = currentMagnitudesDb[static_cast<size_t>(h)];
                    if (hMag > mag - 12.0f &&
                        hMag > currentMagnitudesDb[static_cast<size_t>(h - 1)] &&
                        hMag > currentMagnitudesDb[static_cast<size_t>(h + 1)])
                    {
                        hasStrongHarmonics = true;
                        break;
                    }
                }
            }

            // Check 3rd harmonic (around 3k)
            if (!hasStrongHarmonics)
            {
                int target3k = static_cast<int>(std::round(3.0f * static_cast<float>(k)));
                for (int offset = -1; offset <= 1; ++offset)
                {
                    int h = target3k + offset;
                    if (h > 1 && h < numBins - 1)
                    {
                        float hMag = currentMagnitudesDb[static_cast<size_t>(h)];
                        if (hMag > mag - 14.0f &&
                            hMag > currentMagnitudesDb[static_cast<size_t>(h - 1)] &&
                            hMag > currentMagnitudesDb[static_cast<size_t>(h + 1)])
                        {
                            hasStrongHarmonics = true;
                            break;
                        }
                    }
                }
            }

            float fundTolDb = (analyzedProfile == DetectionProfile::HighFocus && freqHz >= 2500.0f) ? 6.0f : 10.0f;

            // Check fundamental if k is 2nd harmonic (around k/2)
            if (!hasStrongHarmonics)
            {
                int targetHalf = static_cast<int>(std::round(0.5f * static_cast<float>(k)));
                for (int offset = -1; offset <= 1; ++offset)
                {
                    int h = targetHalf + offset;
                    if (h >= minBin && h < numBins - 1)
                    {
                        float hMag = currentMagnitudesDb[static_cast<size_t>(h)];
                        if (hMag > mag - fundTolDb &&
                            hMag > currentMagnitudesDb[static_cast<size_t>(h - 1)] &&
                            hMag > currentMagnitudesDb[static_cast<size_t>(h + 1)])
                        {
                            hasStrongHarmonics = true;
                            break;
                        }
                    }
                }
            }

            // Check fundamental if k is 3rd harmonic (around k/3)
            if (!hasStrongHarmonics)
            {
                int targetThird = static_cast<int>(std::round(static_cast<float>(k) / 3.0f));
                for (int offset = -1; offset <= 1; ++offset)
                {
                    int h = targetThird + offset;
                    if (h >= minBin && h < numBins - 1)
                    {
                        float hMag = currentMagnitudesDb[static_cast<size_t>(h)];
                        if (hMag > mag - fundTolDb &&
                            hMag > currentMagnitudesDb[static_cast<size_t>(h - 1)] &&
                            hMag > currentMagnitudesDb[static_cast<size_t>(h + 1)])
                        {
                            hasStrongHarmonics = true;
                            break;
                        }
                    }
                }
            }
        }

        if (hasStrongHarmonics)
        {
            candidatePersistence[static_cast<size_t>(k)] = 0;
            continue;
        }

        // Gate 6: Growth Rate & Howling Onset / Steady State
        float prevMag = historyMagnitudesDb[static_cast<size_t>(prevFrameIdx)][static_cast<size_t>(k)];
        float growth = mag - prevMag;

        // Feedback is valid if:
        // A) Fast exponential buildup (growth >= minGrowthDb)
        // B) Gradually raised / steady howling
        bool isFastBuildup = (growth >= minGrowthDb);
        float steadyThresh = -28.0f - (sensitivityOffsetDb * 0.4f);
        bool isSustainedHowling = (growth >= -0.6f && mag >= steadyThresh);

        if (!isFastBuildup && !isSustainedHowling)
        {
            candidatePersistence[static_cast<size_t>(k)] = 0;
            continue;
        }

        // Gate 7: Persistence across consecutive frames (2 frames ~ 23 ms)
        candidatePersistence[static_cast<size_t>(k)]++;

        if (candidatePersistence[static_cast<size_t>(k)] >= 2)
        {
            // Parabolic interpolation for exact frequency
            float alpha = currentMagnitudesDb[static_cast<size_t>(k - 1)];
            float beta = currentMagnitudesDb[static_cast<size_t>(k)];
            float gamma = currentMagnitudesDb[static_cast<size_t>(k + 1)];

            float denom = alpha - 2.0f * beta + gamma;
            float delta = 0.0f;
            if (std::abs(denom) > 1e-4f)
                delta = 0.5f * (alpha - gamma) / denom;

            float exactFreq = (static_cast<float>(k) + delta) * binWidth;

            DetectedFeedback df;
            df.frequency = exactFreq;
            df.peakDb = beta;
            df.prominenceDb = prominence;
            df.growthRate = growth / (2000.0f * HOP_SIZE / static_cast<float>(sampleRate));
            df.suggestedCutDb = juce::jlimit(3.0f, 12.0f, prominence - effectiveProminenceThresh + 3.0f);
            df.confidence = juce::jlimit(0.0f, 1.0f, prominence / 20.0f);

            detectedList.push_back(df);

            candidatePersistence[static_cast<size_t>(k)] = 0;
        }
    }
}

void FeedbackDetector::analyzeMusicalFrame(std::vector<DetectedFeedback>& detectedList)
{
    const float binWidth = static_cast<float>(sampleRate) / FFT_SIZE;
    const float hopMs = 1000.0f * HOP_SIZE / static_cast<float>(sampleRate);
    const bool aggressive = analyzedMode == DetectionMode::Aggressive;
    const float sensitivity = sensitivityOffsetDb.load();
    const int minBin = juce::jlimit(3, FFT_SIZE / 2 - 4, static_cast<int>(std::ceil(minDetectionHz / binWidth)));
    const int maxBin = juce::jlimit(minBin, FFT_SIZE / 2 - 4, static_cast<int>(maxDetectionHz / binWidth));
    const auto peakFrequency = [&](int k)
    {
        const float a = currentMagnitudesDb[static_cast<size_t>(k - 1)];
        const float b = currentMagnitudesDb[static_cast<size_t>(k)];
        const float c = currentMagnitudesDb[static_cast<size_t>(k + 1)];
        const float denominator = a - 2.0f * b + c;
        const float delta = std::abs(denominator) > 1.0e-5f
            ? juce::jlimit(-0.5f, 0.5f, 0.5f * (a - c) / denominator) : 0.0f;
        const float interpolated = (static_cast<float>(k) + delta) * binWidth;
        const auto ki = static_cast<size_t>(k);
        if (phaseCoherence[ki] > 0.7f)
        {
            const float pv = static_cast<float>(k * sampleRate / FFT_SIZE
                + std::atan2(phaseSumSin[ki], phaseSumCos[ki]) * sampleRate / (juce::MathConstants<double>::twoPi * HOP_SIZE));
            if (std::abs(pv - interpolated) < binWidth * 0.75f)
                return pv;
        }
        return interpolated;
    };
    for (int k = 0; k < FFT_SIZE / 2; ++k)
        eventCooldownMs[static_cast<size_t>(k)] = juce::jmax(0.0f, eventCooldownMs[static_cast<size_t>(k)] - hopMs);

    const auto previousDuration = trackedDurationMs;
    const auto previousFrequency = trackedFrequency;
    const auto previousCooldown = eventCooldownMs;
    const auto previousStrongPhase = strongPhaseDurationMs;
    for (int k = minBin; k <= maxBin; ++k)
    {
        const auto index = static_cast<size_t>(k);
        const float mag = currentMagnitudesDb[index];
        const float frequency = peakFrequency(k);
        float threshold = (aggressive ? 5.0f : 7.0f) - 0.35f * sensitivity;
        if ((analyzedProfile == DetectionProfile::HighFocus && frequency >= 2000.0f)
            || (analyzedProfile == DetectionProfile::LowFocus && frequency <= 1200.0f)) threshold -= 2.0f;
        const float prominence = mag - musicalBaselineAt(k);
        // No frame-wide dominance test: a candidate may be below the total music RMS.
        const bool peak = mag > currentMagnitudesDb[index - 1] && mag >= currentMagnitudesDb[index + 1];
        const float shoulderDrop = mag - juce::jmax(currentMagnitudesDb[index - 3], currentMagnitudesDb[index + 3]);
        int continuation = -1;
        for (int neighbor = k - 1; neighbor <= k + 1; ++neighbor)
            if (previousDuration[static_cast<size_t>(neighbor)] > 0.0f
                && std::abs(previousFrequency[static_cast<size_t>(neighbor)] - frequency) <= binWidth * 0.45f
                && (continuation < 0 || previousDuration[static_cast<size_t>(neighbor)] > previousDuration[static_cast<size_t>(continuation)]))
                continuation = neighbor;
        const float candidateThreshold = continuation >= 0 ? threshold - 2.0f : threshold;
        if (!peak || frequency < minDetectionHz || frequency > maxDetectionHz
            || mag < -75.0f - 0.35f * sensitivity || prominence < candidateThreshold || phaseCoherence[index] < 0.72f
            || (shoulderDrop < 3.0f && phaseCoherence[index] < 0.90f))
        {
            trackedDurationMs[index] = 0.0f;
            strongPhaseDurationMs[index] = 0.0f;
            // A brief masking dip subtracts evidence instead of discarding the
            // entire stable-frequency track. No evidence is added on this frame.
            if (continuation >= 0 && peak && prominence >= threshold - 4.0f
                && phaseCoherence[index] >= 0.60f && mag >= -75.0f - 0.35f * sensitivity)
            {
                const auto ci = static_cast<size_t>(continuation);
                trackedDurationMs[index] = juce::jmax(0.0f, previousDuration[ci] - hopMs);
                trackedFrequency[index] = previousFrequency[ci];
                eventCooldownMs[index] = juce::jmax(eventCooldownMs[index], previousCooldown[ci]);
            }
            continue;
        }

        // Interpolated frequency ratios avoid multiplying rounded FFT bin indices.
        // Require a family, rather than rejecting feedback for ONE unrelated note.
        const auto familyPeak = [&](float target)
        {
            if (target < 50.0f || target > static_cast<float>(sampleRate) * 0.49f) return false;
            const int center = static_cast<int>(std::round(target / binWidth));
            for (int h = juce::jmax(2, center - 2); h <= juce::jmin(FFT_SIZE / 2 - 3, center + 2); ++h)
            {
                if (std::abs(h - k) < 3) continue;
                const auto hi = static_cast<size_t>(h);
                const float hm = currentMagnitudesDb[hi];
                if (hm > mag - 18.0f && hm > currentMagnitudesDb[hi - 1]
                    && hm >= currentMagnitudesDb[hi + 1] && hm - musicalBaselineAt(h) > 6.0f
                    && phaseCoherence[hi] >= 0.80f
                    && std::abs(peakFrequency(h) - target) < juce::jmax(binWidth * 0.65f, target * 0.004f)) return true;
            }
            return false;
        };
        bool musicalFamily = false;
        for (int order = 1; order <= 8 && !musicalFamily; ++order)
        {
            const float fundamental = frequency / static_cast<float>(order);
            if (fundamental < 50.0f || (order > 1 && !familyPeak(fundamental))) continue;
            int relatives = 0;
            for (int harmonic = 1; harmonic <= 8; ++harmonic)
                if (harmonic != order && familyPeak(fundamental * static_cast<float>(harmonic))) ++relatives;
            musicalFamily = relatives >= 2;
        }
        if (musicalFamily)
        {
            trackedDurationMs[index] = 0.0f;
            strongPhaseDurationMs[index] = 0.0f;
            continue;
        }

        const float frequencyTolerance = juce::jmax(binWidth * 0.45f, frequency * 0.002f);
        // Keep one temporal track when an off-bin peak alternates between adjacent
        // maxima. The anchor stays fixed, preventing vibrato from accumulating hold.
        int best = -1;
        for (int neighbor = k - 1; neighbor <= k + 1; ++neighbor)
        {
            const auto ni = static_cast<size_t>(neighbor);
            if (previousDuration[ni] > 0.0f && std::abs(previousFrequency[ni] - frequency) <= frequencyTolerance
                && (best < 0 || previousDuration[ni] > previousDuration[static_cast<size_t>(best)])) best = neighbor;
        }
        trackedDurationMs[index] = hopMs;
        trackedFrequency[index] = frequency;
        if (best >= 0)
        {
            const auto bi = static_cast<size_t>(best);
            trackedDurationMs[index] += previousDuration[bi];
            trackedFrequency[index] = previousFrequency[bi];
            eventCooldownMs[index] = juce::jmax(eventCooldownMs[index], previousCooldown[bi]);
        }
        strongPhaseDurationMs[index] = phaseCoherence[index] >= 0.90f
            ? hopMs + (best >= 0 ? previousStrongPhase[static_cast<size_t>(best)] : 0.0f) : 0.0f;
        if (validHistoryFrames < HISTORY_FRAMES) continue;

        // NINOS2-T-inspired normalized temporal inverse sparsity, in linear magnitude.
        // This is a hybrid, not a reproduction of the paper's complete HDF pipeline.
        double sum2 = 0.0, sum4 = 0.0;
        float maxDb = rawMagnitudesDb[index];
        for (const auto& history : historyMagnitudesDb) maxDb = juce::jmax(maxDb, history[index]);
        for (const auto& history : historyMagnitudesDb)
        {
            const double power = std::pow(10.0, (history[index] - maxDb) / 10.0);
            sum2 += power;
            sum4 += power * power;
        }
        const float temporal = static_cast<float>((std::sqrt(sum2) / std::pow(sum4 + 1.0e-30, 0.25) - 1.0)
            / (std::pow(static_cast<double>(HISTORY_FRAMES), 0.25) - 1.0));
        // Exponential feedback is approximately a straight rising line in dB.
        // A single old noise valley is not evidence of exponential growth.
        double sumY = 0.0, sumXY = 0.0, sumYY = 0.0;
        constexpr int regressionFrames = HISTORY_FRAMES + 1;
        constexpr double meanX = (regressionFrames - 1) * 0.5;
        double sumXX = 0.0;
        for (int h = 0; h < regressionFrames; ++h)
        {
            const float y = h == HISTORY_FRAMES ? rawMagnitudesDb[index]
                : historyMagnitudesDb[static_cast<size_t>((historyIndex + h) % HISTORY_FRAMES)][index];
            const double x = h - meanX;
            sumY += y;
            sumXY += x * y;
            sumYY += static_cast<double>(y) * y;
            sumXX += x * x;
        }
        const double varianceY = juce::jmax(1.0e-9, sumYY - sumY * sumY / regressionFrames);
        const float growth = static_cast<float>(sumXY / sumXX * HISTORY_FRAMES);
        const float growthFit = static_cast<float>(sumXY * sumXY / (sumXX * varianceY));
        const float intervalMs = HISTORY_FRAMES * hopMs;
        const float growthThreshold = growthSensitivityLevel == 0 ? 4.0f : (growthSensitivityLevel == 2 ? 1.0f : 2.0f);
        const bool growing = growth >= growthThreshold && growthFit >= 0.75f && temporal >= 0.55f
            && strongPhaseDurationMs[index] >= (aggressive ? 90.0f : 130.0f);
        const float plv = phaseCoherence[index];
        const float minimumHold = growing ? (aggressive ? 90.0f : 130.0f) : (aggressive ? 220.0f : 350.0f);
        if (trackedDurationMs[index] < minimumHold || plv < (aggressive ? 0.74f : 0.78f)
            || (!growing && temporal < (aggressive ? 0.90f : 0.92f)) || prominence < threshold || eventCooldownMs[index] > 0.0f) continue;

        DetectedFeedback df;
        df.frequency = frequency;
        df.peakDb = mag;
        df.prominenceDb = prominence;
        df.growthRate = growth / intervalMs;
        df.confidence = juce::jlimit(0.0f, 1.0f, 0.2f * plv + 0.65f * temporal
            + 0.15f * juce::jlimit(0.0f, 1.0f, (prominence - threshold) / 12.0f));
        // Lower spectral thresholds need stronger joint temporal/phase evidence.
        // Overlapped FFT noise can show a high PLV briefly without being feedback.
        if (df.confidence < (aggressive ? 0.78f : 0.76f)) continue;
        df.suggestedCutDb = juce::jlimit(3.0f, 9.0f, prominence - threshold + 3.0f);
        detectedList.push_back(df);
        eventCooldownMs[index] = 150.0f;
    }
}

void FeedbackDetector::setSensitivity(float sensitivityDb)
{
    sensitivityOffsetDb = juce::jlimit(-60.0f, 20.0f, sensitivityDb);
}

void FeedbackDetector::setGrowthSensitivity(int level)
{
    growthSensitivityLevel = juce::jlimit(0, 2, level);
}

void FeedbackDetector::setProfile(DetectionProfile profile)
{
    currentProfile = profile;
}

void FeedbackDetector::setDetectionMode(DetectionMode mode)
{
    currentDetectionMode = mode;
}

void FeedbackDetector::setMinMaxFrequency(float minHz, float maxHz)
{
    minDetectionHz = juce::jmax(50.0f, minHz);
    maxDetectionHz = juce::jmin(20000.0f, maxHz);
}

void FeedbackDetector::copySpectrumTo(std::vector<float>& destMags, std::vector<float>& destBaseline)
{
    std::lock_guard<std::mutex> lock(spectrumMutex);
    if (destMags.size() != uiSpectrum.size())
        destMags.resize(uiSpectrum.size());
    if (destBaseline.size() != uiBaseline.size())
        destBaseline.resize(uiBaseline.size());

    std::copy(uiSpectrum.begin(), uiSpectrum.end(), destMags.begin());
    std::copy(uiBaseline.begin(), uiBaseline.end(), destBaseline.begin());
}

} // namespace LiveAudioLab
