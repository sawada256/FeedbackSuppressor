#pragma once

#include <vector>
#include <array>
#include <mutex>
#include <memory>
#include <atomic>
#include "BiquadNotchFilter.h"
#include "FeedbackDetector.h"

namespace LiveAudioLab
{

struct FilterSlotInfo
{
    int id = -1;
    bool isFixed = false;
    bool isLocked = false;
    bool isBypassed = false;
    float frequency = 1000.0f;
    float targetGainDb = 0.0f;
    float currentGainDb = 0.0f;
    float q = 35.0f;
    float liftRemainingSec = 0.0f;
    float liftDurationSec = 30.0f; // <= 0 means Hold / Infinite
    int triggerCount = 0;
    float deepenCooldownSec = 0.0f; // rate-limit residual-feedback corrections
    float activity = 0.0f;         // 0.0 to 1.0 for UI glow
};

enum class OperationMode
{
    LiveAuto = 0,
    LearnSoundcheck,
    Bypass
};

/**
 * FilterManager coordinates up to 16 Live filters and 16 Fixed filters.
 * Manages allocation, recurrence tracking, dynamic filter lift, and user lock/edits.
 */
class FilterManager
{
public:
    static constexpr int MAX_LIVE_FILTERS = 16;
    static constexpr int MAX_FIXED_FILTERS = 16;
    static constexpr int TOTAL_FILTERS = MAX_LIVE_FILTERS + MAX_FIXED_FILTERS;

    FilterManager();
    ~FilterManager() = default;

    void prepare(double newSampleRate);
    void reset();

    // Process audio buffer through active biquad cascade.
    // Audio thread: lock-free and allocation-free (V2.1).
    void processAudio(juce::AudioBuffer<float>& buffer) noexcept;

    // Apply newly detected feedback instances
    void handleDetections(const std::vector<DetectedFeedback>& detections);

    // Time update for dynamic lift timers & activity decay (call regularly with deltaTimeSec)
    void updateTimers(float deltaTimeSec);

    // Operational parameters
    void setOperationMode(OperationMode mode);
    OperationMode getOperationMode() const noexcept { return currentMode; }

    void setDefaultQ(float q);
    void setMaxCutDepth(float depthDb); // -6 to -40 dB
    void setLiftDuration(float seconds); // <= 0 for Hold

    // User management actions
    void clearAllLive();
    void lockAllLiveToFixed();
    void clearAll();
    void setFilterLocked(int id, bool lock);
    void setFilterBypassed(int id, bool bypass);
    void deleteFilter(int id);
    void addManualFilter(float freqHz, float gainDb, float q, bool isFixed);
    // V2.1: returns the new filter id (or -1) and sets lock/bypass atomically.
    int addFilter(float freqHz, float gainDb, float q, bool isFixed, bool locked, bool bypassed);

    // Get list snapshot for UI table & spectrum overlay
    std::vector<FilterSlotInfo> getFilterSnapshots();
    void getFilterSnapshots(std::vector<FilterSlotInfo>& dest); // reuses dest capacity

    // Calculate total combined magnitude response at frequency for UI curve
    float getCombinedMagnitude(float freqHz);

private:
    // Control side -> audio thread mailbox (seqlock, single writer under filterMutex).
    struct Mailbox
    {
        std::atomic<uint32_t> sequence { 0 };
        std::atomic<float> freq { 1000.0f };
        std::atomic<float> gainDb { 0.0f };
        std::atomic<float> q { 35.0f };
        std::atomic<bool> bypass { true };
        std::atomic<uint32_t> immediateGeneration { 0 };
    };
    struct Shadow { float freq = 1000.0f, gainDb = 0.0f, q = 35.0f; bool bypass = true; uint32_t immediateGeneration = 0; };
    void ctlSetParameters(int index, float freqHz, float gainDb, float q, bool immediate = false) noexcept;
    void ctlSetBypass(int index, bool bypass) noexcept;
    void publish(int index) noexcept;

    int findMatchingFilterIndex(float freqHz, float tolerancePercent = 2.0f);
    int allocateLiveSlot();
    int allocateFixedSlot();

    double sampleRate = 44100.0;
    std::atomic<OperationMode> currentMode { OperationMode::LiveAuto };
    float defaultQ = 35.0f;
    float maxCutDepthDb = -20.0f;
    float liftDurationSec = 30.0f;

    std::mutex filterMutex;
    std::array<FilterSlotInfo, TOTAL_FILTERS> slots;
    std::array<Shadow, TOTAL_FILTERS> shadow;
    std::array<Mailbox, TOTAL_FILTERS> mailbox;
    std::array<std::atomic<float>, TOTAL_FILTERS> sharedCurrentGainDb {};

    // Audio-thread owned
    std::array<BiquadNotchFilter, TOTAL_FILTERS> dspFilters;
    std::array<uint32_t, TOTAL_FILTERS> appliedImmediateGeneration {};

    int nextId = 1;
};

} // namespace LiveAudioLab
