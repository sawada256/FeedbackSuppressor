#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "dsp/FeedbackDetector.h"
#include "dsp/FilterManager.h"
#include "dsp/PanicLimiter.h"

namespace LiveAudioLab
{

/**
 * V3 threading model
 *  - Audio thread (processBlock): notch cascade, analysis FIFO push, limiter.
 *    No locks, no allocation, no host callbacks.
 *  - Analysis worker thread (run): STFT detection, notch bookkeeping, lift
 *    timers (advanced by the number of samples actually processed) and all
 *    parameter changes that touch the filter bank.
 *  - Message thread: UI, state save/restore, latency reporting (AsyncUpdater).
 */
class FeedbackSuppressorAudioProcessor : public juce::AudioProcessor,
                                         public juce::Thread,
                                         private juce::AsyncUpdater
{
public:
    FeedbackSuppressorAudioProcessor();
    ~FeedbackSuppressorAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    FilterManager& getFilterManager() noexcept { return filterManager; }
    FeedbackDetector& getFeedbackDetector() noexcept { return detector; }
    PanicLimiter& getPanicLimiter() noexcept { return panicLimiter; }
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts; }

    // Worker thread for the sidechain STFT analysis.
    void run() override;

    static constexpr float LOOKAHEAD_MS = 3.0f;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void applyControlParameters();      // worker / message thread only
    int lookaheadSamplesFor(int latencyModeIndex) const noexcept;
    void handleAsyncUpdate() override;  // reports latency changes on the message thread
    void startAnalysis();
    void stopAnalysis();

    juce::AudioProcessorValueTreeState apvts;

    FeedbackDetector detector;
    FilterManager filterManager;
    PanicLimiter panicLimiter;

    juce::WaitableEvent analysisEvent;
    std::atomic<bool> shouldExitThread { false };
    std::atomic<int64_t> samplesProcessed { 0 };
    int64_t samplesAccounted = 0; // worker thread
    std::atomic<int> reportedLatency { 0 };

    // Cached raw parameter pointers (read on the audio thread).
    std::atomic<float>* opModeParam = nullptr;
    std::atomic<float>* latencyModeParam = nullptr;
    std::atomic<float>* panicParam = nullptr;

    std::vector<DetectedFeedback> detectedList;
    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<int> currentProgram { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FeedbackSuppressorAudioProcessor)
};

} // namespace LiveAudioLab
