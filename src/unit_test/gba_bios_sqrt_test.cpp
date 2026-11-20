#include "arm7tdmi.hpp"
#include <array>
#include <cassert>
#include <cstdint>

namespace {
class TestBus final : public LOTUSGBA::GbaBus {
public:
    std::array<uint8_t, 64> bytes{};
    bool bios = false;
    uint8_t read8(uint32_t a) const override { return bytes.at(a); }
    uint16_t read16(uint32_t a) const override {
        return static_cast<uint16_t>(read8(a) | (uint16_t{read8(a + 1)} << 8));
    }
    uint32_t read32(uint32_t a) const override {
        return read16(a) | (uint32_t{read16(a + 2)} << 16);
    }
    void write8(uint32_t a, uint8_t v) override { bytes.at(a) = v; }
    void write16(uint32_t a, uint16_t v) override {
        write8(a, static_cast<uint8_t>(v));
        write8(a + 1, static_cast<uint8_t>(v >> 8));
    }
    void write32(uint32_t a, uint32_t v) override {
        write16(a, static_cast<uint16_t>(v));
        write16(a + 2, static_cast<uint16_t>(v >> 16));
    }
    void registerRamReset(uint8_t) override { assert(false); }
    bool hasBios() const override { return bios; }
};

void check(uint32_t input, bool thumb, bool bios = false) {
    TestBus bus;
    bus.bios = bios;
    if (thumb) {
        bus.write16(0, 0x4803); // ldr r0, [pc, #12]: literal at 16
        bus.write16(2, 0xDF08); // swi 8
        bus.write16(4, 0x4604); // mov r4, r0: verify execution resumes
    } else {
        bus.write32(0, 0xE59F0008); // ldr r0, [pc, #8]: literal at 16
        bus.write32(4, 0xEF000008); // swi 8 (current HLE ARM convention)
        bus.write32(8, 0xE1A04000); // mov r4, r0
    }
    bus.write32(16, input);
    LOTUSGBA::Arm7Tdmi cpu;
    cpu.reset(0, 0x03007F00, thumb ? 0xFF : 0xDF);
    assert(cpu.step(bus).success);
    assert(cpu.step(bus).success);
    if (bios) {
        assert(cpu.programCounter() == 8);
        assert(cpu.currentMode() == 0x13);
        assert(!cpu.thumbState());
        assert(cpu.registerValue(0) == input); // BIOS performs the calculation.
        return;
    }
    const uint64_t root = cpu.registerValue(0);
    assert(root * root <= input);
    assert((root + 1) * (root + 1) > input);
    assert(cpu.programCounter() == (thumb ? 4u : 8u));
    assert(cpu.currentMode() == 0x1F);
    assert(cpu.thumbState() == thumb);
    assert(cpu.registerValue(13) == 0x03007F00);
    assert(cpu.step(bus).success);
    assert(cpu.registerValue(4) == root);
}
}

int main() {
    for (bool thumb : {false, true}) {
        for (uint32_t input : {0u, 1u, 2u, 3u, 4u, 15u, 16u, 17u,
                               0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu})
            check(input, thumb);
        for (uint32_t root : {2u, 255u, 256u, 32767u, 65535u}) {
            const uint32_t square = root * root;
            check(square - 1, thumb);
            check(square, thumb);
            check(square + 1, thumb);
        }
        check(81, thumb, true);
    }
}
