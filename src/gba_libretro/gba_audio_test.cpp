#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "gba_system.hpp"

namespace {

using LOTUSGBA::GbaSystem;

constexpr uint32_t SOUNDBIAS = 0x04000088;
constexpr uint32_t SOUNDCNT_L = 0x04000080;
constexpr uint32_t SOUNDCNT_H = 0x04000082;
constexpr uint32_t SOUNDCNT_X = 0x04000084;
constexpr uint32_t SOUND3CNT_L = 0x04000070;
constexpr uint32_t SOUND3CNT_H = 0x04000072;
constexpr uint32_t SOUND3CNT_X = 0x04000074;
constexpr uint32_t WAVE_RAM = 0x04000090;
constexpr uint32_t FIFO_A = 0x040000A0;
constexpr uint32_t FIFO_B = 0x040000A4;

std::vector<uint8_t> readAudioCheckRom() {
    std::ifstream file("../../tests/roms/gba-audio-check/audio-check.gba",
                      std::ios::binary);
    assert(file);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                 std::istreambuf_iterator<char>());
}

std::vector<uint8_t> readDirectSoundCheckRom() {
    std::ifstream file(
        "../../tests/roms/gba-audio-check/direct-sound-check.gba",
        std::ios::binary);
    assert(file);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                 std::istreambuf_iterator<char>());
}

std::vector<int16_t> renderTone(uint16_t soundBias,
                                unsigned framesToCapture = 1) {
    GbaSystem system(readAudioCheckRom());
    assert(system.read16(SOUNDBIAS) == 0x0200);
    assert(system.runFrame().success); // ROM initializes the 440 Hz tone.
    system.takeAudioSamples();

    system.write16(SOUNDBIAS, soundBias);
    assert(system.read16(SOUNDBIAS) == soundBias);
    system.takeAudioSamples(); // Ignore the byte-wise transition on the bus.
    assert(system.runFrame().success);
    system.takeAudioSamples(); // Let the new sample cadence replace held PCM.

    std::vector<int16_t> captured;
    for (unsigned frame = 0; frame < framesToCapture; ++frame) {
        assert(system.runFrame().success);
        const std::vector<int16_t> frameSamples = system.takeAudioSamples();
        captured.insert(captured.end(), frameSamples.begin(), frameSamples.end());
    }
    return captured;
}

void assertToneIsApproximately440Hz(const std::vector<int16_t> &samples) {
    bool hasPositiveSample = false;
    bool hasNegativeSample = false;
    int peak = 0;
    for (size_t index = 0; index + 1 < samples.size(); index += 2) {
        assert(samples[index] == samples[index + 1]);
        hasPositiveSample |= samples[index] > 0;
        hasNegativeSample |= samples[index] < 0;
        peak = std::max(peak, std::abs(static_cast<int>(samples[index])));
    }
    assert(hasPositiveSample && hasNegativeSample && peak != 0);

    // Ignore low-amplitude filter ringing around a square wave's edge.
    const int crossingThreshold = std::max(1, peak / 20);
    bool wasBelowThreshold = false;
    size_t risingEdgeCount = 0;
    for (size_t index = 0; index + 1 < samples.size(); index += 2) {
        if (samples[index] <= -crossingThreshold)
            wasBelowThreshold = true;
        else if (wasBelowThreshold && samples[index] >= crossingThreshold) {
            ++risingEdgeCount;
            wasBelowThreshold = false;
        }
    }
    assert(risingEdgeCount != 0);
    const size_t monoFrameCount = samples.size() / 2;
    constexpr size_t sampleRate = 48000;
    assert(risingEdgeCount * sampleRate >= monoFrameCount * 420);
    assert(risingEdgeCount * sampleRate <= monoFrameCount * 460);
}

void testLicensedHomebrewToneIsDeterministicAndStereo() {
    constexpr unsigned capturedFrames = 4;
    const auto firstRun = renderTone(0x0200, capturedFrames);
    const auto secondRun = renderTone(0x0200, capturedFrames);
    assert(firstRun == secondRun);
    assert(firstRun.size() >= capturedFrames * 800 * 2 &&
           firstRun.size() <= capturedFrames * 804 * 2);
    assertToneIsApproximately440Hz(firstRun);
}

void testSoundBiasClipsTheMixedOutputRange() {
    const auto zeroBias = renderTone(0x0000);
    const auto maximumBias = renderTone(0x03FE);
    assert(!zeroBias.empty() && !maximumBias.empty());

    bool zeroBiasHasOutput = false;
    for (int16_t sample : zeroBias) {
        assert(sample >= 0);
        zeroBiasHasOutput |= sample > 0;
    }
    assert(zeroBiasHasOutput);

    for (int16_t sample : maximumBias)
        assert(sample <= 48); // Positive overflow clips at 10-bit DAC max.
}

void testSoundBiasResolutionChangesTheSamplingCadence() {
    const auto defaultRate = renderTone(0x0200, 4);
    for (unsigned resolution = 1; resolution < 4; ++resolution) {
        const auto samples = renderTone(
            static_cast<uint16_t>(0x0200 | (resolution << 14)), 4);
        assert(samples.size() >= 4 * 800 * 2 && samples.size() <= 4 * 804 * 2);
        assert(defaultRate.size() + 2 >= samples.size() &&
               samples.size() + 2 >= defaultRate.size());
        assert(samples != defaultRate);
        assertToneIsApproximately440Hz(samples);
    }
}

std::vector<int16_t> renderConstantDirectSound(uint8_t value,
                                               unsigned resolution) {
    GbaSystem system(readAudioCheckRom());
    assert(system.runFrame().success);
    system.takeAudioSamples();

    system.write16(SOUNDCNT_L, 0); // Silence the homebrew PSG tone.
    system.write16(SOUNDCNT_H, 0x0104); // FIFO A right, full volume.
    system.write16(SOUNDBIAS, static_cast<uint16_t>(
        0x0200 | (resolution << 14)));
    const uint32_t fifoWord = static_cast<uint32_t>(value) * 0x01010101u;
    for (unsigned word = 0; word < 8; ++word)
        system.write32(FIFO_A, fifoWord);
    system.write16(0x04000100, 0xFFF0); // Keep the last FIFO sample steady.
    system.write16(0x04000102, 0x0080);
    system.takeAudioSamples();

    std::vector<int16_t> captured;
    for (unsigned frame = 0; frame < 4; ++frame) {
        assert(system.runFrame().success);
        const auto samples = system.takeAudioSamples();
        captured.insert(captured.end(), samples.begin(), samples.end());
    }
    return captured;
}

void testSoundBiasSelectsPwmAmplitudeResolution() {
    constexpr int16_t expectedRightOutput[] = {576, 576, 384, 0};
    constexpr size_t firstFrameSamples = 804 * 2;
    for (unsigned resolution = 0; resolution < 4; ++resolution) {
        const auto samples = renderConstantDirectSound(3, resolution);
        assert(samples.size() > firstFrameSamples);

        int64_t sum = 0;
        size_t count = 0;
        for (size_t i = firstFrameSamples + 1; i < samples.size(); i += 2) {
            sum += samples[i];
            ++count;
        }
        assert(count != 0);
        const int64_t average = sum / static_cast<int64_t>(count);
        assert(std::abs(average - expectedRightOutput[resolution]) <= 2);
    }
}

void testSoundBiasRegisterMasksUnusedBits() {
    GbaSystem system(readAudioCheckRom());
    system.write16(SOUNDBIAS, 0xFFFF);
    assert(system.read16(SOUNDBIAS) == 0xC3FE);
}

void testPsgPanRegisterCanRouteOneChannelToOneSide() {
    GbaSystem system(readAudioCheckRom());
    assert(system.runFrame().success);
    system.takeAudioSamples();

    system.write8(SOUNDCNT_L + 1, 0x10); // PSG channel 1 to left only.
    system.takeAudioSamples();
    assert(system.runFrame().success);
    const auto samples = system.takeAudioSamples();

    bool leftHasSignal = false;
    constexpr size_t filterSettlingFrames = 256;
    for (size_t index = filterSettlingFrames * 2;
         index + 1 < samples.size(); index += 2) {
        leftHasSignal |= samples[index] != 0;
        assert(samples[index + 1] == 0);
    }
    assert(leftHasSignal);
}

void testDirectSoundAAndBRouteToSeparateSides() {
    GbaSystem system(readAudioCheckRom());
    assert(system.runFrame().success);
    system.takeAudioSamples();

    system.write8(SOUNDCNT_L + 1, 0); // Silence the PSG channels.
    system.write16(SOUNDCNT_H, 0x2100); // FIFO A right, FIFO B left.
    system.write32(FIFO_A, 0x40404040);
    system.write32(FIFO_B, 0x20202020);
    system.write16(0x04000100, 0xF000);
    system.write16(0x04000102, 0x0080); // Timer 0 clocks both FIFOs.
    system.takeAudioSamples();

    assert(system.runFrame().success);
    const auto samples = system.takeAudioSamples();
    assert(!samples.empty());
    bool leftHasSignal = false;
    bool rightHasSignal = false;
    bool sidesDiffer = false;
    for (size_t index = 0; index + 1 < samples.size(); index += 2) {
        leftHasSignal |= samples[index] != 0;
        rightHasSignal |= samples[index + 1] != 0;
        sidesDiffer |= samples[index] != samples[index + 1];
    }
    assert(leftHasSignal && rightHasSignal && sidesDiffer);
}

void testLicensedHomebrewUsesTimerDmaForStereoDirectSound() {
    GbaSystem system(readDirectSoundCheckRom());
    assert(system.runFrame().success);
    system.takeAudioSamples();

    std::vector<int16_t> samples;
    for (unsigned frame = 0; frame < 4; ++frame) {
        assert(system.runFrame().success);
        const std::vector<int16_t> frameSamples = system.takeAudioSamples();
        samples.insert(samples.end(), frameSamples.begin(), frameSamples.end());
    }
    assert(!samples.empty());

    bool leftHasLargerMagnitude = false;
    bool rightHasExpectedSignal = false;
    bool rightHasNegativeSignal = false;
    size_t rightRisingEdges = 0;
    for (size_t index = 0; index + 1 < samples.size(); index += 2) {
        // The ROM alternates signed samples ±64 on FIFO B (left) and ±32 on
        // FIFO A (right) using timer-triggered repeat DMA.
        leftHasLargerMagnitude |=
            std::abs(static_cast<int>(samples[index])) >
            std::abs(static_cast<int>(samples[index + 1]));
        rightHasExpectedSignal |= samples[index + 1] > 0;
        rightHasNegativeSignal |= samples[index + 1] < 0;
        if (index >= 2 && samples[index - 1] <= 0 && samples[index + 1] > 0)
            ++rightRisingEdges;
    }
    assert(leftHasLargerMagnitude && rightHasExpectedSignal &&
           rightHasNegativeSignal && rightRisingEdges != 0);

    const size_t frameCount = samples.size() / 2;
    constexpr size_t sampleRate = 48000;
    // Timer 0 reloads at 0xF000, producing 4096 samples/second. Alternating
    // signs yield a 2048 Hz rising-edge rate on Direct Sound A.
    assert(rightRisingEdges * sampleRate >= frameCount * 1950);
    assert(rightRisingEdges * sampleRate <= frameCount * 2150);
}

std::vector<int16_t> renderDirectSoundAtVolume(uint16_t soundControlHigh) {
    GbaSystem system(readDirectSoundCheckRom());
    assert(system.runFrame().success); // ROM initializes the FIFOs and timer.
    system.takeAudioSamples();

    system.write16(SOUNDCNT_H, soundControlHigh);
    system.takeAudioSamples(); // Ignore the register-write transition.
    assert(system.runFrame().success);
    return system.takeAudioSamples();
}

void testDirectSoundVolumeBitsSelectHalfOrFullVolume() {
    const auto halfVolume = renderDirectSoundAtVolume(0x2100);
    const auto fullVolume = renderDirectSoundAtVolume(0x210C);
    assert(!halfVolume.empty() && halfVolume.size() == fullVolume.size());

    int halfLeftPeak = 0;
    int halfRightPeak = 0;
    int fullLeftPeak = 0;
    int fullRightPeak = 0;
    for (size_t index = 0; index + 1 < halfVolume.size(); index += 2) {
        halfLeftPeak = std::max(halfLeftPeak,
                                std::abs(static_cast<int>(halfVolume[index])));
        halfRightPeak = std::max(
            halfRightPeak, std::abs(static_cast<int>(halfVolume[index + 1])));
        fullLeftPeak = std::max(fullLeftPeak,
                                std::abs(static_cast<int>(fullVolume[index])));
        fullRightPeak = std::max(
            fullRightPeak, std::abs(static_cast<int>(fullVolume[index + 1])));
    }
    assert(halfLeftPeak > 0 && halfRightPeak > 0);
    assert(fullLeftPeak == halfLeftPeak * 2);
    assert(fullRightPeak == halfRightPeak * 2);
}

std::vector<int16_t> renderGbaWave(bool dimension64, bool force75Percent) {
    GbaSystem system(readAudioCheckRom());
    assert(system.runFrame().success);
    system.takeAudioSamples();

    system.write8(0x04000063, 0); // Silence the ROM's PSG channel 1.
    system.write16(SOUNDCNT_L, 0x4400); // Route channel 3 to both sides.
    system.write16(SOUNDCNT_H, 0x0002); // Full PSG volume.
    system.write16(SOUNDCNT_X, 0x0080); // Enable the PSG.

    system.write8(SOUND3CNT_L, 0x00); // Bank 0 plays; writes address bank 1.
    for (unsigned i = 0; i < 16; ++i)
        system.write8(WAVE_RAM + i, 0x0F);
    system.write8(SOUND3CNT_L, 0x40); // Bank 1 plays; writes address bank 0.
    for (unsigned i = 0; i < 16; ++i)
        system.write8(WAVE_RAM + i, 0xF0);

    const uint8_t control = static_cast<uint8_t>(
        0x80 | (dimension64 ? 0x20 : 0));
    system.write8(SOUND3CNT_L, control);
    assert(system.read8(WAVE_RAM) == 0x0F);
    assert(system.read8(SOUND3CNT_L) == static_cast<uint8_t>(0x1F | control));
    system.write8(SOUND3CNT_L + 1, 0x60);
    assert(system.read8(SOUND3CNT_L + 1) == 0); // Unused high byte.
    system.write8(SOUND3CNT_H, 0); // Maximum duration.
    system.write8(SOUND3CNT_H + 1,
                  static_cast<uint8_t>(0x20 | (force75Percent ? 0x80 : 0)));
    assert(system.read8(SOUND3CNT_H + 1) ==
           static_cast<uint8_t>(0x3F | 0x20 |
                                (force75Percent ? 0x80 : 0)));
    system.write8(SOUND3CNT_X, 0x6C);
    system.write8(SOUND3CNT_X + 1, 0x87); // Trigger at frequency 1900.
    system.takeAudioSamples();

    std::vector<int16_t> captured;
    for (unsigned frame = 0; frame < 3; ++frame) {
        assert(system.runFrame().success);
        const auto samples = system.takeAudioSamples();
        captured.insert(captured.end(), samples.begin(), samples.end());
    }
    return captured;
}

void testGbaWaveChannelUsesTwoBanksIn64StepMode() {
    const auto oneBank = renderGbaWave(false, false);
    const auto twoBanks = renderGbaWave(true, false);
    assert(!oneBank.empty() && oneBank.size() == twoBanks.size());
    assert(oneBank != twoBanks);
}

void testGbaWaveForceVolumeSelects75Percent() {
    const auto fullVolume = renderGbaWave(false, false);
    const auto forced75Percent = renderGbaWave(false, true);
    assert(!fullVolume.empty() && fullVolume.size() == forced75Percent.size());

    uint64_t fullEnergy = 0;
    uint64_t reducedEnergy = 0;
    const size_t firstFrameSamples = 804 * 2;
    for (size_t i = firstFrameSamples; i + 1 < fullVolume.size(); i += 2) {
        fullEnergy += static_cast<uint64_t>(
            std::abs(static_cast<int>(fullVolume[i])));
        reducedEnergy += static_cast<uint64_t>(
            std::abs(static_cast<int>(forced75Percent[i])));
    }
    assert(fullEnergy > 0 && reducedEnergy > 0);
    const double ratio = static_cast<double>(reducedEnergy) /
                         static_cast<double>(fullEnergy);
    assert(ratio > 0.70 && ratio < 0.80);
}

std::vector<int16_t> renderHighResolutionPulse(uint16_t frequency) {
    GbaSystem system(readAudioCheckRom());
    assert(system.runFrame().success);
    system.takeAudioSamples();

    system.write16(SOUNDBIAS, 0xC200); // 262.144 kHz mixer output cadence.
    system.write16(SOUNDCNT_L, 0x1111); // Channel 1, both sides, low master gain.
    system.write16(SOUNDCNT_H, 0x0002); // Full PSG volume.
    system.write16(SOUNDCNT_X, 0x0080);
    system.write8(0x04000062, 0x80); // Channel 1, 50% duty.
    system.write8(0x04000063, 0xF0); // DAC enabled at maximum envelope volume.
    system.write8(0x04000064, static_cast<uint8_t>(frequency));
    system.write8(0x04000065,
                  static_cast<uint8_t>(0x80 | ((frequency >> 8) & 7)));
    system.takeAudioSamples();

    std::vector<int16_t> captured;
    for (unsigned frame = 0; frame < 4; ++frame) {
        assert(system.runFrame().success);
        const auto samples = system.takeAudioSamples();
        captured.insert(captured.end(), samples.begin(), samples.end());
    }
    return captured;
}

double steadyStateRms(const std::vector<int16_t> &samples) {
    constexpr size_t firstFrameSamples = 804 * 2;
    uint64_t energy = 0;
    size_t count = 0;
    for (size_t i = firstFrameSamples; i < samples.size(); i += 2) {
        const int value = samples[i];
        energy += static_cast<uint64_t>(value * value);
        ++count;
    }
    assert(count != 0);
    return std::sqrt(static_cast<double>(energy) /
                     static_cast<double>(count));
}

void testHighResolutionOutputSuppressesOutOfBandTonesBeforeDownsampling() {
    const double audibleTone = steadyStateRms(renderHighResolutionPulse(2032));
    const double outOfBandTone = steadyStateRms(renderHighResolutionPulse(2044));
    assert(audibleTone > 100.0);
    assert(outOfBandTone < audibleTone * 0.35);
}

} // namespace

int main() {
    testLicensedHomebrewToneIsDeterministicAndStereo();
    testSoundBiasClipsTheMixedOutputRange();
    testSoundBiasResolutionChangesTheSamplingCadence();
    testSoundBiasSelectsPwmAmplitudeResolution();
    testSoundBiasRegisterMasksUnusedBits();
    testPsgPanRegisterCanRouteOneChannelToOneSide();
    testDirectSoundAAndBRouteToSeparateSides();
    testLicensedHomebrewUsesTimerDmaForStereoDirectSound();
    testDirectSoundVolumeBitsSelectHalfOrFullVolume();
    testGbaWaveChannelUsesTwoBanksIn64StepMode();
    testGbaWaveForceVolumeSelects75Percent();
    testHighResolutionOutputSuppressesOutOfBandTonesBeforeDownsampling();
}
