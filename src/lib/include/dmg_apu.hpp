#ifndef LOTUSGB_DMG_APU_HPP
#define LOTUSGB_DMG_APU_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

// Initial DMG APU slice: all four DMG sound channels.
class DmgApu final : public IAddressRegion, public IClockedComponent {
public:
    static constexpr unsigned SAMPLE_RATE = 48000;

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    void initializeDmgPostBootState();
    void onDividerApuFallingEdge();

    // Samples are interleaved left/right signed 16-bit PCM.
    std::vector<int16_t> takeAudioSamples();

private:
    void setPower(bool enabled);
    void triggerChannel1();
    void triggerChannel2();
    void triggerChannel3();
    void triggerChannel4();
    void corruptWaveRamOnChannel3Retrigger();
    void advanceChannel1();
    void advanceChannel2();
    void advanceChannel3();
    void advanceChannel4();
    void clockFrameSequencer();
    void clockLength();
    void clockSweep();
    void clockEnvelope();
    int calculateSweepFrequency();
    uint16_t channel1Frequency() const;
    uint16_t channel2Frequency() const;
    uint16_t channel3Frequency() const;
    uint32_t channel4Period() const;
    int channel1Output() const;
    int channel2Output() const;
    int channel3Output() const;
    int channel4Output() const;
    void clockChannel4Lfsr();
    void generateSample();

    uint8_t nr10 = 0;
    uint8_t nr11 = 0;
    uint8_t nr12 = 0;
    uint8_t nr13 = 0;
    uint8_t nr14 = 0;
    uint8_t nr21 = 0;
    uint8_t nr22 = 0;
    uint8_t nr23 = 0;
    uint8_t nr24 = 0;
    uint8_t nr30 = 0;
    uint8_t nr32 = 0;
    uint8_t nr33 = 0;
    uint8_t nr34 = 0;
    uint8_t nr42 = 0;
    uint8_t nr43 = 0;
    uint8_t nr44 = 0;
    uint8_t nr50 = 0;
    uint8_t nr51 = 0;

    bool powered = false;
    bool channel1Enabled = false;
    bool channel1DacEnabled = false;
    bool lengthEnabled = false;
    uint8_t lengthCounter = 0;
    uint8_t dutyPosition = 0;
    uint16_t frequencyTimer = 0;

    bool channel2Enabled = false;
    bool channel2DacEnabled = false;
    bool lengthEnabled2 = false;
    uint8_t lengthCounter2 = 0;
    uint8_t dutyPosition2 = 0;
    uint16_t frequencyTimer2 = 0;

    uint8_t envelopeVolume = 0;
    uint8_t envelopeTimer = 0;
    uint8_t envelopeVolume2 = 0;
    uint8_t envelopeTimer2 = 0;

    bool channel3Enabled = false;
    bool channel3DacEnabled = false;
    bool lengthEnabled3 = false;
    uint16_t lengthCounter3 = 0;
    uint8_t waveSampleIndex = 0;
    uint8_t waveSampleBuffer = 0;
    uint16_t frequencyTimer3 = 0;
    std::array<uint8_t, 16> waveRam{};
    uint64_t elapsedTCycles = 0;
    uint64_t lastWaveRamFetchTCycle = 0;
    bool hasWaveRamFetch = false;

    bool channel4Enabled = false;
    bool channel4DacEnabled = false;
    bool lengthEnabled4 = false;
    uint8_t lengthCounter4 = 0;
    uint8_t envelopeVolume4 = 0;
    uint8_t envelopeTimer4 = 0;
    uint16_t channel4Lfsr = 0;
    uint32_t frequencyTimer4 = 0;

    uint16_t sweepShadowFrequency = 0;
    uint8_t sweepTimer = 0;
    bool sweepEnabled = false;
    bool sweepNegateUsed = false;

    uint8_t frameSequencerStep = 0;
    uint32_t sampleAccumulator = 0;
    int32_t previousLeftInput = 0;
    int32_t previousRightInput = 0;
    int64_t previousLeftOutputQ16 = 0;
    int64_t previousRightOutputQ16 = 0;
    std::vector<int16_t> audioSamples;
};

}

#endif // LOTUSGB_DMG_APU_HPP
