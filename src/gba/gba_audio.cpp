#include "gba_audio.hpp"

#include <algorithm>
#include <cmath>
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
constexpr unsigned GBA_DEFAULT_OUTPUT_RATE = 32768;
constexpr int32_t GBA_PCM_SCALE = 32;
constexpr int32_t GBA_OUTPUT_GAIN = 48;

int16_t applySoundBias(int pcmSample, uint16_t soundBias) {
    // The GBA shifts its signed channel mix into the SOUNDBIAS unsigned DAC
    // range, clips at 10 bits, then quantizes to the PWM duty resolution.
    const int32_t signedDacSample = pcmSample / GBA_PCM_SCALE;
    const int32_t bias = soundBias & 0x03FE;
    const int32_t unsignedDacSample = std::clamp(
        signedDacSample + bias, 0, 0x03FF);
    const unsigned resolution = (soundBias >> 14) & 3;
    const int32_t pwmStep = 1 << (resolution + 1);
    const int32_t quantizedSample = unsignedDacSample / pwmStep * pwmStep;
    const int32_t quantizedBias = bias / pwmStep * pwmStep;
    const int32_t output =
        (quantizedSample - quantizedBias) * GBA_OUTPUT_GAIN;
    return static_cast<int16_t>(std::clamp(output, -32768, 32767));
}

} // namespace

GbaAudio::GbaAudio()
    : psg_(std::make_unique<LOTUSGB::DmgApu>(true,
                                             GBA_DEFAULT_OUTPUT_RATE,
                                             true)) {}

GbaAudio::~GbaAudio() = default;

void GbaAudio::reset() {
    outputRate_ = GBA_DEFAULT_OUTPUT_RATE;
    psg_ = std::make_unique<LOTUSGB::DmgApu>(true, outputRate_, true);
    mixedSamples_.clear();
    for (DirectSoundChannel &channel : directSound_)
        channel = {};
    soundControlHigh_ = 0;
    soundBias_ = 0x0200;
    baseClockRemainder_ = 0;
    frameSequencerRemainder_ = 0;
    outputSamplePhase_ = 0;
    hasPreviousSourceSample_ = false;
    previousSourceLeft_ = 0;
    previousSourceRight_ = 0;
    configureDownsampleFilter(outputRate_);
}

void GbaAudio::setOutputRate(unsigned outputRate) {
    if (outputRate == outputRate_)
        return;

    outputSamplePhase_ = static_cast<uint32_t>(
        static_cast<uint64_t>(outputSamplePhase_) * outputRate / outputRate_);
    outputRate_ = outputRate;
    psg_->setSampleRate(outputRate_);
    configureDownsampleFilter(outputRate_);
}

void GbaAudio::configureDownsampleFilter(unsigned sourceRate) {
    downsampleFilterEnabled_ = sourceRate > SAMPLE_RATE;
    downsampleState_ = {};
    if (!downsampleFilterEnabled_) {
        downsampleFilter_ = {};
        return;
    }

    // Keep the signal below the host Nyquist limit before reducing the GBA's
    // SOUNDBIAS-selected sample cadence to libretro's fixed 48 kHz stream.
    constexpr double PI = 3.14159265358979323846;
    constexpr double cutoff = SAMPLE_RATE * 0.375;
    // Two Butterworth sections retain the audible band while strongly
    // rejecting source energy that would alias in the 48 kHz host stream.
    constexpr double butterworthQ[] = {
        0.541196100146197, 1.30656296487638
    };
    const double k = std::tan(PI * cutoff / sourceRate);
    for (size_t section = 0; section < downsampleFilter_.size(); ++section) {
        const double q = butterworthQ[section];
        const double normalization = 1.0 / (1.0 + k / q + k * k);
        downsampleFilter_[section] = {
            k * k * normalization,
            2.0 * k * k * normalization,
            k * k * normalization,
            2.0 * (k * k - 1.0) * normalization,
            (1.0 - k / q + k * k) * normalization
        };
    }

    const double left = previousSourceLeft_;
    const double right = previousSourceRight_;
    for (unsigned channel = 0; channel < downsampleState_.size(); ++channel) {
        const double current = channel == 0 ? left : right;
        for (BiquadState &state : downsampleState_[channel]) {
            state.input1 = current;
            state.input2 = current;
            state.output1 = current;
            state.output2 = current;
        }
    }
}

int16_t GbaAudio::filterDownsampleSample(int16_t sample, unsigned channel) {
    if (!downsampleFilterEnabled_)
        return sample;

    double output = sample;
    for (size_t section = 0; section < downsampleFilter_.size(); ++section) {
        const BiquadCoefficients &coefficients = downsampleFilter_[section];
        BiquadState &state = downsampleState_[channel][section];
        const double filtered = coefficients.b0 * output +
            coefficients.b1 * state.input1 +
            coefficients.b2 * state.input2 -
            coefficients.a1 * state.output1 -
            coefficients.a2 * state.output2;
        state.input2 = state.input1;
        state.input1 = output;
        state.output2 = state.output1;
        state.output1 = filtered;
        output = filtered;
    }
    return static_cast<int16_t>(std::clamp(
        std::lround(output), -32768L, 32767L));
}

void GbaAudio::appendResampledSample(int16_t left, int16_t right) {
    left = filterDownsampleSample(left, 0);
    right = filterDownsampleSample(right, 1);
    if (!hasPreviousSourceSample_) {
        previousSourceLeft_ = left;
        previousSourceRight_ = right;
        hasPreviousSourceSample_ = true;
    }

    uint32_t hostTicksRemaining = SAMPLE_RATE;
    uint32_t elapsedHostTicks = 0;
    uint32_t sourcePhase = outputSamplePhase_;
    while (sourcePhase + hostTicksRemaining >= outputRate_) {
        const uint32_t ticksToOutput = outputRate_ - sourcePhase;
        elapsedHostTicks += ticksToOutput;
        hostTicksRemaining -= ticksToOutput;

        const int64_t leftDelta =
            static_cast<int32_t>(left) - previousSourceLeft_;
        const int64_t rightDelta =
            static_cast<int32_t>(right) - previousSourceRight_;
        const int32_t interpolatedLeft = static_cast<int32_t>(
            previousSourceLeft_ + leftDelta * elapsedHostTicks / SAMPLE_RATE);
        const int32_t interpolatedRight = static_cast<int32_t>(
            previousSourceRight_ + rightDelta * elapsedHostTicks / SAMPLE_RATE);
        mixedSamples_.push_back(static_cast<int16_t>(std::clamp(
            interpolatedLeft, -32768, 32767)));
        mixedSamples_.push_back(static_cast<int16_t>(std::clamp(
            interpolatedRight, -32768, 32767)));
        sourcePhase = 0;
    }
    outputSamplePhase_ = sourcePhase + hostTicksRemaining;
    previousSourceLeft_ = left;
    previousSourceRight_ = right;
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
                ? 128 : 64;
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
        appendResampledSample(applySoundBias(left, soundBias_),
                              applySoundBias(right, soundBias_));
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
        return 0;
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
    if (address == SOUND_BASE + 0x11)
        return true;
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
            ((soundBias_ & 0xFF00) | value) & 0xC3FE);
        return true;
    }
    if (address == SOUND_BASE + 0x29) {
        soundBias_ = static_cast<uint16_t>(
            ((soundBias_ & 0x00FF) |
             (static_cast<uint16_t>(value) << 8)) & 0xC3FE);
        setOutputRate(GBA_DEFAULT_OUTPUT_RATE << ((soundBias_ >> 14) & 3));
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
