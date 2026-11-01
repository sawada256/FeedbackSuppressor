#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "dsp/FeedbackDetector.h"
#include "dsp/FilterManager.h"
#include "dsp/PanicLimiter.h"
#include "dsp/LookaheadDelay.h"

namespace LiveAudioLab
{

/**
 * V2.1 threading: the audio thread only touches lock-free state; detection,
 * notch bookkeeping, lift timers and filter-bank parameter changes run on the
 * analysis worker; latency changes are reported from the message thread.
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

    // Component accessors for Editor
    FilterManager& getFilterManager() noexcept { return filterManager; }
    FeedbackDetector& getFeedbackDetector() noexcept { return detector; }
    PanicLimiter& getPanicLimiter() noexcept { return panicLimiter; }
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts; }

    // Thread function for sidechain STFT analysis
    void run() override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void applyControlParameters();      // worker / message thread only
    int targetLatencySamples() const noexcept;
    void handleAsyncUpdate() override;
    void startAnalysis();
    void stopAnalysis();

    juce::AudioProcessorValueTreeState apvts;

    FeedbackDetector detector;
    FilterManager filterManager;
    PanicLimiter panicLimiter;
    LookaheadDelay lookaheadDelay;

    juce::WaitableEvent analysisEvent;
    std::atomic<bool> shouldExitThread { false };
    std::atomic<int64_t> samplesProcessed { 0 };
    int64_t samplesAccounted = 0;              // worker thread
    std::atomic<int> reportedLatency { 0 };
    std::atomic<float>* opModeParam = nullptr;
    std::atomic<float>* latencyModeParam = nullptr;
    std::atomic<float>* panicParam = nullptr;

    std::vector<DetectedFeedback> detectedList;
    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<int> currentProgram { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FeedbackSuppressorAudioProcessor)
};

} // namespace LiveAudioLab
