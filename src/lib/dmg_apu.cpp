#include "include/dmg_apu.hpp"

#include <algorithm>

namespace LOTUSGB {

namespace {
constexpr uint16_t NR10 = 0xFF10;
constexpr uint16_t NR11 = 0xFF11;
constexpr uint16_t NR12 = 0xFF12;
constexpr uint16_t NR13 = 0xFF13;
constexpr uint16_t NR14 = 0xFF14;
constexpr uint16_t NR50 = 0xFF24;
constexpr uint16_t NR51 = 0xFF25;
constexpr uint16_t NR52 = 0xFF26;
constexpr uint16_t APU_FRAME_SEQUENCER_PERIOD = 8192;
constexpr uint32_t DMG_MASTER_CLOCK = 4194304;
constexpr uint8_t DUTY_PATTERNS[4] = {0x01, 0x81, 0x87, 0x7E};
}

uint8_t DmgApu::read(uint16_t address) {
    switch (address) {
        case NR10: return static_cast<uint8_t>(0x80 | nr10);
        case NR11: return static_cast<uint8_t>(0x3F | (nr11 & 0xC0));
        case NR12: return nr12;
        case NR13: return nr13;
        case NR14: return static_cast<uint8_t>(0xBF | (nr14 & 0x40));
        case NR50: return nr50;
        case NR51: return nr51;
        case NR52:
            return static_cast<uint8_t>(0x70 | (powered ? 0x80 : 0) |
                                       (channel1Enabled ? 0x01 : 0));
        default: return 0xFF;
    }
}

void DmgApu::write(uint16_t address, uint8_t value) {
    if (address == NR52) {
        setPower((value & 0x80) != 0);
        return;
    }

    // On DMG, the length data remains writable while the APU is powered off.
    if (!powered) {
        if (address == NR11) {
            lengthCounter = static_cast<uint8_t>(64 - (value & 0x3F));
        }
        return;
    }

    switch (address) {
        case NR10:
            nr10 = value & 0x7F;
            if (sweepNegateUsed && (nr10 & 0x08) == 0)
                channel1Enabled = false;
            break;
        case NR11:
            nr11 = value;
            lengthCounter = static_cast<uint8_t>(64 - (value & 0x3F));
            break;
        case NR12:
            nr12 = value;
            channel1DacEnabled = (value & 0xF8) != 0;
            if (!channel1DacEnabled)
                channel1Enabled = false;
            break;
        case NR13:
            nr13 = value;
            break;
        case NR14:
            nr14 = value & 0x47;
            lengthEnabled = (value & 0x40) != 0;
            if (value & 0x80)
                triggerChannel1();
            break;
        case NR50:
            nr50 = value;
            break;
        case NR51:
            nr51 = value;
            break;
        default:
            break;
    }
}

void DmgApu::advanceTCycles(uint64_t tCycles) {
    for (uint64_t cycle = 0; cycle < tCycles; ++cycle) {
        if (powered) {
            advanceChannel1();
            if (++frameSequencerCycles == APU_FRAME_SEQUENCER_PERIOD) {
                frameSequencerCycles = 0;
                clockFrameSequencer();
            }
        }

        sampleAccumulator += SAMPLE_RATE;
        if (sampleAccumulator >= DMG_MASTER_CLOCK) {
            sampleAccumulator -= DMG_MASTER_CLOCK;
            generateSample();
        }
    }
}

void DmgApu::initializeDmgPostBootState() {
    setPower(true);
    nr11 = 0xBF;
    nr12 = 0xF3;
    nr13 = 0xFF;
    nr14 = 0x47;
    channel1Enabled = true;
    channel1DacEnabled = true;
    lengthEnabled = true;
    lengthCounter = 1;
    dutyPosition = 0;
    frequencyTimer = 4;
    envelopeVolume = 15;
    envelopeTimer = 3;
    sweepShadowFrequency = 2047;
    sweepTimer = 8;
    nr50 = 0x77;
    nr51 = 0xF3;
}

std::vector<int16_t> DmgApu::takeAudioSamples() {
    std::vector<int16_t> result;
    result.swap(audioSamples);
    return result;
}

void DmgApu::setPower(bool enabled) {
    if (powered == enabled)
        return;

    powered = enabled;
    if (enabled) {
        frameSequencerCycles = 0;
        frameSequencerStep = 0;
        dutyPosition = 0;
        return;
    }

    // Preserve the channel length data, which remains writable while powered off.
    nr10 = 0;
    nr11 = 0;
    nr12 = 0;
    nr13 = 0;
    nr14 = 0;
    nr50 = 0;
    nr51 = 0;
    channel1Enabled = false;
    channel1DacEnabled = false;
    lengthEnabled = false;
    envelopeVolume = 0;
    envelopeTimer = 0;
    sweepTimer = 0;
    sweepEnabled = false;
    sweepNegateUsed = false;
}

void DmgApu::triggerChannel1() {
    channel1Enabled = channel1DacEnabled;
    if (lengthCounter == 0)
        lengthCounter = 64;
    frequencyTimer = static_cast<uint16_t>(4 * (2048 - channel1Frequency()));
    envelopeVolume = nr12 >> 4;
    const uint8_t envelopePeriod = nr12 & 0x07;
    envelopeTimer = envelopePeriod == 0 ? 8 : envelopePeriod;

    sweepShadowFrequency = channel1Frequency();
    const uint8_t sweepPeriod = (nr10 >> 4) & 0x07;
    sweepTimer = sweepPeriod == 0 ? 8 : sweepPeriod;
    sweepEnabled = sweepPeriod != 0 || (nr10 & 0x07) != 0;
    sweepNegateUsed = false;
    if ((nr10 & 0x07) != 0)
        calculateSweepFrequency();
}

void DmgApu::advanceChannel1() {
    if (!channel1Enabled)
        return;
    if (frequencyTimer > 0)
        --frequencyTimer;
    if (frequencyTimer == 0) {
        frequencyTimer = static_cast<uint16_t>(4 * (2048 - channel1Frequency()));
        dutyPosition = static_cast<uint8_t>((dutyPosition + 1) & 0x07);
    }
}

void DmgApu::clockFrameSequencer() {
    if ((frameSequencerStep & 1) == 0)
        clockLength();
    if (frameSequencerStep == 2 || frameSequencerStep == 6)
        clockSweep();
    if (frameSequencerStep == 7)
        clockEnvelope();
    frameSequencerStep = static_cast<uint8_t>((frameSequencerStep + 1) & 0x07);
}

void DmgApu::clockLength() {
    if (!lengthEnabled || lengthCounter == 0)
        return;
    --lengthCounter;
    if (lengthCounter == 0)
        channel1Enabled = false;
}

void DmgApu::clockSweep() {
    if (sweepTimer > 0)
        --sweepTimer;
    if (sweepTimer != 0)
        return;

    const uint8_t period = (nr10 >> 4) & 0x07;
    sweepTimer = period == 0 ? 8 : period;
    if (!sweepEnabled || period == 0)
        return;

    const int newFrequency = calculateSweepFrequency();
    if (newFrequency < 0 || newFrequency > 2047) {
        channel1Enabled = false;
        return;
    }
    if ((nr10 & 0x07) == 0)
        return;

    sweepShadowFrequency = static_cast<uint16_t>(newFrequency);
    nr13 = static_cast<uint8_t>(newFrequency & 0xFF);
    nr14 = static_cast<uint8_t>((nr14 & 0x40) | ((newFrequency >> 8) & 0x07));
    frequencyTimer = static_cast<uint16_t>(4 * (2048 - newFrequency));
    if (calculateSweepFrequency() > 2047)
        channel1Enabled = false;
}

void DmgApu::clockEnvelope() {
    const uint8_t period = nr12 & 0x07;
    if (!channel1Enabled || period == 0)
        return;
    if (envelopeTimer > 0)
        --envelopeTimer;
    if (envelopeTimer != 0)
        return;

    envelopeTimer = period;
    const bool increase = (nr12 & 0x08) != 0;
    if (increase && envelopeVolume < 15)
        ++envelopeVolume;
    else if (!increase && envelopeVolume > 0)
        --envelopeVolume;
}

int DmgApu::calculateSweepFrequency() {
    const uint8_t shift = nr10 & 0x07;
    const int delta = sweepShadowFrequency >> shift;
    if (nr10 & 0x08) {
        sweepNegateUsed = true;
        return static_cast<int>(sweepShadowFrequency) - delta;
    }
    return static_cast<int>(sweepShadowFrequency) + delta;
}

uint16_t DmgApu::channel1Frequency() const {
    return static_cast<uint16_t>(nr13 | ((nr14 & 0x07) << 8));
}

int DmgApu::channel1Output() const {
    if (!channel1Enabled || !channel1DacEnabled || envelopeVolume == 0)
        return 0;

    const uint8_t duty = (nr11 >> 6) & 0x03;
    const bool high = (DUTY_PATTERNS[duty] & (1U << (7 - dutyPosition))) != 0;
    return high ? envelopeVolume : -static_cast<int>(envelopeVolume);
}

void DmgApu::generateSample() {
    const int channel = channel1Output();
    int left = 0;
    int right = 0;
    if (powered && (nr51 & 0x10))
        left = channel * (((nr50 >> 4) & 0x07) + 1);
    if (powered && (nr51 & 0x01))
        right = channel * ((nr50 & 0x07) + 1);

    // Approximate the DMG output coupling capacitor, which removes DC offset.
    constexpr int32_t HIGH_PASS_COEFFICIENT = 65296;
    const int32_t filteredLeft = left - previousLeftInput +
        static_cast<int32_t>((static_cast<int64_t>(previousLeftOutput) *
                              HIGH_PASS_COEFFICIENT) >> 16);
    const int32_t filteredRight = right - previousRightInput +
        static_cast<int32_t>((static_cast<int64_t>(previousRightOutput) *
                              HIGH_PASS_COEFFICIENT) >> 16);
    previousLeftInput = left;
    previousRightInput = right;
    previousLeftOutput = filteredLeft;
    previousRightOutput = filteredRight;

    constexpr int SAMPLE_SCALE = 256;
    const int leftSample = std::clamp(filteredLeft * SAMPLE_SCALE, -32768, 32767);
    const int rightSample = std::clamp(filteredRight * SAMPLE_SCALE, -32768, 32767);
    audioSamples.push_back(static_cast<int16_t>(leftSample));
    audioSamples.push_back(static_cast<int16_t>(rightSample));
}

}
