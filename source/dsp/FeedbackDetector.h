#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>

namespace LiveAudioLab
{

struct DetectedFeedback
{
    float frequency = 0.0f;
    float peakDb = 0.0f;
    float prominenceDb = 0.0f;
    float growthRate = 0.0f;     // dB / ms
    float suggestedCutDb = 6.0f; // positive attenuation requested from residual spectrum
    float confidence = 0.0f;     // 0.0 to 1.0
};

enum class DetectionProfile
{
    Speech = 0,
    Music,
    Flat1OverF,
    HighFocus,
    LowFocus
};

enum class DetectionMode
{
    Standard = 0,   // "Classic": legacy absolute-level spectral detector (V1/V2 behaviour)
    Advanced,       // Music-aware: relative spectrum + growth + stability + harmonic protection
    Aggressive      // Same features, shorter confirmation and lower thresholds
};

/**
 * Worker-thread STFT feedback detector fed by a lock-free SPSC FIFO.
 *
 * V3 music-aware path (Advanced / Aggressive), per spectral peak:
 *  - PNPR-like prominence over a trimmed neighbourhood (linear power)
 *  - phase-vocoder instantaneous frequency + phase-locking value (PLV)
 *  - frequency-stability track: the spread of the phase-vocoder frequency over
 *    the whole track must stay within ~0.1 % (vibrato / pitch drift of voices
 *    and instruments fails this, a feedback mode does not)
 *  - IMSD-like growth: least-squares slope (dB/s) and fit over 8 and 16 frames
 *  - temporal inverse sparsity (NINOS2-T inspired) for steady candidates
 *  - PHPR-like harmonic-family protection
 * Detection paths: fast growth, slow growth, steady (long hold).
 *
 * Analysis always runs at 44.1/48 kHz: higher host rates are decimated by an
 * integer factor before the FIFO, keeping bin width (~23 Hz) and CPU constant.
 * prepare/reset require producer and consumer to be stopped.
 */
class FeedbackDetector
{
public:
    static constexpr int FFT_ORDER = 11; // 2048 points
    static constexpr int FFT_SIZE = 1 << FFT_ORDER;
    static constexpr int HOP_SIZE = 512; // 75% overlap (~10.7 ms at 48 kHz)
    static constexpr int NUM_BINS = FFT_SIZE / 2;

    FeedbackDetector();
    ~FeedbackDetector() = default;

    void prepare(double newSampleRate);
    void reset();

    // Audio thread: lock-free, allocation-free.
    void pushSamples(const float* const* channelData, int numChannels, int numSamples) noexcept;

    // Worker thread: analyse all complete hops. Returns true if new events were appended.
    bool processNextFrames(std::vector<DetectedFeedback>& detectedList);

    // Parameter controls (any thread)
    void setSensitivity(float sensitivityDb); // -60 to +20 dB offset
    void setGrowthSensitivity(int level);     // 0=Low, 1=Med, 2=High
    void setProfile(DetectionProfile profile);
    void setDetectionMode(DetectionMode mode);
    DetectionMode getDetectionMode() const noexcept { return currentDetectionMode; }
    void setMinMaxFrequency(float minHz, float maxHz);

    // Spectrum data for UI visualization (thread-safe copy)
    void copySpectrumTo(std::vector<float>& destMags, std::vector<float>& destBaseline);

    // Analysis sample rate (host rate divided by the decimation factor).
    double getSampleRate() const noexcept { return sampleRate; }
    int getDecimationFactor() const noexcept { return decimation; }
    unsigned int getDroppedBlockCount() const noexcept { return droppedBlocks.load(std::memory_order_relaxed); }

private:
    void analyzeFrame(std::vector<DetectedFeedback>& detectedList);
    void calculateBaselineEnvelope();
    void calculateMusicalBaselineForUi();
    float baselineAt(int k) noexcept;
    void analyzeMusicalFrame(std::vector<DetectedFeedback>& detectedList);
    void clearAnalysisHistory();
    float getReferenceCurveDb(float freqHz) const;
    void regression(int k, int frames, float& slopeDbPerFrame, float& fit) const noexcept;
    float backgroundGrowth(int k, int frames) const noexcept; // dB/frame slope of everything except bins k-3..k+3
    float temporalSparsity(int k) const noexcept;

    double hostSampleRate = 44100.0;
    double sampleRate = 44100.0; // analysis rate
    int decimation = 1;
    std::atomic<float> sensitivityOffsetDb { 0.0f };
    std::atomic<int> growthSensitivityLevel { 1 }; // 1 = Medium
    std::atomic<DetectionProfile> currentProfile { DetectionProfile::Speech };
    std::atomic<DetectionMode> currentDetectionMode { DetectionMode::Standard }; // plugin default comes from the parameter (Advanced)
    std::atomic<float> minDetectionHz { 150.0f };
    std::atomic<float> maxDetectionHz { 16000.0f };

    // FIFO ring buffer for incoming (decimated) samples
    static constexpr int FIFO_SIZE = 16384;
    std::array<std::vector<float>, 2> fifoBuffer;
    std::atomic<int> fifoWritePos { 0 };
    std::atomic<int> fifoReadPos { 0 };
    std::atomic<unsigned int> droppedBlocks { 0 };
    unsigned int lastDroppedBlocks = 0;
    // decimator state (audio thread)
    float decimAccum[2] = { 0.0f, 0.0f };
    int decimCount = 0;

    // FFT & windowing
    juce::dsp::FFT fft { FFT_ORDER };
    juce::dsp::WindowingFunction<float> window { FFT_SIZE, juce::dsp::WindowingFunction<float>::hann };

    std::array<float, FFT_SIZE * 2> fftBuffer;
    std::array<float, FFT_SIZE * 2> rightFftBuffer;
    std::array<float, NUM_BINS> currentMagnitudesDb;
    std::array<float, NUM_BINS> averagedPower {};
    std::array<float, NUM_BINS> rawMagnitudesDb {};
    std::array<float, NUM_BINS> baselineEnvelopeDb;
    std::array<uint32_t, NUM_BINS> baselineStamp {};
    uint32_t frameCounter = 1;
    bool historyCleared = false;

    // Magnitude history (raw, unsmoothed dB) for growth and temporal sparsity
    static constexpr int HISTORY_FRAMES = 16;
    std::array<std::array<float, NUM_BINS>, HISTORY_FRAMES> historyMagnitudesDb;
    std::array<std::array<float, NUM_BINS>, HISTORY_FRAMES> historySmoothedDb; // growth regression (less leakage noise)
    std::array<double, HISTORY_FRAMES> historyTotalPower {}; // broadband (detection range) smoothed power per frame
    double currentTotalPower = 0.0;
    float currentMaxPeakDb = -120.0f;
    int historyIndex = 0;
    int validHistoryFrames = 0;
    DetectionMode analyzedMode = DetectionMode::Standard;
    DetectionProfile analyzedProfile = DetectionProfile::Speech;

    // Per-bin track state (music-aware path). "prev" copies are swapped each frame.
    struct TrackState
    {
        std::array<float, NUM_BINS> durationMs {};
        std::array<float, NUM_BINS> anchorHz {};
        std::array<float, NUM_BINS> minHz {};       // phase-vocoder frequency extremes over the track
        std::array<float, NUM_BINS> maxHz {};
        std::array<float, NUM_BINS> strongPhaseMs {};
        std::array<float, NUM_BINS> growFrames {};  // consecutive fast-growth frames
        std::array<float, NUM_BINS> minDb {};       // lowest smoothed level since the track started
        std::array<float, NUM_BINS> ghostFrames {}; // frames the track survived without a matching peak
    };
    TrackState track, prevTrack;
    std::array<float, NUM_BINS> cooldownMs {};

    // Legacy (Standard) persistence counter
    std::array<int, NUM_BINS> candidatePersistence;

    // Per-channel phase-vocoder state. Phase increments are stored as cos/sin so
    // the PLV over the window is a sum, not 2 x window trig calls per bin.
    static constexpr int PHASE_FRAMES = 8; // ~85 ms at 48 kHz
    std::array<std::array<float, NUM_BINS>, 2> prevPhaseRad;
    std::array<std::array<std::array<float, NUM_BINS>, PHASE_FRAMES>, 2> phaseCosHistory;
    std::array<std::array<std::array<float, NUM_BINS>, PHASE_FRAMES>, 2> phaseSinHistory;
    std::array<std::array<int, NUM_BINS>, 2> validPhaseFrames {};
    std::array<std::array<bool, NUM_BINS>, 2> hasValidPhase;
    std::array<float, NUM_BINS> phaseFrequency {};
    std::array<float, NUM_BINS> phaseCoherence; // 0.0 to 1.0
    int phaseHistIndex = 0;

    // Thread-safe UI spectrum buffer
    std::mutex spectrumMutex;
    std::vector<float> uiSpectrum;
    std::vector<float> uiBaseline;
};

} // namespace LiveAudioLab
