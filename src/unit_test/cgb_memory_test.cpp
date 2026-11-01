#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "cgb_memory.hpp"
#include "cartridge.hpp"
#include "decoder.hpp"
#include "GBCPU.hpp"
#include "memory_region.hpp"
#include "system_clock.hpp"

namespace {
using namespace LOTUSGB;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void mapCgbMemory(AddressBus &bus, CgbWorkRam &work, CgbVideoRam &video) {
    bus.map(0xC000, 0xDFFF, work);
    bus.mapAlias(0xE000, 0xFDFF, work, 0xC000);
    bus.map(0xFF70, 0xFF70, work);
    bus.map(0x8000, 0x9FFF, video);
    bus.map(0xFF4F, 0xFF4F, video);
}

void expectIndependentAddressLines(IAddressRegion &region, uint16_t base, unsigned bits) {
    region.write(base, 0);
    for (unsigned bit = 0; bit < bits; ++bit)
        region.write(static_cast<uint16_t>(base + (1u << bit)), static_cast<uint8_t>(bit + 1));
    expect(region.read(base) == 0, "individual RAM address lines do not alias onto the base address");
    for (unsigned bit = 0; bit < bits; ++bit)
        expect(region.read(static_cast<uint16_t>(base + (1u << bit))) == bit + 1,
               "RAM address lines retain distinct values instead of wrapping within a bank");
}

void testAddressLinesWithinBanks() {
    CgbWorkRam work; CgbVideoRam video;
    expectIndependentAddressLines(work, 0xC000, 12);
    expectIndependentAddressLines(work, 0xD000, 12);
    work.write(0xFF70, 7);
    expectIndependentAddressLines(work, 0xD000, 12);
    for (uint8_t bank = 0; bank < 2; ++bank) {
        video.write(0xFF4F, bank);
        expectIndependentAddressLines(video, 0x8000, 13);
    }
}

void testAllWorkBanksRemainIndependent() {
    CgbWorkRam work;
    expect(work.read(0xFF70) == 0xF8, "synthetic WRAM reset retains raw SVBK zero");
    for (uint16_t offset = 0; offset < 0x1000; ++offset)
        work.write(0xC000 + offset, static_cast<uint8_t>(offset));
    for (uint8_t bank = 1; bank < 8; ++bank) {
        work.write(0xFF70, bank);
        for (uint16_t offset = 0; offset < 0x1000; ++offset)
            work.write(0xD000 + offset, static_cast<uint8_t>(bank * 29 + offset));
    }
    for (uint8_t bank = 1; bank < 8; ++bank) {
        work.write(0xFF70, bank);
        for (uint16_t offset = 0; offset < 0x1000; ++offset) {
            expect(work.read(0xC000 + offset) == static_cast<uint8_t>(offset),
                   "fixed WRAM bank 0 is unchanged by all bank switches");
            expect(work.read(0xD000 + offset) == static_cast<uint8_t>(bank * 29 + offset),
                   "each selected WRAM bank retains all 4096 bytes");
        }
    }
    work.write(0xFF70, 0xF8);
    expect(work.read(0xFF70) == 0xF8 && work.read(0xD000) == 29,
           "SVBK zero reads as zero but accesses bank 1");
    work.write(0xD000, 0xA5);
    work.write(0xFF70, 0xF9);
    expect(work.read(0xFF70) == 0xF9 && work.read(0xD000) == 0xA5,
           "SVBK ignores high writes and aliases raw zero/one to the same bank");
    work.write(0xBFFF, 0x99); work.write(0xE000, 0x99); work.write(0xFF71, 0x99);
    expect(work.read(0xBFFF) == 0xFF && work.read(0xE000) == 0xFF && work.read(0xFF71) == 0xFF,
           "unmapped component accesses do not wrap into WRAM; echo is a bus mapping");
}

void testEchoAndDmaViewsFollowTheSelectedBank() {
    CgbWorkRam work; CgbVideoRam video; AddressBus bus;
    mapCgbMemory(bus, work, video);
    RamRegion oam(0xFE00, 0xA0); bus.map(0xFE00, 0xFE9F, oam);
    bus.write(0xC000, 0x12); bus.write(0xEFFF, 0x34);
    expect(bus.read(0xE000) == 0x12 && bus.read(0xCFFF) == 0x34,
           "fixed-bank echo aliases work in both directions");
    bus.write(0xFF70, 2); bus.write(0xF000, 0x56); bus.write(0xFDFF, 0x78);
    bus.write(0xDE00, 0xAB); bus.write(0xFE00, 0xCD);
    bus.write(0xFF70, 3); bus.write(0xD000, 0x90);
    expect(bus.read(0xF000) == 0x90 && bus.readForDma(0xD000) == 0x90,
           "echo and device-originated reads observe current bank 3");
    bus.write(0xFF70, 2);
    expect(bus.read(0xD000) == 0x56 && bus.read(0xDDFF) == 0x78 &&
               bus.readForDma(0xF000) == 0x56,
           "bank 2 echo data persists and is visible to a DMA source reader");
    expect(bus.read(0xFE00) == 0xCD && bus.read(0xDE00) == 0xAB,
           "echo ends at FDFF and does not alias OAM onto the last WRAM bytes");
}

void testVideoBanksAndIndependentFetchView() {
    CgbVideoRam video;
    expect(video.read(0xFF4F) == 0xFE, "synthetic VRAM reset selects bank 0");
    for (uint8_t bank = 0; bank < 2; ++bank) {
        video.write(0xFF4F, bank);
        for (uint16_t offset = 0; offset < 0x2000; ++offset)
            video.write(0x8000 + offset, static_cast<uint8_t>(offset + bank * 73));
    }
    for (uint8_t bank = 0; bank < 2; ++bank) {
        video.write(0xFF4F, bank);
        for (uint16_t offset = 0; offset < 0x2000; ++offset) {
            expect(video.read(0x8000 + offset) == static_cast<uint8_t>(offset + bank * 73),
                   "both VRAM banks retain all tile/map bytes independently");
            expect(video.readBank(bank ^ 1, 0x8000 + offset) ==
                       static_cast<uint8_t>(offset + (bank ^ 1) * 73),
                   "explicit fetch bank is independent of CPU VBK");
        }
        expect(video.read(0xFF4F) == static_cast<uint8_t>(0xFE | bank),
               "explicit-bank fetches do not mutate CPU VBK");
    }
    video.write(0xFF4F, 0xFE);
    expect(video.read(0xFF4F) == 0xFE, "VBK masks all write bits except bit zero");
    video.write(0x7FFF, 0x99); video.write(0xA000, 0x99);
    expect(video.read(0x7FFF) == 0xFF && video.read(0xA000) == 0xFF,
           "VRAM address boundaries do not wrap");
    bool rejected = false;
    try { video.readBank(2, 0x8000); }
    catch (const std::out_of_range &) { rejected = true; }
    expect(rejected, "invalid host fetch bank is reported instead of aliased silently");
}

void testCpuProgramExercisesBankingThroughPublicBus() {
    // Original in-memory probe, under the project's MIT license. CPU reset is
    // synthetic; this does not claim a CGB boot/profile or rendered game.
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0] = 0xC3; rom[1] = 0; rom[2] = 1;
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0;
    std::vector<uint8_t> program{0xF3}; // DI
    const auto select = [&](uint8_t reg, uint8_t bank) {
        program.insert(program.end(), {0x3E, bank, 0xE0, reg});
    };
    const auto store = [&](uint16_t address, uint8_t value) {
        program.insert(program.end(), {0x3E, value, 0xEA,
                       static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8)});
    };
    const auto record = [&](uint16_t address, uint8_t slot) {
        program.insert(program.end(), {0xFA, static_cast<uint8_t>(address),
                       static_cast<uint8_t>(address >> 8), 0xEA, slot, 0xC1});
    };
    select(0x70, 1); store(0xD000, 0x11);
    select(0x70, 2); store(0xD000, 0x22);
    select(0x70, 0); record(0xD000, 0); record(0xFF70, 1);
    select(0x70, 2); record(0xF000, 2);
    store(0xF000, 0x33); record(0xD000, 3);
    select(0x70, 1); record(0xD000, 4);
    select(0x4F, 0); store(0x8000, 0xAA);
    select(0x4F, 1); store(0x8000, 0xBB); record(0xFF4F, 5);
    select(0x4F, 0); record(0x8000, 6);
    select(0x4F, 1); record(0x8000, 7);
    store(0xC1FF, 1); program.push_back(0x76);
    std::copy(program.begin(), program.end(), rom.begin() + 0x150);
    Cartridge cartridge(std::move(rom)); CgbWorkRam work; CgbVideoRam video;
    AddressBus bus; mapCgbMemory(bus, work, video); bus.map(0, 0x7FFF, cartridge);
    Decoder decoder(false), cbDecoder(true); GBCPU cpu(bus, decoder, cbDecoder);
    SystemClock clock(cpu);
    bus.setCpuBusAccessCallback([&] { clock.advanceToCpuBusAccess(); });
    cpu.reset();
    for (unsigned cycle = 0; cycle < 2000 && bus.readForDma(0xC1FF) != 1; ++cycle) {
        const auto step = clock.step();
        expect(step.success && step.tCyclesElapsed == 4, "CPU probe advances in normal M-cycles");
    }
    expect(bus.readForDma(0xC1FF) == 1, "CPU probe reaches its completion marker");
    constexpr std::array<uint8_t, 8> expected{{0x11, 0xF8, 0x22, 0x33, 0x11, 0xFF, 0xAA, 0xBB}};
    for (size_t slot = 0; slot < expected.size(); ++slot)
        expect(bus.readForDma(0xC100 + slot) == expected[slot],
               "executed CPU stores the expected bank/echo signature in fixed WRAM");
    std::cout << "CGB memory CPU probe: 8 bank/echo signatures passed in "
              << clock.getElapsedTCycles() / 4 << " M-cycles\n";
}

}

int main() {
    testAddressLinesWithinBanks();
    testAllWorkBanksRemainIndependent();
    testEchoAndDmaViewsFollowTheSelectedBank();
    testVideoBanksAndIndependentFetchView();
    testCpuProgramExercisesBankingThroughPublicBus();
    std::cout << "CGB memory foundation tests passed\n";
}
