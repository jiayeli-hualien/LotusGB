#include "dmg_apu.hpp"

#include <array>
#include <algorithm>
#include <cmath>
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

void testCgbActiveWaveRamAliasesWithoutDmgAccessWindow() {
    DmgApu apu(true);
    setupWaveChannelForRetriggerTest(apu);
    apu.advanceTCycles(25); // Byte 2 is selected, outside the DMG fetch window.
    for (uint16_t address = 0xFF30; address <= 0xFF3F; ++address)
        expect(apu.read(address) == 0x22,
               "active CGB wave RAM reads alias the selected byte outside a DMG fetch cycle");
    apu.write(0xFF3F, 0xAB);
    const auto ram = stopWaveChannelAndReadRam(apu);
    expect(ram[2] == 0xAB && ram[15] == 0xFF,
           "active CGB wave RAM writes use the channel address, not the requested CPU address");
    apu.write(0xFF1A, 0x80); // DAC enabled, channel still stopped.
    apu.write(0xFF3F, 0x5A);
    expect(apu.read(0xFF3F) == 0x5A && apu.read(0xFF32) == 0xAB,
           "inactive CGB wave RAM uses normal addresses even with the DAC enabled");
}

void testCgbRetriggerPreservesWaveRam() {
    for (unsigned cycles : {24u, 76u}) {
        DmgApu apu(true);
        setupWaveChannelForRetriggerTest(apu);
        apu.advanceTCycles(cycles); // Both DMG first-byte and row-corruption windows.
        apu.write(0xFF1E, 0x87);
        const auto ram = stopWaveChannelAndReadRam(apu);
        for (unsigned i = 0; i < ram.size(); ++i)
            expect(ram[i] == static_cast<uint8_t>(i * 0x11),
                   "CGB CH3 retrigger does not apply DMG wave RAM corruption");
    }
}

void testModelSpecificPoweredOffLengthRegisters() {
    for (bool cgb : {false, true}) {
        for (bool writeBeforePowerOff : {false, true}) {
            DmgApu apu(cgb);
            apu.write(0xFF26, 0x80);
            if (!writeBeforePowerOff) apu.write(0xFF26, 0);
            apu.write(0xFF11, 0x3F); apu.write(0xFF16, 0x3F);
            apu.write(0xFF1B, 0xFF); apu.write(0xFF20, 0x3F);
            if (writeBeforePowerOff) apu.write(0xFF26, 0);
            apu.write(0xFF26, 0x80);
            apu.write(0xFF12, 0xF0); apu.write(0xFF17, 0xF0);
            apu.write(0xFF1A, 0x80); apu.write(0xFF21, 0xF0);
            apu.write(0xFF14, 0xC0); apu.write(0xFF19, 0xC0);
            apu.write(0xFF1E, 0xC0); apu.write(0xFF23, 0xC0);
            expect((apu.read(0xFF26) & 15) == 15, "all four length-test channels start active");
            apu.onDividerApuFallingEdge();
            expect((apu.read(0xFF26) & 15) == (cgb ? 15 : 0),
                   "CGB clears lengths at power-off and ignores off-state writes; DMG retains writable lengths");
            // CGB's reloaded full length must expire after 64 (CH1/2/4) or
            // 256 (CH3) length clocks, rather than merely surviving one tick.
            if (cgb) {
                for (unsigned n = 1; n < 127; ++n) apu.onDividerApuFallingEdge();
                expect((apu.read(0xFF26) & 15) == 4, "CGB pulse/noise full lengths expire after 64 clocks");
                for (unsigned n = 127; n < 511; ++n) apu.onDividerApuFallingEdge();
                expect((apu.read(0xFF26) & 15) == 0, "CGB wave full length expires after 256 clocks");
            }
        }
    }
}

void testCgbPcmDigitalRegisters() {
    DmgApu dmg, apu(true);
    expect(dmg.read(0xFF76) == 0xFF && dmg.read(0xFF77) == 0xFF,
           "DMG does not expose CGB PCM registers");
    expect(apu.read(0xFF76) == 0 && apu.read(0xFF77) == 0,
           "powered-off CGB PCM registers report zero");
    apu.write(0xFF26, 0x80);
    apu.write(0xFF11, 0x40); apu.write(0xFF12, 0xA0);
    apu.write(0xFF13, 0xFF); apu.write(0xFF14, 0x87);
    apu.write(0xFF16, 0x40); apu.write(0xFF17, 0x50);
    apu.write(0xFF18, 0xFF); apu.write(0xFF19, 0x87);
    apu.advanceTCycles(32); // One complete eight-step duty period.
    expect(apu.read(0xFF76) == 0x5A, "PCM12 packs CH1 low and CH2 high digital amplitude");
    apu.write(0xFF76, 0); apu.write(0xFF77, 0xFF);
    expect(apu.read(0xFF76) == 0x5A && apu.read(0xFF77) == 0,
           "PCM registers are read-only and independent of NR50/NR51 routing");
    apu.write(0xFF24, 0x77); apu.write(0xFF25, 0xFF);
    expect(apu.read(0xFF76) == 0x5A, "mixer volume and panning do not alter PCM12");
    apu.advanceTCycles(4);
    expect(apu.read(0xFF76) == 0, "pulse low steps report digital zero rather than signed mixer levels");
    apu.advanceTCycles(28);
    apu.write(0xFF12, 0);
    expect(apu.read(0xFF76) == 0x50, "disabling CH1 DAC clears only its PCM nibble");
    apu.write(0xFF26, 0);
    expect(apu.read(0xFF76) == 0 && apu.read(0xFF77) == 0,
           "APU shutdown clears both CGB PCM outputs");
}

void testCgbPcmWaveAndNoiseDigitalLevels() {
    DmgApu wave(true);
    wave.write(0xFF26, 0x80); wave.write(0xFF30, 0xD3); wave.write(0xFF31, 0xC7);
    wave.write(0xFF1A, 0x80); wave.write(0xFF1C, 0x20);
    wave.write(0xFF1D, 0xFE); wave.write(0xFF1E, 0x87);
    wave.advanceTCycles(10);
    expect(wave.read(0xFF77) == 3, "PCM34 low nibble exposes CH3 sample buffer");
    wave.advanceTCycles(4);
    expect(wave.read(0xFF77) == 12, "PCM34 follows subsequent wave samples");
    wave.write(0xFF1C, 0x40);
    expect(wave.read(0xFF77) == 6, "PCM34 includes CH3 digital output-level shift");
    wave.write(0xFF1C, 0x60);
    expect(wave.read(0xFF77) == 3, "CH3 quarter level shifts its digital sample twice");
    wave.write(0xFF1C, 0);
    expect(wave.read(0xFF77) == 0, "CH3 mute reports zero digital output");
    wave.write(0xFF1C, 0x20); wave.write(0xFF1A, 0);
    expect(wave.read(0xFF77) == 0, "stopped wave channel does not expose stale buffered samples");

    DmgApu noise(true);
    noise.write(0xFF26, 0x80); noise.write(0xFF21, 0x90);
    noise.write(0xFF22, 0); noise.write(0xFF23, 0x80);
    noise.advanceTCycles(112);
    expect(noise.read(0xFF77) == 0, "noise digital low level is zero before its first high step");
    noise.advanceTCycles(8);
    expect(noise.read(0xFF77) == 0x90, "PCM34 high nibble exposes CH4 envelope at a high LFSR step");
    noise.write(0xFF21, 0);
    expect(noise.read(0xFF77) == 0, "noise DAC shutdown clears its PCM nibble");
}

void testPulseFirstDutyAndRetriggerPhase() {
    for (unsigned duty = 0; duty < 4; ++duty) {
        DmgApu apu(true);
        apu.write(0xFF26, 0x80);
        apu.write(0xFF11, duty << 6); apu.write(0xFF12, 0x50);
        apu.write(0xFF16, duty << 6); apu.write(0xFF17, 0x50);
        apu.write(0xFF13, 0xFF); apu.write(0xFF18, 0xFF);
        apu.write(0xFF14, 0x87); apu.write(0xFF19, 0x87);
        expect(apu.read(0xFF76) == 0, "both pulse channels initially output zero for every duty");
        apu.advanceTCycles(3);
        expect(apu.read(0xFF76) == 0, "first duty output remains suppressed before first timer edge");
        apu.advanceTCycles(29); // A full eight-step period; suppression is gone.
        const uint8_t wrapped = (duty == 1 || duty == 2) ? 0x55 : 0;
        expect(apu.read(0xFF76) == wrapped, "pulse first-step suppression does not recur after waveform wrap");
        apu.write(0xFF26, 0); apu.write(0xFF26, 0x80);
        apu.write(0xFF11, duty << 6); apu.write(0xFF12, 0x50);
        apu.write(0xFF14, 0x87);
        expect(apu.read(0xFF76) == 0, "APU power cycle restores first-duty suppression");
    }
    DmgApu apu(true);
    apu.write(0xFF26, 0x80);
    apu.write(0xFF11, 0x40); apu.write(0xFF12, 0x50);
    apu.write(0xFF16, 0x40); apu.write(0xFF17, 0x50);
    apu.write(0xFF13, 0xFF); apu.write(0xFF18, 0xFF);
    apu.write(0xFF14, 0x87); apu.write(0xFF19, 0x87);
    apu.advanceTCycles(34); // Wrapped step zero; two low countdown bits remain.
    expect(apu.read(0xFF76) == 0x55, "phase test reaches a noninitial high duty step");
    apu.write(0xFF14, 0x87); apu.write(0xFF19, 0x87);
    expect(apu.read(0xFF76) == 0x55, "retrigger preserves duty index and does not re-suppress output");
    apu.advanceTCycles(5);
    expect(apu.read(0xFF76) == 0x55, "retrigger retains timer low bits instead of resetting four-cycle phase");
    apu.advanceTCycles(1);
    expect(apu.read(0xFF76) == 0, "both retained-phase pulse timers advance on the expected edge");
}

void testSilentPulseDividersKeepDutyPhase() {
    constexpr std::array<uint8_t, 4> patterns{0x01, 0x81, 0x87, 0x7E};
    for (unsigned channel = 0; channel < 2; ++channel) {
        const uint16_t base = channel == 0 ? 0xFF11 : 0xFF16;
        const uint8_t mask = 1 << channel;
        for (unsigned duty = 0; duty < 4; ++duty) {
            for (unsigned period : {4u, 12u, 128u, 8192u}) {
                const unsigned frequency = 2048 - period / 4;
                for (unsigned reason = 0; reason < (channel == 0 ? 3u : 2u); ++reason) {
                    for (unsigned gap : {1u, 2u, 3u, 4u, period - 1, period, period + 1, 8 * period + 3}) {
                        DmgApu apu(true); apu.write(0xFF26, 0x80);
                        apu.write(base, (duty << 6) | 63); apu.write(base + 1, 0x50);
                        apu.write(base + 2, frequency & 255);
                        if (reason == 2) apu.write(0xFF10, 0x19);
                        const uint8_t high = frequency >> 8;
                        apu.write(base + 3, 0x80 | (reason == 0 ? 0x40 : 0) | high);
                        apu.advanceTCycles(period + 1);
                        if (reason == 0) apu.onDividerApuFallingEdge();
                        else if (reason == 1) apu.write(base + 1, 0);
                        else apu.write(0xFF10, 0x11); // Clear negate after a subtract calculation.
                        expect(!(apu.read(0xFF26) & mask), "length, DAC and sweep disable the pulse output");
                        apu.advanceTCycles(gap);
                        expect(((apu.read(0xFF76) >> (channel * 4)) & 15) == 0,
                               "a muted pulse remains digitally silent while its divider advances");
                        apu.write(base + 1, 0x50);
                        if (reason == 2) apu.write(0xFF10, 0);
                        apu.write(base + 3, 0x80 | high);
                        unsigned position = ((period + 1 + gap) / period) & 7;
                        const auto expectPhase = [&] {
                            const uint8_t expected = (patterns[duty] & (1 << (7 - position))) ? 5 : 0;
                            expect(((apu.read(0xFF76) >> (channel * 4)) & 15) == expected,
                                   "retrigger retains the golden duty phase reached during the silent interval");
                        };
                        expectPhase();
                        const unsigned remaining = period - ((period + 1 + gap) % period);
                        unsigned reload = period | (remaining & 3);
                        for (unsigned edge = 0; edge < 8; ++edge) {
                            apu.advanceTCycles(reload - 1); expectPhase();
                            apu.advanceTCycles(1); position = (position + 1) & 7; expectPhase();
                            reload = period;
                        }
                    }
                }
            }
        }
    }
}
void testPulseDividerWaitsForFirstTrigger() {
    for (unsigned channel = 0; channel < 2; ++channel) {
        const uint16_t pulse = channel == 0 ? 0xFF11 : 0xFF16;
        DmgApu silent(true); silent.write(0xFF26, 0x80);
        silent.write(pulse, 0xC0); silent.write(pulse + 2, 0xFF); silent.write(pulse + 3, 0x87);
        expect((silent.read(0xFF26) & 3) == 0, "trigger with the DAC off keeps pulse output disabled");
        silent.advanceTCycles(5); silent.write(pulse + 1, 0x50); silent.write(pulse + 3, 0x87);
        expect(silent.read(0xFF76) == (channel == 0 ? 5 : 0x50),
               "a DAC-off trigger still starts the divider and consumes first-duty suppression");
        silent.write(0xFF26, 0); silent.write(0xFF26, 0x80);
        silent.advanceTCycles(19);
        silent.write(pulse, 0xC0); silent.write(pulse + 1, 0x50);
        silent.write(pulse + 2, 0xFF); silent.write(pulse + 3, 0x87);
        expect(silent.read(0xFF76) == 0, "power cycling restores an untriggered divider and suppressed first step");
        silent.advanceTCycles(4);
        expect(silent.read(0xFF76) == (channel == 0 ? 5 : 0x50),
               "power cycling resets silent duty history to the initial position");
        for (unsigned idle : {1u, 4u, 19u, 47u}) {
            DmgApu apu(true); apu.write(0xFF26, 0x80);
            const uint16_t base = channel == 0 ? 0xFF11 : 0xFF16;
            apu.write(base, 0xC0); apu.write(base + 1, 0x50); apu.write(base + 2, 0xFF);
            apu.write(base + 3, 7); // Configure the period without triggering.
            apu.advanceTCycles(idle); apu.write(base + 3, 0x87);
            expect(apu.read(0xFF76) == 0, "an untriggered divider does not advance duty or first-step suppression");
            apu.advanceTCycles(4);
            expect(apu.read(0xFF76) == (channel == 0 ? 5 : 0x50),
                   "first trigger starts at duty step zero, then advances to the first high step");
        }
    }
}

void setupOneTickLengths(DmgApu &apu) {
    apu.write(0xFF26, 0x80);
    apu.write(0xFF11, 63); apu.write(0xFF16, 63);
    apu.write(0xFF1B, 255); apu.write(0xFF20, 63);
    apu.write(0xFF12, 0xF0); apu.write(0xFF17, 0xF0);
    apu.write(0xFF1A, 0x80); apu.write(0xFF21, 0xF0);
    for (auto port : {0xFF14, 0xFF19, 0xFF1E, 0xFF23}) apu.write(port, 0x80);
}

void testExtraLengthEnableClocks() {
    for (bool cgb : {false, true}) {
        for (unsigned phase = 0; phase < 8; ++phase) {
            DmgApu apu(cgb); setupOneTickLengths(apu);
            for (unsigned i = 0; i < phase; ++i) apu.onDividerApuFallingEdge();
            for (auto port : {0xFF14, 0xFF19, 0xFF1E, 0xFF23}) apu.write(port, 0x40);
            expect((apu.read(0xFF26) & 15) == (phase & 1 ? 0 : 15),
                   "length enable clocks immediately only before non-length sequencer steps");
        }
        DmgApu apu(cgb); setupOneTickLengths(apu);
        apu.onDividerApuFallingEdge(); // Next step 1 does not clock lengths.
        for (auto port : {0xFF14, 0xFF19, 0xFF1E, 0xFF23}) apu.write(port, 0);
        expect((apu.read(0xFF26) & 15) == 15,
               "DMG/CGB-E disabled-to-disabled writes do not apply early-CGB length quirk");
        // A two-tick length loses exactly one count on the rising enable edge.
        apu.write(0xFF11, 62); apu.write(0xFF16, 62);
        apu.write(0xFF1B, 254); apu.write(0xFF20, 62);
        for (unsigned repeat = 0; repeat < 2; ++repeat)
            for (auto port : {0xFF14, 0xFF19, 0xFF1E, 0xFF23}) apu.write(port, 0x40);
        expect((apu.read(0xFF26) & 15) == 15, "repeated enabled writes do not double-clock lengths");
        apu.onDividerApuFallingEdge(); apu.onDividerApuFallingEdge();
        expect((apu.read(0xFF26) & 15) == 0, "remaining one count expires on the next regular length clock");
    }
}

void testLengthTriggerReloadAndEnableCollision() {
    for (bool cgb : {false, true}) {
        for (unsigned phase = 0; phase < 8; ++phase) {
            for (bool expireOnEnable : {false, true}) {
                DmgApu apu(cgb); setupOneTickLengths(apu);
                if (!expireOnEnable) {
                    for (auto port : {0xFF14, 0xFF19, 0xFF1E, 0xFF23}) apu.write(port, 0x40);
                    apu.onDividerApuFallingEdge(); // Expire, then disable the counters.
                    for (auto port : {0xFF14, 0xFF19, 0xFF1E, 0xFF23}) apu.write(port, 0);
                    for (unsigned i = 1; i < 8; ++i) apu.onDividerApuFallingEdge();
                }
                for (unsigned i = 0; i < phase; ++i) apu.onDividerApuFallingEdge();
                for (auto port : {0xFF14, 0xFF19, 0xFF1E, 0xFF23}) apu.write(port, 0xC0);
                expect((apu.read(0xFF26) & 15) == 15,
                       "trigger re-enables DAC-backed channels when enable clock expires a counter");
                // Existing one-tick data survives even-phase trigger; otherwise
                // zero reloads as maximum minus the odd-phase extra clock.
                const unsigned pulse = expireOnEnable && !(phase & 1) ? 1 : 64 - (phase & 1);
                const unsigned wave = expireOnEnable && !(phase & 1) ? 1 : 256 - (phase & 1);
                unsigned next = phase, lengthTicks = 0;
                while (lengthTicks < wave) {
                    apu.onDividerApuFallingEdge();
                    if ((next & 1) == 0) ++lengthTicks;
                    next = (next + 1) & 7;
                    const unsigned expected = (lengthTicks < pulse ? 11 : 0) |
                                              (lengthTicks < wave ? 4 : 0);
                    expect((apu.read(0xFF26) & 15) == expected,
                           "trigger reloads all four lengths with exactly one applicable extra clock");
                }
            }
        }
    }
}

std::array<uint8_t, 3> observeEnvelopeVolumes(DmgApu &apu) {
    std::array<uint8_t, 3> peak{};
    // Clock only the generators. Observe a full pulse cycle and more than
    // the longest zero run of short-mode noise without advancing DIV-APU.
    for (unsigned i = 0; i < 256; ++i) {
        apu.advanceTCycles(1);
        const auto pulse = apu.read(0xFF76), noise = apu.read(0xFF77);
        peak[0] = std::max(peak[0], static_cast<uint8_t>(pulse & 15));
        peak[1] = std::max(peak[1], static_cast<uint8_t>(pulse >> 4));
        peak[2] = std::max(peak[2], static_cast<uint8_t>(noise >> 4));
    }
    return peak;
}

void testEnvelopeTriggerReloadPhase() {
    for (unsigned phase = 0; phase < 8; ++phase) {
        for (unsigned period = 0; period < 8; ++period) {
            for (bool increase : {false, true}) {
                DmgApu apu(true);
                apu.write(0xFF26, 0x80);
                apu.write(0xFF11, 0x40); apu.write(0xFF16, 0x40);
                apu.write(0xFF13, 0xFF); apu.write(0xFF18, 0xFF); apu.write(0xFF22, 8);
                const uint8_t envelope = 0x70 | (increase ? 8 : 0) | period;
                apu.write(0xFF12, envelope); apu.write(0xFF17, envelope); apu.write(0xFF21, envelope);
                for (unsigned n = 0; n < phase; ++n) apu.onDividerApuFallingEdge();
                for (auto port : {0xFF14, 0xFF19}) apu.write(port, 0x87);
                apu.write(0xFF23, 0x80);
                expect(observeEnvelopeVolumes(apu) == std::array<uint8_t, 3>{7, 7, 7},
                       "trigger loads envelope initial volumes independently of sequencer phase");
                unsigned next = phase, envelopeTicks = 0;
                // Include two subsequent changes to check that the extra delay
                // applies to the trigger reload, not every envelope reload.
                const unsigned ticksToTest = period ? 3 * period + 1 : 10;
                while (envelopeTicks < ticksToTest) {
                    apu.onDividerApuFallingEdge();
                    if (next == 7) {
                        ++envelopeTicks;
                        unsigned changes = 0;
                        if (period && envelopeTicks >= period + (phase == 7))
                            changes = 1 + (envelopeTicks - period - (phase == 7)) / period;
                        const uint8_t expected = increase ? std::min(15u, 7 + changes)
                                                         : (changes > 7 ? 0 : 7 - changes);
                        const auto observed = observeEnvelopeVolumes(apu);
                        expect(observed == std::array<uint8_t, 3>{expected, expected, expected},
                               "pulse/noise envelopes preserve phase-7 trigger delay, normal reload pace and period-zero disable");
                    }
                    next = (next + 1) & 7;
                }
            }
        }
    }
}

void startEnvelopeProbe(DmgApu &apu, uint8_t control) {
    apu.write(0xFF26, 0x80);
    apu.write(0xFF11, 0x40); apu.write(0xFF16, 0x40);
    apu.write(0xFF13, 0xFF); apu.write(0xFF18, 0xFF); apu.write(0xFF22, 8);
    for (auto port : {0xFF12, 0xFF17, 0xFF21}) apu.write(port, control);
    for (auto port : {0xFF14, 0xFF19}) apu.write(port, 0x87);
    apu.write(0xFF23, 0x80);
}
void envelopeTick(DmgApu &apu) {
    for (unsigned n = 0; n < 8; ++n) apu.onDividerApuFallingEdge();
}
void testEnvelopeStopsUntilRetrigger() {
    for (uint8_t period = 1; period < 8; ++period) {
        for (bool increase : {false, true}) {
            DmgApu atLimit(true);
            const uint8_t control = 0x70 | (increase ? 8 : 0) | period;
            startEnvelopeProbe(atLimit, control);
            for (unsigned tick = 0; tick < (increase ? 8u : 7u) * period; ++tick) envelopeTick(atLimit);
            for (auto port : {0xFF12, 0xFF17, 0xFF21}) atLimit.write(port, control ^ 8);
            auto expectedAtLimit = observeEnvelopeVolumes(atLimit);
            for (auto &volume : expectedAtLimit) volume = increase ? volume - 1 : volume + 1;
            for (unsigned tick = 0; tick < period; ++tick) envelopeTick(atLimit);
            expect(observeEnvelopeVolumes(atLimit) == expectedAtLimit,
                   "reaching the last valid volume does not stop updates until an out-of-range attempt");
            DmgApu apu(true);
            startEnvelopeProbe(apu, control);
            // Attempt one step past the boundary; this stops automatic updates
            // without disabling the generator or its DAC.
            const unsigned changes = increase ? 9 : 8;
            for (unsigned tick = 0; tick < changes * period; ++tick) envelopeTick(apu);
            const uint8_t limit = increase ? 15 : 0;
            expect(observeEnvelopeVolumes(apu) == std::array<uint8_t, 3>{limit, limit, limit} &&
                       (apu.read(0xFF26) & 11) == 11,
                   "all envelope periods stop at their range boundary without disabling channels");
            for (auto port : {0xFF12, 0xFF17, 0xFF21}) apu.write(port, control ^ 8);
            // Observe the post-write volume rather than assuming a revision's
            // immediate NRx2 write quirk. Later clocks must not resume updates.
            const auto afterWrite = observeEnvelopeVolumes(apu);
            for (unsigned tick = 0; tick < 3 * period; ++tick) envelopeTick(apu);
            expect(observeEnvelopeVolumes(apu) == afterWrite,
                   "changing direction does not restart an envelope that stopped automatic updates");
            for (auto port : {0xFF14, 0xFF19}) apu.write(port, 0x87);
            apu.write(0xFF23, 0x80);
            for (unsigned tick = 0; tick < period; ++tick) envelopeTick(apu);
            const uint8_t restarted = increase ? 6 : 8;
            expect(observeEnvelopeVolumes(apu) == std::array<uint8_t, 3>{restarted, restarted, restarted},
                   "retrigger re-enables stopped envelopes and reloads initial volume and pace");
        }
    }
}
void testEnvelopePeriodZeroKeepsDividerPhase() {
    for (bool increase : {false, true}) {
        for (unsigned elapsed = 1; elapsed <= 9; ++elapsed) {
            DmgApu apu(true); startEnvelopeProbe(apu, 0x70 | (increase ? 8 : 0));
            for (unsigned tick = 0; tick < elapsed; ++tick) envelopeTick(apu);
            expect(observeEnvelopeVolumes(apu) == std::array<uint8_t, 3>{7, 7, 7},
                   "period zero suppresses automatic volume changes");
            for (auto port : {0xFF12, 0xFF17, 0xFF21}) apu.write(port, 0x72 | (increase ? 8 : 0));
            const auto afterWrite = observeEnvelopeVolumes(apu);
            const unsigned remaining = 8 - elapsed % 8;
            for (unsigned tick = 1; tick <= remaining + 2; ++tick) {
                envelopeTick(apu);
                auto expected = afterWrite;
                const unsigned steps = tick < remaining ? 0 : 1 + (tick - remaining) / 2;
                for (auto &volume : expected) volume = increase ? volume + steps : volume - steps;
                expect(observeEnvelopeVolumes(apu) == expected,
                       "enabling a nonzero pace retains the period-zero divider's eight-tick phase");
            }
        }
    }
}

void testZeroPaceNrx2WritesManuallyStepActiveEnvelopes() {
    DmgApu cgb(true);
    startEnvelopeProbe(cgb, 0x78); // Volume 7, increase mode, period 0.
    expect(observeEnvelopeVolumes(cgb) == std::array<uint8_t, 3>{7, 7, 7},
           "zero-pace channels start with their programmed volume");

    uint8_t expected = 7;
    for (unsigned write = 0; write < 10; ++write) {
        for (auto port : {0xFF12, 0xFF17, 0xFF21}) cgb.write(port, 0x08);
        expected = static_cast<uint8_t>((expected + 1) & 0x0F);
        expect(observeEnvelopeVolumes(cgb) ==
                   std::array<uint8_t, 3>{expected, expected, expected},
               "active zero-pace increase writes step pulse/noise volume modulo sixteen");
    }
    expect((cgb.read(0xFF26) & 0x0B) == 0x0B,
           "manual envelope steps leave both pulse and noise channels active");

    const auto prepareConstantDmgNoise = [](DmgApu &apu) {
        apu.write(0xFF26, 0x80);
        apu.write(0xFF21, 0x58); // Volume 5, increase mode, period 0.
        apu.write(0xFF22, 0x00);
        apu.write(0xFF23, 0x80);
        apu.write(0xFF24, 0x77);
        apu.write(0xFF25, 0x88); // Route noise to both outputs.
        apu.advanceTCycles(15 * 8); // The 15-bit LFSR has reached all ones.
        apu.write(0xFF22, 0xE0);   // Hold that output while measuring the write.
        apu.advanceTCycles(40000);
        apu.takeAudioSamples();
    };
    DmgApu dmg;
    DmgApu dmgControl;
    prepareConstantDmgNoise(dmg);
    prepareConstantDmgNoise(dmgControl);
    dmg.write(0xFF21, 0x08);
    dmg.advanceTCycles(500);
    dmgControl.advanceTCycles(500);
    const auto steppedPcm = dmg.takeAudioSamples();
    const auto steadyPcm = dmgControl.takeAudioSamples();
    expect(!steppedPcm.empty() && steppedPcm.size() == steadyPcm.size(),
           "DMG manual-volume comparison captures matching audio intervals");
    expect(steppedPcm != steadyPcm,
           "the documented zero-pace NRx2 write changes DMG mixer output too");
}

void testSweepPreservesCurrentPulseCountdown() {
    for (bool subtract : {false, true}) {
        const unsigned frequency = subtract ? 1024 : 512;
        const unsigned oldPeriod = 4 * (2048 - frequency);
        const unsigned updated = subtract ? frequency / 2 : frequency + frequency / 2;
        const unsigned newPeriod = 4 * (2048 - updated);
        for (unsigned offset : {0u, 1u, 4u, 511u, oldPeriod - 1}) {
            DmgApu swept(true), manual(true);
            for (auto *apu : {&swept, &manual}) {
                apu->write(0xFF26, 0x80); apu->write(0xFF11, 0xC0);
                apu->write(0xFF12, 0x90); apu->write(0xFF13, frequency & 255);
                apu->write(0xFF10, apu == &swept ? (subtract ? 0x19 : 0x11) : 0);
                apu->write(0xFF14, 0x80 | (frequency >> 8));
                apu->advanceTCycles(offset);
                for (unsigned step = 0; step < 3; ++step) apu->onDividerApuFallingEdge();
            }
            // Apply the same period update through CPU-visible registers to
            // a control with sweep disabled; neither update is a trigger.
            manual.write(0xFF13, updated & 255); manual.write(0xFF14, updated >> 8);
            const unsigned remaining = oldPeriod - offset;
            expect((swept.read(0xFF26) & 1) != 0, "valid sweep update leaves channel active");
            for (unsigned t = 1; t <= remaining + 7 * newPeriod; ++t) {
                swept.advanceTCycles(1); manual.advanceTCycles(1);
                expect(swept.read(0xFF76) == manual.read(0xFF76),
                       "sweep and nontrigger register updates preserve identical pulse edge timing");
                if (t < remaining)
                    expect(swept.read(0xFF76) == 0, "sweep does not advance the original pending duty edge early");
                if (t == remaining)
                    expect(swept.read(0xFF76) == 9, "pending duty edge retains old countdown after sweep");
                if (t == remaining + 6 * newPeriod)
                    expect(swept.read(0xFF76) == 0, "subsequent duty steps reload the new swept period");
            }
        }
    }
}

void testSweepOverflowAndNegateContracts() {
    for (bool cgb : {false, true}) {
        DmgApu immediate(cgb);
        immediate.write(0xFF26, 0x80); immediate.write(0xFF12, 0xF0);
        immediate.write(0xFF10, 1); // Pace zero still performs trigger overflow check.
        immediate.write(0xFF13, 0); immediate.write(0xFF14, 0x86);
        expect(!(immediate.read(0xFF26) & 1), "trigger sweep overflow disables CH1 even at zero pace");
        DmgApu lookahead(cgb);
        lookahead.write(0xFF26, 0x80); lookahead.write(0xFF12, 0xF0);
        lookahead.write(0xFF10, 0x11); lookahead.write(0xFF13, 0); lookahead.write(0xFF14, 0x84);
        expect(lookahead.read(0xFF26) & 1, "first sweep calculation fits before lookahead case");
        for (unsigned step = 0; step < 3; ++step) lookahead.onDividerApuFallingEdge();
        expect(!(lookahead.read(0xFF26) & 1), "second overflow calculation disables CH1 after valid frequency update");
        DmgApu negate(cgb);
        negate.write(0xFF26, 0x80); negate.write(0xFF12, 0xF0);
        negate.write(0xFF10, 0x19); negate.write(0xFF13, 0); negate.write(0xFF14, 0x84);
        expect(negate.read(0xFF26) & 1, "negative sweep trigger remains active");
        negate.write(0xFF10, 0x11);
        expect(!(negate.read(0xFF26) & 1), "clearing negate after subtraction calculation disables CH1");
    }
}

void testMixedDacBiasCancellation() {
    for (bool cgb : {false, true}) {
        DmgApu apu(cgb); apu.write(0xFF26, 0x80);
        apu.write(0xFF12, 8); // Inactive pulse generator, enabled DAC: positive bias.
        for (uint16_t address = 0xFF30; address <= 0xFF3F; ++address) apu.write(address, 0xFF);
        apu.write(0xFF1A, 0x80); apu.write(0xFF1C, 0x20);
        apu.write(0xFF1D, 0xFE); apu.write(0xFF1E, 0x87);
        apu.write(0xFF24, 0x77); apu.write(0xFF25, 0x55);
        apu.advanceTCycles(4096);
        const auto samples = apu.takeAudioSamples();
        expect(!samples.empty() && std::all_of(samples.begin(), samples.end(), [](int16_t x) { return x == 0; }),
               "inactive pulse DAC and constant maximum wave DAC cancel before stereo filtering");
        apu.write(0xFF25, 0x44); apu.advanceTCycles(88);
        const auto waveOnly = apu.takeAudioSamples();
        expect(!waveOnly.empty() && waveOnly[0] == -3840 && waveOnly[1] == -3840,
               "unrouting the inactive DAC exposes the wave DAC's negative bias transition");
    }
}

void testDacTransferFromDigitalLevels() {
    const std::array<uint16_t, 4> dacRegisters{0xFF12, 0xFF17, 0xFF1A, 0xFF21};
    for (bool cgb : {false, true}) {
        for (unsigned channel = 0; channel < 4; ++channel) {
            for (unsigned volume = 0; volume < 16; ++volume) {
                for (uint8_t master : {uint8_t(0), uint8_t(7)}) {
                    DmgApu apu(cgb); apu.write(0xFF26, 0x80);
                    apu.write(0xFF24, master << 4);
                    apu.write(0xFF25, 0x10 << channel);
                    const uint8_t envelope = static_cast<uint8_t>((volume << 4) | 8);
                    if (channel < 2) {
                        const uint16_t base = channel == 0 ? 0xFF11 : 0xFF16;
                        apu.write(base, 0xC0); apu.write(base + 1, envelope);
                        apu.write(base + 2, 0xF0); apu.write(base + 3, 0x87);
                    } else if (channel == 2) {
                        for (uint16_t address = 0xFF30; address <= 0xFF3F; ++address)
                            apu.write(address, static_cast<uint8_t>(volume * 17));
                        apu.write(0xFF1A, 0x80); apu.write(0xFF1C, 0x20);
                        apu.write(0xFF1D, 0xFE); apu.write(0xFF1E, 0x87);
                    } else {
                        apu.write(0xFF21, envelope); apu.write(0xFF22, 8); apu.write(0xFF23, 0x80);
                    }
                    apu.advanceTCycles(88); // First 48-kHz sample, at a high generator step.
                    if (cgb) {
                        const auto pcm = apu.read(channel < 2 ? 0xFF76 : 0xFF77);
                        expect(((pcm >> ((channel & 1) * 4)) & 15) == volume,
                               "CGB digital observation retains every generator level before DAC conversion");
                    }
                    const auto samples = apu.takeAudioSamples();
                    expect(samples.size() == 2 && samples[0] ==
                               (15 - 2 * static_cast<int>(volume)) * (master + 1) * 32 && samples[1] == 0,
                           "each DAC maps digital 0..15 to positive..negative bias before nonzero NR50 gain and NR51 panning");
                }
            }
            DmgApu stopped(cgb); stopped.write(0xFF26, 0x80);
            stopped.write(dacRegisters[channel], channel == 2 ? 0x80 : 8);
            stopped.write(0xFF24, 0x70); stopped.write(0xFF25, 0x10 << channel);
            stopped.advanceTCycles(88);
            expect((stopped.read(0xFF26) & 15) == 0 && stopped.takeAudioSamples()[0] == 3840,
                   "enabled DAC emits digital-zero bias even when its generator was never triggered");
            DmgApu disconnected(cgb); disconnected.write(0xFF26, 0x80);
            // Another, unrouted DAC keeps the common HPF connected.
            disconnected.write(channel == 0 ? 0xFF1A : 0xFF12, channel == 0 ? 0x80 : 8);
            disconnected.write(0xFF24, 0x70); disconnected.write(0xFF25, 0x10 << channel);
            disconnected.advanceTCycles(88);
            expect(disconnected.takeAudioSamples()[0] == 0,
                   "a disabled DAC contributes zero even while another channel keeps the mixer connected");
        }
    }
}

void testModelSpecificHighPassDecay() {
    std::array<int16_t, 2> halfMillisecond{};
    for (bool cgb : {false, true}) {
        DmgApu apu(cgb);
        apu.write(0xFF26, 0x80);
        for (uint16_t address = 0xFF30; address <= 0xFF3F; ++address) apu.write(address, 0xFF);
        apu.write(0xFF1A, 0x80); apu.write(0xFF1C, 0x20);
        apu.write(0xFF1D, 0xFE); apu.write(0xFF1E, 0x87);
        apu.write(0xFF24, 0x77); apu.write(0xFF25, 0x40);
        apu.advanceTCycles(41944); // Ten milliseconds, constant wave DAC input.
        const auto samples = apu.takeAudioSamples();
        expect(samples.size() >= 256 && samples[0] == -3840,
               "constant wave input produces a known initial high-pass step");
        const double perClock = cgb ? 0.998943 : 0.999958;
        for (unsigned frame : {1u, 8u, 24u, 64u, 127u}) {
            const double expected = -3840 * std::pow(perClock, frame * 4194304.0 / 48000);
            expect(std::abs(samples[frame * 2] - expected) <= 33,
                   "model high-pass decay follows documented master-clock charge factor at 48 kHz within PCM quantization");
            expect(samples[frame * 2 + 1] == 0, "model filter does not leak into an unrouted output");
        }
        halfMillisecond[cgb ? 1 : 0] = std::abs(samples[48]);
        apu.write(0xFF1A, 0); apu.advanceTCycles(1024);
        for (auto sample : apu.takeAudioSamples())
            expect(sample == 0, "both model filters disconnect immediately with all DACs off");
    }
    expect(halfMillisecond[1] < halfMillisecond[0] / 4,
           "CGB high-pass removes constant bias faster than DMG");
}

}

int main() {
    testSilentPulseDividersKeepDutyPhase();
    testPulseDividerWaitsForFirstTrigger();
    testEnvelopeStopsUntilRetrigger();
    testEnvelopePeriodZeroKeepsDividerPhase();
    testZeroPaceNrx2WritesManuallyStepActiveEnvelopes();
    testDacTransferFromDigitalLevels();
    testMixedDacBiasCancellation();
    testModelSpecificHighPassDecay();
    testSweepPreservesCurrentPulseCountdown();
    testSweepOverflowAndNegateContracts();
    testEnvelopeTriggerReloadPhase();
    testExtraLengthEnableClocks();
    testLengthTriggerReloadAndEnableCollision();
    testPulseFirstDutyAndRetriggerPhase();
    testCgbPcmDigitalRegisters();
    testCgbPcmWaveAndNoiseDigitalLevels();
    testCgbActiveWaveRamAliasesWithoutDmgAccessWindow();
    testCgbRetriggerPreservesWaveRam();
    testModelSpecificPoweredOffLengthRegisters();
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
