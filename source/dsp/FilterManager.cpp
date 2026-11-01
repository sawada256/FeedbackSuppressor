#include "FilterManager.h"
#include <algorithm>
#include <cmath>

namespace LiveAudioLab
{

FilterManager::FilterManager()
{
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        slots[i].id = -1; // inactive
        slots[i].isFixed = (i >= MAX_LIVE_FILTERS);
    }
}

void FilterManager::prepare(double newSampleRate)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    // Audio is stopped here, so the audio-thread filters may be set directly.
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        const auto si = static_cast<size_t>(i);
        auto& sh = shadow[si];
        if (slots[si].id != -1)
            sh = { slots[si].frequency, slots[si].targetGainDb, slots[si].q, slots[si].isBypassed, sh.immediateGeneration };
        else
            sh = { 1000.0f, 0.0f, defaultQ, true, sh.immediateGeneration };
        dspFilters[si].prepare(sampleRate);
        dspFilters[si].setBypass(sh.bypass);
        dspFilters[si].setParameters(sh.freq, sh.gainDb, sh.q, true);
        appliedImmediateGeneration[si] = sh.immediateGeneration;
        sharedCurrentGainDb[si].store(dspFilters[si].getCurrentGainDb(), std::memory_order_relaxed);
        publish(i);
    }
}

void FilterManager::reset()
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        dspFilters[i].reset();
    }
}

void FilterManager::ctlSetParameters(int index, float freqHz, float gainDb, float q, bool immediate) noexcept
{
    auto& sh = shadow[static_cast<size_t>(index)];
    sh.freq = freqHz; sh.gainDb = gainDb; sh.q = q;
    if (immediate) ++sh.immediateGeneration;
    publish(index);
}

void FilterManager::ctlSetBypass(int index, bool bypass) noexcept
{
    shadow[static_cast<size_t>(index)].bypass = bypass;
    publish(index);
}

void FilterManager::publish(int index) noexcept
{
    // Single writer (callers hold filterMutex). Odd sequence = write in progress.
    auto& box = mailbox[static_cast<size_t>(index)];
    const auto& sh = shadow[static_cast<size_t>(index)];
    const uint32_t seq = box.sequence.load(std::memory_order_relaxed);
    box.sequence.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    box.freq.store(sh.freq, std::memory_order_relaxed);
    box.gainDb.store(sh.gainDb, std::memory_order_relaxed);
    box.q.store(sh.q, std::memory_order_relaxed);
    box.bypass.store(sh.bypass, std::memory_order_relaxed);
    box.immediateGeneration.store(sh.immediateGeneration, std::memory_order_relaxed);
    box.sequence.store(seq + 2, std::memory_order_release);
}

void FilterManager::processAudio(juce::AudioBuffer<float>& buffer) noexcept
{
    // V2 returned immediately in Bypass mode, removing every notch at once
    // (click). V2.1 fades all notches out and back in with the normal smoothing.
    const bool bypassAll = currentMode.load(std::memory_order_relaxed) == OperationMode::Bypass;
    auto* const* channels = buffer.getArrayOfWritePointers();
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        const auto si = static_cast<size_t>(i);
        auto& box = mailbox[si];
        auto& filter = dspFilters[si];
        const uint32_t seq1 = box.sequence.load(std::memory_order_acquire);
        if ((seq1 & 1u) == 0u)
        {
            const float f = box.freq.load(std::memory_order_relaxed);
            const float g = box.gainDb.load(std::memory_order_relaxed);
            const float q = box.q.load(std::memory_order_relaxed);
            const bool bypass = box.bypass.load(std::memory_order_relaxed);
            const uint32_t gen = box.immediateGeneration.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (box.sequence.load(std::memory_order_relaxed) == seq1)
            {
                filter.setBypass(bypass || bypassAll);
                const bool immediate = gen != appliedImmediateGeneration[si];
                appliedImmediateGeneration[si] = gen;
                filter.setParameters(f, g, q, immediate);
            }
        }
        else
        {
            filter.setBypass(filter.isBypassed() || bypassAll); // keep last consistent targets
        }

        filter.process(channels, numChannels, numSamples);
        sharedCurrentGainDb[si].store(filter.getCurrentGainDb(), std::memory_order_relaxed);
    }
}

void FilterManager::handleDetections(const std::vector<DetectedFeedback>& detections)
{
    if (currentMode == OperationMode::Bypass || detections.empty())
        return;

    std::lock_guard<std::mutex> lock(filterMutex);

    for (const auto& df : detections)
    {
        if (!std::isfinite(df.frequency) || !std::isfinite(df.suggestedCutDb)
            || df.frequency < 20.0f || df.frequency >= sampleRate * 0.49) continue;
        int matchIdx = findMatchingFilterIndex(df.frequency);

        if (matchIdx >= 0)
        {
            // Existing filter found near this frequency!
            auto& slot = slots[matchIdx];
            if (slot.isBypassed || slot.isLocked) continue;
            slot.activity = 1.0f;
            slot.triggerCount++;

            // If feedback is continuing, deepen notch cut (up to maxCutDepthDb)
            if (slot.deepenCooldownSec <= 0.0f)
            {
                slot.targetGainDb = juce::jmax(maxCutDepthDb, slot.targetGainDb - 3.0f);
                slot.deepenCooldownSec = 0.15f;
                if (!slot.isFixed) slot.frequency = df.frequency;
            }

            // Reset lift timer
            if (!slot.isFixed && !slot.isLocked)
            {
                slot.liftRemainingSec = liftDurationSec;
            }

            ctlSetParameters(matchIdx, slot.frequency, slot.targetGainDb, slot.q);
        }
        else
        {
            // New feedback point! Allocate appropriate slot
            int slotIdx = -1;
            bool asFixed = (currentMode == OperationMode::LearnSoundcheck);

            if (asFixed)
            {
                slotIdx = allocateFixedSlot();
            }
            else
            {
                slotIdx = allocateLiveSlot();
            }

            if (slotIdx >= 0)
            {
                auto& slot = slots[slotIdx];
                slot.id = nextId++;
                slot.isFixed = asFixed;
                slot.isLocked = false; // fixed frequency can deepen until explicitly locked
                slot.isBypassed = false;
                slot.frequency = df.frequency;
                slot.q = defaultQ;
                slot.targetGainDb = juce::jmax(maxCutDepthDb, -juce::jlimit(3.0f, 12.0f, df.suggestedCutDb));
                slot.deepenCooldownSec = 0.15f;
                slot.currentGainDb = 0.0f;
                slot.liftRemainingSec = liftDurationSec;
                slot.liftDurationSec = liftDurationSec;
                slot.triggerCount = 1;
                slot.activity = 1.0f;

                ctlSetBypass(slotIdx, false);
                ctlSetParameters(slotIdx, slot.frequency, slot.targetGainDb, slot.q);
            }
        }
    }
}

void FilterManager::updateTimers(float deltaTimeSec)
{
    std::lock_guard<std::mutex> lock(filterMutex);

    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        auto& slot = slots[i];
        if (slot.id == -1)
            continue;

        slot.deepenCooldownSec = juce::jmax(0.0f, slot.deepenCooldownSec - deltaTimeSec);

        // Activity fade
        slot.activity = juce::jmax(0.0f, slot.activity - deltaTimeSec * 1.5f);

        // Update current gain from DSP filter
        slot.currentGainDb = sharedCurrentGainDb[static_cast<size_t>(i)].load(std::memory_order_relaxed);

        // Check if dynamic lift is enabled for Live filter
        if (!slot.isFixed && !slot.isLocked && slot.liftDurationSec > 0.0f)
        {
            if (slot.liftRemainingSec > 0.0f)
            {
                slot.liftRemainingSec -= deltaTimeSec;
            }
            else
            {
                // Timer expired -> lift filter (ramp gain back to 0 dB)
                slot.targetGainDb += 4.0f * deltaTimeSec; // +4 dB / sec recovery
                if (slot.targetGainDb >= 0.0f)
                {
                    slot.targetGainDb = 0.0f;
                    ctlSetParameters(i, slot.frequency, 0.0f, slot.q);
                    if (std::abs(slot.currentGainDb) < 0.2f)
                    {
                        // Fully lifted: free the slot!
                        slot.id = -1;
                        ctlSetBypass(i, true); // V2.1: fade out at its own frequency instead of an instant jump
                    }
                }
                else
                {
                    ctlSetParameters(i, slot.frequency, slot.targetGainDb, slot.q);
                }
            }
        }
    }
}

int FilterManager::findMatchingFilterIndex(float freqHz, float tolerancePercent)
{
    float minDiff = 1e9f;
    int bestIdx = -1;

    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        if (slots[i].id != -1)
        {
            float percentDiff = std::abs(slots[i].frequency - freqHz) / freqHz * 100.0f;
            const float bandwidthTolerance = juce::jmax(2.0f, 0.5f * slots[i].frequency / slots[i].q);
            if (percentDiff <= tolerancePercent && std::abs(slots[i].frequency - freqHz) <= bandwidthTolerance
                && percentDiff < minDiff)
            {
                minDiff = percentDiff;
                bestIdx = i;
            }
        }
    }
    return bestIdx;
}

int FilterManager::allocateLiveSlot()
{
    // 1. Look for inactive Live slot
    for (int i = 0; i < MAX_LIVE_FILTERS; ++i)
    {
        if (slots[i].id == -1)
            return i;
    }

    // 2. All occupied: steal the one closest to lifting or with lowest cut
    int stealIdx = -1;
    float lowestLift = 1e9f;
    for (int i = 0; i < MAX_LIVE_FILTERS; ++i)
    {
        if (!slots[i].isLocked && slots[i].liftRemainingSec < lowestLift)
        {
            lowestLift = slots[i].liftRemainingSec;
            stealIdx = i;
        }
    }
    return stealIdx;
}

int FilterManager::allocateFixedSlot()
{
    // 1. Look for inactive Fixed slot
    for (int i = MAX_LIVE_FILTERS; i < TOTAL_FILTERS; ++i)
    {
        if (slots[i].id == -1)
            return i;
    }

    // Soundcheck filters are retained until explicitly cleared.
    return -1;
}

void FilterManager::setOperationMode(OperationMode mode)
{
    currentMode = mode;
}

void FilterManager::setDefaultQ(float q)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    float newQ = juce::jlimit(5.0f, 120.0f, q);
    if (std::abs(newQ - defaultQ) > 0.05f)
    {
        defaultQ = newQ;
        for (int i = 0; i < TOTAL_FILTERS; ++i)
        {
            if (slots[static_cast<size_t>(i)].id != -1 && !slots[static_cast<size_t>(i)].isLocked)
            {
                slots[static_cast<size_t>(i)].q = defaultQ;
                ctlSetParameters(i, slots[static_cast<size_t>(i)].frequency,
                                                                 slots[static_cast<size_t>(i)].targetGainDb,
                                                                 slots[static_cast<size_t>(i)].q);
            }
        }
    }
}

void FilterManager::setMaxCutDepth(float depthDb)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    float newDepth = juce::jlimit(-40.0f, -6.0f, depthDb);
    if (std::abs(newDepth - maxCutDepthDb) > 0.05f)
    {
        maxCutDepthDb = newDepth;
        for (int i = 0; i < TOTAL_FILTERS; ++i)
        {
            if (slots[static_cast<size_t>(i)].id != -1 && !slots[static_cast<size_t>(i)].isLocked)
            {
                if (slots[static_cast<size_t>(i)].targetGainDb < maxCutDepthDb)
                {
                    slots[static_cast<size_t>(i)].targetGainDb = maxCutDepthDb;
                    ctlSetParameters(i, slots[static_cast<size_t>(i)].frequency,
                                                                     slots[static_cast<size_t>(i)].targetGainDb,
                                                                     slots[static_cast<size_t>(i)].q);
                }
            }
        }
    }
}

void FilterManager::setLiftDuration(float seconds)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    if (liftDurationSec == seconds) return;
    liftDurationSec = seconds;
    for (auto& slot : slots)
        if (slot.id != -1 && !slot.isFixed && !slot.isLocked)
        {
            slot.liftDurationSec = seconds;
            slot.liftRemainingSec = seconds;
        }
}

void FilterManager::clearAllLive()
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < MAX_LIVE_FILTERS; ++i)
    {
        if (!slots[i].isLocked)
        {
            slots[i].id = -1;
            ctlSetBypass(i, true); // V2.1: fade out at its own frequency instead of an instant jump
        }
    }
}

void FilterManager::lockAllLiveToFixed()
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < MAX_LIVE_FILTERS; ++i)
    {
        if (slots[i].id != -1)
        {
            slots[i].isLocked = true;
            slots[i].isFixed = true;
        }
    }
}

void FilterManager::clearAll()
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        slots[i].id = -1;
        ctlSetBypass(i, true); // V2.1: fade out at its own frequency instead of an instant jump
    }
}

void FilterManager::setFilterLocked(int id, bool lock)
{
    std::lock_guard<std::mutex> lockGuard(filterMutex);
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        if (slots[i].id == id)
        {
            slots[i].isLocked = lock;
            if (lock)
                slots[i].isFixed = true;
            break;
        }
    }
}

void FilterManager::setFilterBypassed(int id, bool bypass)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        if (slots[i].id == id)
        {
            slots[i].isBypassed = bypass;
            ctlSetBypass(i, bypass);
            break;
        }
    }
}

void FilterManager::deleteFilter(int id)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        if (slots[i].id == id)
        {
            slots[i].id = -1;
            ctlSetBypass(i, true); // V2.1: fade out at its own frequency instead of an instant jump
            break;
        }
    }
}

void FilterManager::addManualFilter(float freqHz, float gainDb, float q, bool isFixed)
{
    addFilter(freqHz, gainDb, q, isFixed, isFixed, false);
}

int FilterManager::addFilter(float freqHz, float gainDb, float q, bool isFixed, bool locked, bool bypassed)
{
    if (! std::isfinite(freqHz) || ! std::isfinite(gainDb) || ! std::isfinite(q))
        return -1;
    std::lock_guard<std::mutex> lock(filterMutex);
    int slotIdx = isFixed ? allocateFixedSlot() : allocateLiveSlot();
    if (slotIdx < 0)
        return -1;

    auto& slot = slots[slotIdx];
    slot.id = nextId++;
    slot.isFixed = isFixed;
    slot.isLocked = locked;
    if (locked) slot.isFixed = true;
    slot.isBypassed = bypassed;
    slot.frequency = freqHz;
    slot.q = q;
    slot.targetGainDb = gainDb;
    slot.currentGainDb = gainDb;
    slot.liftRemainingSec = liftDurationSec;
    slot.liftDurationSec = liftDurationSec;
    slot.triggerCount = 1;
    slot.deepenCooldownSec = 0.15f;
    slot.activity = 1.0f;

    shadow[static_cast<size_t>(slotIdx)].bypass = bypassed;
    ctlSetParameters(slotIdx, freqHz, gainDb, q, true);
    sharedCurrentGainDb[static_cast<size_t>(slotIdx)].store(bypassed ? 0.0f : juce::jlimit(-60.0f, 0.0f, gainDb), std::memory_order_relaxed);
    return slot.id;
}

std::vector<FilterSlotInfo> FilterManager::getFilterSnapshots()
{
    std::vector<FilterSlotInfo> result;
    result.reserve(TOTAL_FILTERS);
    getFilterSnapshots(result);
    return result;
}

void FilterManager::getFilterSnapshots(std::vector<FilterSlotInfo>& result)
{
    result.clear();
    std::lock_guard<std::mutex> lock(filterMutex);

    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        if (slots[i].id != -1)
        {
            FilterSlotInfo info = slots[i];
            info.currentGainDb = sharedCurrentGainDb[static_cast<size_t>(i)].load(std::memory_order_relaxed);
            result.push_back(info);
        }
    }
}

float FilterManager::getCombinedMagnitude(float freqHz)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    float totalMag = 1.0f;

    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        if (slots[i].id != -1 && !slots[i].isBypassed)
        {
        {
            BiquadNotchFilter f; // UI-side model of the current response (no access to audio-thread state)
            f.prepare(sampleRate);
            f.setParameters(slots[i].frequency, sharedCurrentGainDb[static_cast<size_t>(i)].load(std::memory_order_relaxed), slots[i].q, true);
            totalMag *= f.getMagnitudeForFrequency(freqHz);
        }
        }
    }
    return totalMag;
}

} // namespace LiveAudioLab
