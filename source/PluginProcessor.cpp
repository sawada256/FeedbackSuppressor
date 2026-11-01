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

    // Latency Mode (Zero Latency 0ms, Lookahead 3ms)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "latencyMode", 1 }, "Latency Mode",
        juce::StringArray { "Zero Latency (0ms)", "Lookahead (3ms)" }, 0));

    // Panic Limiter (On / Off)
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "panicLimiter", 1 }, "Panic Limiter", true));

    // Detection Mode (legacy, music-aware, sensitive music-aware)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "detectionMode", 1 }, "Detection Mode",
        juce::StringArray { "Standard (Safe)", "Advanced (Music Aware)", "Aggressive (Mix Hunter)" }, 1));

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
    detectedList.reserve(16);
    // V2.1: the analysis thread starts in prepareToPlay (V2 started it here too).
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

int FeedbackSuppressorAudioProcessor::targetLatencySamples() const noexcept
{
    const int latIdx = static_cast<int>(latencyModeParam->load());
    return latIdx == 1 ? static_cast<int>(std::round(0.003 * currentSampleRate.load())) : 0;
}

void FeedbackSuppressorAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    stopAnalysis();
    currentSampleRate.store(sampleRate > 0.0 ? sampleRate : 44100.0);

    detector.prepare(sampleRate);
    filterManager.prepare(sampleRate);
    panicLimiter.prepare(sampleRate);
    lookaheadDelay.prepare(sampleRate, static_cast<int>(sampleRate * 0.05)); // 50ms buffer

    applyControlParameters();
    lookaheadDelay.setLookaheadMs(static_cast<int>(latencyModeParam->load()) == 1 ? 3.0f : 0.0f);
    const int latency = targetLatencySamples();
    setLatencySamples(latency);
    reportedLatency.store(latency);

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
    lookaheadDelay.reset();
}

bool FeedbackSuppressorAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}

void FeedbackSuppressorAudioProcessor::applyControlParameters()
{
    // Same parameter mapping as V2's updateParameters(), but never called on the
    // audio thread (these setters take the filter-bank mutex).
    detector.setSensitivity(apvts.getRawParameterValue("sensitivity")->load());
    detector.setGrowthSensitivity(static_cast<int>(apvts.getRawParameterValue("growthRateSens")->load()));
    detector.setProfile(static_cast<DetectionProfile>(static_cast<int>(apvts.getRawParameterValue("profile")->load())));
    if (auto* detModeVal = apvts.getRawParameterValue("detectionMode"))
        detector.setDetectionMode(static_cast<DetectionMode>(static_cast<int>(detModeVal->load())));

    filterManager.setDefaultQ(apvts.getRawParameterValue("notchQ")->load());
    filterManager.setMaxCutDepth(apvts.getRawParameterValue("maxCut")->load());

    const int liftIdx = static_cast<int>(apvts.getRawParameterValue("liftTime")->load());
    float liftSeconds = 30.0f;
    switch (liftIdx)
    {
        case 0: liftSeconds = 5.0f; break;
        case 1: liftSeconds = 15.0f; break;
        case 2: liftSeconds = 30.0f; break;
        case 3: liftSeconds = 60.0f; break;
        case 4: liftSeconds = 300.0f; break;
        case 5: liftSeconds = -1.0f; break; // Hold / Infinite
        default: liftSeconds = 30.0f; break;
    }
    filterManager.setLiftDuration(liftSeconds);

    filterManager.setOperationMode(static_cast<OperationMode>(static_cast<int>(opModeParam->load())));
    panicLimiter.setEnabled(panicParam->load() > 0.5f);
}

void FeedbackSuppressorAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels == 0 || numSamples == 0)
        return;

    // Lock-free parameter reads only (V2 called the mutex-taking setters here).
    filterManager.setOperationMode(static_cast<OperationMode>(static_cast<int>(opModeParam->load())));
    panicLimiter.setEnabled(panicParam->load() > 0.5f);
    lookaheadDelay.setLookaheadMs(static_cast<int>(latencyModeParam->load()) == 1 ? 3.0f : 0.0f);

    // 2. Optional lookahead delay (0ms or 3ms)
    lookaheadDelay.process(buffer);

    // 3. Notch Filter Cascade (Direct Form II Transposed biquads: 0-sample latency)
    filterManager.processAudio(buffer);

    // Analyze residual after EQ so successful suppression stops further deepening.
    // This sidechain introduces no audio-path buffering.
    detector.pushSamples(buffer.getArrayOfReadPointers(), numChannels, numSamples);
    samplesProcessed.fetch_add(numSamples, std::memory_order_relaxed);
    analysisEvent.signal();

    // 4. Panic Limiter (Hearing/speaker protection)
    panicLimiter.process(buffer);

    // 5. Lift timers now advance on the analysis thread (by samples processed).
}

void FeedbackSuppressorAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // V2.1: keep the reported latency while the host bypasses the plug-in.
    lookaheadDelay.setLookaheadMs(static_cast<int>(latencyModeParam->load()) == 1 ? 3.0f : 0.0f);
    lookaheadDelay.process(buffer);
}

void FeedbackSuppressorAudioProcessor::handleAsyncUpdate()
{
    const int latency = reportedLatency.load();
    if (getLatencySamples() != latency)
        setLatencySamples(latency);
}

void FeedbackSuppressorAudioProcessor::run()
{
    while (!shouldExitThread.load(std::memory_order_relaxed))
    {
        analysisEvent.wait(20);

        if (shouldExitThread.load(std::memory_order_relaxed))
            break;

        applyControlParameters();
        const int latency = targetLatencySamples();
        if (latency != reportedLatency.load())
        {
            reportedLatency.store(latency);
            triggerAsyncUpdate(); // V2 called setLatencySamples() on the audio thread
        }

        detectedList.clear();
        if (detector.processNextFrames(detectedList))
        {
            if (!detectedList.empty())
            {
                filterManager.handleDetections(detectedList);
            }
        }

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
        switch (currentProgram.load())
        {
            case 0: param->setValueNotifyingHost(param->convertTo0to1(0.0f)); break; // Speech
            case 1: param->setValueNotifyingHost(param->convertTo0to1(1.0f)); break; // Music
            case 2: param->setValueNotifyingHost(param->convertTo0to1(3.0f)); break; // High Focus
            case 3: param->setValueNotifyingHost(param->convertTo0to1(4.0f)); break; // Low Focus
            case 4: param->setValueNotifyingHost(param->convertTo0to1(2.0f)); break; // Flat (1/f)
            default: break;
        }
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

    // Persist Fixed & Locked filter slots for session/preset recall
    auto filtersSnapshot = filterManager.getFilterSnapshots();
    auto* filtersXml = xml->createNewChildElement("FixedFilters");
    for (const auto& f : filtersSnapshot)
    {
        if (f.isFixed || f.isLocked)
        {
            auto* slotXml = filtersXml->createNewChildElement("Filter");
            slotXml->setAttribute("frequency", static_cast<double>(f.frequency));
            slotXml->setAttribute("gainDb", static_cast<double>(f.targetGainDb));
            slotXml->setAttribute("q", static_cast<double>(f.q));
            slotXml->setAttribute("bypassed", f.isBypassed);
            slotXml->setAttribute("locked", f.isLocked);
        }
    }

    copyXmlToBinary(*xml, destData);
}

void FeedbackSuppressorAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState == nullptr || ! xmlState->hasTagName(apvts.state.getType()))
        return;

    // V2.1: detach the filter list before restoring the parameter tree (V2 left
    // it inside the APVTS state), and always replace the fixed set so a preset
    // without fixed filters clears stale ones.
    std::unique_ptr<juce::XmlElement> filtersXml;
    if (auto* f = xmlState->getChildByName("FixedFilters"))
    {
        filtersXml = std::make_unique<juce::XmlElement>(*f);
        xmlState->removeChildElement(f, true);
    }
    apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
    applyControlParameters();

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
            // V2 guessed the new id with getFilterSnapshots().back(); addFilter returns it.
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
