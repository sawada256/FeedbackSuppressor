// Processor-level smoke test: worker thread + audio callback interplay, state
// save/restore of Fixed/Locked notches, latency reporting through the message
// thread, host bypass delay, editor construction.
#include <cmath>
#include <cstdio>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

using namespace LiveAudioLab;

namespace
{
int failures = 0;
void check(bool condition, const juce::String& message)
{
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", message.toRawUTF8());
    if (! condition) ++failures;
}

void pump(int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

void setParam(FeedbackSuppressorAudioProcessor& p, const char* id, float plainValue)
{
    auto* param = p.getAPVTS().getParameter(id);
    param->setValueNotifyingHost(param->convertTo0to1(plainValue));
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const double sr = 48000.0;
    const int blockSize = 256;

    std::printf("[1] Real-time run with building feedback\n");
    auto proc = std::make_unique<FeedbackSuppressorAudioProcessor>();
    proc->setPlayConfigDetails(2, 2, sr, blockSize);
    proc->prepareToPlay(sr, blockSize);
    check(proc->getLatencySamples() == 0, "zero latency by default");

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(2, blockSize);
    double phase = 0.0;
    float lastOutputPeak = 0.0f;
    for (int b = 0; b < static_cast<int>(3.0 * sr / blockSize); ++b)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const double t = (b * blockSize + i) / sr;
            phase += juce::MathConstants<double>::twoPi * 2503.7 / sr;
            const double level = std::pow(10.0, std::min(-6.0, -50.0 + 60.0 * t) / 20.0); // +60 dB/s, open loop
            const float s = static_cast<float>(level * std::sin(phase));
            buffer.setSample(0, i, s);
            buffer.setSample(1, i, s);
        }
        proc->processBlock(buffer, midi);
        if (b % 8 == 0) juce::Thread::sleep(1); // let the worker run roughly in real time
        lastOutputPeak = buffer.getMagnitude(0, blockSize);
    }
    pump(100);
    bool notchFound = false;
    for (const auto& s : proc->getFilterManager().getFilterSnapshots())
        if (std::abs(s.frequency - 2503.7f) < 3.0f && s.currentGainDb < -6.0f) notchFound = true;
    check(notchFound, "worker thread placed a notch on the building tone");
    check(lastOutputPeak < 0.5f, "output tone is attenuated (peak " + juce::String(lastOutputPeak, 3) + ")");

    std::printf("[2] Latency reporting and host bypass delay\n");
    setParam(*proc, "latencyMode", 1.0f);
    buffer.clear();
    proc->processBlock(buffer, midi);
    for (int i = 0; i < 20 && proc->getLatencySamples() == 0; ++i) { proc->processBlock(buffer, midi); pump(20); }
    const int expected = static_cast<int>(std::round(0.003 * sr));
    check(proc->getLatencySamples() == expected, "Lookahead latency reported from the message thread: "
                                                 + juce::String(proc->getLatencySamples()) + " samples");
    // Let the delay-change crossfade finish, then send an impulse through host bypass.
    for (int i = 0; i < 4; ++i) { buffer.clear(); proc->processBlock(buffer, midi); }
    buffer.clear();
    buffer.setSample(0, 0, 0.5f);
    proc->processBlockBypassed(buffer, midi);
    int emerged = -1;
    for (int i = 0; i < blockSize; ++i) if (std::abs(buffer.getSample(0, i)) > 0.25f) { emerged = i; break; }
    check(emerged == expected, "host bypass keeps the reported delay (impulse at " + juce::String(emerged) + ")");

    std::printf("[3] State save / restore\n");
    auto& fm = proc->getFilterManager();
    fm.clearAll();
    const int a = fm.addFilter(812.0f, -9.0f, 40.0f, true, true, false);
    const int b = fm.addFilter(3150.0f, -6.0f, 60.0f, true, false, true);
    check(a > 0 && b > 0, "fixed filters added");
    setParam(*proc, "notchQ", 50.0f);
    setParam(*proc, "detectionMode", 2.0f);
    juce::MemoryBlock state;
    proc->getStateInformation(state);

    auto restored = std::make_unique<FeedbackSuppressorAudioProcessor>();
    restored->setPlayConfigDetails(2, 2, sr, blockSize);
    restored->prepareToPlay(sr, blockSize);
    restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    auto snaps = restored->getFilterManager().getFilterSnapshots();
    bool lockedOk = false, bypassOk = false;
    for (const auto& s : snaps)
    {
        if (std::abs(s.frequency - 812.0f) < 0.01f && s.isLocked && ! s.isBypassed && std::abs(s.targetGainDb + 9.0f) < 0.01f) lockedOk = true;
        if (std::abs(s.frequency - 3150.0f) < 0.01f && ! s.isLocked && s.isBypassed && s.isFixed) bypassOk = true;
    }
    check(snaps.size() == 2 && lockedOk && bypassOk, "fixed filters restored with depth, lock and bypass flags");
    check(std::abs(restored->getAPVTS().getRawParameterValue("notchQ")->load() - 50.0f) < 0.01f
          && static_cast<int>(restored->getAPVTS().getRawParameterValue("detectionMode")->load()) == 2,
          "parameters restored");
    // Loading a state without fixed filters clears stale ones.
    restored->getFilterManager().clearAll();
    juce::MemoryBlock empty;
    restored->getStateInformation(empty);
    proc->setStateInformation(empty.getData(), static_cast<int>(empty.getSize()));
    check(proc->getFilterManager().getFilterSnapshots().empty(), "preset without fixed filters clears old ones");

    std::printf("[4] Editor construction\n");
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc->createEditor());
        check(editor != nullptr, "editor created");
        pump(200);
    }

    restored->releaseResources();
    proc->releaseResources();
    restored.reset();
    proc.reset();

    if (failures > 0) { std::printf("%d processor check(s) FAILED\n", failures); return 1; }
    std::printf("All processor checks passed.\n");
    return 0;
}
