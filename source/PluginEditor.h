#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "ui/CustomControls.h"
#include "ui/SpectrumVisualizer.h"
#include "ui/FilterListView.h"

namespace LiveAudioLab
{

class FeedbackSuppressorAudioProcessorEditor : public juce::AudioProcessorEditor,
                                              public juce::Timer
{
public:
    explicit FeedbackSuppressorAudioProcessorEditor(FeedbackSuppressorAudioProcessor&);
    ~FeedbackSuppressorAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    FeedbackSuppressorAudioProcessor& processorRef;

    ModernLookAndFeel modernLnf;
    juce::TooltipWindow tooltipWindow { this, 600 };

    // Components
    SpectrumVisualizer spectrumVisualizer;
    FilterListView filterListView;

    // Controls
    juce::Slider sensitivitySlider;
    juce::Slider notchQSlider;
    juce::Slider maxCutSlider;

    juce::Label sensitivityLabel { {}, "Sensitivity" };
    juce::Label notchQLabel { {}, "Notch Q" };
    juce::Label maxCutLabel { {}, "Max Cut" };

    juce::ComboBox profileCombo;
    juce::ComboBox detectionModeCombo;
    juce::ComboBox growthSensCombo;
    juce::ComboBox liftTimeCombo;
    juce::ComboBox opModeCombo;
    juce::ComboBox latencyModeCombo;

    juce::Label profileLabel { {}, "Profile" };
    juce::Label detectionModeLabel { {}, "Engine Mode" };
    juce::Label growthSensLabel { {}, "Growth Rate" };
    juce::Label liftTimeLabel { {}, "Lift Timer" };
    juce::Label opModeLabel { {}, "Mode" };
    juce::Label latencyModeLabel { {}, "Latency" };

    juce::ToggleButton panicLimiterButton { "Panic Protection" };
    IndicatorBadge panicBadge { "PANIC LIMIT", juce::Colour(0xffff1744) };
    IndicatorBadge activeBadge { "MONITORING", juce::Colour(0xff00e5ff) };

    // APVTS Attachments
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttachment> sensitivityAttach;
    std::unique_ptr<SliderAttachment> notchQAttach;
    std::unique_ptr<SliderAttachment> maxCutAttach;

    std::unique_ptr<ComboAttachment> profileAttach;
    std::unique_ptr<ComboAttachment> detectionModeAttach;
    std::unique_ptr<ComboAttachment> growthSensAttach;
    std::unique_ptr<ComboAttachment> liftTimeAttach;
    std::unique_ptr<ComboAttachment> opModeAttach;
    std::unique_ptr<ComboAttachment> latencyModeAttach;

    std::unique_ptr<ButtonAttachment> panicLimiterAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FeedbackSuppressorAudioProcessorEditor)
};

} // namespace LiveAudioLab
