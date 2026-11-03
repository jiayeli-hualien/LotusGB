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

void applyZeroPaceEnvelopeWrite(uint8_t previous, uint8_t value,
                                uint8_t &currentVolume, bool channelEnabled,
                                bool envelopeRunning) {
    // The documented cross-model zombie-mode case: an active increase
    // envelope with period zero gains one volume step on an x8 -> x8 write.
    if (channelEnabled && envelopeRunning &&
        (previous & 0x0F) == 0x08 && (value & 0x0F) == 0x08)
        currentVolume = static_cast<uint8_t>((currentVolume + 1) & 0x0F);
}
}

uint8_t DmgApu::read(uint16_t address) {
    if (address >= WAVE_RAM_START && address <= WAVE_RAM_END) {
        if (channel3Enabled) {
            const bool fetchCycle = hasWaveRamFetch &&
                                    elapsedTCycles == lastWaveRamFetchTCycle;
            const uint8_t value = (cgbMode || fetchCycle) ? waveRam[waveSampleIndex / 2] : 0xFF;
            return value;
        }
        return waveRam[address - WAVE_RAM_START];
    }

    switch (address) {
        // Digital generator levels are observed before panning, master volume,
        // DAC conversion and the high-pass filter. No extra state is needed.
        case 0xFF76:
            return cgbMode ? static_cast<uint8_t>(channel1DigitalOutput() |
                               (channel2DigitalOutput() << 4)) : 0xFF;
        case 0xFF77:
            return cgbMode ? static_cast<uint8_t>(channel3DigitalOutput() |
                               (channel4DigitalOutput() << 4)) : 0xFF;
        case NR10: return static_cast<uint8_t>(0x80 | nr10);
        case NR11: return static_cast<uint8_t>(0x3F | (nr11 & 0xC0));
        case NR12: return nr12;
        case NR13: return 0xFF;
        case NR14: return static_cast<uint8_t>(0xBF | (nr14 & 0x40));
        case NR21: return static_cast<uint8_t>(0x3F | (nr21 & 0xC0));
        case NR22: return nr22;
        case NR23: return 0xFF;
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
        } else if (cgbMode || (hasWaveRamFetch &&
                   elapsedTCycles == lastWaveRamFetchTCycle)) {
            waveRam[waveSampleIndex / 2] = value;
        }
        return;
    }

    // On DMG, the length data remains writable while the APU is powered off.
    if (!powered) {
        if (cgbMode) return; // CGB has no writable length-register exception.
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

    // DMG and the selected CGB-E revision share this length-enable edge.
    // frameSequencerStep identifies the next DIV-APU step.
    const auto updateLengthEnable = [&](bool &enabled, auto &counter, bool &channel) {
        const bool nextEnabled = (value & 0x40) != 0;
        if (!enabled && nextEnabled && (frameSequencerStep & 1) && counter > 0) {
            if (--counter == 0 && !(value & 0x80)) channel = false;
        }
        enabled = nextEnabled;
    };
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
            applyZeroPaceEnvelopeWrite(nr12, value, envelopeVolume,
                                       channel1Enabled, envelopeRunning);
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
            updateLengthEnable(lengthEnabled, lengthCounter, channel1Enabled);
            if (value & 0x80)
                triggerChannel1();
            break;
        case NR21:
            nr21 = value;
            lengthCounter2 = static_cast<uint8_t>(64 - (value & 0x3F));
            break;
        case NR22:
            applyZeroPaceEnvelopeWrite(nr22, value, envelopeVolume2,
                                       channel2Enabled, envelopeRunning2);
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
            updateLengthEnable(lengthEnabled2, lengthCounter2, channel2Enabled);
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
            updateLengthEnable(lengthEnabled3, lengthCounter3, channel3Enabled);
            if (value & 0x80)
                triggerChannel3();
            break;
        case NR41:
            lengthCounter4 = static_cast<uint8_t>(64 - (value & 0x3F));
            break;
        case NR42:
            applyZeroPaceEnvelopeWrite(nr42, value, envelopeVolume4,
                                       channel4Enabled, envelopeRunning4);
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
            updateLengthEnable(lengthEnabled4, lengthCounter4, channel4Enabled);
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
    firstDutySuppressed = false; // Boot sound has already clocked channel 1.
    firstDutySuppressed2 = true;
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
    envelopeRunning = envelopeRunning2 = envelopeRunning4 = false;
    // Synthetic post-boot startup represents the already-settled boot tone.
    // CH1's enabled DAC receives digital zero; initialize the HPF input bias
    // rather than replaying its power-on charging transient at $0100.
    previousLeftInput = previousRightInput = 15 * 8;
    previousLeftOutputQ16 = previousRightOutputQ16 = 0;
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
        firstDutySuppressed = firstDutySuppressed2 = true;
        frequencyTimer = frequencyTimer2 = 0;
        waveSampleBuffer = 0;
        return;
    }

    // Only DMG preserves length data across power-off.
    if (cgbMode) {
        lengthCounter = lengthCounter2 = lengthCounter4 = 0;
        lengthCounter3 = 0;
    }
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
    envelopeRunning = envelopeRunning2 = envelopeRunning4 = false;
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
        lengthCounter = 64 - (lengthEnabled && (frameSequencerStep & 1));
    frequencyTimer = static_cast<uint16_t>(4 * (2048 - channel1Frequency()) | (frequencyTimer & 3));
    envelopeRunning = true;
    envelopeVolume = nr12 >> 4;
    const uint8_t envelopePeriod = nr12 & 0x07;
    envelopeTimer = (envelopePeriod == 0 ? 8 : envelopePeriod) + (frameSequencerStep == 7);

    sweepShadowFrequency = channel1Frequency();
    const uint8_t sweepPeriod = (nr10 >> 4) & 0x07;
    sweepTimer = sweepPeriod == 0 ? 8 : sweepPeriod;
    sweepEnabled = sweepPeriod != 0 || (nr10 & 0x07) != 0;
    sweepNegateUsed = false;
    if ((nr10 & 0x07) != 0 && calculateSweepFrequency() > 2047)
        channel1Enabled = false;
}

void DmgApu::triggerChannel2() {
    channel2Enabled = channel2DacEnabled;
    if (lengthCounter2 == 0)
        lengthCounter2 = 64 - (lengthEnabled2 && (frameSequencerStep & 1));
    frequencyTimer2 = static_cast<uint16_t>(4 * (2048 - channel2Frequency()) | (frequencyTimer2 & 3));
    envelopeRunning2 = true;
    envelopeVolume2 = nr22 >> 4;
    const uint8_t envelopePeriod = nr22 & 0x07;
    envelopeTimer2 = (envelopePeriod == 0 ? 8 : envelopePeriod) + (frameSequencerStep == 7);
}

void DmgApu::triggerChannel3() {
    if (channel3Enabled && !cgbMode)
        corruptWaveRamOnChannel3Retrigger();

    channel3Enabled = channel3DacEnabled;
    if (lengthCounter3 == 0)
        lengthCounter3 = 256 - (lengthEnabled3 && (frameSequencerStep & 1));
    // The first wave fetch follows the trigger by three additional APU ticks.
    frequencyTimer3 = static_cast<uint16_t>(T_CYCLES_PER_APU_TICK *
                                            (2048 - channel3Frequency() +
                                             CH3_TRIGGER_STARTUP_APU_TICKS));
    waveSampleIndex = 0;
    hasWaveRamFetch = false;
}

void DmgApu::corruptWaveRamOnChannel3Retrigger() {
    // The wave RAM read is on the 2 MHz APU clock. The DMG collision window
    // is the APU cycle immediately before that read, two CPU T-cycles later.
    if (frequencyTimer3 != 2)
        return;

    const uint8_t sampleIndex = static_cast<uint8_t>((waveSampleIndex + 1) & 0x1F);
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
        lengthCounter4 = 64 - (lengthEnabled4 && (frameSequencerStep & 1));
    const uint8_t envelopePeriod = nr42 & 0x07;
    envelopeRunning4 = true;
    envelopeVolume4 = nr42 >> 4;
    envelopeTimer4 = (envelopePeriod == 0 ? 8 : envelopePeriod) + (frameSequencerStep == 7);
    channel4Lfsr = 0;
    frequencyTimer4 = channel4Period();
}

void DmgApu::advanceChannel1() {
    // The duty divider starts at the first trigger and keeps running when
    // length, DAC or sweep gates the channel output. Zero means untriggered;
    // every running divider reloads before returning from this clock.
    if (frequencyTimer == 0)
        return;
    if (frequencyTimer > 0)
        --frequencyTimer;
    if (frequencyTimer == 0) {
        frequencyTimer = static_cast<uint16_t>(4 * (2048 - channel1Frequency()));
        dutyPosition = static_cast<uint8_t>((dutyPosition + 1) & 0x07);
        firstDutySuppressed = false;
    }
}

void DmgApu::advanceChannel2() {
    if (frequencyTimer2 == 0)
        return;
    if (frequencyTimer2 > 0)
        --frequencyTimer2;
    if (frequencyTimer2 == 0) {
        frequencyTimer2 = static_cast<uint16_t>(4 * (2048 - channel2Frequency()));
        dutyPosition2 = static_cast<uint8_t>((dutyPosition2 + 1) & 0x07);
        firstDutySuppressed2 = false;
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
    // Sweep changes the reload value, not the running period divider.
    // The new period takes effect when the current duty step ends.
    if (calculateSweepFrequency() > 2047)
        channel1Enabled = false;
}

void DmgApu::clockEnvelope() {
    const auto clock = [](uint8_t control, uint8_t &timer, uint8_t &volume, bool &running) {
        // Period zero disables volume changes, not the period divider. Keep
        // counting/reloading even after automatic updates have stopped.
        if (timer == 0 || --timer != 0) return;
        const uint8_t period = control & 7;
        timer = period ? period : 8;
        if (!running || period == 0) return;
        const int next = static_cast<int>(volume) + ((control & 8) ? 1 : -1);
        if (next < 0 || next > 15) running = false;
        else volume = static_cast<uint8_t>(next);
    };
    clock(nr12, envelopeTimer, envelopeVolume, envelopeRunning);
    clock(nr22, envelopeTimer2, envelopeVolume2, envelopeRunning2);
    clock(nr42, envelopeTimer4, envelopeVolume4, envelopeRunning4);
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

uint8_t DmgApu::channel1DigitalOutput() const {
    if (!channel1Enabled || !channel1DacEnabled || envelopeVolume == 0)
        return 0;

    const uint8_t duty = (nr11 >> 6) & 0x03;
    const bool high = !firstDutySuppressed && (DUTY_PATTERNS[duty] & (1U << (7 - dutyPosition))) != 0;
    return high ? envelopeVolume : 0;
}

uint8_t DmgApu::channel2DigitalOutput() const {
    if (!channel2Enabled || !channel2DacEnabled || envelopeVolume2 == 0)
        return 0;

    const uint8_t duty = (nr21 >> 6) & 0x03;
    const bool high = !firstDutySuppressed2 && (DUTY_PATTERNS[duty] & (1U << (7 - dutyPosition2))) != 0;
    return high ? envelopeVolume2 : 0;
}

uint8_t DmgApu::channel3DigitalOutput() const {
    const uint8_t outputLevel = (nr32 >> 5) & 0x03;
    if (!channel3Enabled || !channel3DacEnabled || outputLevel == 0)
        return 0;
    return static_cast<uint8_t>(waveSampleBuffer >> (outputLevel - 1));
}

uint8_t DmgApu::channel4DigitalOutput() const {
    if (!channel4Enabled || !channel4DacEnabled)
        return 0;
    return (channel4Lfsr & 0x01) ? envelopeVolume4 : 0;
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

    // An enabled DAC maps digital 0..15 to analog +15..-15. A stopped
    // generator still supplies digital zero; only disabling its DAC removes
    // that channel's bias. The HPF handles the resulting DC transitions.
    const auto dac = [](bool enabled, uint8_t digital) {
        return enabled ? 15 - 2 * static_cast<int>(digital) : 0;
    };
    const int channel1 = dac(channel1DacEnabled, channel1DigitalOutput());
    const int channel2 = dac(channel2DacEnabled, channel2DigitalOutput());
    const int channel3 = dac(channel3DacEnabled, channel3DigitalOutput());
    const int channel4 = dac(channel4DacEnabled, channel4DigitalOutput());
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

    // Approximate each model’s output coupling capacitor, which removes DC offset.
    // Keep fractional filter state so negative feedback can decay to zero
    // instead of sticking at a DC offset due to signed integer rounding.
    constexpr int64_t Q16_ONE = 1LL << 16;
    // round(65536 * charge^(4194304 / 48000)); charge per master clock:
    // DMG=0.999958, CGB=0.998943 (Pan Docs Audio Details).
    const int64_t highPassCoefficient = cgbMode ? 59751 : 65296;
    const int64_t filteredLeftQ16 =
        (static_cast<int64_t>(left - previousLeftInput) * Q16_ONE) +
        (previousLeftOutputQ16 * highPassCoefficient) / Q16_ONE;
    const int64_t filteredRightQ16 =
        (static_cast<int64_t>(right - previousRightInput) * Q16_ONE) +
        (previousRightOutputQ16 * highPassCoefficient) / Q16_ONE;
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
