#include "gba_audio.hpp"

#include <algorithm>
#include <utility>

#include "dmg_apu.hpp"

namespace LOTUSGBA {
namespace {

constexpr uint32_t IO_BASE = 0x04000000;
constexpr uint32_t SOUND_BASE = IO_BASE + 0x060;
constexpr uint32_t FIFO_A = IO_BASE + 0x0A0;
constexpr uint32_t FIFO_B = IO_BASE + 0x0A4;
constexpr uint32_t CPU_CLOCK_HZ = 16777216;
constexpr uint32_t GBA_FRAME_SEQUENCER_PERIOD = CPU_CLOCK_HZ / 512;

} // namespace

GbaAudio::GbaAudio() : psg_(std::make_unique<LOTUSGB::DmgApu>(true)) {}

GbaAudio::~GbaAudio() = default;

void GbaAudio::reset() {
    psg_ = std::make_unique<LOTUSGB::DmgApu>(true);
    mixedSamples_.clear();
    for (DirectSoundChannel &channel : directSound_)
        channel = {};
    soundControlHigh_ = 0;
    soundBias_ = 0x0200;
    sound3ControlHigh_ = 0;
    baseClockRemainder_ = 0;
    frameSequencerRemainder_ = 0;
}

void GbaAudio::advanceCycles(uint32_t gbaCycles) {
    const uint64_t baseClockCycles =
        static_cast<uint64_t>(baseClockRemainder_) + gbaCycles;
    const uint64_t apuCycles = baseClockCycles / 4;
    baseClockRemainder_ = static_cast<uint32_t>(baseClockCycles % 4);
    psg_->advanceTCycles(apuCycles);

    std::vector<int16_t> samples = psg_->takeAudioSamples();
    const unsigned psgVolume = soundControlHigh_ & 3;
    constexpr unsigned psgScaleNumerator[] = {1, 2, 4, 4};
    const bool masterEnabled = (psg_->read(0xFF26) & 0x80) != 0;
    int directLeft = 0;
    int directRight = 0;
    if (masterEnabled) {
        for (unsigned id = 0; id < directSound_.size(); ++id) {
            const unsigned leftBit = id == 0 ? 9 : 13;
            const unsigned rightBit = id == 0 ? 8 : 12;
            const unsigned volumeBit = id == 0 ? 2 : 3;
            const int scale = (soundControlHigh_ & (1u << volumeBit)) != 0
                ? 64 : 128;
            const int sample =
                static_cast<int>(directSound_[id].currentSample) * scale;
            if ((soundControlHigh_ & (1u << leftBit)) != 0)
                directLeft += sample;
            if ((soundControlHigh_ & (1u << rightBit)) != 0)
                directRight += sample;
        }
    }

    for (size_t frame = 0; frame + 1 < samples.size(); frame += 2) {
        const int left = static_cast<int>(samples[frame]) *
            static_cast<int>(psgScaleNumerator[psgVolume]) / 4 + directLeft;
        const int right = static_cast<int>(samples[frame + 1]) *
            static_cast<int>(psgScaleNumerator[psgVolume]) / 4 + directRight;
        mixedSamples_.push_back(static_cast<int16_t>(
            std::clamp(left, -32768, 32767)));
        mixedSamples_.push_back(static_cast<int16_t>(
            std::clamp(right, -32768, 32767)));
    }

    uint64_t frameCycles =
        static_cast<uint64_t>(frameSequencerRemainder_) + gbaCycles;
    while (frameCycles >= GBA_FRAME_SEQUENCER_PERIOD) {
        psg_->onDividerApuFallingEdge();
        frameCycles -= GBA_FRAME_SEQUENCER_PERIOD;
    }
    frameSequencerRemainder_ = static_cast<uint32_t>(frameCycles);
}

uint16_t GbaAudio::psgAddress(uint32_t address) {
    switch (address) {
    case SOUND_BASE + 0x00: return 0xFF10;
    case SOUND_BASE + 0x02: return 0xFF11;
    case SOUND_BASE + 0x03: return 0xFF12;
    case SOUND_BASE + 0x04: return 0xFF13;
    case SOUND_BASE + 0x05: return 0xFF14;
    case SOUND_BASE + 0x08: return 0xFF16;
    case SOUND_BASE + 0x09: return 0xFF17;
    case SOUND_BASE + 0x0C: return 0xFF18;
    case SOUND_BASE + 0x0D: return 0xFF19;
    case SOUND_BASE + 0x10: return 0xFF1A;
    case SOUND_BASE + 0x12: return 0xFF1B;
    case SOUND_BASE + 0x13: return 0xFF1C;
    case SOUND_BASE + 0x14: return 0xFF1D;
    case SOUND_BASE + 0x15: return 0xFF1E;
    case SOUND_BASE + 0x18: return 0xFF20;
    case SOUND_BASE + 0x19: return 0xFF21;
    case SOUND_BASE + 0x1C: return 0xFF22;
    case SOUND_BASE + 0x1D: return 0xFF23;
    case SOUND_BASE + 0x20: return 0xFF24;
    case SOUND_BASE + 0x21: return 0xFF25;
    case SOUND_BASE + 0x24: return 0xFF26;
    default:
        if (address >= SOUND_BASE + 0x30 && address <= SOUND_BASE + 0x3F)
            return static_cast<uint16_t>(0xFF30 + address - (SOUND_BASE + 0x30));
        return 0;
    }
}

uint8_t GbaAudio::fifoId(uint32_t address) {
    if (address >= FIFO_A && address < FIFO_A + 4)
        return 0;
    if (address >= FIFO_B && address < FIFO_B + 4)
        return 1;
    return 0xFF;
}

bool GbaAudio::handlesRegister(uint32_t address) const {
    if (psgAddress(address) != 0)
        return true;
    return address == SOUND_BASE + 0x11 ||
           (address >= SOUND_BASE + 0x22 && address <= SOUND_BASE + 0x25) ||
           (address >= SOUND_BASE + 0x28 && address <= SOUND_BASE + 0x29);
}

uint8_t GbaAudio::readRegister(uint32_t address) {
    const uint16_t mapped = psgAddress(address);
    if (mapped != 0)
        return psg_->read(mapped);
    if (address == SOUND_BASE + 0x11)
        return sound3ControlHigh_;
    if (address == SOUND_BASE + 0x22)
        return static_cast<uint8_t>(soundControlHigh_);
    if (address == SOUND_BASE + 0x23)
        return static_cast<uint8_t>(soundControlHigh_ >> 8);
    if (address == SOUND_BASE + 0x24)
        return psg_->read(0xFF26);
    if (address == SOUND_BASE + 0x25)
        return 0;
    if (address == SOUND_BASE + 0x28)
        return static_cast<uint8_t>(soundBias_);
    if (address == SOUND_BASE + 0x29)
        return static_cast<uint8_t>(soundBias_ >> 8);
    return 0;
}

void GbaAudio::resetFifo(unsigned fifo) {
    if (fifo >= directSound_.size())
        return;
    DirectSoundChannel &channel = directSound_[fifo];
    channel.readPosition = 0;
    channel.writePosition = 0;
    channel.size = 0;
    channel.currentSample = 0;
}

bool GbaAudio::writeRegister(uint32_t address, uint8_t value) {
    const uint16_t mapped = psgAddress(address);
    if (mapped != 0) {
        psg_->write(mapped, value);
        return true;
    }
    if (address == SOUND_BASE + 0x11) {
        sound3ControlHigh_ = value & 0x60;
        return true;
    }
    if (address == SOUND_BASE + 0x22) {
        soundControlHigh_ = static_cast<uint16_t>(
            (soundControlHigh_ & 0xFF00) | value);
        return true;
    }
    if (address == SOUND_BASE + 0x23) {
        soundControlHigh_ = static_cast<uint16_t>(
            (soundControlHigh_ & 0x00FF) |
            (static_cast<uint16_t>(value & 0x77) << 8));
        if ((value & 0x08) != 0)
            resetFifo(0);
        if ((value & 0x80) != 0)
            resetFifo(1);
        return true;
    }
    if (address == SOUND_BASE + 0x24) {
        psg_->write(0xFF26, value & 0x80);
        return true;
    }
    if (address == SOUND_BASE + 0x25)
        return true;
    if (address == SOUND_BASE + 0x28) {
        soundBias_ = static_cast<uint16_t>(
            (soundBias_ & 0xFF00) | value);
        return true;
    }
    if (address == SOUND_BASE + 0x29) {
        soundBias_ = static_cast<uint16_t>(
            (soundBias_ & 0x00FF) | (static_cast<uint16_t>(value) << 8));
        return true;
    }
    return false;
}

bool GbaAudio::writeFifo(uint32_t address, uint8_t value) {
    const uint8_t id = fifoId(address);
    if (id >= directSound_.size())
        return false;

    DirectSoundChannel &channel = directSound_[id];
    if (channel.size < channel.fifo.size()) {
        channel.fifo[channel.writePosition] = static_cast<int8_t>(value);
        channel.writePosition = (channel.writePosition + 1) % channel.fifo.size();
        ++channel.size;
    }
    return true;
}

uint8_t GbaAudio::onTimerOverflow(unsigned timer) {
    uint8_t refillMask = 0;
    for (unsigned id = 0; id < directSound_.size(); ++id) {
        const unsigned timerBit = id == 0 ? 10 : 14;
        if (((soundControlHigh_ >> timerBit) & 1) != timer)
            continue;

        DirectSoundChannel &channel = directSound_[id];
        if (channel.size != 0) {
            channel.currentSample = channel.fifo[channel.readPosition];
            channel.readPosition = (channel.readPosition + 1) % channel.fifo.size();
            --channel.size;
        }
        if (channel.size <= 16)
            refillMask |= static_cast<uint8_t>(1u << id);
    }
    return refillMask;
}

uint32_t GbaAudio::fifoAddress(unsigned fifo) const {
    if (fifo == 0)
        return FIFO_A;
    if (fifo == 1)
        return FIFO_B;
    return 0;
}

std::vector<int16_t> GbaAudio::takeSamples() {
    return std::exchange(mixedSamples_, {});
}

} // namespace LOTUSGBA
