#include "CustomControls.h"
#include <cmath>

namespace LiveAudioLab
{

ModernLookAndFeel::ModernLookAndFeel()
{
    setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(0xff121418));
    setColour(juce::Slider::thumbColourId, juce::Colour(0xff00e5ff));
    setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff00e5ff));
    setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff222832));
    setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff1a1e24));
    setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff343c48));
    setColour(juce::ComboBox::textColourId, juce::Colours::white);
    setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff161a20));
    setColour(juce::PopupMenu::textColourId, juce::Colours::white);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(0xff00e5ff).withAlpha(0.3f));
    setColour(juce::TextButton::buttonColourId, juce::Colour(0xff202630));
    setColour(juce::TextButton::textColourOffId, juce::Colours::white);
}

void ModernLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPosProportional, float rotaryStartAngle,
                                         float rotaryEndAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(4.0f);
    auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    auto center = bounds.getCentre();

    float currentAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    float trackWidth = 5.0f;
    float dialRadius = radius - trackWidth - 4.0f;

    // Background track
    juce::Path bgTrack;
    bgTrack.addCentredArc(center.x, center.y, radius - trackWidth * 0.5f, radius - trackWidth * 0.5f,
                          0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(juce::Colour(0xff222834));
    g.strokePath(bgTrack, juce::PathStrokeType(trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Active value track
    if (sliderPosProportional > 0.001f)
    {
        juce::Path valueTrack;
        valueTrack.addCentredArc(center.x, center.y, radius - trackWidth * 0.5f, radius - trackWidth * 0.5f,
                                 0.0f, rotaryStartAngle, currentAngle, true);
        juce::Colour activeColour = slider.findColour(juce::Slider::rotarySliderFillColourId);
        g.setColour(activeColour);
        g.strokePath(valueTrack, juce::PathStrokeType(trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Inner dial body
    juce::Colour dialDark(0xff181c22);
    juce::Colour dialLight(0xff2a3240);
    g.setGradientFill(juce::ColourGradient(dialLight, center.x, center.y - dialRadius,
                                           dialDark, center.x, center.y + dialRadius, false));
    g.fillEllipse(center.x - dialRadius, center.y - dialRadius, dialRadius * 2.0f, dialRadius * 2.0f);

    // Inner border
    g.setColour(juce::Colour(0xff3c4658).withAlpha(0.6f));
    g.drawEllipse(center.x - dialRadius, center.y - dialRadius, dialRadius * 2.0f, dialRadius * 2.0f, 1.2f);

    // Pointer notch / line
    juce::Path pointer;
    float pointerLen = dialRadius * 0.65f;
    pointer.startNewSubPath(center.x + std::sin(currentAngle) * (dialRadius * 0.25f),
                            center.y - std::cos(currentAngle) * (dialRadius * 0.25f));
    pointer.lineTo(center.x + std::sin(currentAngle) * (dialRadius * 0.25f + pointerLen),
                   center.y - std::cos(currentAngle) * (dialRadius * 0.25f + pointerLen));

    g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId));
    g.strokePath(pointer, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void ModernLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPos, float minSliderPos, float maxSliderPos,
                                         const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
}

void ModernLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                             const juce::Colour& backgroundColour,
                                             bool shouldDrawButtonAsHighlighted,
                                             bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    float corner = 6.0f;

    juce::Colour base = button.getToggleState() ? juce::Colour(0xff00e5ff).withAlpha(0.35f) : backgroundColour;
    if (shouldDrawButtonAsDown)
        base = base.darker(0.2f);
    else if (shouldDrawButtonAsHighlighted)
        base = base.brighter(0.15f);

    g.setColour(base);
    g.fillRoundedRectangle(bounds, corner);

    juce::Colour border = button.getToggleState() ? juce::Colour(0xff00e5ff) : juce::Colour(0xff384252);
    g.setColour(border);
    g.drawRoundedRectangle(bounds, corner, 1.2f);
}

void ModernLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                     int, int, int, int, juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(0, 0, static_cast<float>(width), static_cast<float>(height)).reduced(1.0f);
    float corner = 6.0f;

    g.setColour(findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(bounds, corner);

    g.setColour(findColour(juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(bounds, corner, 1.0f);

    // Chevron arrow
    juce::Path arrow;
    float arrowX = static_cast<float>(width) - 18.0f;
    float arrowY = static_cast<float>(height) * 0.5f - 2.0f;
    arrow.startNewSubPath(arrowX, arrowY);
    arrow.lineTo(arrowX + 4.0f, arrowY + 5.0f);
    arrow.lineTo(arrowX + 8.0f, arrowY);

    g.setColour(juce::Colour(0xff00e5ff));
    g.strokePath(arrow, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void ModernLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    g.setColour(findColour(juce::PopupMenu::backgroundColourId));
    g.fillRoundedRectangle(0, 0, static_cast<float>(width), static_cast<float>(height), 6.0f);
    g.setColour(juce::Colour(0xff343c48));
    g.drawRoundedRectangle(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f, 6.0f, 1.0f);
}

IndicatorBadge::IndicatorBadge(const juce::String& text, juce::Colour activeColor)
    : label(text), color(activeColor)
{
}

void IndicatorBadge::setActive(bool active)
{
    if (isActive != active)
    {
        isActive = active;
        repaint();
    }
}

void IndicatorBadge::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    float corner = bounds.getHeight() * 0.5f;

    juce::Colour bg = isActive ? color.withAlpha(0.25f) : juce::Colour(0xff1c2026);
    juce::Colour border = isActive ? color : juce::Colour(0xff2d3440);

    g.setColour(bg);
    g.fillRoundedRectangle(bounds, corner);

    g.setColour(border);
    g.drawRoundedRectangle(bounds, corner, 1.2f);

    // Indicator dot
    float dotRadius = 4.0f;
    float dotX = bounds.getX() + 10.0f;
    float dotY = bounds.getCentreY() - dotRadius;
    g.setColour(isActive ? color : juce::Colour(0xff4a5568));
    g.fillEllipse(dotX, dotY, dotRadius * 2.0f, dotRadius * 2.0f);

    // Text
    g.setFont(juce::FontOptions(11.0f).withStyle("Bold"));
    g.setColour(isActive ? juce::Colours::white : juce::Colour(0xff8892a0));
    g.drawText(label, bounds.withTrimmedLeft(24.0f), juce::Justification::centredLeft, true);
}

} // namespace LiveAudioLab
