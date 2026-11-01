#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "boot_rom.hpp"
#include "game_boy_system.hpp"

namespace {
using namespace LOTUSGB;
void expect(bool condition, const char *message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
template<class Exception, class F> void rejects(F action, const char *message) {
    try { action(); } catch (const Exception &) { return; }
    expect(false, message);
}
std::vector<uint8_t> cartridgeImage() {
    std::vector<uint8_t> rom(0x10000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
    rom[0x143] = 0xC0; rom[0x147] = 0x03; // CGB-only, MBC1+RAM+battery
    rom[0x148] = 1; rom[0x149] = 2;
    rom[0x1FF] = 0x19; rom[0x200] = 0x29;
    rom[0x8FF] = 0x89; rom[0x900] = 0x90;
    rom[0x8000] = 0x32; // ROM bank 2, visible at $4000 after mapper write
    return rom;
}
void testSplitOverlayAndMapper() {
    Cartridge cart(cartridgeImage());
    std::vector<uint8_t> image(0x900, 0xD3);
    image[0xFF] = 0xAF; image[0x200] = 0xB2; image[0x8FF] = 0xB8;
    BootRom boot(cart, image, HardwareModel::CgbE);
    expect(boot.read(0) == 0xD3 && boot.read(0xFF) == 0xAF, "first boot window includes both boundaries");
    expect(boot.read(0x100) == 0xC3 && boot.read(0x143) == 0xC0 && boot.read(0x1FF) == 0x19,
           "whole cartridge header hole stays visible during boot");
    expect(boot.read(0x200) == 0xB2 && boot.read(0x8FF) == 0xB8 && boot.read(0x900) == 0x90,
           "second boot window ends exactly at $08FF");
    boot.write(0x2000, 2);
    expect(boot.read(0x4000) == 0x32, "boot overlay forwards mapper writes to shared cartridge");
    boot.write(0xFF50, 0);
    expect(boot.isMapped(), "zero does not disable boot overlay");
    boot.write(0xFF50, 0x11);
    expect(!boot.isMapped() && boot.read(0x200) == 0x29 && boot.read(0x8FF) == 0x89,
           "nonzero FF50 reveals cartridge in both windows");
    boot.write(0xFF50, 0);
    expect(!boot.isMapped(), "boot disable latch is one-way");
    rejects<std::invalid_argument>([&] { BootRom bad(cart, std::vector<uint8_t>(0x800), HardwareModel::CgbE); },
                                  "ambiguous packed CGB boot image rejected");
    rejects<std::invalid_argument>([&] { BootRom bad(cart, image); }, "CGB image rejected by DMG mapper");
}
std::vector<uint8_t> bootFixture() {
    std::vector<uint8_t> boot(0x900, 0xD3); // original fixture, not a vendor BIOS
    const uint8_t entry[] = {0xF3, 0xC3, 0x00, 0x02}; // DI; JP $0200
    std::copy(std::begin(entry), std::end(entry), boot.begin());
    const uint8_t second[] = {
        0x3E, 0x07, 0xE0, 0x70,             // SVBK=7
        0x3E, 0x77, 0xEA, 0x00, 0xD0,       // bank7[$D000]=77
        0x3E, 0x01, 0xE0, 0x4F,             // VBK=1
        0x3E, 0xBB, 0xEA, 0x00, 0x80,       // bank1[$8000]=BB
        0xC3, 0xFC, 0x00};                  // return to first boot window
    std::copy(std::begin(second), std::end(second), boot.begin() + 0x200);
    boot[0xFC] = 0x3E; boot[0xFD] = 0x11; // handoff A=$11
    boot[0xFE] = 0xE0; boot[0xFF] = 0x50; // unmap, next PC=$0100
    return boot;
}
void testSystemHandoffAndBanks() {
    auto rom = cartridgeImage();
    const uint8_t program[] = {
        0x47,                               // B=boot handoff A
        0xFA, 0x00, 0xF0, 0x4F,             // C=bank7 via echo ($F000)
        0xFA, 0x00, 0x80, 0x57,             // D=VRAM bank1
        0x3E, 0x00, 0xE0, 0x70,             // SVBK=0, effective bank1
        0x3E, 0x22, 0xEA, 0x00, 0xD0,
        0x3E, 0x01, 0xE0, 0x70,
        0xFA, 0x00, 0xD0, 0x5F,             // E=bank1 after 0/1 alias
        0x3E, 0x00, 0xE0, 0x4F,             // VBK=0
        0x3E, 0xAA, 0xEA, 0x00, 0x80,
        0x3E, 0x01, 0xE0, 0x4F,
        0xFA, 0x00, 0x80, 0x67,             // H=retained bank1
        0x3E, 0x07, 0xE0, 0x70,
        0xFA, 0x00, 0xD0, 0x6F,             // L=retained bank7
        0x18, 0xFE};
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x150);
    auto system = GameBoySystem::createCgbBootProbe(rom, bootFixture());
    expect(system.getHardwareModel() == HardwareModel::CgbE, "explicit CGB-E composition");
    bool handedOff = false;
    for (unsigned i = 0; i < 300; ++i) {
        auto step = system.stepOneCycle();
        expect(step.success && step.tCyclesElapsed == 4, "probe advances shared CPU and clock at normal speed");
        auto r = system.getCpuRegisters();
        if (r.getRefPC() == 0x100) {
            expect(r.getRefA() == 0x11, "original boot fixture hands A=$11 to cartridge");
            handedOff = true;
        }
        if (r.getRefL() == 0x77) break;
    }
    auto r = system.getCpuRegisters();
    expect(handedOff && r.getRefB() == 0x11 && r.getRefC() == 0x77 && r.getRefD() == 0xBB
           && r.getRefE() == 0x22 && r.getRefH() == 0xBB && r.getRefL() == 0x77,
           "public system executes split boot then cartridge with persistent WRAM/VRAM banks");
    auto dmg = GameBoySystem::createForTest(rom);
    expect(dmg.getHardwareModel() == HardwareModel::Dmg, "existing constructors retain DMG model");
    const auto state = dmg.saveState();
    const auto ticks = system.getElapsedTCycles();
    const auto pc = r.getRefPC();
    rejects<std::logic_error>([&] { system.saveState(); }, "CGB cannot emit a DMG-format snapshot");
    rejects<std::logic_error>([&] { system.loadState(state.data(), state.size()); }, "CGB cannot accept DMG snapshots");
    expect(system.getElapsedTCycles() == ticks && system.getCpuRegisters().getRefPC() == pc,
           "unsupported snapshot requests leave running CGB probe unchanged");
}
void testUnsupportedModesFailExplicitly() {
    auto rom = cartridgeImage();
    rejects<std::invalid_argument>([&] { GameBoySystem::createCgbBootProbe(rom, {}); }, "missing CGB boot fixture rejected");
    rom[0x143] = 0;
    rejects<std::invalid_argument>([&] { GameBoySystem::createCgbBootProbe(rom, bootFixture()); },
                                  "DMG compatibility mode requires future explicit implementation");
    rom[0x143] = 0x80; // dual-mode cartridge is eligible for the native probe
    const uint8_t lcd[] = {0x3E, 0x80, 0xE0, 0x40, 0x18, 0xFE};
    std::copy(std::begin(lcd), std::end(lcd), rom.begin() + 0x150);
    auto system = GameBoySystem::createCgbBootProbe(rom, bootFixture());
    rejects<std::logic_error>([&] {
        for (unsigned i = 0; i < 300; ++i) {
            expect(system.stepOneCycle().success, "LCD-enable probe reaches unsupported PPU boundary");
        }
    }, "synthetic boot fixture remains limited to LCD off");
}
}
int main() {
    testSplitOverlayAndMapper();
    testSystemHandoffAndBanks();
    testUnsupportedModesFailExplicitly();
    std::cout << "CGB split-boot and shared-system probe tests passed\n";
}
