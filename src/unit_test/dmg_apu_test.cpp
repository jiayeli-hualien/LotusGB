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

}

int main() {
    testChannelOneProducesStereoSamplesAtConfiguredRate();
    testMaximumChannelOutputLeavesHeadroomInPcmSamples();
    testPowerOffMutesAndIgnoresChannelWrites();
    std::cout << "DMG APU tests passed\n";
}
