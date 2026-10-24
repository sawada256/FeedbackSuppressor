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

    uiSpectrum.resize(NUM_BINS, -120.0f);
    uiBaseline.resize(NUM_BINS, -120.0f);
}

void FeedbackDetector::prepare(double newSampleRate)
{
    hostSampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    // Keep the analysis at 44.1/48 kHz so bin width, hop time and CPU do not
    // depend on the host rate (V2 analysed 96 kHz with 47 Hz bins).
    decimation = juce::jmax(1, static_cast<int>(std::floor(hostSampleRate / 44100.0 + 1.0e-6)));
    if (decimation > 1 && (decimation & (decimation - 1)) != 0)
        decimation = 1 << static_cast<int>(std::floor(std::log2(static_cast<double>(decimation))));
    sampleRate = hostSampleRate / decimation;
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
    decimAccum[0] = decimAccum[1] = 0.0f;
    decimCount = 0;
    historyCleared = false;
    clearAnalysisHistory();
}

void FeedbackDetector::clearAnalysisHistory()
{
    if (historyCleared) return; // V2 re-cleared ~300 KB on every silent hop
    fftBuffer.fill(0.0f);
    rightFftBuffer.fill(0.0f);
    currentMagnitudesDb.fill(-120.0f);
    averagedPower.fill(0.0f);
    rawMagnitudesDb.fill(-120.0f);
    baselineEnvelopeDb.fill(-120.0f);
    baselineStamp.fill(0);
    for (auto& channel : prevPhaseRad) channel.fill(0.0f);
    phaseCoherence.fill(0.0f);
    phaseFrequency.fill(0.0f);
    for (auto& channel : hasValidPhase) channel.fill(false);
    for (auto& channel : validPhaseFrames) channel.fill(0);
    for (auto& hist : historyMagnitudesDb) hist.fill(-120.0f);
    for (auto& hist : historySmoothedDb) hist.fill(-120.0f);
    historyTotalPower.fill(0.0);
    for (auto& channel : phaseCosHistory) for (auto& h : channel) h.fill(0.0f);
    for (auto& channel : phaseSinHistory) for (auto& h : channel) h.fill(0.0f);
    candidatePersistence.fill(0);
    track = TrackState {};
    prevTrack = TrackState {};
    cooldownMs.fill(0.0f);
    historyIndex = phaseHistIndex = validHistoryFrames = 0;
    historyCleared = true;
}

void FeedbackDetector::pushSamples(const float* const* channelData, int numChannels, int numSamples) noexcept
{
    if (numChannels <= 0 || numSamples <= 0 || channelData == nullptr || channelData[0] == nullptr)
        return;

    int writePos = fifoWritePos.load(std::memory_order_relaxed);
    const int readPos = fifoReadPos.load(std::memory_order_acquire);
    const int freeSpace = (readPos - writePos - 1 + FIFO_SIZE) % FIFO_SIZE;
    const int needed = numSamples / decimation + 1;
    // SPSC ownership: never overwrite samples still being read by the worker.
    if (needed > freeSpace)
    {
        droppedBlocks.fetch_add(1, std::memory_order_release);
        return;
    }
    const float* left = channelData[0];
    const float* right = numChannels > 1 && channelData[1] != nullptr ? channelData[1] : left;
    const float scale = 1.0f / static_cast<float>(decimation);
    for (int i = 0; i < numSamples; ++i)
    {
        const float l = std::isfinite(left[i]) ? left[i] : 0.0f;
        const float r = std::isfinite(right[i]) ? right[i] : 0.0f;
        if (decimation == 1)
        {
            fifoBuffer[0][static_cast<size_t>(writePos)] = l;
            fifoBuffer[1][static_cast<size_t>(writePos)] = r;
            writePos = (writePos + 1) % FIFO_SIZE;
            continue;
        }
        // Boxcar decimator: nulls at every multiple of the new sample rate, so
        // the bands that would alias onto low frequencies are suppressed.
        decimAccum[0] += l;
        decimAccum[1] += r;
        if (++decimCount == decimation)
        {
            fifoBuffer[0][static_cast<size_t>(writePos)] = decimAccum[0] * scale;
            fifoBuffer[1][static_cast<size_t>(writePos)] = decimAccum[1] * scale;
            writePos = (writePos + 1) % FIFO_SIZE;
            decimAccum[0] = decimAccum[1] = 0.0f;
            decimCount = 0;
        }
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
        historyCleared = false;
        clearAnalysisHistory();
        // Discard the pre-gap tail; do not stitch discontinuous audio into phase history.
        if (drops != lastDroppedBlocks)
            fifoReadPos.store(fifoWritePos.load(std::memory_order_acquire), std::memory_order_release);
        lastDroppedBlocks = drops;
        analyzedMode = mode;
        analyzedProfile = profile;
    }
    int readPos = fifoReadPos.load(std::memory_order_relaxed);
    const int writePos = fifoWritePos.load(std::memory_order_acquire);
    int available = writePos - readPos;
    if (available < 0)
        available += FIFO_SIZE;

    bool foundAny = false;
    bool analysedAny = false;

    while (available >= FFT_SIZE)
    {
        float sumSquares = 0.0f;
        for (int i = 0; i < FFT_SIZE; ++i)
        {
            const int idx = (readPos + i) % FIFO_SIZE;
            const float s = fifoBuffer[0][static_cast<size_t>(idx)];
            const float r = fifoBuffer[1][static_cast<size_t>(idx)];
            rightFftBuffer[static_cast<size_t>(i)] = r;
            fftBuffer[static_cast<size_t>(i)] = s;
            sumSquares += juce::jmax(s * s, r * r);
        }
        std::fill(fftBuffer.begin() + FFT_SIZE, fftBuffer.end(), 0.0f);
        std::fill(rightFftBuffer.begin() + FFT_SIZE, rightFftBuffer.end(), 0.0f);

        const float frameRms = std::sqrt(sumSquares / static_cast<float>(FFT_SIZE));
        const float frameRmsDb = 20.0f * std::log10(frameRms + 1e-6f);

        // Absolute guard against numerical noise; musical modes use a lower floor.
        if (frameRmsDb < (mode == DetectionMode::Standard ? -55.0f : -80.0f))
        {
            clearAnalysisHistory();
            readPos = (readPos + HOP_SIZE) % FIFO_SIZE;
            fifoReadPos.store(readPos, std::memory_order_release);
            available -= HOP_SIZE;
            continue;
        }
        historyCleared = false;
        analysedAny = true;
        ++frameCounter;

        window.multiplyWithWindowingTable(fftBuffer.data(), FFT_SIZE);
        window.multiplyWithWindowingTable(rightFftBuffer.data(), FFT_SIZE);
        fft.performRealOnlyForwardTransform(fftBuffer.data(), true);
        fft.performRealOnlyForwardTransform(rightFftBuffer.data(), true);

        const float norm = 2.0f / static_cast<float>(FFT_SIZE);
        const float spectralAlpha = static_cast<float>(1.0 - std::exp(-HOP_SIZE / (0.035 * sampleRate)));
        const bool musical = mode != DetectionMode::Standard;
        const float hzPerRad = static_cast<float>(sampleRate / (juce::MathConstants<double>::twoPi * HOP_SIZE));
        const float binWidth = static_cast<float>(sampleRate / FFT_SIZE);
        const auto ph = static_cast<size_t>(phaseHistIndex);

        for (int k = 0; k < NUM_BINS; ++k)
        {
            const auto ki = static_cast<size_t>(k);
            const float re = fftBuffer[2 * ki], im = fftBuffer[2 * ki + 1];
            const float rr = rightFftBuffer[2 * ki], ri = rightFftBuffer[2 * ki + 1];
            const float pl = re * re + im * im, pr = rr * rr + ri * ri;
            const int channel = pr > pl ? 1 : 0;
            float normalizedPower = juce::jmax(pl, pr) * norm * norm;
            rawMagnitudesDb[ki] = 10.0f * std::log10(normalizedPower + 1.0e-12f);
            if (musical)
            {
                auto& average = averagedPower[ki];
                if (validHistoryFrames == 0) average = normalizedPower;
                else average += spectralAlpha * (normalizedPower - average);
                normalizedPower = average;
            }
            currentMagnitudesDb[ki] = 10.0f * std::log10(normalizedPower + 1.0e-12f);

            if (! musical)
                continue; // the legacy detector does not use phase

            // Phase vocoder per channel: deviation of the measured phase advance
            // from the bin-centre advance. Choosing the louder spectrum must not
            // create artificial discontinuities, so both channels are tracked.
            float sumCosSel = 0.0f, sumSinSel = 0.0f, plvSel = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto ci = static_cast<size_t>(ch);
                const float phase = ch == 0 ? std::atan2(im, re) : std::atan2(ri, rr);
                float plv = 0.0f, sumCos = 0.0f, sumSin = 0.0f;
                if (hasValidPhase[ci][ki])
                {
                    // Expected advance 2*pi*k*HOP/N; HOP/N = 1/4 so it is k*pi/2 (mod 2*pi).
                    const float expected = juce::MathConstants<float>::halfPi * static_cast<float>(k & 3);
                    const float error = phase - prevPhaseRad[ci][ki] - expected;
                    phaseCosHistory[ci][ph][ki] = std::cos(error);
                    phaseSinHistory[ci][ph][ki] = std::sin(error);
                    for (int h = 0; h < PHASE_FRAMES; ++h)
                    {
                        sumCos += phaseCosHistory[ci][static_cast<size_t>(h)][ki];
                        sumSin += phaseSinHistory[ci][static_cast<size_t>(h)][ki];
                    }
                    auto& valid = validPhaseFrames[ci][ki];
                    valid = juce::jmin(PHASE_FRAMES, valid + 1);
                    if (valid >= PHASE_FRAMES)
                        plv = std::sqrt(sumCos * sumCos + sumSin * sumSin) / static_cast<float>(PHASE_FRAMES);
                }
                else
                {
                    validPhaseFrames[ci][ki] = 0;
                    hasValidPhase[ci][ki] = true;
                    for (int h = 0; h < PHASE_FRAMES; ++h)
                    {
                        phaseCosHistory[ci][static_cast<size_t>(h)][ki] = 0.0f;
                        phaseSinHistory[ci][static_cast<size_t>(h)][ki] = 0.0f;
                    }
                }
                prevPhaseRad[ci][ki] = phase;
                if (ch == channel) { sumCosSel = sumCos; sumSinSel = sumSin; plvSel = plv; }
            }
            phaseCoherence[ki] = plvSel;
            phaseFrequency[ki] = static_cast<float>(k) * binWidth
                + (plvSel > 0.0f ? std::atan2(sumSinSel, sumCosSel) * hzPerRad : 0.0f);
        }
        if (musical)
            phaseHistIndex = (phaseHistIndex + 1) % PHASE_FRAMES;

        const auto previousCount = detectedList.size();
        if (musical)
        {
            analyzeMusicalFrame(detectedList);
            if ((frameCounter & 3u) == 0u)
                calculateMusicalBaselineForUi();
        }
        else
        {
            calculateBaselineEnvelope();
            analyzeFrame(detectedList);
        }
        if (detectedList.size() > previousCount)
            foundAny = true;

        historyMagnitudesDb[static_cast<size_t>(historyIndex)] = rawMagnitudesDb;
        if (musical)
        {
            historySmoothedDb[static_cast<size_t>(historyIndex)] = currentMagnitudesDb;
            historyTotalPower[static_cast<size_t>(historyIndex)] = currentTotalPower;
        }
        historyIndex = (historyIndex + 1) % HISTORY_FRAMES;
        validHistoryFrames = juce::jmin(validHistoryFrames + 1, HISTORY_FRAMES);

        readPos = (readPos + HOP_SIZE) % FIFO_SIZE;
        fifoReadPos.store(readPos, std::memory_order_release);
        available -= HOP_SIZE;
    }

    // Copy for UI; never block the analysis on a painting UI.
    if (analysedAny)
    {
        std::unique_lock<std::mutex> lock(spectrumMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            std::copy(currentMagnitudesDb.begin(), currentMagnitudesDb.end(), uiSpectrum.begin());
            std::copy(baselineEnvelopeDb.begin(), baselineEnvelopeDb.end(), uiBaseline.begin());
        }
    }

    return foundAny;
}

void FeedbackDetector::calculateBaselineEnvelope()
{
    // Legacy (Standard) baseline: mean dB of +-14 bins excluding +-2, or the reference tilt.
    const int windowRadius = 14;
    for (int k = 0; k < NUM_BINS; ++k)
    {
        const float freqHz = static_cast<float>(k) * static_cast<float>(sampleRate) / static_cast<float>(FFT_SIZE);
        const float refOffset = getReferenceCurveDb(freqHz);
        const int startK = juce::jmax(0, k - windowRadius);
        const int endK = juce::jmin(NUM_BINS - 1, k + windowRadius);
        float sum = 0.0f;
        int count = 0;
        for (int j = startK; j <= endK; ++j)
            if (std::abs(j - k) > 2) { sum += currentMagnitudesDb[static_cast<size_t>(j)]; ++count; }
        float localAvg = count > 0 ? (sum / static_cast<float>(count)) : currentMagnitudesDb[static_cast<size_t>(k)];

        if (analyzedProfile == DetectionProfile::HighFocus && freqHz >= 2000.0f)
            localAvg -= juce::jlimit(2.0f, 6.0f, 2.5f + 1.5f * std::log2(freqHz / 2000.0f));
        else if (analyzedProfile == DetectionProfile::LowFocus && freqHz >= 80.0f && freqHz <= 1200.0f)
            localAvg -= 3.5f;

        baselineEnvelopeDb[static_cast<size_t>(k)] = juce::jmax(localAvg, refOffset);
    }
}

float FeedbackDetector::baselineAt(int k) noexcept
{
    // Music-aware background: trimmed (lowest 75 %) mean of LINEAR power over
    // +-14 bins excluding the +-2 main-lobe bins. Computed on demand for peak
    // bins only and cached per frame (V2 sorted 28 values with 28 pow() calls
    // for every one of the 1024 bins on every frame).
    const auto ki = static_cast<size_t>(k);
    if (baselineStamp[ki] == frameCounter)
        return baselineEnvelopeDb[ki];
    std::array<float, 28> shoulders {};
    int n = 0;
    for (int j = juce::jmax(0, k - 14); j <= juce::jmin(NUM_BINS - 1, k + 14); ++j)
        if (std::abs(j - k) > 2)
            shoulders[static_cast<size_t>(n++)] = averagedPower[static_cast<size_t>(j)];
    const int keep = juce::jmax(1, n * 3 / 4);
    if (keep < n)
        std::nth_element(shoulders.begin(), shoulders.begin() + keep, shoulders.begin() + n);
    double power = 0.0;
    for (int j = 0; j < keep; ++j)
        power += shoulders[static_cast<size_t>(j)];
    const float db = static_cast<float>(10.0 * std::log10(power / keep + 1.0e-12));
    baselineEnvelopeDb[ki] = db;
    baselineStamp[ki] = frameCounter;
    return db;
}

void FeedbackDetector::calculateMusicalBaselineForUi()
{
    for (int k = 0; k < NUM_BINS; ++k)
        baselineAt(k);
}

float FeedbackDetector::backgroundGrowth(int k, int frames) const noexcept
{
    const auto frameIndex = [&](int h) { return static_cast<size_t>((historyIndex + HISTORY_FRAMES - (frames - 1) + h) % HISTORY_FRAMES); };
    const double meanX = (frames - 1) * 0.5;
    double sumY = 0.0, sumXY = 0.0, sumXX = 0.0;
    for (int h = 0; h < frames; ++h)
    {
        const bool current = h == frames - 1;
        double total = current ? currentTotalPower : historyTotalPower[frameIndex(h)];
        for (int j = k - 3; j <= k + 3; ++j)
        {
            const auto ji = static_cast<size_t>(j);
            total -= current ? static_cast<double>(averagedPower[ji])
                             : std::pow(10.0, historySmoothedDb[frameIndex(h)][ji] / 10.0);
        }
        const double y = 10.0 * std::log10(juce::jmax(total, 1.0e-12));
        const double x = h - meanX;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
    }
    return static_cast<float>(sumXY / sumXX);
}

float FeedbackDetector::temporalSparsity(int k) const noexcept
{
    // NINOS2-T inspired normalised temporal inverse sparsity of linear power
    // over the 16-frame history + current frame: 1 = perfectly steady, 0 = one
    // isolated frame.
    const auto ki = static_cast<size_t>(k);
    float maxDb = rawMagnitudesDb[ki];
    for (const auto& history : historyMagnitudesDb) maxDb = juce::jmax(maxDb, history[ki]);
    double sum2 = 0.0, sum4 = 0.0;
    for (const auto& history : historyMagnitudesDb)
    {
        const double power = std::pow(10.0, (history[ki] - maxDb) / 10.0);
        sum2 += power;
        sum4 += power * power;
    }
    return static_cast<float>((std::sqrt(sum2) / std::pow(sum4 + 1.0e-30, 0.25) - 1.0)
                              / (std::pow(static_cast<double>(HISTORY_FRAMES), 0.25) - 1.0));
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

void FeedbackDetector::regression(int k, int frames, float& slopeDbPerFrame, float& fit) const noexcept
{
    // Least-squares line through the last (frames - 1) history frames plus the
    // current frame (oldest -> newest), on the 35 ms-smoothed spectrum. Exponential growth is a straight
    // line in dB: slope = growth, fit = R^2 (consistency of the inter-frame
    // magnitude slope, cf. IMSD).
    const auto ki = static_cast<size_t>(k);
    const double meanX = (frames - 1) * 0.5;
    double sumY = 0.0, sumXY = 0.0, sumYY = 0.0, sumXX = 0.0;
    for (int h = 0; h < frames; ++h)
    {
        const float y = h == frames - 1
            ? currentMagnitudesDb[ki]
            : historySmoothedDb[static_cast<size_t>((historyIndex + HISTORY_FRAMES - (frames - 1) + h) % HISTORY_FRAMES)][ki];
        const double x = h - meanX;
        sumY += y;
        sumXY += x * y;
        sumYY += static_cast<double>(y) * y;
        sumXX += x * x;
    }
    const double varianceY = sumYY - sumY * sumY / frames;
    slopeDbPerFrame = static_cast<float>(sumXY / sumXX);
    fit = varianceY > 1.0e-9 ? static_cast<float>(sumXY * sumXY / (sumXX * varianceY)) : 0.0f;
}

void FeedbackDetector::analyzeMusicalFrame(std::vector<DetectedFeedback>& detectedList)
{
    const float binWidth = static_cast<float>(sampleRate) / FFT_SIZE;
    const float hopMs = 1000.0f * HOP_SIZE / static_cast<float>(sampleRate);
    const float framesPerSecond = 1000.0f / hopMs;
    const bool aggressive = analyzedMode == DetectionMode::Aggressive;
    const float sensitivity = sensitivityOffsetDb.load();
    const int growthLevel = growthSensitivityLevel.load();
    const float minHz = minDetectionHz.load(), maxHz = maxDetectionHz.load();
    const int minBin = juce::jlimit(3, NUM_BINS - 4, static_cast<int>(std::ceil(minHz / binWidth)));
    const int maxBin = juce::jlimit(minBin, NUM_BINS - 4, static_cast<int>(maxHz / binWidth));

    // Growth thresholds in dB/s (sample-rate independent). Feedback build-up at
    // a modest loop-gain excess of 0.5-3 dB per 10-30 ms round trip is roughly
    // 15-300 dB/s; musical crescendos rarely exceed ~10 dB/s for long.
    const float fastGrowthDbPerSec = growthLevel == 0 ? 50.0f : (growthLevel == 2 ? 14.0f : 25.0f);
    const float slowGrowthDbPerSec = growthLevel == 0 ? 12.0f : (growthLevel == 2 ? 3.0f : 6.0f);
    const float steadyHoldMs = aggressive ? 450.0f : 700.0f;

    const auto peakFrequency = [&](int k)
    {
        const auto ki = static_cast<size_t>(k);
        // The phase-vocoder estimate (8-frame circular mean) is unbiased and far
        // more precise than magnitude interpolation once the phase is coherent.
        const float a = currentMagnitudesDb[ki - 1], b = currentMagnitudesDb[ki], c = currentMagnitudesDb[ki + 1];
        const float denominator = a - 2.0f * b + c;
        const float delta = std::abs(denominator) > 1.0e-5f ? juce::jlimit(-0.5f, 0.5f, 0.5f * (a - c) / denominator) : 0.0f;
        const float interpolated = (static_cast<float>(k) + delta) * binWidth;
        if (phaseCoherence[ki] > 0.7f && std::abs(phaseFrequency[ki] - interpolated) < binWidth * 0.75f)
            return phaseFrequency[ki];
        return interpolated;
    };

    for (auto& c : cooldownMs)
        c = juce::jmax(0.0f, c - hopMs);

    currentTotalPower = 0.0;
    currentMaxPeakDb = -120.0f;
    for (int k = minBin; k <= maxBin; ++k)
    {
        currentTotalPower += averagedPower[static_cast<size_t>(k)];
        currentMaxPeakDb = juce::jmax(currentMaxPeakDb, currentMagnitudesDb[static_cast<size_t>(k)]);
    }
    // Partials more than 45 dB (Aggressive: 50 dB) below the loudest component
    // are inaudible next to it; ignoring them removes onset/formant artefacts of
    // weak upper harmonics without the V1 "must dominate the frame" rule.
    const float relativeFloor = currentMaxPeakDb - (aggressive ? 50.0f : 45.0f);

    std::swap(track, prevTrack);
    track.durationMs.fill(0.0f);
    track.strongPhaseMs.fill(0.0f);
    track.growFrames.fill(0.0f);
    track.ghostFrames.fill(0.0f);

    for (int k = minBin; k <= maxBin; ++k)
    {
        const auto index = static_cast<size_t>(k);
        const float mag = currentMagnitudesDb[index];
        if (! (mag > currentMagnitudesDb[index - 1] && mag >= currentMagnitudesDb[index + 1]))
            continue;

        const float frequency = peakFrequency(k);
        if (frequency < minHz || frequency > maxHz)
            continue;

        float threshold = (aggressive ? 5.0f : 7.0f) - 0.35f * sensitivity;
        if ((analyzedProfile == DetectionProfile::HighFocus && frequency >= 2000.0f)
            || (analyzedProfile == DetectionProfile::LowFocus && frequency <= 1200.0f))
            threshold -= 2.0f;
        const float prominence = mag - baselineAt(k);
        const float plv = phaseCoherence[index];
        const float levelFloor = -75.0f - 0.35f * sensitivity;
        const float shoulderDrop = mag - juce::jmax(currentMagnitudesDb[index - 3], currentMagnitudesDb[index + 3]);

        // Continue the track of the neighbouring bin whose anchor matches.
        const float trackTolerance = juce::jmax(binWidth * 0.45f, frequency * 0.002f);
        int continuation = -1;
        for (int neighbor = k - 1; neighbor <= k + 1; ++neighbor)
        {
            const auto ni = static_cast<size_t>(neighbor);
            if (prevTrack.durationMs[ni] > 0.0f && std::abs(prevTrack.anchorHz[ni] - frequency) <= trackTolerance
                && (continuation < 0 || prevTrack.durationMs[ni] > prevTrack.durationMs[static_cast<size_t>(continuation)]))
                continuation = neighbor;
        }
        const auto ci = static_cast<size_t>(juce::jmax(0, continuation));
        const float candidateThreshold = continuation >= 0 ? threshold - 2.0f : threshold;

        if (mag < levelFloor || mag < relativeFloor || prominence < candidateThreshold || plv < 0.72f || (shoulderDrop < 3.0f && plv < 0.90f))
        {
            // A brief masking dip subtracts evidence instead of discarding the
            // stable-frequency track; no evidence is added on this frame.
            if (continuation >= 0 && prominence >= threshold - 4.0f && plv >= 0.60f && mag >= levelFloor && mag >= relativeFloor - 6.0f)
            {
                track.durationMs[index] = juce::jmax(0.0f, prevTrack.durationMs[ci] - hopMs);
                track.anchorHz[index] = prevTrack.anchorHz[ci];
                track.minHz[index] = prevTrack.minHz[ci];
                track.maxHz[index] = prevTrack.maxHz[ci];
                track.minDb[index] = juce::jmin(prevTrack.minDb[ci], mag);
            }
            continue;
        }

        // PHPR-like protection: a peak that belongs to a harmonic family (it
        // is, or has, >= 2 other phase-coherent partials at precise integer
        // ratios) is a musical note, not a single feedback mode.
        const auto familyPeak = [&](float target)
        {
            if (target < 50.0f || target > static_cast<float>(sampleRate) * 0.49f) return false;
            const int center = static_cast<int>(std::round(target / binWidth));
            for (int h = juce::jmax(2, center - 2); h <= juce::jmin(NUM_BINS - 3, center + 2); ++h)
            {
                if (std::abs(h - k) < 3) continue;
                const auto hi = static_cast<size_t>(h);
                const float hm = currentMagnitudesDb[hi];
                if (hm > mag - 18.0f && hm > currentMagnitudesDb[hi - 1] && hm >= currentMagnitudesDb[hi + 1]
                    && phaseCoherence[hi] >= 0.80f && hm - baselineAt(h) > 6.0f
                    && std::abs(peakFrequency(h) - target) < juce::jmax(binWidth * 0.65f, target * 0.004f))
                    return true;
            }
            return false;
        };
        // Two relatives always mark a note. A single precise relative (high
        // voices and flutes have few strong partials) only blocks the slow and
        // steady paths; a fast exponential build-up is still allowed through,
        // and so is a very loud (possibly self-distorting) tone.
        int bestRelatives = 0;
        for (int order = 1; order <= 8 && bestRelatives < 2; ++order)
        {
            const float fundamental = frequency / static_cast<float>(order);
            if (fundamental < 50.0f || (order > 1 && ! familyPeak(fundamental))) continue;
            int relatives = order > 1 ? 1 : 0; // the fundamental itself is a relative
            for (int harmonic = 2; harmonic <= 8 && relatives < 2; ++harmonic)
                if (harmonic != order && familyPeak(fundamental * static_cast<float>(harmonic))) ++relatives;
            bestRelatives = juce::jmax(bestRelatives, relatives);
        }
        const bool musicalFamily = bestRelatives >= 2;
        const bool partialFamily = bestRelatives >= 1 && mag < -12.0f;
        if (musicalFamily)
            continue;

        // ---- Track update -------------------------------------------------
        const bool hasCont = continuation >= 0;
        track.durationMs[index] = hopMs + (hasCont ? prevTrack.durationMs[ci] : 0.0f);
        track.anchorHz[index] = hasCont ? prevTrack.anchorHz[ci] : frequency;
        // Strong-phase evidence: grows while PLV >= 0.9, is kept through
        // moderately coherent frames (weak tones in noise flicker around 0.9)
        // and is lost below 0.8.
        const float previousStrong = hasCont ? prevTrack.strongPhaseMs[ci] : 0.0f;
        track.strongPhaseMs[index] = plv >= 0.90f ? previousStrong + hopMs : (plv >= 0.80f ? previousStrong : 0.0f);

        // Frequency-stability envelope of the phase-vocoder frequency. The
        // envelope contracts exponentially toward the current estimate
        // (tau 250 ms) so early noisy/contaminated estimates of a weak build-up
        // do not condemn it forever, while vibrato (period ~0.2 s) and pitch
        // drift keep re-widening it.
        const float baseTolerance = juce::jmax(1.0f, 0.0006f * frequency);
        const float relax = 1.0f - std::exp(-hopMs / 250.0f);
        const float pv = plv >= 0.85f ? phaseFrequency[index] : frequency;
        float lo = hasCont ? prevTrack.minHz[ci] : pv;
        float hi = hasCont ? prevTrack.maxHz[ci] : pv;
        lo += relax * (pv - lo);
        hi += relax * (pv - hi);
        if (plv >= 0.85f)
        {
            lo = juce::jmin(lo, pv);
            hi = juce::jmax(hi, pv);
        }
        if (hi < lo) lo = hi = 0.5f * (lo + hi);
        track.minHz[index] = lo;
        track.maxHz[index] = hi;
        track.minDb[index] = hasCont ? juce::jmin(prevTrack.minDb[ci], mag) : mag;
        const float rise = mag - track.minDb[index];
        // Allowance for estimator noise: PV frequency std ~ hzPerRad / (SNR * sqrt(frames)).
        const float snr = std::pow(10.0f, juce::jmin(prominence, 40.0f) / 20.0f);
        const float spreadTolerance = baseTolerance + 21.0f / snr;
        const bool frequencyStable = (hi - lo) <= spreadTolerance;

        if (validHistoryFrames < 8)
            continue;

        // ---- Growth (IMSD-like) -------------------------------------------
        float slope8, fit8;
        regression(k, 8, slope8, fit8);
        const float growth8 = slope8 * framesPerSecond;
        // Note onsets and crescendos raise every partial and the background
        // together; feedback grows on its own. Use growth relative to the rest
        // of the spectrum (excluding this peak's main lobe).
        const float background8 = backgroundGrowth(k, 8) * framesPerSecond;
        const float relativeGrowth8 = growth8 - juce::jmax(0.0f, background8);
        // The newest frames must still be rising: a note attack is steep but
        // then plateaus, an unstable loop keeps growing.
        float slope4, fit4;
        regression(k, 4, slope4, fit4);
        const bool fastGrowing = growth8 >= fastGrowthDbPerSec && relativeGrowth8 >= 0.6f * fastGrowthDbPerSec && fit8 >= 0.85f
                                 && slope4 * framesPerSecond >= 0.5f * fastGrowthDbPerSec;
        track.growFrames[index] = fastGrowing ? 1.0f + (hasCont ? prevTrack.growFrames[ci] : 0.0f) : 0.0f;

        const float strong = track.strongPhaseMs[index];
        const float duration = track.durationMs[index];
        // Fast path also needs a real rise: amplitude modulation of a partial
        // (vibrato through a formant, tremolo) produces short steep slopes but
        // not several dB of sustained exponential growth.
        const bool fastPath = validHistoryFrames >= 12 // no onset-from-silence artefacts right after a reset
                              && track.growFrames[index] >= 3.0f
                              && rise >= (aggressive ? 3.0f : 4.0f)
                              && strong >= (aggressive ? 40.0f : 60.0f) && frequencyStable;

        bool slowPath = false, steadyPath = false;
        float growth16 = growth8, temporal = 0.0f;
        if (! fastPath && validHistoryFrames >= HISTORY_FRAMES)
        {
            float slope16, fit16;
            regression(k, HISTORY_FRAMES + 1, slope16, fit16);
            growth16 = slope16 * framesPerSecond;
            temporal = temporalSparsity(k);
            const float background16 = backgroundGrowth(k, HISTORY_FRAMES + 1) * framesPerSecond;
            // Exponential growth has the same slope over 8 and 16 frames; an
            // onset early in the window inflates the 16-frame slope only.
            slowPath = growth16 >= slowGrowthDbPerSec && growth8 >= 0.5f * growth16
                       && growth16 - juce::jmax(0.0f, background16) >= 0.6f * slowGrowthDbPerSec && fit16 >= 0.75f && temporal >= 0.50f
                       && rise >= (aggressive ? 2.5f : 3.0f)
                       && strong >= (aggressive ? 80.0f : 100.0f) && frequencyStable && ! partialFamily;
            steadyPath = duration >= steadyHoldMs && strong >= 200.0f && plv >= 0.85f
                         && temporal >= (aggressive ? 0.90f : 0.92f) && frequencyStable && ! partialFamily;
        }

        // Reporting needs the candidate within 35 dB (Aggressive: 38 dB) of the
        // loudest component: weaker partials are tracked but cannot trigger.
        const bool audible = mag >= currentMaxPeakDb - (aggressive ? 38.0f : 35.0f);
        if (! (fastPath || slowPath || steadyPath) || ! audible || prominence < threshold || cooldownMs[index] > 0.0f)
            continue;

        DetectedFeedback df;
        df.frequency = frequency;
        df.peakDb = mag;
        df.prominenceDb = prominence;
        df.growthRate = (fastPath ? growth8 : growth16) / 1000.0f; // dB per ms
        df.confidence = juce::jlimit(0.0f, 1.0f, 0.4f * plv + 0.3f * (fastPath || slowPath ? 1.0f : temporal)
                                                     + 0.3f * juce::jlimit(0.0f, 1.0f, (prominence - threshold) / 12.0f));
        // Initial depth from the residual prominence; a fast build-up gets a
        // head start because it will otherwise overshoot during the next steps.
        df.suggestedCutDb = juce::jlimit(3.0f, 12.0f, prominence - threshold + 3.0f + (fastPath ? 3.0f : 0.0f));
        detectedList.push_back(df);
        for (int n = k - 1; n <= k + 1; ++n)
            cooldownMs[static_cast<size_t>(n)] = 150.0f;
    }

    // A feedback mode briefly swallowed by a passing partial (the bin is no
    // longer a local maximum) keeps its track for up to 3 frames, losing one
    // frame of evidence each time, instead of restarting from zero.
    for (int k = minBin; k <= maxBin; ++k)
    {
        const auto ki = static_cast<size_t>(k);
        if (prevTrack.durationMs[ki] <= 0.0f || prevTrack.ghostFrames[ki] >= 3.0f)
            continue;
        if (track.durationMs[ki - 1] > 0.0f || track.durationMs[ki] > 0.0f || track.durationMs[ki + 1] > 0.0f)
            continue;
        track.durationMs[ki] = juce::jmax(0.0f, prevTrack.durationMs[ki] - hopMs);
        track.anchorHz[ki] = prevTrack.anchorHz[ki];
        track.minHz[ki] = prevTrack.minHz[ki];
        track.maxHz[ki] = prevTrack.maxHz[ki];
        track.minDb[ki] = prevTrack.minDb[ki];
        track.strongPhaseMs[ki] = prevTrack.strongPhaseMs[ki];
        track.ghostFrames[ki] = prevTrack.ghostFrames[ki] + 1.0f;
    }
}

void FeedbackDetector::setSensitivity(float sensitivityDb)
{
    sensitivityOffsetDb = juce::jlimit(-60.0f, 20.0f, std::isfinite(sensitivityDb) ? sensitivityDb : 0.0f);
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
