#ifndef LOTUSGBA_GBA_AUDIO_HPP
#define LOTUSGBA_GBA_AUDIO_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace LOTUSGB {
class DmgApu;
}

namespace LOTUSGBA {

class GbaAudio final {
public:
    static constexpr unsigned SAMPLE_RATE = 48000;

    GbaAudio();
    ~GbaAudio();

    void reset();
    void advanceCycles(uint32_t gbaCycles);
    bool handlesRegister(uint32_t address) const;
    uint8_t readRegister(uint32_t address);
    bool writeRegister(uint32_t address, uint8_t value);
    bool writeFifo(uint32_t address, uint8_t value);
    uint8_t onTimerOverflow(unsigned timer);
    uint32_t fifoAddress(unsigned fifo) const;
    std::vector<int16_t> takeSamples();

private:
    struct DirectSoundChannel {
        std::array<int8_t, 32> fifo{};
        unsigned readPosition = 0;
        unsigned writePosition = 0;
        unsigned size = 0;
        int8_t currentSample = 0;
    };

    static uint16_t psgAddress(uint32_t address);
    static uint8_t fifoId(uint32_t address);
    void resetFifo(unsigned fifo);

    std::unique_ptr<LOTUSGB::DmgApu> psg_;
    std::array<DirectSoundChannel, 2> directSound_{};
    std::vector<int16_t> mixedSamples_;
    uint16_t soundControlHigh_ = 0;
    uint16_t soundBias_ = 0x0200;
    uint8_t sound3ControlHigh_ = 0;
    uint32_t baseClockRemainder_ = 0;
    uint32_t frameSequencerRemainder_ = 0;
};

} // namespace LOTUSGBA

#endif // LOTUSGBA_GBA_AUDIO_HPP
