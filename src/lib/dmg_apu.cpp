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
constexpr uint16_t NR30 = 0xFF1A;
constexpr uint16_t NR31 = 0xFF1B;
constexpr uint16_t NR32 = 0xFF1C;
constexpr uint16_t NR33 = 0xFF1D;
constexpr uint16_t NR34 = 0xFF1E;
constexpr uint16_t NR41 = 0xFF20;
constexpr uint16_t NR42 = 0xFF21;
constexpr uint16_t NR43 = 0xFF22;
constexpr uint16_t NR44 = 0xFF23;
constexpr uint16_t WAVE_RAM_START = 0xFF30;
constexpr uint16_t WAVE_RAM_END = 0xFF3F;
constexpr uint16_t NR50 = 0xFF24;
constexpr uint16_t NR51 = 0xFF25;
constexpr uint16_t NR52 = 0xFF26;
constexpr uint16_t T_CYCLES_PER_APU_TICK = 2;
constexpr uint16_t CH3_TRIGGER_STARTUP_APU_TICKS = 3;
constexpr uint32_t DMG_MASTER_CLOCK = 4194304;
constexpr uint8_t DUTY_PATTERNS[4] = {0x01, 0x81, 0x87, 0x7E};
}

uint8_t DmgApu::read(uint16_t address) {
    if (address >= WAVE_RAM_START && address <= WAVE_RAM_END) {
        if (channel3Enabled) {
            const bool fetchCycle = hasWaveRamFetch &&
                                    elapsedTCycles == lastWaveRamFetchTCycle;
            const uint8_t value = fetchCycle ? waveRam[waveSampleIndex / 2] : 0xFF;
            return value;
        }
        return waveRam[address - WAVE_RAM_START];
    }

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
        case NR30: return static_cast<uint8_t>(0x7F | (nr30 & 0x80));
        case NR31: return 0xFF;
        case NR32: return static_cast<uint8_t>(0x9F | (nr32 & 0x60));
        case NR33: return 0xFF;
        case NR34: return static_cast<uint8_t>(0xBF | (nr34 & 0x40));
        case NR41: return 0xFF;
        case NR42: return nr42;
        case NR43: return nr43;
        case NR44: return static_cast<uint8_t>(0xBF | (nr44 & 0x40));
        case NR50: return nr50;
        case NR51: return nr51;
        case NR52:
            return static_cast<uint8_t>(0x70 | (powered ? 0x80 : 0) |
                                       (channel1Enabled ? 0x01 : 0) |
                                       (channel2Enabled ? 0x02 : 0) |
                                       (channel3Enabled ? 0x04 : 0) |
                                       (channel4Enabled ? 0x08 : 0));
        default: return 0xFF;
    }
}

void DmgApu::write(uint16_t address, uint8_t value) {
    if (address == NR52) {
        setPower((value & 0x80) != 0);
        return;
    }

    if (address >= WAVE_RAM_START && address <= WAVE_RAM_END) {
        if (!channel3Enabled) {
            waveRam[address - WAVE_RAM_START] = value;
        } else if (hasWaveRamFetch &&
                   elapsedTCycles == lastWaveRamFetchTCycle) {
            waveRam[waveSampleIndex / 2] = value;
        }
        return;
    }

    // On DMG, the length data remains writable while the APU is powered off.
    if (!powered) {
        if (address == NR11) {
            lengthCounter = static_cast<uint8_t>(64 - (value & 0x3F));
        } else if (address == NR21) {
            lengthCounter2 = static_cast<uint8_t>(64 - (value & 0x3F));
        } else if (address == NR31) {
            lengthCounter3 = static_cast<uint16_t>(256 - value);
        } else if (address == NR41) {
            lengthCounter4 = static_cast<uint8_t>(64 - (value & 0x3F));
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
        case NR30:
            nr30 = value & 0x80;
            channel3DacEnabled = (value & 0x80) != 0;
            if (!channel3DacEnabled)
                channel3Enabled = false;
            break;
        case NR31:
            lengthCounter3 = static_cast<uint16_t>(256 - value);
            break;
        case NR32:
            nr32 = value & 0x60;
            break;
        case NR33:
            nr33 = value;
            break;
        case NR34:
            nr34 = value & 0x47;
            lengthEnabled3 = (value & 0x40) != 0;
            if (value & 0x80)
                triggerChannel3();
            break;
        case NR41:
            lengthCounter4 = static_cast<uint8_t>(64 - (value & 0x3F));
            break;
        case NR42:
            nr42 = value;
            channel4DacEnabled = (value & 0xF8) != 0;
            if (!channel4DacEnabled)
                channel4Enabled = false;
            break;
        case NR43:
            nr43 = value;
            break;
        case NR44:
            nr44 = value & 0x40;
            lengthEnabled4 = (value & 0x40) != 0;
            if (value & 0x80)
                triggerChannel4();
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
        ++elapsedTCycles;
        if (powered) {
            advanceChannel1();
            advanceChannel2();
            advanceChannel3();
            advanceChannel4();
        }

        sampleAccumulator += SAMPLE_RATE;
        if (sampleAccumulator >= DMG_MASTER_CLOCK) {
            sampleAccumulator -= DMG_MASTER_CLOCK;
            generateSample();
        }
    }
}

void DmgApu::onDividerApuFallingEdge() {
    if (powered)
        clockFrameSequencer();
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
    nr30 = 0;
    nr32 = 0;
    nr33 = 0;
    nr34 = 0;
    nr42 = 0;
    nr43 = 0;
    nr44 = 0;
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
    channel3Enabled = false;
    channel3DacEnabled = false;
    lengthEnabled3 = false;
    lengthCounter3 = 0;
    waveSampleIndex = 0;
    waveSampleBuffer = 0;
    frequencyTimer3 = 0;
    channel4Enabled = false;
    channel4DacEnabled = false;
    lengthEnabled4 = false;
    lengthCounter4 = 0;
    envelopeVolume4 = 0;
    envelopeTimer4 = 0;
    channel4Lfsr = 0;
    frequencyTimer4 = 0;
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
        frameSequencerStep = 0;
        dutyPosition = 0;
        dutyPosition2 = 0;
        waveSampleBuffer = 0;
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
    nr30 = 0;
    nr32 = 0;
    nr33 = 0;
    nr34 = 0;
    nr42 = 0;
    nr43 = 0;
    nr44 = 0;
    nr50 = 0;
    nr51 = 0;
    channel1Enabled = false;
    channel1DacEnabled = false;
    channel2Enabled = false;
    channel2DacEnabled = false;
    channel3Enabled = false;
    channel3DacEnabled = false;
    channel4Enabled = false;
    channel4DacEnabled = false;
    lengthEnabled = false;
    lengthEnabled2 = false;
    lengthEnabled3 = false;
    lengthEnabled4 = false;
    envelopeVolume = 0;
    envelopeTimer = 0;
    envelopeVolume2 = 0;
    envelopeTimer2 = 0;
    envelopeVolume4 = 0;
    envelopeTimer4 = 0;
    channel4Lfsr = 0;
    frequencyTimer4 = 0;
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

void DmgApu::triggerChannel3() {
    if (channel3Enabled)
        corruptWaveRamOnChannel3Retrigger();

    channel3Enabled = channel3DacEnabled;
    if (lengthCounter3 == 0)
        lengthCounter3 = 256;
    // The first wave fetch follows the trigger by three additional APU ticks.
    frequencyTimer3 = static_cast<uint16_t>(T_CYCLES_PER_APU_TICK *
                                            (2048 - channel3Frequency() +
                                             CH3_TRIGGER_STARTUP_APU_TICKS));
    waveSampleIndex = 0;
    hasWaveRamFetch = false;
}

void DmgApu::corruptWaveRamOnChannel3Retrigger() {
    const bool fetchedThisCycle = hasWaveRamFetch &&
                                  elapsedTCycles == lastWaveRamFetchTCycle;
    const bool fetchImminent = frequencyTimer3 > 0 && frequencyTimer3 <= 2;
    if (!fetchedThisCycle && !fetchImminent)
        return;

    const uint8_t sampleIndex = fetchedThisCycle
                                    ? waveSampleIndex
                                    : static_cast<uint8_t>((waveSampleIndex + 1) & 0x1F);
    const uint8_t byteIndex = sampleIndex / 2;
    if (byteIndex < 4) {
        waveRam[0] = waveRam[byteIndex];
        return;
    }

    // On DMG, a CH3 retrigger colliding with a wave RAM read copies the
    // selected four-byte RAM row into the start of wave RAM.
    const uint8_t rowStart = byteIndex & 0x0C;
    const std::array<uint8_t, 4> row = {
        waveRam[rowStart], waveRam[rowStart + 1],
        waveRam[rowStart + 2], waveRam[rowStart + 3]
    };
    for (size_t i = 0; i < row.size(); ++i)
        waveRam[i] = row[i];
}

void DmgApu::triggerChannel4() {
    channel4Enabled = channel4DacEnabled;
    if (lengthCounter4 == 0)
        lengthCounter4 = 64;
    const uint8_t envelopePeriod = nr42 & 0x07;
    envelopeVolume4 = nr42 >> 4;
    envelopeTimer4 = envelopePeriod == 0 ? 8 : envelopePeriod;
    channel4Lfsr = 0;
    frequencyTimer4 = channel4Period();
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

void DmgApu::advanceChannel3() {
    if (!channel3Enabled)
        return;
    if (frequencyTimer3 > 0)
        --frequencyTimer3;
    if (frequencyTimer3 == 0) {
        frequencyTimer3 = static_cast<uint16_t>(T_CYCLES_PER_APU_TICK *
                                                (2048 - channel3Frequency()));
        waveSampleIndex = static_cast<uint8_t>((waveSampleIndex + 1) & 0x1F);
        const uint8_t waveByte = waveRam[waveSampleIndex / 2];
        lastWaveRamFetchTCycle = elapsedTCycles;
        hasWaveRamFetch = true;
        waveSampleBuffer = (waveSampleIndex & 1) == 0
                               ? static_cast<uint8_t>(waveByte >> 4)
                               : static_cast<uint8_t>(waveByte & 0x0F);
    }
}

void DmgApu::advanceChannel4() {
    if (!channel4Enabled || (nr43 >> 4) >= 14)
        return;
    if (frequencyTimer4 > 0)
        --frequencyTimer4;
    if (frequencyTimer4 == 0) {
        frequencyTimer4 = channel4Period();
        clockChannel4Lfsr();
    }
}

void DmgApu::clockChannel4Lfsr() {
    const uint16_t bit0 = channel4Lfsr & 0x01;
    const uint16_t bit1 = (channel4Lfsr >> 1) & 0x01;
    const uint16_t feedback = bit0 == bit1 ? 1 : 0;
    channel4Lfsr = static_cast<uint16_t>((channel4Lfsr >> 1) | (feedback << 14));
    if (nr43 & 0x08)
        channel4Lfsr = static_cast<uint16_t>((channel4Lfsr & ~(1 << 6)) |
                                             (feedback << 6));
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
    if (lengthEnabled3 && lengthCounter3 > 0) {
        --lengthCounter3;
        if (lengthCounter3 == 0)
            channel3Enabled = false;
    }
    if (lengthEnabled4 && lengthCounter4 > 0) {
        --lengthCounter4;
        if (lengthCounter4 == 0)
            channel4Enabled = false;
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
    if (channel2Enabled && period2 != 0) {
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

    const uint8_t period4 = nr42 & 0x07;
    if (channel4Enabled && period4 != 0) {
        if (envelopeTimer4 > 0)
            --envelopeTimer4;
        if (envelopeTimer4 == 0) {
            envelopeTimer4 = period4;
            const bool increase = (nr42 & 0x08) != 0;
            if (increase && envelopeVolume4 < 15)
                ++envelopeVolume4;
            else if (!increase && envelopeVolume4 > 0)
                --envelopeVolume4;
        }
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

uint16_t DmgApu::channel3Frequency() const {
    return static_cast<uint16_t>(nr33 | ((nr34 & 0x07) << 8));
}

uint32_t DmgApu::channel4Period() const {
    const uint8_t clockShift = nr43 >> 4;
    if (clockShift >= 14)
        return 0;
    const uint8_t dividerCode = nr43 & 0x07;
    const uint32_t period = dividerCode == 0 ? 8 : 16 * dividerCode;
    return period << clockShift;
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

int DmgApu::channel3Output() const {
    if (!channel3Enabled || !channel3DacEnabled)
        return 0;

    const uint8_t outputLevel = (nr32 >> 5) & 0x03;
    if (outputLevel == 0)
        return 0;
    const uint8_t sample = static_cast<uint8_t>(waveSampleBuffer >> (outputLevel - 1));
    return static_cast<int>(sample) * 2 - 15;
}

int DmgApu::channel4Output() const {
    if (!channel4Enabled || !channel4DacEnabled || envelopeVolume4 == 0)
        return 0;

    // Center the two NR42 DAC levels around zero for the existing high-pass mixer.
    return (channel4Lfsr & 0x01) ? envelopeVolume4
                                 : -static_cast<int>(envelopeVolume4);
}

void DmgApu::generateSample() {
    // The DMG disconnects the master output when every channel DAC is off.
    // Keep the high-pass filter state frozen while disconnected, but do not
    // let its residual charge leak to the audio output.
    if (!channel1DacEnabled && !channel2DacEnabled &&
        !channel3DacEnabled && !channel4DacEnabled) {
        audioSamples.push_back(0);
        audioSamples.push_back(0);
        return;
    }

    const int channel1 = channel1Output();
    const int channel2 = channel2Output();
    const int channel3 = channel3Output();
    const int channel4 = channel4Output();
    int left = 0;
    int right = 0;
    if (powered) {
        if (nr51 & 0x10)
            left += channel1;
        if (nr51 & 0x20)
            left += channel2;
        if (nr51 & 0x40)
            left += channel3;
        if (nr51 & 0x80)
            left += channel4;
        if (nr51 & 0x01)
            right += channel1;
        if (nr51 & 0x02)
            right += channel2;
        if (nr51 & 0x04)
            right += channel3;
        if (nr51 & 0x08)
            right += channel4;
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
