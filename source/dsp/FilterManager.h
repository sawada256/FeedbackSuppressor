#pragma once

#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include <memory>
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
    bool isReleasing = false;      // lift in progress (gain ramping back to 0 dB)
};

enum class OperationMode
{
    LiveAuto = 0,
    LearnSoundcheck,
    Bypass
};

/**
 * FilterManager coordinates up to 16 Live filters and 16 Fixed filters.
 *
 * Threading (V3): every control-side method (detections, timers, UI edits,
 * snapshots) runs on non-audio threads and is serialised by a mutex. The audio
 * thread never locks: it reads per-slot targets through a seqlock mailbox and
 * owns the biquad state. Removing or bypassing a notch ramps it to 0 dB
 * instead of dropping it from the cascade (no clicks).
 */
class FilterManager
{
public:
    static constexpr int MAX_LIVE_FILTERS = 16;
    static constexpr int MAX_FIXED_FILTERS = 16;
    static constexpr int TOTAL_FILTERS = MAX_LIVE_FILTERS + MAX_FIXED_FILTERS;
    static constexpr float MIN_BANDWIDTH_HZ = 4.0f; // automatic notches never get narrower than this

    FilterManager();
    ~FilterManager() = default;

    // Call while audio is stopped.
    void prepare(double newSampleRate);
    void reset();

    // Audio thread: lock-free, allocation-free.
    void processAudio(juce::AudioBuffer<float>& buffer) noexcept;

    // Control side (worker/UI threads).
    void handleDetections(const std::vector<DetectedFeedback>& detections);
    void updateTimers(float deltaTimeSec);

    void setOperationMode(OperationMode mode);
    OperationMode getOperationMode() const noexcept { return currentMode.load(std::memory_order_relaxed); }

    void setDefaultQ(float q);
    void setMaxCutDepth(float depthDb); // -6 to -40 dB
    void setLiftDuration(float seconds); // <= 0 for Hold

    void clearAllLive();
    void lockAllLiveToFixed();
    void clearAll();
    void setFilterLocked(int id, bool lock);
    void setFilterBypassed(int id, bool bypass);
    void deleteFilter(int id);
    void addManualFilter(float freqHz, float gainDb, float q, bool isFixed);
    // Returns the new filter id or -1 when no slot is available.
    int addFilter(float freqHz, float gainDb, float q, bool isFixed, bool locked, bool bypassed);

    std::vector<FilterSlotInfo> getFilterSnapshots();
    void getFilterSnapshots(std::vector<FilterSlotInfo>& dest); // reuses dest capacity

    // Combined magnitude (linear) at a frequency, from the current notch gains.
    float getCombinedMagnitude(float freqHz);

    double getSampleRate() const noexcept { return sampleRate; }

    // Q actually used for an automatic notch at freqHz (bandwidth floor at low frequencies).
    static float effectiveQ(float defaultQ, float freqHz) noexcept;

private:
    struct Mailbox
    {
        std::atomic<uint32_t> sequence { 0 };
        std::atomic<float> freq { 1000.0f };
        std::atomic<float> gainDb { 0.0f };
        std::atomic<float> q { 35.0f };
        std::atomic<bool> enabled { false };
    };
    struct RecentNotch
    {
        float frequency = 0.0f;
        float depthDb = 0.0f;
        float ageSec = 1.0e9f;
    };

    int findMatchingFilterIndex(float freqHz, float tolerancePercent = 2.0f) const;
    int allocateLiveSlot() const;
    int allocateFixedSlot() const;
    void publish(int index) noexcept;
    void freeSlot(int index) noexcept;
    void rememberNotch(const FilterSlotInfo& slot);
    float recalledDepth(float freqHz) const;

    double sampleRate = 44100.0;
    std::atomic<OperationMode> currentMode { OperationMode::LiveAuto };
    float defaultQ = 35.0f;
    float maxCutDepthDb = -20.0f;
    float liftDurationSec = 30.0f;

    std::mutex filterMutex;
    std::array<FilterSlotInfo, TOTAL_FILTERS> slots;
    std::array<Mailbox, TOTAL_FILTERS> mailbox;
    std::array<std::atomic<float>, TOTAL_FILTERS> sharedCurrentGainDb {};
    std::array<RecentNotch, 8> recentNotches {};

    // Audio-thread owned
    std::array<BiquadNotchFilter, TOTAL_FILTERS> dspFilters;
    std::array<float, TOTAL_FILTERS> lastFreq {}, lastGain {}, lastQ {};
    std::array<bool, TOTAL_FILTERS> lastEnabled {};

    int nextId = 1;
};

} // namespace LiveAudioLab
