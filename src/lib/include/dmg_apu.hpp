#ifndef LOTUSGB_DMG_APU_HPP
#define LOTUSGB_DMG_APU_HPP

#include <cstdint>
#include <vector>

#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

// Initial DMG APU slice: channel 1 pulse generation and stereo sample output.
class DmgApu final : public IAddressRegion, public IClockedComponent {
public:
    static constexpr unsigned SAMPLE_RATE = 48000;

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    void initializeDmgPostBootState();

    // Samples are interleaved left/right signed 16-bit PCM.
    std::vector<int16_t> takeAudioSamples();

private:
    void setPower(bool enabled);
    void triggerChannel1();
    void advanceChannel1();
    void clockFrameSequencer();
    void clockLength();
    void clockSweep();
    void clockEnvelope();
    int calculateSweepFrequency();
    uint16_t channel1Frequency() const;
    int channel1Output() const;
    void generateSample();

    uint8_t nr10 = 0;
    uint8_t nr11 = 0;
    uint8_t nr12 = 0;
    uint8_t nr13 = 0;
    uint8_t nr14 = 0;
    uint8_t nr50 = 0;
    uint8_t nr51 = 0;

    bool powered = false;
    bool channel1Enabled = false;
    bool channel1DacEnabled = false;
    bool lengthEnabled = false;
    uint8_t lengthCounter = 0;
    uint8_t dutyPosition = 0;
    uint16_t frequencyTimer = 0;

    uint8_t envelopeVolume = 0;
    uint8_t envelopeTimer = 0;

    uint16_t sweepShadowFrequency = 0;
    uint8_t sweepTimer = 0;
    bool sweepEnabled = false;
    bool sweepNegateUsed = false;

    uint16_t frameSequencerCycles = 0;
    uint8_t frameSequencerStep = 0;
    uint32_t sampleAccumulator = 0;
    int32_t previousLeftInput = 0;
    int32_t previousRightInput = 0;
    int32_t previousLeftOutput = 0;
    int32_t previousRightOutput = 0;
    std::vector<int16_t> audioSamples;
};

}

#endif // LOTUSGB_DMG_APU_HPP
