#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace LiveAudioLab
{

juce::AudioProcessorValueTreeState::ParameterLayout FeedbackSuppressorAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Sensitivity (-60dB to +18dB)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "sensitivity", 1 }, "Sensitivity",
        juce::NormalisableRange<float>(-60.0f, 18.0f, 0.5f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // Growth Rate Sensitivity (Low, Med, High)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "growthRateSens", 1 }, "Growth Sensitivity",
        juce::StringArray { "Low", "Medium", "High" }, 1));

    // Detection Profile (Speech, Music, Flat 1/f, High Focus, Low Focus)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "profile", 1 }, "Profile",
        juce::StringArray { "Speech", "Music", "Flat (1/f)", "High Focus", "Low Focus" }, 0));

    // Notch Q (5.0 to 120.0, default 35.0 ~1/24 octave)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "notchQ", 1 }, "Notch Q",
        juce::NormalisableRange<float>(5.0f, 120.0f, 1.0f, 0.5f), 35.0f));

    // Max Cut Depth (-6dB to -40dB)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "maxCut", 1 }, "Max Cut Depth",
        juce::NormalisableRange<float>(-40.0f, -6.0f, 1.0f), -20.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // Live Filter Lift Time (5s, 15s, 30s, 60s, 5m, Hold)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "liftTime", 1 }, "Live Lift Time",
        juce::StringArray { "5s", "15s", "30s", "60s", "5m", "Hold / Off" }, 2));

    // Operation Mode (Live Auto, Learn / Soundcheck, Bypass)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "opMode", 1 }, "Operation Mode",
        juce::StringArray { "Live Auto", "Learn (Soundcheck)", "Bypass" }, 0));

    // Latency Mode: 0 ms, or a 3 ms lookahead for the panic limiter
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "latencyMode", 1 }, "Latency Mode",
        juce::StringArray { "Zero Latency (0 ms)", "Lookahead Limiter (3 ms)" }, 0));

    // Panic Limiter (On / Off)
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "panicLimiter", 1 }, "Panic Limiter", true));

    // Detection Mode. Index 0 keeps the legacy detector ("Classic").
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "detectionMode", 1 }, "Detection Mode",
        juce::StringArray { "Classic (Legacy)", "Advanced (Music Aware)", "Aggressive (Fast)" }, 1));

    return { params.begin(), params.end() };
}

FeedbackSuppressorAudioProcessor::FeedbackSuppressorAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("FeedbackDetectorThread"),
      apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    opModeParam = apvts.getRawParameterValue("opMode");
    latencyModeParam = apvts.getRawParameterValue("latencyMode");
    panicParam = apvts.getRawParameterValue("panicLimiter");
    detectedList.reserve(64);
    // Analysis starts in prepareToPlay, once sample rate and buffers are known.
}

FeedbackSuppressorAudioProcessor::~FeedbackSuppressorAudioProcessor()
{
    cancelPendingUpdate();
    stopAnalysis();
}

void FeedbackSuppressorAudioProcessor::startAnalysis()
{
    shouldExitThread.store(false);
    startThread(juce::Thread::Priority::normal);
}

void FeedbackSuppressorAudioProcessor::stopAnalysis()
{
    shouldExitThread.store(true);
    analysisEvent.signal();
    stopThread(-1);
}

int FeedbackSuppressorAudioProcessor::lookaheadSamplesFor(int latencyModeIndex) const noexcept
{
    return latencyModeIndex == 1
        ? static_cast<int>(std::round(LOOKAHEAD_MS * 0.001 * currentSampleRate.load()))
        : 0;
}

void FeedbackSuppressorAudioProcessor::prepareToPlay(double sampleRate, int)
{
    stopAnalysis();
    currentSampleRate.store(sampleRate > 0.0 ? sampleRate : 44100.0);

    detector.prepare(sampleRate);
    filterManager.prepare(sampleRate);
    panicLimiter.prepare(sampleRate, static_cast<int>(std::ceil(0.010 * sampleRate))); // 10 ms capacity

    applyControlParameters();
    const int lookahead = lookaheadSamplesFor(static_cast<int>(latencyModeParam->load()));
    panicLimiter.setLookaheadSamples(lookahead);
    panicLimiter.reset(); // start at the requested delay without a fade
    setLatencySamples(lookahead);
    reportedLatency.store(lookahead);

    samplesProcessed.store(0);
    samplesAccounted = 0;
    startAnalysis();
}

void FeedbackSuppressorAudioProcessor::releaseResources()
{
    stopAnalysis();
    detector.reset();
    filterManager.reset();
    panicLimiter.reset();
}

bool FeedbackSuppressorAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainOutputChannelSet() == layouts.getMainInputChannelSet();
}

void FeedbackSuppressorAudioProcessor::applyControlParameters()
{
    // Never called on the audio thread: these setters take the filter mutex.
    detector.setSensitivity(apvts.getRawParameterValue("sensitivity")->load());
    detector.setGrowthSensitivity(static_cast<int>(apvts.getRawParameterValue("growthRateSens")->load()));
    detector.setProfile(static_cast<DetectionProfile>(juce::jlimit(0, 4, static_cast<int>(apvts.getRawParameterValue("profile")->load()))));
    detector.setDetectionMode(static_cast<DetectionMode>(juce::jlimit(0, 2, static_cast<int>(apvts.getRawParameterValue("detectionMode")->load()))));

    filterManager.setDefaultQ(apvts.getRawParameterValue("notchQ")->load());
    filterManager.setMaxCutDepth(apvts.getRawParameterValue("maxCut")->load());

    static constexpr float liftChoices[] = { 5.0f, 15.0f, 30.0f, 60.0f, 300.0f, -1.0f };
    const int liftIdx = juce::jlimit(0, 5, static_cast<int>(apvts.getRawParameterValue("liftTime")->load()));
    filterManager.setLiftDuration(liftChoices[liftIdx]);

    filterManager.setOperationMode(static_cast<OperationMode>(juce::jlimit(0, 2, static_cast<int>(opModeParam->load()))));
    panicLimiter.setEnabled(panicParam->load() > 0.5f);
}

void FeedbackSuppressorAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = juce::jmin(getTotalNumInputChannels(), buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();
    if (numChannels == 0 || numSamples == 0)
        return;

    // Only lock-free atomics are touched here.
    filterManager.setOperationMode(static_cast<OperationMode>(juce::jlimit(0, 2, static_cast<int>(opModeParam->load()))));
    panicLimiter.setEnabled(panicParam->load() > 0.5f);
    // The worker thread notices the change and reports the latency via the
    // message thread (V2 called setLatencySamples() from the audio thread).
    panicLimiter.setLookaheadSamples(lookaheadSamplesFor(static_cast<int>(latencyModeParam->load())));

    // 1. Notch cascade (zero latency).
    filterManager.processAudio(buffer);

    // 2. Analyse the residual after the notches, so successful suppression stops
    //    further deepening. This sidechain adds no audio-path buffering.
    detector.pushSamples(buffer.getArrayOfReadPointers(), numChannels, numSamples);
    samplesProcessed.fetch_add(numSamples, std::memory_order_relaxed);
    analysisEvent.signal();

    // 3. Panic limiter (optionally with lookahead delay).
    panicLimiter.process(buffer);
}

void FeedbackSuppressorAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Keep the reported latency when the host bypasses the plugin.
    panicLimiter.processDelayOnly(buffer);
}

void FeedbackSuppressorAudioProcessor::handleAsyncUpdate()
{
    const int lookahead = panicLimiter.getLookaheadSamples();
    if (getLatencySamples() != lookahead)
        setLatencySamples(lookahead);
}

void FeedbackSuppressorAudioProcessor::run()
{
    while (! shouldExitThread.load(std::memory_order_relaxed))
    {
        analysisEvent.wait(20);
        if (shouldExitThread.load(std::memory_order_relaxed))
            break;

        applyControlParameters();
        if (panicLimiter.getLookaheadSamples() != reportedLatency.load())
        {
            reportedLatency.store(panicLimiter.getLookaheadSamples());
            triggerAsyncUpdate();
        }

        detectedList.clear();
        if (detector.processNextFrames(detectedList) && ! detectedList.empty())
            filterManager.handleDetections(detectedList);

        // Timers advance with the audio actually processed (also correct for
        // offline rendering faster than real time).
        const int64_t processed = samplesProcessed.load(std::memory_order_relaxed);
        const int64_t delta = processed - samplesAccounted;
        if (delta > 0)
        {
            samplesAccounted = processed;
            filterManager.updateTimers(static_cast<float>(static_cast<double>(delta) / currentSampleRate.load()));
        }
    }
}

int FeedbackSuppressorAudioProcessor::getNumPrograms()
{
    return 5;
}

int FeedbackSuppressorAudioProcessor::getCurrentProgram()
{
    return currentProgram.load();
}

void FeedbackSuppressorAudioProcessor::setCurrentProgram(int index)
{
    currentProgram.store(juce::jlimit(0, 4, index));

    if (auto* param = apvts.getParameter("profile"))
    {
        static constexpr float profileForProgram[] = { 0.0f, 1.0f, 3.0f, 4.0f, 2.0f };
        param->setValueNotifyingHost(param->convertTo0to1(profileForProgram[currentProgram.load()]));
    }
}

const juce::String FeedbackSuppressorAudioProcessor::getProgramName(int index)
{
    switch (index)
    {
        case 0: return "Default (Speech Focus)";
        case 1: return "Music Live";
        case 2: return "High Focus (Anti-Squeal)";
        case 3: return "Low Focus (Anti-Boom)";
        case 4: return "Flat (1/f) Neutral";
        default: return {};
    }
}

void FeedbackSuppressorAudioProcessor::changeProgramName(int, const juce::String&)
{
}

juce::AudioProcessorEditor* FeedbackSuppressorAudioProcessor::createEditor()
{
    return new FeedbackSuppressorAudioProcessorEditor(*this);
}

void FeedbackSuppressorAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    if (xml == nullptr)
        return;
    xml->setAttribute("pluginVersion", JucePlugin_VersionString);

    // Persist Fixed & Locked filter slots for session/preset recall.
    // Live (temporary) notches are intentionally not saved.
    auto* filtersXml = xml->createNewChildElement("FixedFilters");
    for (const auto& f : filterManager.getFilterSnapshots())
    {
        if (! (f.isFixed || f.isLocked))
            continue;
        auto* slotXml = filtersXml->createNewChildElement("Filter");
        slotXml->setAttribute("frequency", static_cast<double>(f.frequency));
        slotXml->setAttribute("gainDb", static_cast<double>(f.targetGainDb));
        slotXml->setAttribute("q", static_cast<double>(f.q));
        slotXml->setAttribute("bypassed", f.isBypassed);
        slotXml->setAttribute("locked", f.isLocked);
    }

    copyXmlToBinary(*xml, destData);
}

void FeedbackSuppressorAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState == nullptr || ! xmlState->hasTagName(apvts.state.getType()))
        return;

    // Separate the filter list from the parameter tree before replacing it.
    std::unique_ptr<juce::XmlElement> filtersXml;
    if (auto* f = xmlState->getChildByName("FixedFilters"))
    {
        filtersXml = std::make_unique<juce::XmlElement>(*f);
        xmlState->removeChildElement(f, true);
    }
    apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
    applyControlParameters();

    // Restore saved Fixed / Locked filters (always replace the current fixed set,
    // so loading a preset without fixed filters clears stale ones).
    filterManager.clearAll();
    if (filtersXml != nullptr)
    {
        for (auto* slotXml : filtersXml->getChildIterator())
        {
            if (! slotXml->hasTagName("Filter"))
                continue;
            const float freq = static_cast<float>(slotXml->getDoubleAttribute("frequency", 1000.0));
            const float gain = static_cast<float>(slotXml->getDoubleAttribute("gainDb", -12.0));
            const float q = static_cast<float>(slotXml->getDoubleAttribute("q", 35.0));
            const bool bypassed = slotXml->getBoolAttribute("bypassed", false);
            const bool locked = slotXml->getBoolAttribute("locked", true);
            filterManager.addFilter(freq, gain, q, true, locked, bypassed);
        }
    }
}

} // namespace LiveAudioLab

// Plugin instantiation entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LiveAudioLab::FeedbackSuppressorAudioProcessor();
}
