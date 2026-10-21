#include "dmg_apu.hpp"

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
    apu.write(0xFF11, 0x80); // 50% duty
    apu.write(0xFF12, 0xF0); // DAC on, volume 15
    apu.write(0xFF13, 0x00);
    apu.write(0xFF14, 0x86); // trigger, frequency 1536
    apu.write(0xFF24, 0x77); // full left/right volume
    apu.write(0xFF25, 0x11); // route channel 1 to both outputs

    apu.advanceTCycles(50000);
    expect(!apu.takeAudioSamples().empty(),
           "channel produces samples before being stopped");

    apu.write(0xFF12, 0x00); // Disable the channel DAC.
    apu.advanceTCycles(500000);
    const auto samples = apu.takeAudioSamples();
    expect(samples.size() >= 64, "enough silent tail samples were generated");
    for (auto it = samples.end() - 64; it != samples.end(); ++it)
        expect(*it == 0,
               "high-pass filter settles to digital silence after channel stops");
}

}

int main() {
    testChannelOneProducesStereoSamplesAtConfiguredRate();
    testChannelTwoProducesAudioOnItsIndependentStereoRoute();
    testMaximumChannelOutputLeavesHeadroomInPcmSamples();
    testPowerOffMutesAndIgnoresChannelWrites();
    testDmgPostBootStateHasFadedStartupChannel();
    testHighPassOutputDecaysToSilenceAfterChannelStops();
    std::cout << "DMG APU tests passed\n";
}
