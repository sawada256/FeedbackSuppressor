#include "FilterManager.h"
#include <algorithm>
#include <cmath>

namespace LiveAudioLab
{

FilterManager::FilterManager()
{
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        slots[static_cast<size_t>(i)].id = -1; // inactive
        slots[static_cast<size_t>(i)].isFixed = (i >= MAX_LIVE_FILTERS);
        sharedCurrentGainDb[static_cast<size_t>(i)].store(0.0f, std::memory_order_relaxed);
    }
}

float FilterManager::effectiveQ(float q, float freqHz) noexcept
{
    // Constant-Q notches become extremely narrow at low frequencies (Q 35 at
    // 100 Hz is 2.9 Hz wide), smaller than the detector's frequency error and
    // the drift of a room mode. Commercial units widen low notches (e.g. dbx
    // AFS2 uses a fixed 8 Hz width below ~930 Hz in its music setting).
    return juce::jlimit(1.0f, 150.0f, juce::jmin(q, freqHz / MIN_BANDWIDTH_HZ));
}

void FilterManager::prepare(double newSampleRate)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        const auto si = static_cast<size_t>(i);
        dspFilters[si].prepare(sampleRate);
        const auto& slot = slots[si];
        const bool active = slot.id != -1 && ! slot.isBypassed;
        dspFilters[si].setTarget(slot.frequency, active ? slot.targetGainDb : 0.0f, slot.q);
        dspFilters[si].snapToTarget();
        lastEnabled[si] = active;
        publish(i);
    }
}

void FilterManager::reset()
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (auto& f : dspFilters)
        f.reset();
}

void FilterManager::publish(int index) noexcept
{
    // Single writer (callers hold filterMutex). Seqlock: odd = write in progress.
    auto& box = mailbox[static_cast<size_t>(index)];
    const auto& slot = slots[static_cast<size_t>(index)];
    const uint32_t seq = box.sequence.load(std::memory_order_relaxed);
    box.sequence.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    box.freq.store(slot.frequency, std::memory_order_relaxed);
    box.gainDb.store(slot.targetGainDb, std::memory_order_relaxed);
    box.q.store(slot.q, std::memory_order_relaxed);
    box.enabled.store(slot.id != -1 && ! slot.isBypassed, std::memory_order_relaxed);
    box.sequence.store(seq + 2, std::memory_order_release);
}

void FilterManager::processAudio(juce::AudioBuffer<float>& buffer) noexcept
{
    const bool bypassAll = currentMode.load(std::memory_order_relaxed) == OperationMode::Bypass;
    const int numChannels = juce::jmin(2, buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return;
    auto* const* channels = buffer.getArrayOfWritePointers();

    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        const auto si = static_cast<size_t>(i);
        auto& box = mailbox[si];
        const uint32_t seq1 = box.sequence.load(std::memory_order_acquire);
        if ((seq1 & 1u) == 0u)
        {
            const float f = box.freq.load(std::memory_order_relaxed);
            const float g = box.gainDb.load(std::memory_order_relaxed);
            const float q = box.q.load(std::memory_order_relaxed);
            const bool en = box.enabled.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (box.sequence.load(std::memory_order_relaxed) == seq1)
            {
                lastFreq[si] = f; lastGain[si] = g; lastQ[si] = q; lastEnabled[si] = en;
            }
        }
        // Otherwise keep the previous consistent set for this block.

        const bool active = lastEnabled[si] && ! bypassAll;
        auto& filter = dspFilters[si];
        filter.setTarget(lastFreq[si], active ? lastGain[si] : 0.0f, lastQ[si]);
        filter.process(channels, numChannels, numSamples);
        sharedCurrentGainDb[si].store(filter.getCurrentGainDb(), std::memory_order_relaxed);
    }
}

void FilterManager::handleDetections(const std::vector<DetectedFeedback>& detections)
{
    if (currentMode.load() == OperationMode::Bypass || detections.empty())
        return;

    std::lock_guard<std::mutex> lock(filterMutex);

    for (const auto& df : detections)
    {
        if (! std::isfinite(df.frequency) || ! std::isfinite(df.suggestedCutDb)
            || df.frequency < 20.0f || df.frequency >= sampleRate * 0.49)
            continue;
        const int matchIdx = findMatchingFilterIndex(df.frequency);

        if (matchIdx >= 0)
        {
            auto& slot = slots[static_cast<size_t>(matchIdx)];
            if (slot.isBypassed || slot.isLocked) continue;
            slot.activity = 1.0f;
            slot.triggerCount++;

            // Residual feedback after the notch: deepen in 3 dB steps, rate limited.
            if (slot.deepenCooldownSec <= 0.0f)
            {
                const float base = slot.isReleasing ? juce::jmin(slot.targetGainDb, -3.0f) : slot.targetGainDb;
                slot.targetGainDb = juce::jmax(maxCutDepthDb, base - 3.0f);
                slot.deepenCooldownSec = 0.15f;
                if (! slot.isFixed)
                {
                    // Track slow drift of the mode, but smoothly (half-way step).
                    slot.frequency += 0.5f * (df.frequency - slot.frequency);
                    slot.q = effectiveQ(defaultQ, slot.frequency);
                }
            }
            slot.isReleasing = false;
            if (! slot.isFixed)
                slot.liftRemainingSec = slot.liftDurationSec;

            publish(matchIdx);
        }
        else
        {
            const bool asFixed = (currentMode.load() == OperationMode::LearnSoundcheck);
            const int slotIdx = asFixed ? allocateFixedSlot() : allocateLiveSlot();
            if (slotIdx < 0) continue;

            auto& slot = slots[static_cast<size_t>(slotIdx)];
            if (slot.id != -1 && ! slot.isFixed)
                rememberNotch(slot); // stolen
            const float recalled = recalledDepth(df.frequency);
            float cut = juce::jlimit(3.0f, 12.0f, df.suggestedCutDb);
            if (recalled > 0.0f) cut = juce::jmax(cut, recalled - 3.0f); // known trouble frequency: restart deeper

            slot.id = nextId++;
            slot.isFixed = asFixed;
            slot.isLocked = false; // soundcheck filters can deepen until explicitly locked
            slot.isBypassed = false;
            slot.isReleasing = false;
            slot.frequency = df.frequency;
            slot.q = effectiveQ(defaultQ, df.frequency);
            slot.targetGainDb = juce::jmax(maxCutDepthDb, -cut);
            slot.deepenCooldownSec = 0.15f;
            slot.currentGainDb = 0.0f;
            slot.liftDurationSec = liftDurationSec;
            slot.liftRemainingSec = liftDurationSec;
            slot.triggerCount = 1;
            slot.activity = 1.0f;
            publish(slotIdx);
        }
    }
}

void FilterManager::updateTimers(float deltaTimeSec)
{
    if (! (deltaTimeSec > 0.0f)) return;
    std::lock_guard<std::mutex> lock(filterMutex);

    for (auto& r : recentNotches)
        r.ageSec += deltaTimeSec;

    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        auto& slot = slots[static_cast<size_t>(i)];
        if (slot.id == -1)
            continue;

        slot.deepenCooldownSec = juce::jmax(0.0f, slot.deepenCooldownSec - deltaTimeSec);
        slot.activity = juce::jmax(0.0f, slot.activity - deltaTimeSec * 1.5f);
        slot.currentGainDb = sharedCurrentGainDb[static_cast<size_t>(i)].load(std::memory_order_relaxed);

        if (slot.isFixed || slot.isLocked || slot.liftDurationSec <= 0.0f)
            continue;

        if (slot.liftRemainingSec > 0.0f)
        {
            slot.liftRemainingSec -= deltaTimeSec;
            continue;
        }

        // Timer expired: release the notch gradually (+4 dB/s). If feedback
        // returns during the release the detector re-deepens it.
        if (! slot.isReleasing)
            rememberNotch(slot);
        slot.isReleasing = true;
        slot.targetGainDb = juce::jmin(0.0f, slot.targetGainDb + 4.0f * deltaTimeSec);
        if (slot.targetGainDb >= 0.0f && std::abs(slot.currentGainDb) < 0.2f)
            freeSlot(i);
        else
            publish(i);
    }
}

void FilterManager::rememberNotch(const FilterSlotInfo& slot)
{
    // Keep the deepest cut seen per frequency for 2 minutes.
    const float depth = -juce::jmin(slot.targetGainDb, slot.currentGainDb);
    if (depth < 3.0f) return;
    RecentNotch* target = nullptr;
    for (auto& r : recentNotches)
        if (r.ageSec < 120.0f && std::abs(r.frequency - slot.frequency) <= juce::jmax(3.0f, 0.01f * slot.frequency))
            target = &r;
    if (target == nullptr)
        target = &*std::max_element(recentNotches.begin(), recentNotches.end(),
                                    [](const RecentNotch& a, const RecentNotch& b) { return a.ageSec < b.ageSec; });
    target->frequency = slot.frequency;
    target->depthDb = juce::jmax(depth, target->ageSec < 120.0f ? target->depthDb : 0.0f);
    target->ageSec = 0.0f;
}

float FilterManager::recalledDepth(float freqHz) const
{
    for (const auto& r : recentNotches)
        if (r.ageSec < 120.0f && std::abs(r.frequency - freqHz) <= juce::jmax(3.0f, 0.01f * freqHz))
            return r.depthDb;
    return 0.0f;
}

void FilterManager::freeSlot(int index) noexcept
{
    auto& slot = slots[static_cast<size_t>(index)];
    slot.id = -1;
    slot.isLocked = false;
    slot.isBypassed = false;
    slot.isReleasing = false;
    slot.isFixed = index >= MAX_LIVE_FILTERS;
    slot.targetGainDb = 0.0f;
    publish(index); // audio thread ramps the gain to 0 dB, then idles
}

int FilterManager::findMatchingFilterIndex(float freqHz, float tolerancePercent) const
{
    float minDiff = 1e9f;
    int bestIdx = -1;

    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        const auto& slot = slots[static_cast<size_t>(i)];
        if (slot.id == -1) continue;
        const float absDiff = std::abs(slot.frequency - freqHz);
        const float percentDiff = absDiff / freqHz * 100.0f;
        const float bandwidthTolerance = juce::jmax(2.0f, 0.5f * slot.frequency / slot.q);
        if (percentDiff <= tolerancePercent && absDiff <= bandwidthTolerance && percentDiff < minDiff)
        {
            minDiff = percentDiff;
            bestIdx = i;
        }
    }
    return bestIdx;
}

int FilterManager::allocateLiveSlot() const
{
    // 1. Prefer a free slot whose biquad has already faded out.
    int freeButFading = -1;
    for (int i = 0; i < MAX_LIVE_FILTERS; ++i)
    {
        if (slots[static_cast<size_t>(i)].id != -1) continue;
        if (std::abs(sharedCurrentGainDb[static_cast<size_t>(i)].load(std::memory_order_relaxed)) < 0.05f)
            return i;
        if (freeButFading < 0) freeButFading = i;
    }
    if (freeButFading >= 0) return freeButFading;

    // 2. All occupied: steal the unlocked live notch that matters least —
    // one already releasing, otherwise the shallowest, then the oldest timer.
    int stealIdx = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < MAX_LIVE_FILTERS; ++i)
    {
        const auto& slot = slots[static_cast<size_t>(i)];
        if (slot.isLocked || slot.isFixed) continue;
        const float score = (slot.isReleasing ? -100.0f : 0.0f) - slot.targetGainDb * 10.0f
                          + juce::jmax(0.0f, slot.liftRemainingSec) * 0.01f;
        if (score < bestScore) { bestScore = score; stealIdx = i; }
    }
    return stealIdx;
}

int FilterManager::allocateFixedSlot() const
{
    for (int i = MAX_LIVE_FILTERS; i < TOTAL_FILTERS; ++i)
        if (slots[static_cast<size_t>(i)].id == -1)
            return i;
    return -1; // Soundcheck filters are retained until explicitly cleared.
}

void FilterManager::setOperationMode(OperationMode mode)
{
    currentMode.store(mode);
}

void FilterManager::setDefaultQ(float q)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    const float newQ = juce::jlimit(5.0f, 120.0f, q);
    if (std::abs(newQ - defaultQ) <= 0.05f) return;
    defaultQ = newQ;
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        auto& slot = slots[static_cast<size_t>(i)];
        if (slot.id != -1 && ! slot.isLocked)
        {
            slot.q = effectiveQ(defaultQ, slot.frequency);
            publish(i);
        }
    }
}

void FilterManager::setMaxCutDepth(float depthDb)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    const float newDepth = juce::jlimit(-40.0f, -6.0f, depthDb);
    if (std::abs(newDepth - maxCutDepthDb) <= 0.05f) return;
    maxCutDepthDb = newDepth;
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        auto& slot = slots[static_cast<size_t>(i)];
        if (slot.id != -1 && ! slot.isLocked && slot.targetGainDb < maxCutDepthDb)
        {
            slot.targetGainDb = maxCutDepthDb;
            publish(i);
        }
    }
}

void FilterManager::setLiftDuration(float seconds)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    if (liftDurationSec == seconds) return;
    liftDurationSec = seconds;
    for (auto& slot : slots)
        if (slot.id != -1 && ! slot.isFixed && ! slot.isLocked)
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
        const auto& slot = slots[static_cast<size_t>(i)];
        if (slot.id != -1 && ! slot.isLocked && ! slot.isFixed)
            freeSlot(i);
    }
}

void FilterManager::lockAllLiveToFixed()
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < MAX_LIVE_FILTERS; ++i)
    {
        auto& slot = slots[static_cast<size_t>(i)];
        if (slot.id != -1)
        {
            slot.isLocked = true;
            slot.isFixed = true;
            if (slot.isReleasing)
            {
                // Freeze at the depth reached, not mid-release.
                slot.isReleasing = false;
                slot.targetGainDb = juce::jmin(slot.targetGainDb, slot.currentGainDb);
            }
            publish(i);
        }
    }
}

void FilterManager::clearAll()
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < TOTAL_FILTERS; ++i)
        if (slots[static_cast<size_t>(i)].id != -1)
            freeSlot(i);
}

void FilterManager::setFilterLocked(int id, bool lockState)
{
    std::lock_guard<std::mutex> lockGuard(filterMutex);
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        auto& slot = slots[static_cast<size_t>(i)];
        if (slot.id == id)
        {
            slot.isLocked = lockState;
            if (lockState)
            {
                slot.isFixed = true;
                slot.isReleasing = false;
            }
            publish(i);
            break;
        }
    }
}

void FilterManager::setFilterBypassed(int id, bool bypass)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        if (slots[static_cast<size_t>(i)].id == id)
        {
            slots[static_cast<size_t>(i)].isBypassed = bypass;
            publish(i);
            break;
        }
    }
}

void FilterManager::deleteFilter(int id)
{
    std::lock_guard<std::mutex> lock(filterMutex);
    for (int i = 0; i < TOTAL_FILTERS; ++i)
    {
        if (slots[static_cast<size_t>(i)].id == id)
        {
            freeSlot(i);
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
    const int slotIdx = isFixed ? allocateFixedSlot() : allocateLiveSlot();
    if (slotIdx < 0)
        return -1;

    auto& slot = slots[static_cast<size_t>(slotIdx)];
    slot.id = nextId++;
    slot.isFixed = isFixed || locked;
    slot.isLocked = locked;
    slot.isBypassed = bypassed;
    slot.isReleasing = false;
    slot.frequency = juce::jlimit(20.0f, static_cast<float>(sampleRate * 0.49), freqHz);
    slot.q = juce::jlimit(1.0f, 150.0f, q);
    slot.targetGainDb = juce::jlimit(-60.0f, 0.0f, gainDb);
    slot.currentGainDb = 0.0f;
    slot.liftRemainingSec = liftDurationSec;
    slot.liftDurationSec = liftDurationSec;
    slot.triggerCount = 1;
    slot.deepenCooldownSec = 0.15f;
    slot.activity = 1.0f;
    publish(slotIdx);
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
        const auto& slot = slots[static_cast<size_t>(i)];
        if (slot.id != -1)
        {
            FilterSlotInfo info = slot;
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
        const auto& slot = slots[static_cast<size_t>(i)];
        const float g = sharedCurrentGainDb[static_cast<size_t>(i)].load(std::memory_order_relaxed);
        if (std::abs(g) > 0.01f)
            totalMag *= BiquadNotchFilter::magnitudeAt(sampleRate, slot.frequency, g, slot.q, freqHz);
    }
    return totalMag;
}

} // namespace LiveAudioLab
