#include "include/dmg_apu.hpp"

#include <algorithm>

namespace LOTUSGB {

namespace {
constexpr uint16_t NR10 = 0xFF10;
constexpr uint16_t NR11 = 0xFF11;
constexpr uint16_t NR12 = 0xFF12;
constexpr uint16_t NR13 = 0xFF13;
constexpr uint16_t NR14 = 0xFF14;
constexpr uint16_t NR21 = 0xFF16;
constexpr uint16_t NR22 = 0xFF17;
constexpr uint16_t NR23 = 0xFF18;
constexpr uint16_t NR24 = 0xFF19;
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
        case NR21: return static_cast<uint8_t>(0x3F | (nr21 & 0xC0));
        case NR22: return nr22;
        case NR23: return nr23;
        case NR24: return static_cast<uint8_t>(0xBF | (nr24 & 0x40));
        case NR50: return nr50;
        case NR51: return nr51;
        case NR52:
            return static_cast<uint8_t>(0x70 | (powered ? 0x80 : 0) |
                                       (channel1Enabled ? 0x01 : 0) |
                                       (channel2Enabled ? 0x02 : 0));
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
        } else if (address == NR21) {
            lengthCounter2 = static_cast<uint8_t>(64 - (value & 0x3F));
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
        case NR21:
            nr21 = value;
            lengthCounter2 = static_cast<uint8_t>(64 - (value & 0x3F));
            break;
        case NR22:
            nr22 = value;
            channel2DacEnabled = (value & 0xF8) != 0;
            if (!channel2DacEnabled)
                channel2Enabled = false;
            break;
        case NR23:
            nr23 = value;
            break;
        case NR24:
            nr24 = value & 0x47;
            lengthEnabled2 = (value & 0x40) != 0;
            if (value & 0x80)
                triggerChannel2();
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
            advanceChannel2();
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
    nr14 = 0x07;
    nr21 = 0;
    nr22 = 0;
    nr23 = 0;
    nr24 = 0;
    channel1Enabled = true;
    channel1DacEnabled = true;
    lengthEnabled = false;
    lengthCounter = 1;
    dutyPosition = 0;
    frequencyTimer = 4;
    channel2Enabled = false;
    channel2DacEnabled = false;
    lengthEnabled2 = false;
    lengthCounter2 = 0;
    dutyPosition2 = 0;
    frequencyTimer2 = 0;
    envelopeVolume2 = 0;
    envelopeTimer2 = 0;
    // The DMG boot sound has already faded out by the time execution reaches
    // $0100, even though channel 1 remains enabled and NR12 still reads $F3.
    envelopeVolume = 0;
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
        dutyPosition2 = 0;
        return;
    }

    // Preserve the channel length data, which remains writable while powered off.
    nr10 = 0;
    nr11 = 0;
    nr12 = 0;
    nr13 = 0;
    nr14 = 0;
    nr21 = 0;
    nr22 = 0;
    nr23 = 0;
    nr24 = 0;
    nr50 = 0;
    nr51 = 0;
    channel1Enabled = false;
    channel1DacEnabled = false;
    channel2Enabled = false;
    channel2DacEnabled = false;
    lengthEnabled = false;
    lengthEnabled2 = false;
    envelopeVolume = 0;
    envelopeTimer = 0;
    envelopeVolume2 = 0;
    envelopeTimer2 = 0;
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

void DmgApu::triggerChannel2() {
    channel2Enabled = channel2DacEnabled;
    if (lengthCounter2 == 0)
        lengthCounter2 = 64;
    frequencyTimer2 = static_cast<uint16_t>(4 * (2048 - channel2Frequency()));
    envelopeVolume2 = nr22 >> 4;
    const uint8_t envelopePeriod = nr22 & 0x07;
    envelopeTimer2 = envelopePeriod == 0 ? 8 : envelopePeriod;
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

void DmgApu::advanceChannel2() {
    if (!channel2Enabled)
        return;
    if (frequencyTimer2 > 0)
        --frequencyTimer2;
    if (frequencyTimer2 == 0) {
        frequencyTimer2 = static_cast<uint16_t>(4 * (2048 - channel2Frequency()));
        dutyPosition2 = static_cast<uint8_t>((dutyPosition2 + 1) & 0x07);
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
    if (lengthEnabled && lengthCounter > 0) {
        --lengthCounter;
        if (lengthCounter == 0)
            channel1Enabled = false;
    }
    if (lengthEnabled2 && lengthCounter2 > 0) {
        --lengthCounter2;
        if (lengthCounter2 == 0)
            channel2Enabled = false;
    }
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
    if (channel1Enabled && period != 0) {
        if (envelopeTimer > 0)
            --envelopeTimer;
        if (envelopeTimer == 0) {
            envelopeTimer = period;
            const bool increase = (nr12 & 0x08) != 0;
            if (increase && envelopeVolume < 15)
                ++envelopeVolume;
            else if (!increase && envelopeVolume > 0)
                --envelopeVolume;
        }
    }

    const uint8_t period2 = nr22 & 0x07;
    if (!channel2Enabled || period2 == 0)
        return;
    if (envelopeTimer2 > 0)
        --envelopeTimer2;
    if (envelopeTimer2 == 0) {
        envelopeTimer2 = period2;
        const bool increase = (nr22 & 0x08) != 0;
        if (increase && envelopeVolume2 < 15)
            ++envelopeVolume2;
        else if (!increase && envelopeVolume2 > 0)
            --envelopeVolume2;
    }
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

uint16_t DmgApu::channel2Frequency() const {
    return static_cast<uint16_t>(nr23 | ((nr24 & 0x07) << 8));
}

int DmgApu::channel1Output() const {
    if (!channel1Enabled || !channel1DacEnabled || envelopeVolume == 0)
        return 0;

    const uint8_t duty = (nr11 >> 6) & 0x03;
    const bool high = (DUTY_PATTERNS[duty] & (1U << (7 - dutyPosition))) != 0;
    return high ? envelopeVolume : -static_cast<int>(envelopeVolume);
}

int DmgApu::channel2Output() const {
    if (!channel2Enabled || !channel2DacEnabled || envelopeVolume2 == 0)
        return 0;

    const uint8_t duty = (nr21 >> 6) & 0x03;
    const bool high = (DUTY_PATTERNS[duty] & (1U << (7 - dutyPosition2))) != 0;
    return high ? envelopeVolume2 : -static_cast<int>(envelopeVolume2);
}

void DmgApu::generateSample() {
    const int channel1 = channel1Output();
    const int channel2 = channel2Output();
    int left = 0;
    int right = 0;
    if (powered) {
        if (nr51 & 0x10)
            left += channel1;
        if (nr51 & 0x20)
            left += channel2;
        if (nr51 & 0x01)
            right += channel1;
        if (nr51 & 0x02)
            right += channel2;
        left *= ((nr50 >> 4) & 0x07) + 1;
        right *= (nr50 & 0x07) + 1;
    }

    // Approximate the DMG output coupling capacitor, which removes DC offset.
    // Keep fractional filter state so negative feedback can decay to zero
    // instead of sticking at a DC offset due to signed integer rounding.
    constexpr int64_t Q16_ONE = 1LL << 16;
    constexpr int64_t HIGH_PASS_COEFFICIENT = 65296;
    const int64_t filteredLeftQ16 =
        (static_cast<int64_t>(left - previousLeftInput) * Q16_ONE) +
        (previousLeftOutputQ16 * HIGH_PASS_COEFFICIENT) / Q16_ONE;
    const int64_t filteredRightQ16 =
        (static_cast<int64_t>(right - previousRightInput) * Q16_ONE) +
        (previousRightOutputQ16 * HIGH_PASS_COEFFICIENT) / Q16_ONE;
    const int32_t filteredLeft = static_cast<int32_t>(filteredLeftQ16 / Q16_ONE);
    const int32_t filteredRight = static_cast<int32_t>(filteredRightQ16 / Q16_ONE);
    previousLeftInput = left;
    previousRightInput = right;
    previousLeftOutputQ16 = filteredLeftQ16;
    previousRightOutputQ16 = filteredRightQ16;

    // Leave room for four channels and their largest bipolar transitions
    // before converting the mixed signal to signed 16-bit PCM.
    constexpr int SAMPLE_SCALE = 32;
    const int leftSample = std::clamp(filteredLeft * SAMPLE_SCALE, -32768, 32767);
    const int rightSample = std::clamp(filteredRight * SAMPLE_SCALE, -32768, 32767);
    audioSamples.push_back(static_cast<int16_t>(leftSample));
    audioSamples.push_back(static_cast<int16_t>(rightSample));
}

}
