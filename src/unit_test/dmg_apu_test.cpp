#include "dmg_apu.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

using namespace LOTUSGB;

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testPulseFrequencyLowRegistersAreWriteOnly() {
    DmgApu apu;
    apu.write(0xFF13, 0x34);
    apu.write(0xFF18, 0x56);
    expect(apu.read(0xFF13) == 0xFF,
           "NR13 reads as all ones despite retaining its written frequency");
    expect(apu.read(0xFF18) == 0xFF,
           "NR23 reads as all ones despite retaining its written frequency");
}

void testChannelOneProducesStereoSamplesAtConfiguredRate() {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    apu.write(0xFF11, 0x80); // 50% duty
    apu.write(0xFF12, 0xF0); // DAC on, volume 15
    apu.write(0xFF13, 0x00);
    apu.write(0xFF14, 0x86); // trigger, frequency 1536
    apu.write(0xFF24, 0x77); // full left/right volume
    apu.write(0xFF25, 0x11); // route channel 1 to both outputs

    apu.advanceTCycles(50000);
    const auto samples = apu.takeAudioSamples();
    expect(samples.size() == 572 * 2,
           "APU emits the expected number of interleaved stereo samples");

    bool hasNonzeroSample = false;
    for (size_t i = 0; i < samples.size(); i += 2) {
        expect(samples[i] == samples[i + 1],
               "equal channel routing produces matching left and right samples");
        hasNonzeroSample |= samples[i] != 0;
    }
    expect(hasNonzeroSample, "triggered pulse channel produces audible PCM");
}

void testChannelTwoProducesAudioOnItsIndependentStereoRoute() {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    apu.write(0xFF16, 0x40); // 50% duty
    apu.write(0xFF17, 0xF0); // DAC on, volume 15
    apu.write(0xFF18, 0x00);
    apu.write(0xFF19, 0x86); // trigger, frequency 1536
    apu.write(0xFF24, 0x70); // maximum left, zero right volume
    apu.write(0xFF25, 0x20); // route channel 2 to the left output

    expect(apu.read(0xFF26) == 0xF2,
           "NR52 reports channel 2 active without enabling channel 1");
    apu.advanceTCycles(50000);
    const auto samples = apu.takeAudioSamples();

    bool hasLeftAudio = false;
    for (size_t i = 0; i < samples.size(); i += 2) {
        hasLeftAudio |= samples[i] != 0;
        expect(samples[i + 1] == 0,
               "channel 2 does not leak into the unrouted right output");
    }
    expect(hasLeftAudio, "triggered channel 2 produces audible PCM");

    apu.write(0xFF17, 0x00); // Disable channel 2 DAC.
    expect((apu.read(0xFF26) & 0x02) == 0,
           "disabling channel 2 DAC clears its NR52 status bit");
}

void testWaveChannelReadsWaveRamAndRoutesStereoOutput() {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    for (uint16_t address = 0xFF30; address <= 0xFF3F; ++address)
        apu.write(address, 0xF0);

    expect(apu.read(0xFF30) == 0xF0,
           "wave RAM can be read normally while channel 3 is inactive");
    apu.write(0xFF1A, 0x80); // Enable channel 3 DAC.
    apu.write(0xFF1B, 0x00); // Maximum length.
    apu.write(0xFF1C, 0x20); // Full output level.
    apu.write(0xFF1D, 0xFE);
    apu.write(0xFF1E, 0x87); // Trigger at frequency 2046.
    apu.write(0xFF24, 0x70); // Maximum left, zero right volume.
    apu.write(0xFF25, 0x40); // Route channel 3 to the left output.

    expect(apu.read(0xFF26) == 0xF4,
           "NR52 reports channel 3 active");
    expect(apu.read(0xFF30) == 0xFF,
           "wave RAM reads are blocked until channel 3 fetches a sample");

    apu.advanceTCycles(9);
    expect(apu.read(0xFF3F) == 0xFF,
           "wave RAM remains blocked until the trigger startup delay expires");
    apu.advanceTCycles(1);
    expect(apu.read(0xFF3F) == 0xF0,
           "a wave RAM read on the channel 3 fetch cycle aliases the fetched byte");
    apu.write(0xFF3F, 0x00);
    expect(apu.read(0xFF30) == 0x00,
           "a wave RAM write on the fetch cycle targets the byte selected by channel 3");
    apu.advanceTCycles(1);
    expect(apu.read(0xFF30) == 0xFF,
           "wave RAM access is blocked after the channel 3 fetch cycle");

    apu.advanceTCycles(50000);
    const auto samples = apu.takeAudioSamples();
    bool hasLeftAudio = false;
    bool changesOverTime = false;
    int16_t previousLeft = samples.empty() ? 0 : samples[0];
    for (size_t i = 0; i < samples.size(); i += 2) {
        hasLeftAudio |= samples[i] != 0;
        changesOverTime |= samples[i] != previousLeft;
        previousLeft = samples[i];
        expect(samples[i + 1] == 0,
               "channel 3 does not leak into the unrouted right output");
    }
    expect(hasLeftAudio, "wave channel produces audio from wave RAM samples");
    expect(changesOverTime, "wave channel output follows the stored samples");

    apu.write(0xFF1A, 0x00); // Disable channel 3 DAC.
    expect((apu.read(0xFF26) & 0x04) == 0,
           "disabling channel 3 DAC clears its NR52 status bit");
    expect(apu.read(0xFF30) == 0x00,
           "a same-cycle write updates the channel-selected wave RAM byte");
    expect(apu.read(0xFF31) == 0xF0,
           "the same-cycle write does not use the CPU-requested address");
    apu.write(0xFF30, 0xAA);
    expect(apu.read(0xFF30) == 0xAA,
           "wave RAM is accessible again after channel 3 stops");
}

void setupWaveChannelForRetriggerTest(DmgApu &apu) {
    apu.write(0xFF26, 0x80);
    for (uint16_t address = 0xFF30; address <= 0xFF3F; ++address) {
        const uint8_t nibble = static_cast<uint8_t>(address - 0xFF30);
        apu.write(address, static_cast<uint8_t>((nibble << 4) | nibble));
    }
    apu.write(0xFF1A, 0x80); // Enable channel 3 DAC.
    apu.write(0xFF1D, 0xFE);
    apu.write(0xFF1E, 0x87); // Trigger at frequency 2046.
}

std::array<uint8_t, 16> stopWaveChannelAndReadRam(DmgApu &apu) {
    apu.write(0xFF1A, 0x00);
    std::array<uint8_t, 16> result{};
    for (uint16_t address = 0xFF30; address <= 0xFF3F; ++address)
        result[address - 0xFF30] = apu.read(address);
    return result;
}

void testWaveRamRetriggerCorruptsFirstByteTwoTCyclesBeforeFetchInFirstRow() {
    DmgApu apu;
    setupWaveChannelForRetriggerTest(apu);
    apu.advanceTCycles(24); // Next sample index 5 reads wave RAM byte 2 at cycle 26.
    apu.write(0xFF1E, 0x87); // Retrigger one 2 MHz APU cycle before the read.

    const auto waveRam = stopWaveChannelAndReadRam(apu);
    expect(waveRam[0] == 0x22,
           "a colliding retrigger copies the selected byte into wave RAM byte 0");
    expect(waveRam[1] == 0x11 && waveRam[2] == 0x22,
           "a first-row retrigger leaves the other wave RAM bytes intact");
}

void testWaveRamRetriggerCopiesSelectedAlignedRowToFirstFourBytes() {
    DmgApu apu;
    setupWaveChannelForRetriggerTest(apu);
    apu.advanceTCycles(76); // Sample index 18 reads byte 9 at cycle 78.
    apu.write(0xFF1E, 0x87); // Retrigger one 2 MHz APU cycle before the read.

    const auto waveRam = stopWaveChannelAndReadRam(apu);
    expect(waveRam[0] == 0x88 && waveRam[1] == 0x99 &&
               waveRam[2] == 0xAA && waveRam[3] == 0xBB,
           "a later-row retrigger copies that aligned four-byte row to wave RAM start");
    expect(waveRam[8] == 0x88 && waveRam[9] == 0x99 &&
               waveRam[10] == 0xAA && waveRam[11] == 0xBB,
           "the source row remains unchanged by wave RAM corruption");
}

void testWaveRamRetriggerOneTCycleBeforeFetchDoesNotCorruptRam() {
    DmgApu apu;
    setupWaveChannelForRetriggerTest(apu);
    apu.advanceTCycles(45); // One T-cycle before the sample at index 10 / byte 5.
    apu.write(0xFF1E, 0x87);

    const auto waveRam = stopWaveChannelAndReadRam(apu);
    for (uint8_t i = 0; i < waveRam.size(); ++i) {
        const uint8_t nibble = i;
        expect(waveRam[i] == static_cast<uint8_t>((nibble << 4) | nibble),
               "a retrigger one T-cycle before the read preserves wave RAM");
    }
}

void testWaveRamRetriggerOutsideReadWindowDoesNotCorruptRam() {
    DmgApu apu;
    setupWaveChannelForRetriggerTest(apu);
    apu.advanceTCycles(43); // Three T-cycles before the next sample fetch.
    apu.write(0xFF1E, 0x87);

    const auto waveRam = stopWaveChannelAndReadRam(apu);
    for (uint8_t i = 0; i < waveRam.size(); ++i) {
        const uint8_t nibble = i;
        expect(waveRam[i] == static_cast<uint8_t>((nibble << 4) | nibble),
               "a retrigger outside the wave RAM read window preserves wave RAM");
    }
}

void testWaveRamRetriggerOnFetchCycleDoesNotCorruptRam() {
    DmgApu apu;
    setupWaveChannelForRetriggerTest(apu);
    apu.advanceTCycles(46); // Sample index 10 has just read wave RAM byte 5.
    apu.write(0xFF1E, 0x87);

    const auto waveRam = stopWaveChannelAndReadRam(apu);
    for (uint8_t i = 0; i < waveRam.size(); ++i) {
        const uint8_t nibble = i;
        expect(waveRam[i] == static_cast<uint8_t>((nibble << 4) | nibble),
               "a retrigger on the just-completed read cycle preserves wave RAM");
    }
}

std::vector<int16_t> renderNoiseChannel(uint8_t widthMode, uint8_t clockShift = 0) {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    apu.write(0xFF21, 0xF0); // DAC on, volume 15
    apu.write(0xFF22, static_cast<uint8_t>((clockShift << 4) | widthMode));
    apu.write(0xFF23, 0x80); // trigger channel 4
    apu.write(0xFF24, 0x70); // maximum left, zero right volume
    apu.write(0xFF25, 0x80); // route channel 4 to the left output
    apu.advanceTCycles(50000);
    return apu.takeAudioSamples();
}

void testNoiseChannelUsesIndependentStereoRouteAndStatus() {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    apu.write(0xFF20, 0x3F); // load length counter with one
    apu.write(0xFF21, 0xF0); // DAC on, volume 15
    apu.write(0xFF22, 0x00); // 15-bit mode, fastest divider
    apu.write(0xFF23, 0x80); // trigger channel 4
    apu.write(0xFF24, 0x70); // maximum left, zero right volume
    apu.write(0xFF25, 0x80); // route only channel 4 to the left output

    expect(apu.read(0xFF20) == 0xFF,
           "channel 4 length register is write-only");
    expect(apu.read(0xFF21) == 0xF0 && apu.read(0xFF22) == 0x00,
           "channel 4 envelope and polynomial registers read back their values");
    expect(apu.read(0xFF23) == 0xBF,
           "channel 4 trigger is write-only while length enable reads back");
    expect(apu.read(0xFF26) == 0xF8,
           "NR52 reports channel 4 active");

    apu.advanceTCycles(50000);
    const auto samples = apu.takeAudioSamples();
    bool hasLeftAudio = false;
    bool changesOverTime = false;
    int16_t previousLeft = samples.empty() ? 0 : samples[0];
    for (size_t i = 0; i < samples.size(); i += 2) {
        hasLeftAudio |= samples[i] != 0;
        changesOverTime |= samples[i] != previousLeft;
        previousLeft = samples[i];
        expect(samples[i + 1] == 0,
               "channel 4 does not leak into the unrouted right output");
    }
    expect(hasLeftAudio, "triggered noise channel produces audible PCM");
    expect(changesOverTime, "noise channel output changes as its LFSR advances");

    apu.write(0xFF21, 0x00); // Disable channel 4 DAC.
    expect((apu.read(0xFF26) & 0x08) == 0,
           "disabling channel 4 DAC clears its NR52 status bit");
}

void testNoiseChannelLengthExpiresOnFrameSequencerClock() {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    apu.write(0xFF20, 0x3F); // length counter = 1
    apu.write(0xFF21, 0xF0); // DAC on, volume 15
    apu.write(0xFF22, 0x00);
    apu.write(0xFF23, 0xC0); // trigger with length counting enabled

    expect((apu.read(0xFF26) & 0x08) != 0,
           "channel 4 starts before its length timer expires");
    apu.advanceTCycles(8192);
    expect((apu.read(0xFF26) & 0x08) != 0,
           "APU T-cycles alone do not clock the DIV-driven frame sequencer");
    apu.onDividerApuFallingEdge();
    expect((apu.read(0xFF26) & 0x08) == 0,
           "channel 4 stops when its one-step length counter expires");
}

void testNoiseChannelWidthModeChangesItsLfsrSequence() {
    const auto longSequence = renderNoiseChannel(0x00);
    const auto shortSequence = renderNoiseChannel(0x08);
    expect(longSequence.size() == shortSequence.size() && !longSequence.empty(),
           "both noise LFSR width modes produce equally timed PCM output");

    bool sequencesDiffer = false;
    for (size_t i = 0; i < longSequence.size(); ++i)
        sequencesDiffer |= longSequence[i] != shortSequence[i];
    expect(sequencesDiffer,
           "7-bit mode feeds back into bit 6 and differs from 15-bit mode");
}

void testNoiseChannelStopsClockingAtMaximumShift() {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    apu.write(0xFF21, 0xF0); // DAC on, volume 15
    apu.write(0xFF22, 0xE0); // shift 14 stops LFSR clocks
    apu.write(0xFF23, 0x80); // trigger channel 4
    apu.write(0xFF24, 0x70);
    apu.write(0xFF25, 0x80);

    apu.advanceTCycles(500000);
    const auto samples = apu.takeAudioSamples();
    expect((apu.read(0xFF26) & 0x08) != 0,
           "a stopped LFSR clock does not disable channel 4");
    expect(samples.size() > 64, "stopped noise channel still produces PCM samples");
    const size_t tailStart = samples.size() - 64;
    for (size_t i = tailStart; i < samples.size(); i += 2) {
        expect(samples[i] == 0,
               "constant noise output decays through the output high-pass filter");
        expect(samples[i + 1] == 0,
               "unrouted right output remains silent for stopped noise");
    }
}

void testMaximumChannelOutputLeavesHeadroomInPcmSamples() {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    apu.write(0xFF11, 0x00); // 12.5% duty gives a large DC step between phases
    apu.write(0xFF12, 0xF0); // DAC on, maximum envelope volume
    apu.write(0xFF13, 0x00);
    apu.write(0xFF14, 0x86); // trigger, frequency 1536
    apu.write(0xFF24, 0x77); // maximum output volume
    apu.write(0xFF25, 0x11); // route channel 1 to both outputs

    apu.advanceTCycles(50000);
    const auto samples = apu.takeAudioSamples();
    int peak = 0;
    for (int16_t sample : samples) {
        const int magnitude = sample < 0 ? -static_cast<int>(sample) : sample;
        if (magnitude > peak)
            peak = magnitude;
        expect(sample > -32768 && sample < 32767,
               "maximum channel output does not clip to the int16 limits");
    }
    expect(peak > 1000,
           "maximum channel output remains at a useful level after headroom scaling");
}

void testPowerOffMutesAndIgnoresChannelWrites() {
    DmgApu apu;
    apu.initializeDmgPostBootState();
    apu.write(0xFF26, 0x00);
    apu.write(0xFF12, 0xF0);
    expect((apu.read(0xFF26) & 0x80) == 0,
           "NR52 reports that the APU is powered off");
    expect(apu.read(0xFF12) == 0,
           "channel register writes are ignored while powered off");

    apu.advanceTCycles(50000);
    const auto samples = apu.takeAudioSamples();
    for (int16_t sample : samples)
        expect(sample == 0, "powered-off APU emits silence");
}

void testDmgPostBootStateHasFadedStartupChannel() {
    DmgApu apu;
    apu.initializeDmgPostBootState();

    expect(apu.read(0xFF12) == 0xF3,
           "DMG post-boot NR12 retains its hardware register value");
    expect(apu.read(0xFF14) == 0xBF,
           "DMG post-boot NR14 has length counting disabled");
    expect(apu.read(0xFF26) == 0xF1,
           "DMG post-boot channel 1 remains enabled after its sound fades");

    apu.advanceTCycles(50000);
    const auto samples = apu.takeAudioSamples();
    expect(!samples.empty(), "post-boot APU generates its initial PCM samples");
    for (int16_t sample : samples)
        expect(sample == 0,
               "fast post-boot startup does not replay an aliased boot-sound burst");
    expect((apu.read(0xFF26) & 0x01) != 0,
           "post-boot channel remains enabled with its length counter disabled");
}

void testHighPassOutputDecaysToSilenceAfterChannelStops() {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    apu.write(0xFF11, 0xBE); // 50% duty, length counter = 2
    apu.write(0xFF12, 0xF0); // DAC on, volume 15
    apu.write(0xFF13, 0xFF);
    apu.write(0xFF14, 0xC7); // trigger, enable length, frequency 2047
    apu.write(0xFF24, 0x77); // full left/right volume
    apu.write(0xFF25, 0x11); // route channel 1 to both outputs

    apu.advanceTCycles(8192);
    apu.onDividerApuFallingEdge(); // first length clock leaves the channel active
    expect(!apu.takeAudioSamples().empty(),
           "channel produces samples before being stopped");
    expect((apu.read(0xFF26) & 0x01) != 0,
           "channel remains active after its first length clock");

    apu.advanceTCycles(16384);
    apu.onDividerApuFallingEdge();
    apu.onDividerApuFallingEdge(); // sequencer steps 1 and 2; step 2 stops the channel
    expect((apu.read(0xFF26) & 0x01) == 0,
           "length counter stops the channel while its DAC remains enabled");

    apu.advanceTCycles(500000);
    const auto samples = apu.takeAudioSamples();
    expect(samples.size() >= 64, "enough silent tail samples were generated");
    for (auto it = samples.end() - 64; it != samples.end(); ++it)
        expect(*it == 0,
               "high-pass filter settles to digital silence after channel stops");
}

void testAllChannelDacsOffMuteOutputImmediately() {
    DmgApu apu;
    apu.write(0xFF26, 0x80);
    apu.write(0xFF11, 0x80); // 50% duty
    apu.write(0xFF12, 0xF0); // DAC on, volume 15
    apu.write(0xFF13, 0x00);
    apu.write(0xFF14, 0x86); // trigger, frequency 1536
    apu.write(0xFF24, 0x77); // full left/right volume
    apu.write(0xFF25, 0x11); // route channel 1 to both outputs

    apu.advanceTCycles(50000);
    const auto playingSamples = apu.takeAudioSamples();
    bool hasSound = false;
    for (int16_t sample : playingSamples)
        hasSound |= sample != 0;
    expect(hasSound, "channel output is audible before its DAC is disabled");

    apu.write(0xFF12, 0x00); // Disable the only enabled DAC.
    apu.advanceTCycles(50000);
    const auto mutedSamples = apu.takeAudioSamples();
    expect(!mutedSamples.empty(), "audio samples continue after DAC shutdown");
    for (int16_t sample : mutedSamples)
        expect(sample == 0,
               "all DACs off disconnects the output without a filter tail");
}

}

int main() {
    testPulseFrequencyLowRegistersAreWriteOnly();
    testChannelOneProducesStereoSamplesAtConfiguredRate();
    testChannelTwoProducesAudioOnItsIndependentStereoRoute();
    testWaveChannelReadsWaveRamAndRoutesStereoOutput();
    testWaveRamRetriggerCorruptsFirstByteTwoTCyclesBeforeFetchInFirstRow();
    testWaveRamRetriggerCopiesSelectedAlignedRowToFirstFourBytes();
    testWaveRamRetriggerOneTCycleBeforeFetchDoesNotCorruptRam();
    testWaveRamRetriggerOutsideReadWindowDoesNotCorruptRam();
    testWaveRamRetriggerOnFetchCycleDoesNotCorruptRam();
    testNoiseChannelUsesIndependentStereoRouteAndStatus();
    testNoiseChannelLengthExpiresOnFrameSequencerClock();
    testNoiseChannelWidthModeChangesItsLfsrSequence();
    testNoiseChannelStopsClockingAtMaximumShift();
    testMaximumChannelOutputLeavesHeadroomInPcmSamples();
    testPowerOffMutesAndIgnoresChannelWrites();
    testDmgPostBootStateHasFadedStartupChannel();
    testHighPassOutputDecaysToSilenceAfterChannelStops();
    testAllChannelDacsOffMuteOutputImmediately();
    std::cout << "DMG APU tests passed\n";
}
