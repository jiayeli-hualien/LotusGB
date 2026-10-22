#include <algorithm>
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
    Cartridge cartridge(std::move(rom));
    RamRegion workRam(0xC000, 0x2000);

    AddressBus bus;
    bus.map(0x0000, 0x7FFF, cartridge);
    bus.map(0xA000, 0xBFFF, cartridge);
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
    expect(bus.read(0xA000) == 0xFF,
           "ROM-only cartridge does not expose external RAM");
}

void testCartridgeRejectsUnsupportedSizesAndTypes() {
    bool emptyRejected = false;
    try {
        Cartridge cartridge({});
    } catch (const std::invalid_argument &) {
        emptyRejected = true;
    }
    expect(emptyRejected, "ROM-only cartridge rejects empty ROM data");

    bool bankedSizeRejected = false;
    try {
        Cartridge cartridge(std::vector<uint8_t>(0x10000, 0));
    } catch (const std::invalid_argument &) {
        bankedSizeRejected = true;
    }
    expect(bankedSizeRejected, "ROM-only cartridge rejects ROMs requiring banking");

    std::vector<uint8_t> unsupportedRom(0x8000, 0);
    unsupportedRom[0x0147] = 0x19; // MBC5 is not implemented yet.
    bool unsupportedTypeRejected = false;
    try {
        Cartridge cartridge(std::move(unsupportedRom));
    } catch (const std::invalid_argument &) {
        unsupportedTypeRejected = true;
    }
    expect(unsupportedTypeRejected, "cartridge rejects unsupported mapper types");

    std::vector<uint8_t> oversizedMbc2(0x80000, 0);
    oversizedMbc2[0x0147] = 0x06;
    bool oversizedMbc2Rejected = false;
    try {
        Cartridge cartridge(std::move(oversizedMbc2));
    } catch (const std::invalid_argument &) {
        oversizedMbc2Rejected = true;
    }
    expect(oversizedMbc2Rejected,
           "MBC2 rejects ROMs larger than its 16 selectable banks");
}

void testMbc1RomAndRamBanking() {
    constexpr size_t ROM_BANKS = 64;
    std::vector<uint8_t> rom(ROM_BANKS * 0x4000, 0);
    for (size_t bank = 0; bank < ROM_BANKS; ++bank)
        std::fill(rom.begin() + bank * 0x4000,
                  rom.begin() + (bank + 1) * 0x4000,
                  static_cast<uint8_t>(bank));
    rom[0x0147] = 0x03; // MBC1 + RAM + battery
    rom[0x0148] = 0x05; // 1 MiB, 64 banks
    rom[0x0149] = 0x03; // 32 KiB, four RAM banks

    Cartridge cartridge(std::move(rom));
    AddressBus bus;
    bus.map(0x0000, 0x7FFF, cartridge);
    bus.map(0xA000, 0xBFFF, cartridge);

    expect(bus.read(0x4000) == 1,
           "MBC1 starts with ROM bank 1 in the switchable window");
    bus.write(0x2000, 0x02);
    expect(bus.read(0x4000) == 2,
           "MBC1 selects a lower ROM bank through the $2000 register");
    bus.write(0x2000, 0x00);
    expect(bus.read(0x4000) == 1,
           "MBC1 maps a selected lower bank of zero to bank 1");
    bus.write(0x2000, 0x01);
    bus.write(0x4000, 0x01);
    expect(bus.read(0x4000) == 33,
           "MBC1 combines the high and low ROM bank registers");

    bus.write(0x6000, 0x01);
    expect(bus.read(0x0000) == 32,
           "MBC1 RAM banking mode switches the lower ROM window");
    expect(bus.read(0x4000) == 33,
           "MBC1 RAM banking mode keeps the upper ROM bank selection");

    expect(bus.read(0xA000) == 0xFF,
           "MBC1 external RAM reads are disabled after startup");
    bus.write(0x4000, 0x00);
    bus.write(0x0000, 0x1A);
    bus.write(0xA000, 0xA2);
    expect(bus.read(0xA000) == 0xA2,
           "MBC1 enables external RAM when the low nibble is $A");
    bus.write(0x4000, 0x01);
    bus.write(0xA000, 0xB1);
    bus.write(0x4000, 0x02);
    bus.write(0xA000, 0xC2);
    bus.write(0x4000, 0x01);
    expect(bus.read(0xA000) == 0xB1,
           "MBC1 selects an independent external RAM bank");
    bus.write(0x6000, 0x00);
    expect(bus.read(0xA000) == 0xA2,
           "MBC1 ROM banking mode selects external RAM bank zero");

    expect(cartridge.hasBattery() && cartridge.getExternalRamSize() == 0x8000,
           "MBC1 battery cartridge exposes its declared save RAM");
    expect(cartridge.getExternalRamData() != nullptr,
           "MBC1 battery cartridge provides stable save RAM storage");
}

void testMbc2RomAndNibbleRam() {
    constexpr size_t ROM_BANKS = 16;
    std::vector<uint8_t> rom(ROM_BANKS * 0x4000, 0);
    for (size_t bank = 0; bank < ROM_BANKS; ++bank)
        std::fill(rom.begin() + bank * 0x4000,
                  rom.begin() + (bank + 1) * 0x4000,
                  static_cast<uint8_t>(bank));
    rom[0x0147] = 0x06; // MBC2 + battery
    rom[0x0148] = 0x03; // 256 KiB, 16 banks
    rom[0x0149] = 0x00; // MBC2 RAM is built into the mapper

    Cartridge cartridge(std::move(rom));
    AddressBus bus;
    bus.map(0x0000, 0x7FFF, cartridge);
    bus.map(0xA000, 0xBFFF, cartridge);

    expect(bus.read(0x0000) == 0,
           "MBC2 keeps ROM bank zero in the lower ROM window");
    expect(bus.read(0x4000) == 1,
           "MBC2 starts with ROM bank one in the switchable window");
    bus.write(0x2100, 0x0F);
    expect(bus.read(0x4000) == 15,
           "MBC2 selects the highest ROM bank through an A8-set address");
    bus.write(0x0100, 0x00);
    expect(bus.read(0x4000) == 1,
           "MBC2 maps selected ROM bank zero to bank one");

    expect(cartridge.hasBattery() &&
               cartridge.getExternalRamSize() == 0x0200,
           "MBC2 battery RAM is exposed as a 512-byte nibble buffer");
    auto batteryRam = cartridge.getBatteryRamStorage();
    expect(batteryRam && batteryRam->size() == 0x0200,
           "MBC2 battery cartridge provides 512 bytes of save RAM");
    expect(bus.read(0xA000) == 0xFF,
           "MBC2 RAM reads are disabled after startup");
    bus.write(0xA000, 0xAB);
    bus.write(0x0000, 0x0A);
    expect(bus.read(0xA000) == 0xFF,
           "MBC2 ignores RAM writes while RAM is disabled");

    bus.write(0xA000, 0xAB);
    expect(bus.read(0xA000) == 0xFB,
           "MBC2 stores the low nibble and returns high data bits set");
    expect(cartridge.getExternalRamData()[0] == 0x0B,
           "MBC2 save RAM stores one nibble per byte");
    expect(bus.read(0xA200) == 0xFB,
           "MBC2 RAM mirrors every 512 bytes across its address window");

    bus.write(0xA200, 0x04);
    expect(bus.read(0xA000) == 0xF4,
           "writes through an MBC2 RAM echo update the same nibble");
    bus.write(0xBFFF, 0x19);
    expect(bus.read(0xA1FF) == 0xF9 && bus.read(0xBFFF) == 0xF9,
           "MBC2 wraps the final RAM nibble through its address mask");
    expect(cartridge.getExternalRamData()[0x01FF] == 0x09,
           "MBC2 mirrors map to the matching nibble in save RAM");

    bus.write(0x2100, 0x0F);
    bus.write(0x2000, 0x0A);
    expect(bus.read(0x4000) == 15,
           "MBC2 address bit A8 distinguishes RAM control from ROM banking");
    expect(bus.read(0xA000) == 0xF4,
           "MBC2 ROM bank writes do not change enabled RAM contents");

    std::vector<uint8_t> volatileRom(0x8000, 0);
    volatileRom[0x0147] = 0x05; // MBC2 without battery
    Cartridge volatileCartridge(std::move(volatileRom));
    expect(!volatileCartridge.hasBattery() &&
               volatileCartridge.getExternalRamSize() == 0x0200 &&
               !volatileCartridge.getBatteryRamStorage(),
           "MBC2 without a battery has temporary RAM but no save block");
}

void testMbc3RomAndRamBanking() {
    constexpr size_t ROM_BANKS = 128;
    std::vector<uint8_t> rom(ROM_BANKS * 0x4000, 0);
    for (size_t bank = 0; bank < ROM_BANKS; ++bank)
        std::fill(rom.begin() + bank * 0x4000,
                  rom.begin() + (bank + 1) * 0x4000,
                  static_cast<uint8_t>(bank));
    rom[0x0147] = 0x13; // MBC3 + RAM + battery
    rom[0x0148] = 0x06; // 2 MiB, 128 banks
    rom[0x0149] = 0x03; // 32 KiB, four RAM banks

    Cartridge cartridge(std::move(rom));
    AddressBus bus;
    bus.map(0x0000, 0x7FFF, cartridge);
    bus.map(0xA000, 0xBFFF, cartridge);

    expect(bus.read(0x0000) == 0,
           "MBC3 keeps ROM bank zero in the lower ROM window");
    expect(bus.read(0x4000) == 1,
           "MBC3 starts with ROM bank one in the switchable window");
    bus.write(0x2000, 0x7F);
    expect(bus.read(0x4000) == 0x7F,
           "MBC3 selects its highest ROM bank with the seven-bit register");
    bus.write(0x2000, 0x00);
    expect(bus.read(0x4000) == 1,
           "MBC3 maps ROM bank zero to bank one");

    expect(bus.read(0xA000) == 0xFF,
           "MBC3 external RAM reads are disabled after startup");
    bus.write(0x0000, 0x0A);
    bus.write(0x4000, 0x02);
    bus.write(0xA000, 0x5A);
    expect(bus.read(0xA000) == 0x5A,
           "MBC3 reads and writes the selected external RAM bank");
    bus.write(0x4000, 0x01);
    expect(bus.read(0xA000) == 0xFF,
           "MBC3 external RAM banks keep independent contents");
    bus.write(0x4000, 0x08);
    expect(bus.read(0xA000) == 0xFF,
           "MBC3 timer register selections remain unmapped without RTC support");
    bus.write(0xA000, 0xA5);
    bus.write(0x4000, 0x00);
    expect(bus.read(0xA000) == 0xFF,
           "unsupported MBC3 register selections do not alter RAM");
    bus.write(0x4000, 0x02);
    expect(bus.read(0xA000) == 0x5A,
           "MBC3 RAM retains data after an unsupported register selection");

    expect(cartridge.hasBattery() && cartridge.getExternalRamSize() == 0x8000,
           "MBC3 battery cartridge exposes its declared save RAM");
    expect(cartridge.getExternalRamData() != nullptr,
           "MBC3 battery cartridge provides save RAM storage");
}

void testMbc3RejectsUnsupportedRtcAndRamSizes() {
    for (uint8_t type : {uint8_t{0x0F}, uint8_t{0x10}}) {
        std::vector<uint8_t> rom(0x8000, 0);
        rom[0x0147] = type;
        rom[0x0149] = 0x02;
        bool rejected = false;
        try {
            Cartridge cartridge(std::move(rom));
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        expect(rejected, "MBC3 RTC cartridges are rejected until RTC is implemented");
    }

    std::vector<uint8_t> unsupportedRamRom(0x8000, 0);
    unsupportedRamRom[0x0147] = 0x13;
    unsupportedRamRom[0x0149] = 0x04; // MBC30-sized RAM
    bool unsupportedRamRejected = false;
    try {
        Cartridge cartridge(std::move(unsupportedRamRom));
    } catch (const std::invalid_argument &) {
        unsupportedRamRejected = true;
    }
    expect(unsupportedRamRejected,
           "MBC3 rejects the larger MBC30 RAM size until it is implemented");
}

void testMbc3SupportsSmallExternalRamSizes() {
    const std::pair<uint8_t, size_t> ramSizes[] = {
        {0x01, 0x0800}, // 2 KiB
        {0x02, 0x2000}, // 8 KiB
    };
    for (const auto &[sizeCode, expectedSize] : ramSizes) {
        std::vector<uint8_t> rom(0x8000, 0);
        rom[0x0147] = 0x12; // MBC3 + RAM
        rom[0x0149] = sizeCode;
        Cartridge cartridge(std::move(rom));
        expect(cartridge.getExternalRamSize() == expectedSize,
               "MBC3 allocates the external RAM size declared by the header");
    }
}

}

int main() {
    testAddressBusRoutesMappedRanges();
    testAddressBusRejectsInvalidMappings();
    testRomOnlyCartridgeAndRamRegions();
    testCartridgeRejectsUnsupportedSizesAndTypes();
    testMbc1RomAndRamBanking();
    testMbc2RomAndNibbleRam();
    testMbc3RomAndRamBanking();
    testMbc3RejectsUnsupportedRtcAndRamSizes();
    testMbc3SupportsSmallExternalRamSizes();
    std::cout << "Address bus tests passed\n";
}
