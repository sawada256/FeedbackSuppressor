#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../dsp/FeedbackDetector.h"
#include "../dsp/FilterManager.h"
#include <vector>

namespace LiveAudioLab
{

class SpectrumVisualizer : public juce::Component,
                           public juce::Timer
{
public:
    SpectrumVisualizer(FeedbackDetector& detectorRef, FilterManager& managerRef);
    ~SpectrumVisualizer() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

private:
    float freqToX(float freqHz) const;
    float xToFreq(float x) const;
    float dbToY(float db) const;

    FeedbackDetector& detector;
    FilterManager& filterManager;

    std::vector<float> spectrumMags;
    std::vector<float> baselineMags;
    std::vector<FilterSlotInfo> filterSnapshots;

    // Smoothed spectrum for silky animation
    std::vector<float> smoothedSpectrum;

    // Lock-free precomputed EQ response curve for silky UI rendering
    void updateEqPath();
    juce::Path cachedEqPath;
};

} // namespace LiveAudioLab
