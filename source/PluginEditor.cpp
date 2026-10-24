#include "PluginEditor.h"

namespace LiveAudioLab
{

FeedbackSuppressorAudioProcessorEditor::FeedbackSuppressorAudioProcessorEditor(FeedbackSuppressorAudioProcessor& p)
    : AudioProcessorEditor(&p),
      processorRef(p),
      spectrumVisualizer(p.getFeedbackDetector(), p.getFilterManager()),
      filterListView(p.getFilterManager())
{
    setLookAndFeel(&modernLnf);

    // Setup Rotaries
    auto setupSlider = [this](juce::Slider& s, juce::Label& l, const juce::String& suffix = "") {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 65, 18);
        s.setTextValueSuffix(suffix);
        addAndMakeVisible(s);

        l.setFont(juce::FontOptions(12.0f).withStyle("Bold"));
        l.setJustificationType(juce::Justification::centred);
        l.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));
        addAndMakeVisible(l);
    };

    setupSlider(sensitivitySlider, sensitivityLabel, " dB");
    setupSlider(notchQSlider, notchQLabel, "");
    setupSlider(maxCutSlider, maxCutLabel, " dB");

    // Setup Combos
    auto setupCombo = [this](juce::ComboBox& c, juce::Label& l, const juce::StringArray& items) {
        c.addItemList(items, 1);
        addAndMakeVisible(c);

        l.setFont(juce::FontOptions(11.0f));
        l.setJustificationType(juce::Justification::centredLeft);
        l.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));
        addAndMakeVisible(l);
    };

    setupCombo(profileCombo, profileLabel, { "Speech", "Music", "Flat (1/f)", "High Focus", "Low Focus" });
    setupCombo(detectionModeCombo, detectionModeLabel, { "Classic (Legacy)", "Advanced (Music Aware)", "Aggressive (Fast)" });
    detectionModeCombo.setTooltip("Advanced (recommended): reacts to tones that grow on their own at a perfectly stable pitch and protects "
                                  "harmonic notes, vibrato and pitch drift. Aggressive: lower thresholds and shorter confirmation, more "
                                  "false cuts on music. Classic: the V1/V2 level-based detector (many false cuts on vocals).");
    setupCombo(growthSensCombo, growthSensLabel, { "Low", "Medium", "High" });
    setupCombo(liftTimeCombo, liftTimeLabel, { "5s", "15s", "30s", "60s", "5m", "Hold / Off" });
    setupCombo(opModeCombo, opModeLabel, { "Live Auto", "Learn (Soundcheck)", "Bypass" });
    setupCombo(latencyModeCombo, latencyModeLabel, { "Zero Latency (0 ms)", "Lookahead Limiter (3 ms)" });
    latencyModeCombo.setTooltip("Zero Latency: no delay at all. Lookahead Limiter: delays the output by 3 ms so the panic limiter "
                                "catches sudden peaks without overshoot (latency is reported to the host).");
    opModeCombo.setTooltip("Live Auto: temporary notches that lift after the Lift Timer. Learn (Soundcheck): new notches become Fixed "
                           "and are saved with the project. Bypass: all notches fade out (detection pauses).");
    profileCombo.setTooltip("Classic detector reference curve. In Advanced/Aggressive, High/Low Focus lower the threshold by 2 dB "
                            "above 2 kHz / below 1.2 kHz.");
    growthSensCombo.setTooltip("How fast a tone must grow to be treated as building feedback (Medium: 25 dB/s fast path, 6 dB/s slow path).");
    liftTimeCombo.setTooltip("How long a Live notch stays before it is released (+4 dB/s). If the feedback returns, the notch "
                             "comes back deeper. Hold keeps Live notches until cleared.");
    sensitivitySlider.setTooltip("Detection threshold offset. Higher = catches weaker feedback earlier but cuts more often.");
    notchQSlider.setTooltip("Notch width for new notches (35 = about 1/24 octave). Low-frequency notches are never narrower than 4 Hz.");
    maxCutSlider.setTooltip("Deepest cut a single notch may reach. Notches start at 3-12 dB and deepen in 3 dB steps only while feedback remains.");

    panicLimiterButton.setToggleState(true, juce::dontSendNotification);
    panicLimiterButton.setTooltip("Emergency limiter at -0.5 dBFS that protects ears and speakers while a notch is being placed.");
    addAndMakeVisible(panicLimiterButton);

    addAndMakeVisible(panicBadge);
    addAndMakeVisible(activeBadge);

    addAndMakeVisible(spectrumVisualizer);
    addAndMakeVisible(filterListView);

    // APVTS Bindings
    auto& apvts = processorRef.getAPVTS();
    sensitivityAttach = std::make_unique<SliderAttachment>(apvts, "sensitivity", sensitivitySlider);
    notchQAttach = std::make_unique<SliderAttachment>(apvts, "notchQ", notchQSlider);
    maxCutAttach = std::make_unique<SliderAttachment>(apvts, "maxCut", maxCutSlider);

    profileAttach = std::make_unique<ComboAttachment>(apvts, "profile", profileCombo);
    detectionModeAttach = std::make_unique<ComboAttachment>(apvts, "detectionMode", detectionModeCombo);
    growthSensAttach = std::make_unique<ComboAttachment>(apvts, "growthRateSens", growthSensCombo);
    liftTimeAttach = std::make_unique<ComboAttachment>(apvts, "liftTime", liftTimeCombo);
    opModeAttach = std::make_unique<ComboAttachment>(apvts, "opMode", opModeCombo);
    latencyModeAttach = std::make_unique<ComboAttachment>(apvts, "latencyMode", latencyModeCombo);

    panicLimiterAttach = std::make_unique<ButtonAttachment>(apvts, "panicLimiter", panicLimiterButton);

    setResizable(true, true);
    setResizeLimits(880, 560, 1400, 900);
    setSize(980, 640);

    startTimerHz(20);
}

FeedbackSuppressorAudioProcessorEditor::~FeedbackSuppressorAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void FeedbackSuppressorAudioProcessorEditor::timerCallback()
{
    float gr = processorRef.getPanicLimiter().getGainReductionDb();
    panicBadge.setActive(gr < -0.5f);

    auto mode = processorRef.getFilterManager().getOperationMode();
    activeBadge.setActive(mode != OperationMode::Bypass);
}

void FeedbackSuppressorAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff121418));

    // Header Background
    auto headerArea = getLocalBounds().removeFromTop(56);
    g.setColour(juce::Colour(0xff181c24));
    g.fillRect(headerArea);

    g.setColour(juce::Colour(0xff2a3240));
    g.drawHorizontalLine(55, 0.0f, static_cast<float>(getWidth()));

    // Title
    g.setFont(juce::FontOptions(18.0f).withStyle("Bold"));
    g.setColour(juce::Colour(0xff00e5ff));
    g.drawText("FEEDBACK SUPPRESSOR V3", 20, 10, 280, 20, juce::Justification::centredLeft);

    g.setFont(juce::FontOptions(10.0f));
    g.setColour(juce::Colour(0xff64748b));
    g.drawText("LIVE ACOUSTIC DEFENDER • ZERO LATENCY IIR • v" JucePlugin_VersionString, 20, 30, 360, 16, juce::Justification::centredLeft);

    // Section outline for controls
    auto bounds = getLocalBounds();
    bounds.removeFromTop(56);
    int topH = static_cast<int>(static_cast<float>(bounds.getHeight()) * 0.48f);
    bounds.removeFromTop(topH);

    auto leftArea = bounds.removeFromLeft(330).reduced(8);
    g.setColour(juce::Colour(0xff181c24));
    g.fillRoundedRectangle(leftArea.toFloat(), 6.0f);
    g.setColour(juce::Colour(0xff2d3542));
    g.drawRoundedRectangle(leftArea.toFloat(), 6.0f, 1.0f);

    // Section title
    g.setFont(juce::FontOptions(11.0f).withStyle("Bold"));
    g.setColour(juce::Colour(0xff94a3b8));
    g.drawText("DETECTOR & NOTCH PARAMETERS", leftArea.getX() + 12, leftArea.getY() + 8, 220, 18, juce::Justification::centredLeft);
}

void FeedbackSuppressorAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    // 1. Header
    auto header = area.removeFromTop(56);

    int headerRightX = header.getRight() - 14;

    // Badges
    headerRightX -= 110;
    panicBadge.setBounds(headerRightX, 14, 105, 28);

    headerRightX -= 120;
    activeBadge.setBounds(headerRightX, 14, 115, 28);

    // Latency combo
    headerRightX -= 155;
    latencyModeCombo.setBounds(headerRightX, 16, 145, 24);

    // Mode combo
    headerRightX -= 140;
    opModeCombo.setBounds(headerRightX, 16, 130, 24);

    // 2. Spectrum Visualizer (Top 48% of remaining area)
    int specHeight = static_cast<int>(area.getHeight() * 0.48f);
    spectrumVisualizer.setBounds(area.removeFromTop(specHeight).reduced(8, 4));

    // 3. Bottom Panels (Controls on Left, Filter Table on Right)
    auto bottomArea = area.reduced(8, 4);

    // Left controls panel
    auto leftPanel = bottomArea.removeFromLeft(330);
    auto leftInner = leftPanel.reduced(10);
    leftInner.removeFromTop(24); // title padding

    // Rotary sliders row
    auto sliderRow = leftInner.removeFromTop(105);
    int sliderW = sliderRow.getWidth() / 3;

    auto s1 = sliderRow.removeFromLeft(sliderW);
    sensitivityLabel.setBounds(s1.removeFromTop(18));
    sensitivitySlider.setBounds(s1);

    auto s2 = sliderRow.removeFromLeft(sliderW);
    notchQLabel.setBounds(s2.removeFromTop(18));
    notchQSlider.setBounds(s2);

    auto s3 = sliderRow;
    maxCutLabel.setBounds(s3.removeFromTop(18));
    maxCutSlider.setBounds(s3);

    leftInner.removeFromTop(8);

    // Combos list
    auto addComboRow = [&leftInner](juce::Label& lbl, juce::ComboBox& box) {
        auto row = leftInner.removeFromTop(28);
        lbl.setBounds(row.removeFromLeft(90));
        box.setBounds(row.removeFromLeft(190));
        leftInner.removeFromTop(4);
    };

    addComboRow(profileLabel, profileCombo);
    addComboRow(detectionModeLabel, detectionModeCombo);
    addComboRow(growthSensLabel, growthSensCombo);
    addComboRow(liftTimeLabel, liftTimeCombo);

    panicLimiterButton.setBounds(leftInner.removeFromTop(26).withTrimmedLeft(4));

    // Right table panel
    filterListView.setBounds(bottomArea.reduced(4));
}

} // namespace LiveAudioLab
