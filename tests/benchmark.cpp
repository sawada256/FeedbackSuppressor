// Feedback suppressor benchmark: measurable metrics shared by V2 and V3.
// Uses only the public detector/filter API common to both versions so the same
// file can be compiled against an older source tree for before/after numbers.
//
// Metrics:
//   1. Detection time of exponentially building feedback over a sung phrase
//   2. False-positive events per minute on synthetic voice / instruments
//   3. Frequency estimation error of reported events
//   4. CPU time per audio second (detector analysis, notch cascade)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include "dsp/FeedbackDetector.h"
#include "dsp/FilterManager.h"

using namespace LiveAudioLab;

namespace
{
constexpr double twoPi = 6.283185307179586;

// ---------------------------------------------------------------------------
// Synthetic sources
// ---------------------------------------------------------------------------
struct Voice
{
    // Vowel formants (Hz, bandwidth Hz, gain)
    struct Formant { double f, bw, g; };
    std::vector<Formant> formants { { 700, 110, 1.0 }, { 1220, 120, 0.5 }, { 2600, 160, 0.25 }, { 3300, 250, 0.12 } };
    double sr;
    double phase = 0;
    double jitter = 0;
    std::mt19937 rng;
    std::normal_distribution<double> gauss { 0.0, 1.0 };
    Voice(double s, unsigned seed) : sr(s), rng(seed) {}

    double envelope(double f) const
    {
        double g = 0.02;
        for (const auto& fm : formants)
        {
            const double x = (f - fm.f) / fm.bw;
            g += fm.g / (1.0 + x * x);
        }
        return g;
    }
    // f0: instantaneous fundamental; vibratoCents applied by the caller
    double next(double f0, double maxHarmonicHz = 8000.0)
    {
        jitter = 0.9999 * jitter + 0.00005 * gauss(rng); // slow pitch drift, ~0.35 % (6 cents) std, 200 ms
        const double f = f0 * (1.0 + jitter);
        phase += twoPi * f / sr;
        if (phase > twoPi * 1.0e6) phase = std::fmod(phase, twoPi);
        double out = 0;
        for (int h = 1; h * f < maxHarmonicHz && h <= 40; ++h)
            out += envelope(h * f) / std::sqrt(static_cast<double>(h)) * std::sin(h * phase);
        return out;
    }
};

struct Melody
{
    std::vector<double> notesHz;
    double noteSec;
    double vibratoRate, vibratoCents, vibratoDelay;
    double f0At(double t, double& noteTime) const
    {
        const int idx = static_cast<int>(t / noteSec) % static_cast<int>(notesHz.size());
        noteTime = std::fmod(t, noteSec);
        double cents = 0;
        if (noteTime > vibratoDelay)
            cents = vibratoCents * std::sin(twoPi * vibratoRate * (noteTime - vibratoDelay));
        return notesHz[static_cast<size_t>(idx)] * std::pow(2.0, cents / 1200.0);
    }
};

double noteEnvelope(double noteTime, double noteSec)
{
    const double attack = std::min(1.0, noteTime / 0.04);
    const double release = std::min(1.0, (noteSec - noteTime) / 0.06);
    return std::max(0.0, std::min(attack, release));
}

using Generator = std::function<void(double time, float& left, float& right)>;

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------
struct Event { double time; float freq; };

std::vector<Event> runDetector(double sr, DetectionMode mode, DetectionProfile profile, double seconds,
                               const Generator& gen, double* cpuSeconds = nullptr, int blockSize = 256)
{
    FeedbackDetector detector;
    detector.prepare(sr);
    detector.setDetectionMode(mode);
    detector.setProfile(profile);
    std::vector<Event> events;
    std::vector<DetectedFeedback> list;
    list.reserve(64);
    juce::AudioBuffer<float> block(2, blockSize);
    double cpu = 0;
    const int total = static_cast<int>(seconds * sr);
    for (int start = 0; start < total; start += blockSize)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            float l = 0, r = 0;
            gen((start + i) / sr, l, r);
            block.setSample(0, i, l);
            block.setSample(1, i, r);
        }
        detector.pushSamples(block.getArrayOfReadPointers(), 2, blockSize);
        list.clear();
        const auto t0 = std::chrono::steady_clock::now();
        detector.processNextFrames(list);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (const auto& e : list)
            events.push_back({ (start + blockSize) / sr, e.frequency });
    }
    if (cpuSeconds) *cpuSeconds = cpu;
    return events;
}

const char* modeName(DetectionMode m)
{
    return m == DetectionMode::Standard ? "Standard" : (m == DetectionMode::Advanced ? "Advanced" : "Aggressive");
}

// ---------------------------------------------------------------------------
// 1. Feedback build-up detection time
// ---------------------------------------------------------------------------
void benchDetectionTime(DetectionMode mode, double& meanMs, double& maxMs, int& missed, double& meanErrCents, int& otherEvents,
                        double* perRateMs = nullptr)
{
    const double sr = 48000;
    std::vector<double> freqs { 315.7, 812.3, 1253.1, 2507.9, 4111.4, 6803.6 };
    std::vector<double> rates { 10.0, 30.0, 100.0 }; // dB per second
    if (const char* f = std::getenv("FS_FREQ")) freqs = { std::atof(f) };
    if (const char* r = std::getenv("FS_RATE")) rates = { std::atof(r), std::atof(r), std::atof(r) };
    double sum = 0; int count = 0; maxMs = 0; missed = 0; double errSum = 0; otherEvents = 0;
    for (size_t ri = 0; ri < rates.size(); ++ri)
    {
        const double rate = rates[ri];
        double rateSum = 0; int rateCount = 0;
        for (double target : freqs)
        {
            Voice voice(sr, 7);
            Melody melody { { 196.0, 220.0, 246.9, 261.6, 293.7 }, 0.9, 5.5, 35.0, 0.25 };
            std::mt19937 rng(11);
            std::normal_distribution<float> noise(0.0f, 0.003f);
            double fbPhase = 0;
            const double onset = 1.5;
            Generator gen = [&](double t, float& l, float& r)
            {
                double nt;
                const double f0 = melody.f0At(t, nt);
                const double v = 0.12 * noteEnvelope(nt, melody.noteSec) * voice.next(f0);
                double fb = 0;
                if (t >= onset)
                {
                    // Starts 45 dB below the voice peak and grows exponentially, saturating at -6 dBFS.
                    const double levelDb = std::min(-6.0, -60.0 + rate * (t - onset));
                    fbPhase += twoPi * target / sr;
                    fb = std::pow(10.0, levelDb / 20.0) * std::sin(fbPhase);
                }
                const float s = static_cast<float>(v + fb) + noise(rng);
                l = s; r = s;
            };
            const double dur = onset + 60.0 / rate + 2.0;
            const auto events = runDetector(sr, mode, DetectionProfile::Speech, dur, gen);
            double first = -1; float firstErr = 0;
            for (const auto& e : events)
            {
                const bool match = std::abs(e.freq - target) < std::max(12.0, target * 0.01);
                if (match && e.time >= onset && first < 0) { first = e.time - onset; firstErr = e.freq - static_cast<float>(target); }
                if (!match) ++otherEvents;
            }
            if (std::getenv("FS_CASES")) std::printf("  case %s rate=%.0f f=%.1f first=%.0f ms\n", modeName(mode), rate, target, first * 1000.0);
            if (first < 0) { ++missed; continue; }
            sum += first * 1000.0; ++count; maxMs = std::max(maxMs, first * 1000.0);
            rateSum += first * 1000.0; ++rateCount;
            errSum += std::abs(1200.0 * std::log2((target + firstErr) / target));
        }
        if (perRateMs) perRateMs[ri] = rateCount ? rateSum / rateCount : -1;
    }
    meanMs = count ? sum / count : -1;
    meanErrCents = count ? errSum / count : -1;
}

// ---------------------------------------------------------------------------
// 2. False positives on musical sources (events per minute)
// ---------------------------------------------------------------------------
struct FpResult { std::string name; double perMinute; };

std::vector<FpResult> benchFalsePositives(DetectionMode mode, DetectionProfile profile)
{
    const double sr = 48000, dur = 30.0;
    std::vector<FpResult> out;
    auto measure = [&](const std::string& name, const Generator& gen)
    {
        if (const char* only = std::getenv("FS_ONLY"); only != nullptr && name != only) return;
        const auto events = runDetector(sr, mode, profile, dur, gen);
        out.push_back({ name, events.size() * 60.0 / dur });
    };
    {   // Sung phrase with natural vibrato
        Voice voice(sr, 3);
        Melody m { { 220.0, 246.9, 277.2, 293.7, 329.6, 293.7, 277.2, 246.9 }, 0.8, 5.5, 40.0, 0.3 };
        std::mt19937 rng(5); std::normal_distribution<float> n(0.0f, 0.002f);
        measure("vocal vibrato", [&](double t, float& l, float& r) {
            double nt; const double f0 = m.f0At(t, nt);
            l = r = static_cast<float>(0.15 * noteEnvelope(nt, m.noteSec) * voice.next(f0)) + n(rng); });
    }
    {   // Long straight-tone notes (no vibrato), high voice
        Voice voice(sr, 4);
        Melody m { { 392.0, 440.0, 523.3, 587.3 }, 2.0, 5.0, 0.0, 0.3 };
        std::mt19937 rng(6); std::normal_distribution<float> n(0.0f, 0.002f);
        measure("vocal straight", [&](double t, float& l, float& r) {
            double nt; const double f0 = m.f0At(t, nt);
            l = r = static_cast<float>(0.15 * noteEnvelope(nt, m.noteSec) * voice.next(f0)) + n(rng); });
    }
    {   // Flute-like: strong fundamental, weak 2nd/3rd, breath noise, light vibrato
        Melody m { { 523.3, 587.3, 659.3, 784.0, 880.0 }, 1.5, 4.5, 12.0, 0.4 };
        std::mt19937 rng(8); std::normal_distribution<float> n(0.0f, 0.004f);
        double ph = 0, drift = 0; std::normal_distribution<double> g(0, 1);
        measure("flute-like", [&](double t, float& l, float& r) {
            double nt; double f0 = m.f0At(t, nt);
            drift = 0.9999 * drift + 0.00003 * g(rng); // ~0.2 % std
            ph += twoPi * f0 * (1 + drift) / sr;
            const double env = noteEnvelope(nt, m.noteSec) * (1.0 + 0.05 * std::sin(twoPi * 3.1 * t));
            const double s = env * (0.2 * std::sin(ph) + 0.05 * std::sin(2 * ph) + 0.02 * std::sin(3 * ph));
            l = r = static_cast<float>(s) + n(rng); });
    }
    {   // Electric-piano/organ chords: 3 notes with 6 harmonics each, 2 s sustain, no vibrato
        const std::vector<std::vector<double>> chords { { 261.6, 329.6, 392.0 }, { 220.0, 261.6, 329.6 },
                                                        { 174.6, 220.0, 261.6 }, { 196.0, 246.9, 293.7 } };
        std::array<double, 3> ph {};
        std::mt19937 rng(9); std::normal_distribution<float> n(0.0f, 0.002f);
        measure("organ chords", [&](double t, float& l, float& r) {
            const int idx = static_cast<int>(t / 2.0) % 4; const double nt = std::fmod(t, 2.0);
            double s = 0;
            for (int k = 0; k < 3; ++k)
            {
                ph[static_cast<size_t>(k)] += twoPi * chords[static_cast<size_t>(idx)][static_cast<size_t>(k)] / sr;
                for (int h = 1; h <= 6; ++h) s += 0.04 / h * std::sin(h * ph[static_cast<size_t>(k)]);
            }
            l = r = static_cast<float>(s * noteEnvelope(nt, 2.0)) + n(rng); });
    }
    {   // Pink-ish band noise (applause/cymbal-like) + speech-like voice without vibrato, short syllables
        Voice voice(sr, 10);
        Melody m { { 150.0, 170.0, 140.0, 160.0, 180.0, 130.0 }, 0.22, 5.0, 0.0, 1.0 };
        std::mt19937 rng(12); std::normal_distribution<float> n(0.0f, 0.01f);
        float lp = 0;
        measure("speech+noise", [&](double t, float& l, float& r) {
            double nt; const double f0 = m.f0At(t, nt);
            lp = 0.9f * lp + 0.1f * n(rng);
            l = r = static_cast<float>(0.12 * noteEnvelope(nt, m.noteSec) * voice.next(f0 * (1.0 - 0.3 * nt))) + lp + 0.2f * n(rng); });
    }
    return out;
}

// ---------------------------------------------------------------------------
// 3. Frequency estimation error (steady tone in noise; error of every event)
// ---------------------------------------------------------------------------
void benchFrequencyError(DetectionMode mode, double& meanHz, double& maxHz, double& meanCents, int& detectedCount, int total)
{
    const double sr = 48000;
    std::mt19937 pick(1234);
    std::uniform_real_distribution<double> logF(std::log(200.0), std::log(8000.0));
    double sum = 0, sumC = 0; maxHz = 0; detectedCount = 0; int n = 0;
    for (int i = 0; i < total; ++i)
    {
        const double f = std::exp(logF(pick));
        std::mt19937 rng(100 + i); std::normal_distribution<float> noise(0.0f, 0.001f);
        double ph = 0;
        Generator gen = [&](double t, float& l, float& r) {
            ph += twoPi * f / sr;
            const double level = std::min(0.05, 0.002 * std::pow(10.0, 1.0 * t)); // +20 dB/s build-up
            l = r = static_cast<float>(level * std::sin(ph)) + noise(rng); };
        const auto events = runDetector(sr, mode, DetectionProfile::Flat1OverF, 3.0, gen);
        bool any = false;
        for (const auto& e : events)
        {
            if (std::abs(e.freq - f) > 50) continue;
            any = true;
            const double err = std::abs(e.freq - f);
            sum += err; sumC += std::abs(1200.0 * std::log2(e.freq / f)); ++n;
            maxHz = std::max(maxHz, err);
        }
        if (any) ++detectedCount;
    }
    meanHz = n ? sum / n : -1;
    meanCents = n ? sumC / n : -1;
}

// ---------------------------------------------------------------------------
// 4. CPU per audio second
// ---------------------------------------------------------------------------
void benchCpu(DetectionMode mode, double& detectorMsPerSec, double& filterMsPerSec)
{
    const double sr = 48000, dur = 20.0;
    Voice voice(sr, 21);
    Melody m { { 220.0, 246.9, 277.2, 293.7 }, 0.8, 5.5, 40.0, 0.3 };
    std::mt19937 rng(22); std::normal_distribution<float> n(0.0f, 0.01f);
    Generator gen = [&](double t, float& l, float& r) {
        double nt; const double f0 = m.f0At(t, nt);
        l = static_cast<float>(0.15 * voice.next(f0)) + n(rng); r = l * 0.9f + n(rng); };
    double cpu = 0;
    runDetector(sr, mode, DetectionProfile::Speech, dur, gen, &cpu);
    detectorMsPerSec = cpu * 1000.0 / dur;

    FilterManager manager;
    manager.prepare(sr);
    for (int i = 0; i < 12; ++i)
        manager.addManualFilter(300.0f + 517.0f * static_cast<float>(i), -9.0f, 40.0f, true);
    const int blockSize = 256;
    juce::AudioBuffer<float> block(2, blockSize);
    double fcpu = 0;
    std::mt19937 rng2(3); std::uniform_real_distribution<float> u(-0.1f, 0.1f);
    for (int start = 0; start < static_cast<int>(dur * sr); start += blockSize)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < blockSize; ++i) block.setSample(ch, i, u(rng2));
        const auto t0 = std::chrono::steady_clock::now();
        manager.processAudio(block);
        fcpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        manager.updateTimers(static_cast<float>(blockSize / sr));
    }
    filterMsPerSec = fcpu * 1000.0 / dur;
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    std::printf("== Feedback Suppressor benchmark ==\n");
    for (auto mode : { DetectionMode::Advanced, DetectionMode::Aggressive, DetectionMode::Standard })
    {
        double meanMs, maxMs, errC, perRate[3]; int missed, other;
        benchDetectionTime(mode, meanMs, maxMs, missed, errC, other, perRate);
        std::printf("[%s] build-up detection (18 cases: 6 freqs x 10/30/100 dB/s): mean=%.0f ms max=%.0f ms missed=%d "
                    "first-event error=%.2f cents, unrelated events=%d\n", modeName(mode), meanMs, maxMs, missed, errC, other);
        std::printf("[%s]   per growth rate: 10 dB/s=%.0f ms, 30 dB/s=%.0f ms, 100 dB/s=%.0f ms (onset level -60 dBFS)\n",
                    modeName(mode), perRate[0], perRate[1], perRate[2]);
        double total = 0;
        for (const auto& fp : benchFalsePositives(mode, DetectionProfile::Speech))
        {
            std::printf("[%s] false positives  %-16s %6.1f events/min\n", modeName(mode), fp.name.c_str(), fp.perMinute);
            total += fp.perMinute;
        }
        std::printf("[%s] false positives  TOTAL            %6.1f events/min\n", modeName(mode), total);
        double mHz, xHz, mC; int det;
        const int nf = quick ? 10 : 30;
        benchFrequencyError(mode, mHz, xHz, mC, det, nf);
        std::printf("[%s] frequency error: mean=%.3f Hz (%.2f cents) max=%.3f Hz, tones detected %d/%d\n",
                    modeName(mode), mHz, mC, xHz, det, nf);
        double dMs, fMs;
        benchCpu(mode, dMs, fMs);
        std::printf("[%s] CPU per audio second: detector=%.2f ms, 12-notch stereo cascade=%.2f ms\n", modeName(mode), dMs, fMs);
    }
    return 0;
}
