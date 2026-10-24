#include "SpectrumVisualizer.h"
#include <cmath>

namespace LiveAudioLab
{

SpectrumVisualizer::SpectrumVisualizer(FeedbackDetector& detectorRef, FilterManager& managerRef)
    : detector(detectorRef), filterManager(managerRef)
{
    spectrumMags.resize(FeedbackDetector::FFT_SIZE / 2, -100.0f);
    baselineMags.resize(FeedbackDetector::FFT_SIZE / 2, -100.0f);
    smoothedSpectrum.resize(FeedbackDetector::FFT_SIZE / 2, -100.0f);

    startTimerHz(40); // 40 FPS smooth visualizer
}

SpectrumVisualizer::~SpectrumVisualizer()
{
    stopTimer();
}

void SpectrumVisualizer::timerCallback()
{
    detector.copySpectrumTo(spectrumMags, baselineMags);
    filterManager.getFilterSnapshots(filterSnapshots);

    // Smooth spectrum animation
    for (size_t i = 0; i < spectrumMags.size(); ++i)
    {
        if (spectrumMags[i] > smoothedSpectrum[i])
            smoothedSpectrum[i] = spectrumMags[i]; // fast peak rise
        else
            smoothedSpectrum[i] += 0.25f * (spectrumMags[i] - smoothedSpectrum[i]); // smooth falloff
    }

    updateEqPath();
    repaint();
}

void SpectrumVisualizer::resized()
{
    updateEqPath();
}

float SpectrumVisualizer::freqToX(float freqHz) const
{
    const float minF = 20.0f;
    const float maxF = 20000.0f;
    float clampedF = juce::jlimit(minF, maxF, freqHz);
    float norm = std::log10(clampedF / minF) / std::log10(maxF / minF);
    return norm * static_cast<float>(getWidth());
}

float SpectrumVisualizer::xToFreq(float x) const
{
    const float minF = 20.0f;
    const float maxF = 20000.0f;
    float norm = juce::jlimit(0.0f, 1.0f, x / static_cast<float>(getWidth()));
    return minF * std::pow(maxF / minF, norm);
}

float SpectrumVisualizer::dbToY(float db) const
{
    const float minDb = -80.0f;
    const float maxDb = 6.0f;
    float clamped = juce::jlimit(minDb, maxDb, db);
    float norm = (maxDb - clamped) / (maxDb - minDb);
    return norm * static_cast<float>(getHeight());
}

void SpectrumVisualizer::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    const float w = bounds.getWidth();
    const float h = bounds.getHeight();

    if (w <= 10.0f || h <= 10.0f)
        return;

    // 1. Background
    juce::Colour bgTop(0xff12151b);
    juce::Colour bgBtm(0xff0c0e12);
    g.setGradientFill(juce::ColourGradient(bgTop, 0, 0, bgBtm, 0, h, false));
    g.fillRect(bounds);

    // 2. Frequency Grid Lines & Labels
    const float gridFreqs[] = { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f };
    const char* freqLabels[] = { "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };

    g.setFont(juce::FontOptions(10.0f));
    for (int i = 0; i < 9; ++i)
    {
        float x = freqToX(gridFreqs[i]);
        g.setColour(juce::Colour(0xff222834).withAlpha(0.6f));
        g.drawVerticalLine(static_cast<int>(x), 0.0f, h);

        g.setColour(juce::Colour(0xff556275));
        g.drawText(freqLabels[i], static_cast<int>(x) + 4, static_cast<int>(h) - 18, 36, 16, juce::Justification::centredLeft);
    }

    // 3. dB Grid Lines & Labels
    const float gridDbs[] = { 0.0f, -20.0f, -40.0f, -60.0f };
    for (float db : gridDbs)
    {
        float y = dbToY(db);
        g.setColour(juce::Colour(0xff222834).withAlpha(0.6f));
        g.drawHorizontalLine(static_cast<int>(y), 0.0f, w);

        g.setColour(juce::Colour(0xff556275));
        g.drawText(juce::String(static_cast<int>(db)) + " dB", 6, static_cast<int>(y) - 14, 45, 14, juce::Justification::centredLeft);
    }

    // 4. Baseline / 1/f Envelope Curve (dashed amber line)
    if (!baselineMags.empty())
    {
        juce::Path baselinePath;
        bool started = false;
        const double sr = detector.getSampleRate();
        const int numBins = static_cast<int>(baselineMags.size());

        for (int k = 1; k < numBins; ++k)
        {
            float f = (static_cast<float>(k) * static_cast<float>(sr)) / static_cast<float>(FeedbackDetector::FFT_SIZE);
            if (f < 20.0f) continue;
            if (f > 20000.0f) break;

            float x = freqToX(f);
            float y = dbToY(baselineMags[static_cast<size_t>(k)]);

            if (!started)
            {
                baselinePath.startNewSubPath(x, y);
                started = true;
            }
            else
            {
                baselinePath.lineTo(x, y);
            }
        }

        g.setColour(juce::Colour(0xffffb300).withAlpha(0.45f));
        float dashes[] = { 4.0f, 4.0f };
        juce::Path dashedBaseline;
        juce::PathStrokeType(1.2f).createDashedStroke(dashedBaseline, baselinePath, dashes, 2);
        g.fillPath(dashedBaseline);
    }

    // 5. Input RTA Spectrum (Neon Cyan Area + Glowing Stroke)
    if (!smoothedSpectrum.empty())
    {
        juce::Path spectrumPath;
        const double sr = detector.getSampleRate();
        const int numBins = static_cast<int>(smoothedSpectrum.size());
        bool started = false;

        for (int k = 1; k < numBins; ++k)
        {
            float f = (static_cast<float>(k) * static_cast<float>(sr)) / static_cast<float>(FeedbackDetector::FFT_SIZE);
            if (f < 20.0f) continue;
            if (f > 20000.0f) break;

            float x = freqToX(f);
            float y = dbToY(smoothedSpectrum[k]);

            if (!started)
            {
                spectrumPath.startNewSubPath(x, y);
                started = true;
            }
            else
            {
                spectrumPath.lineTo(x, y);
            }
        }

        if (started)
        {
            // Fill underneath
            juce::Path fillPath = spectrumPath;
            fillPath.lineTo(w, h);
            fillPath.lineTo(0, h);
            fillPath.closeSubPath();

            juce::Colour gradTop = juce::Colour(0xff00e5ff).withAlpha(0.22f);
            juce::Colour gradBtm = juce::Colour(0xff003366).withAlpha(0.02f);
            g.setGradientFill(juce::ColourGradient(gradTop, 0, h * 0.3f, gradBtm, 0, h, false));
            g.fillPath(fillPath);

            // Glowing line stroke
            g.setColour(juce::Colour(0xff00e5ff).withAlpha(0.85f));
            g.strokePath(spectrumPath, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // 6. Combined Notch Filter Response Curve (Bright white/cyan with downward dips)
    // Completely lock-free and precomputed to prevent UI thread stalling
    if (!cachedEqPath.isEmpty())
    {
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.strokePath(cachedEqPath, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // 7. Active Feedback Markers & Pill Tags
    for (const auto& slot : filterSnapshots)
    {
        if (slot.id == -1 || slot.isBypassed)
            continue;

        float x = freqToX(slot.frequency);
        float cutDb = slot.currentGainDb;

        // Choose color: Fixed (Amber) vs Live (Cyan / Red if recently active)
        juce::Colour markerColor;
        if (slot.isFixed)
            markerColor = juce::Colour(0xffff9100);
        else if (slot.activity > 0.3f)
            markerColor = juce::Colour(0xffff1744); // Flash red on feedback trigger!
        else
            markerColor = juce::Colour(0xff00e5ff);

        // Vertical notch line
        g.setColour(markerColor.withAlpha(0.7f));
        float dashes[] = { 3.0f, 3.0f };
        g.drawDashedLine(juce::Line<float>(x, 0.0f, x, h), dashes, 2, 1.2f);

        // Notch depth node circle
        float nodeY = dbToY(cutDb);
        g.setColour(markerColor);
        g.fillEllipse(x - 4.5f, nodeY - 4.5f, 9.0f, 9.0f);
        g.setColour(juce::Colours::white);
        g.drawEllipse(x - 4.5f, nodeY - 4.5f, 9.0f, 9.0f, 1.2f);

        // Pill badge at top of marker
        juce::String tagText;
        if (slot.frequency >= 1000.0f)
            tagText = juce::String(slot.frequency / 1000.0f, 2) + " kHz";
        else
            tagText = juce::String(static_cast<int>(slot.frequency)) + " Hz";

        tagText += " (" + juce::String(static_cast<int>(cutDb)) + "dB)";

        auto fontOptions = juce::FontOptions(10.0f).withStyle("Bold");
        g.setFont(fontOptions);
        juce::Font font(fontOptions);
        int textW = font.getStringWidth(tagText) + 12;
        int textH = 18;
        int tagX = juce::jlimit(2, static_cast<int>(w) - textW - 2, static_cast<int>(x) - textW / 2);
        int tagY = 8;

        g.setColour(juce::Colour(0xff181c24).withAlpha(0.9f));
        g.fillRoundedRectangle(static_cast<float>(tagX), static_cast<float>(tagY),
                               static_cast<float>(textW), static_cast<float>(textH), 4.0f);

        g.setColour(markerColor);
        g.drawRoundedRectangle(static_cast<float>(tagX), static_cast<float>(tagY),
                               static_cast<float>(textW), static_cast<float>(textH), 4.0f, 1.0f);

        g.setColour(juce::Colours::white);
        g.drawText(tagText, tagX, tagY, textW, textH, juce::Justification::centred);
    }

    // Outer border
    g.setColour(juce::Colour(0xff2d3542));
    g.drawRect(bounds, 1.2f);
}

void SpectrumVisualizer::updateEqPath()
{
    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());

    if (w <= 10.0f || h <= 10.0f)
        return;

    // Filter out inactive or negligible filters
    struct ActiveFilterParams
    {
        float b0, b1, b2, a1, a2;
    };

    std::vector<ActiveFilterParams> activeFilters;
    activeFilters.reserve(filterSnapshots.size());
    // The notch filters run at the host rate (the detector may analyse a decimated signal).
    const double sr = filterManager.getSampleRate();

    for (const auto& slot : filterSnapshots)
    {
        if (slot.id != -1 && !slot.isBypassed && std::abs(slot.currentGainDb) > 0.05f)
        {
            const double w0 = 2.0 * juce::MathConstants<double>::pi * static_cast<double>(slot.frequency) / sr;
            const double cosW0 = std::cos(w0);
            const double sinW0 = std::sin(w0);
            const double A = std::pow(10.0, static_cast<double>(slot.currentGainDb) / 40.0);
            const double alpha = sinW0 / (2.0 * static_cast<double>(slot.q));

            const double b0_un = 1.0 + alpha * A;
            const double b1_un = -2.0 * cosW0;
            const double b2_un = 1.0 - alpha * A;
            const double a0_un = 1.0 + alpha / A;
            const double a1_un = -2.0 * cosW0;
            const double a2_un = 1.0 - alpha / A;
            const double invA0 = 1.0 / a0_un;

            activeFilters.push_back({
                static_cast<float>(b0_un * invA0),
                static_cast<float>(b1_un * invA0),
                static_cast<float>(b2_un * invA0),
                static_cast<float>(a1_un * invA0),
                static_cast<float>(a2_un * invA0)
            });
        }
    }

    cachedEqPath.clear();

    if (activeFilters.empty())
    {
        // Flat response line at 0 dB
        float y0 = dbToY(0.0f);
        cachedEqPath.startNewSubPath(0.0f, y0);
        cachedEqPath.lineTo(w, y0);
        return;
    }

    const int numCurvePoints = static_cast<int>(w / 2.0f);
    bool started = false;

    for (int i = 0; i <= numCurvePoints; ++i)
    {
        float x = static_cast<float>(i * 2);
        float f = xToFreq(x);

        // Combined magnitude calculation
        float totalMag = 1.0f;
        const double w_freq = 2.0 * juce::MathConstants<double>::pi * static_cast<double>(f) / sr;
        const std::complex<double> z1 = std::polar(1.0, -w_freq);
        const std::complex<double> z2 = std::polar(1.0, -2.0 * w_freq);

        for (const auto& af : activeFilters)
        {
            const std::complex<double> num = static_cast<double>(af.b0) + static_cast<double>(af.b1) * z1 + static_cast<double>(af.b2) * z2;
            const std::complex<double> den = 1.0 + static_cast<double>(af.a1) * z1 + static_cast<double>(af.a2) * z2;
            totalMag *= static_cast<float>(std::abs(num / den));
        }

        float magDb = 20.0f * std::log10(totalMag + 1e-5f);
        float y = dbToY(magDb);

        if (!started)
        {
            cachedEqPath.startNewSubPath(x, y);
            started = true;
        }
        else
        {
            cachedEqPath.lineTo(x, y);
        }
    }
}

} // namespace LiveAudioLab
