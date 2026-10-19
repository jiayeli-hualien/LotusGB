#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "address_bus.hpp"
#include "cartridge.hpp"
#include "memory_region.hpp"

namespace {

using namespace LOTUSGB;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TestRegion final : public IAddressRegion {
public:
    uint8_t read(uint16_t address) override {
        reads.push_back(address);
        return value;
    }

    void write(uint16_t address, uint8_t writtenValue) override {
        writes.emplace_back(address, writtenValue);
        value = writtenValue;
    }

    uint8_t value = 0;
    std::vector<uint16_t> reads;
    std::vector<std::pair<uint16_t, uint8_t>> writes;
};

void testAddressBusRoutesMappedRanges() {
    AddressBus bus;
    TestRegion workRam;
    TestRegion io;
    workRam.value = 0x12;
    io.value = 0x34;

    bus.map(0xC000, 0xCFFF, workRam);
    bus.map(0xFF00, 0xFF7F, io);

    expect(bus.read(0xC123) == 0x12, "bus routes a read to its mapped region");
    expect(workRam.reads == std::vector<uint16_t>{0xC123},
           "mapped region receives the original CPU address");
    expect(bus.read(0xFF44) == 0x34, "bus routes a separate range independently");
    bus.write(0xC456, 0xA5);
    expect(workRam.writes == std::vector<std::pair<uint16_t, uint8_t>>{{0xC456, 0xA5}},
           "bus routes writes to the mapped region");

    const size_t previousReadCount = workRam.reads.size();
    bus.write(0x8000, 0xFF);
    expect(bus.read(0x8000) == 0xFF, "unmapped reads return the current default value");
    expect(workRam.reads.size() == previousReadCount,
           "unmapped accesses do not reach mapped regions");
}

void testAddressBusRejectsInvalidMappings() {
    AddressBus bus;
    TestRegion first;
    TestRegion second;
    bus.map(0x0000, 0x00FF, first);

    bool overlapRejected = false;
    try {
        bus.map(0x00F0, 0x0100, second);
    } catch (const std::invalid_argument &) {
        overlapRejected = true;
    }
    expect(overlapRejected, "bus rejects overlapping address mappings");

    bool invalidRangeRejected = false;
    try {
        bus.map(0x0200, 0x0100, second);
    } catch (const std::invalid_argument &) {
        invalidRangeRejected = true;
    }
    expect(invalidRangeRejected, "bus rejects ranges with reversed endpoints");
}

void testRomOnlyCartridgeAndRamRegions() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0] = 0xC3;
    rom[0x7FFF] = 0x42;
    RomOnlyCartridge cartridge(std::move(rom));
    RamRegion workRam(0xC000, 0x2000);

    AddressBus bus;
    bus.map(0x0000, 0x7FFF, cartridge);
    bus.map(0xC000, 0xDFFF, workRam);

    expect(bus.read(0x0000) == 0xC3, "cartridge exposes the first ROM byte");
    expect(bus.read(0x7FFF) == 0x42, "cartridge exposes the end of its ROM window");
    bus.write(0x0000, 0x00);
    expect(bus.read(0x0000) == 0xC3, "writes do not alter ROM-only cartridge data");

    expect(bus.read(0xC000) == 0, "work RAM starts initialized to zero");
    bus.write(0xC000, 0xA5);
    bus.write(0xDFFF, 0x5A);
    expect(bus.read(0xC000) == 0xA5, "work RAM stores data at its first address");
    expect(bus.read(0xDFFF) == 0x5A, "work RAM stores data at its final address");
    expect(bus.read(0xE000) == 0xFF, "unmapped echo RAM remains unmapped for now");
}

void testRomOnlyCartridgeRejectsUnsupportedSizes() {
    bool emptyRejected = false;
    try {
        RomOnlyCartridge cartridge({});
    } catch (const std::invalid_argument &) {
        emptyRejected = true;
    }
    expect(emptyRejected, "ROM-only cartridge rejects empty ROM data");

    bool bankedSizeRejected = false;
    try {
        RomOnlyCartridge cartridge(std::vector<uint8_t>(0x10000, 0));
    } catch (const std::invalid_argument &) {
        bankedSizeRejected = true;
    }
    expect(bankedSizeRejected, "ROM-only cartridge rejects ROMs requiring banking");
}

}

int main() {
    testAddressBusRoutesMappedRanges();
    testAddressBusRejectsInvalidMappings();
    testRomOnlyCartridgeAndRamRegions();
    testRomOnlyCartridgeRejectsUnsupportedSizes();
    std::cout << "Address bus tests passed\n";
}
