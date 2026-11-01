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
    Standard = 0,   // Legacy conservative spectral detector
    Advanced,       // Relative spectrum + temporal persistence + harmonic protection
    Aggressive      // Deep squeal hunter: high sensitivity in complex music mix
};

/**
 * Worker-thread STFT analysis of a stereo SPSC FIFO. Standard retains the legacy
 * spectral gates; musical modes combine local contrast, normalized temporal
 * inverse sparsity, stable frequency, per-channel phase and harmonic protection.
 * prepare/reset require producer and consumer to be stopped.
 */
class FeedbackDetector
{
public:
    static constexpr int FFT_ORDER = 11; // 2048 points
    static constexpr int FFT_SIZE = 1 << FFT_ORDER;
    static constexpr int HOP_SIZE = 512; // 75% overlap (~11.6ms at 44.1kHz)

    FeedbackDetector();
    ~FeedbackDetector() = default;

    void prepare(double newSampleRate);
    void reset();

    // Push samples from audio thread into ring buffer (lock-free)
    void pushSamples(const float* const* channelData, int numChannels, int numSamples);

    // Run analysis if enough samples are available in the ring buffer.
    // Returns true if new feedback events were detected.
    bool processNextFrames(std::vector<DetectedFeedback>& detectedList);

    // Parameter controls
    void setSensitivity(float sensitivityDb); // -60 to +20 dB offset
    void setGrowthSensitivity(int level);     // 0=Low, 1=Med, 2=High
    void setProfile(DetectionProfile profile);
    void setDetectionMode(DetectionMode mode);
    DetectionMode getDetectionMode() const noexcept { return currentDetectionMode; }
    void setMinMaxFrequency(float minHz, float maxHz);

    // Spectrum data for UI visualization (thread-safe copy)
    void copySpectrumTo(std::vector<float>& destMags, std::vector<float>& destBaseline);

    double getSampleRate() const noexcept { return sampleRate; }

private:
    void analyzeFrame(std::vector<DetectedFeedback>& detectedList);
    void calculateBaselineEnvelope();
    float musicalBaselineAt(int k) noexcept; // lazily computed, cached per frame (V2.1)
    void analyzeMusicalFrame(std::vector<DetectedFeedback>& detectedList);
    void clearAnalysisHistory();
    float getReferenceCurveDb(float freqHz) const;

    double sampleRate = 44100.0;
    std::atomic<float> sensitivityOffsetDb { 0.0f };
    std::atomic<int> growthSensitivityLevel { 1 }; // 1 = Medium
    std::atomic<DetectionProfile> currentProfile { DetectionProfile::Speech };
    std::atomic<DetectionMode> currentDetectionMode { DetectionMode::Standard };
    std::atomic<float> minDetectionHz { 150.0f };
    std::atomic<float> maxDetectionHz { 16000.0f };

    // FIFO ring buffer for incoming samples
    static constexpr int FIFO_SIZE = 16384;
    std::array<std::vector<float>, 2> fifoBuffer;
    std::atomic<int> fifoWritePos { 0 };
    std::atomic<int> fifoReadPos { 0 };
    std::atomic<unsigned int> droppedBlocks { 0 };
    unsigned int lastDroppedBlocks = 0;

    // FFT & windowing
    juce::dsp::FFT fft { FFT_ORDER };
    juce::dsp::WindowingFunction<float> window { FFT_SIZE, juce::dsp::WindowingFunction<float>::hann };

    std::array<float, FFT_SIZE * 2> fftBuffer;
    std::array<float, FFT_SIZE * 2> rightFftBuffer;
    std::array<float, FFT_SIZE / 2> currentMagnitudesDb;
    std::array<float, FFT_SIZE / 2> averagedPower {};
    std::array<float, FFT_SIZE / 2> rawMagnitudesDb {};
    std::array<float, FFT_SIZE / 2> baselineEnvelopeDb;

    // Magnitude history for growth and normalized temporal inverse sparsity
    static constexpr int HISTORY_FRAMES = 16;
    std::array<std::array<float, FFT_SIZE / 2>, HISTORY_FRAMES> historyMagnitudesDb;
    int historyIndex = 0;
    int validHistoryFrames = 0;
    DetectionMode analyzedMode = DetectionMode::Standard;
    DetectionProfile analyzedProfile = DetectionProfile::Speech;
    std::array<float, FFT_SIZE / 2> trackedFrequency {};
    std::array<float, FFT_SIZE / 2> trackedDurationMs {};
    std::array<float, FFT_SIZE / 2> strongPhaseDurationMs {};
    std::array<float, FFT_SIZE / 2> eventCooldownMs {};

    // Persistence counter for candidates
    std::array<int, FFT_SIZE / 2> candidatePersistence;

    // Per-channel instantaneous phase difference and circular coherence (PLV)
    std::array<std::array<float, FFT_SIZE / 2>, 2> prevPhaseRad;
    std::array<float, FFT_SIZE / 2> phaseSumCos {}; // PLV sums of the selected channel
    std::array<float, FFT_SIZE / 2> phaseSumSin {};
    std::array<float, FFT_SIZE / 2> phaseCoherence; // 0.0 to 1.0
    static constexpr int PHASE_HIST_FRAMES = 32; // maximum; actual window is ~85 ms (8 frames at 48 kHz)
    // V2.1: cos/sin of each stored phase error instead of the error itself, so the
    // PLV sum over the window needs no trig calls (bit-identical sums).
    std::array<std::array<std::array<float, FFT_SIZE / 2>, PHASE_HIST_FRAMES>, 2> phaseCosHistory;
    std::array<std::array<std::array<float, FFT_SIZE / 2>, PHASE_HIST_FRAMES>, 2> phaseSinHistory;
    std::array<uint32_t, FFT_SIZE / 2> baselineStamp {};
    uint32_t frameCounter = 1;
    bool historyCleared = false;
    bool lastHopAnalysedMusical = false;
    int phaseHistIndex = 0;
    std::array<std::array<bool, FFT_SIZE / 2>, 2> hasValidPhase;
    std::array<std::array<int, FFT_SIZE / 2>, 2> validPhaseFrames {};

    // Thread-safe UI spectrum buffer
    std::mutex spectrumMutex;
    std::vector<float> uiSpectrum;
    std::vector<float> uiBaseline;
};

} // namespace LiveAudioLab
