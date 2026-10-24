#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace LiveAudioLab
{

class ModernLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ModernLookAndFeel();

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider& slider) override;

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          const juce::Slider::SliderStyle, juce::Slider& slider) override;

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override;

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;
};

class IndicatorBadge : public juce::Component
{
public:
    IndicatorBadge(const juce::String& text, juce::Colour activeColor);

    void setActive(bool active);
    void paint(juce::Graphics& g) override;

private:
    juce::String label;
    juce::Colour color;
    bool isActive = false;
};

} // namespace LiveAudioLab
