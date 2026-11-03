#include <algorithm>
#include <cstdlib>
#include <iostream>
#include "game_boy_system.hpp"
#include "../../tests/include/cgb_compat_rom.hpp"
namespace {
using namespace LOTUSGB;
void expect(bool ok, const char *message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void testBootstrapLockAndState() {
    const auto rom = Test::makeCgbCompatibilityRom();
    const auto boot = Test::makeCgbCompatibilityBootRom();
    GameBoySystem original(rom, HardwareModel::CgbE, nullptr, nullptr, boot);
    GameBoySystem restored(rom, HardwareModel::CgbE, nullptr, nullptr, boot);
    unsigned steps = 0;
    for (; steps < 2000 && original.getCpuRegisters().getRefPC() != 0xFE; ++steps)
        expect(original.stepOneCycle().success, "compatibility bootstrap executes");
    expect(steps < 2000 && !original.isDmgCompatibilityMode(), "native features remain available before FF50");
    const auto beforeLock = original.saveState();
    expect(beforeLock[8] == 13 && beforeLock[beforeLock.size() - 49] == 4 && beforeLock[beforeLock.size() - 48] == 1,
           "current snapshots capture pending KEY0 and OPRI selections before lock");
    restored.loadState(beforeLock.data(), beforeLock.size());
    for (steps = 0; steps < 2000 && original.getSaveRamData()[0x40] != 1; ++steps)
        expect(original.stepOneCycle().success && restored.stepOneCycle().success, "CPU enters compatibility mode and probes registers");
    expect(original.isDmgCompatibilityMode() && restored.isDmgCompatibilityMode(), "FF50 commits and locks compatibility mode");
    const auto *ram = original.getSaveRamData();
    expect(ram[0] == 0x11 && ram[0x40] == 1, "original bootstrap hands off CGB hardware identity and cartridge runs");
    for (unsigned i = 1; i <= 15; ++i) expect(ram[i] == 0xFF, "native CGB registers remain inaccessible after attempted unlock");
    expect(ram[16] == 0x34 && ram[17] == 0x56 && ram[18] == 0xFF,
           "compatibility preserves FF72/73/75 storage while FF74 is locked");
    expect(ram[0x20] == 0x55 && ram[0x21] == 0xFF && ram[0x22] == 0xFF,
           "compatibility selects normal WRAM/VRAM banks and masks fast serial selection");
    expect(original.saveState() == restored.saveState(), "pre-lock snapshot replays boot selection and complete CPU state");
    const auto locked = original.saveState();
    restored.loadState(locked.data(), locked.size());
    for (unsigned frame = 0; frame < 3; ++frame) {
        expect(original.stepForPresentation().reason == PresentationReason::VBlank &&
                   restored.stepForPresentation().reason == PresentationReason::VBlank, "compatibility produces color frames");
    }
    const auto &colors = original.getCompletedColorFrameBuffer();
    expect(std::all_of(colors.begin(), colors.end(), [](uint16_t x) { return x == 0x03E0; }),
           "BGP indexes the bootstrap's retained CGB palette; blocked writes cannot replace it");
    expect(colors == restored.getCompletedColorFrameBuffer() && original.takeAudioSamples() == restored.takeAudioSamples() &&
               original.saveState() == restored.saveState(), "locked compatibility mode replays exact video, audio and state");
    auto nativeHeader = rom; nativeHeader[0x143] = 0x80;
    GameBoySystem bootSelected(nativeHeader, HardwareModel::CgbE, nullptr, nullptr, boot);
    for (unsigned cycle = 0; cycle < 2000; ++cycle) expect(bootSelected.stepOneCycle().success, "boot can select compatibility independently of header");
    expect(bootSelected.isDmgCompatibilityMode(), "KEY0 selection overrides a color-capable header");
    auto nativeBoot = boot;
    for (size_t i = 0x200; i + 3 < nativeBoot.size(); ++i)
        if (nativeBoot[i] == 0x3E && nativeBoot[i + 1] == 4 && nativeBoot[i + 2] == 0xE0 && nativeBoot[i + 3] == 0x4C) nativeBoot[i + 1] = 0;
    GameBoySystem nativeSelected(rom, HardwareModel::CgbE, nullptr, nullptr, nativeBoot);
    for (unsigned cycle = 0; cycle < 2000; ++cycle) expect(nativeSelected.stepOneCycle().success, "boot can keep native features for a monochrome header");
    expect(!nativeSelected.isDmgCompatibilityMode(), "boot-selected native mode is not guessed from cartridge header");
    GameBoySystem fast(rom, HardwareModel::CgbE);
    expect(fast.isDmgCompatibilityMode(), "explicit CGB hardware supports monochrome cartridges without a BIOS");
    GameBoySystem dmg(rom);
    expect(!dmg.isDmgCompatibilityMode() && dmg.getHardwareModel() == HardwareModel::Dmg,
           "default monochrome selection remains actual DMG hardware");
}
void testCompatibilityRenderer() {
    InterruptRegisters irq; CgbVideoRam video; DmgPpu ppu(irq, &video, true);
    const auto palette = [&](bool object, unsigned number, std::initializer_list<uint16_t> colors) {
        const uint16_t index = object ? 0xFF6A : 0xFF68;
        ppu.write(index, 0x80 | (number * 8));
        for (auto color : colors) { ppu.write(index + 1, color & 255); ppu.write(index + 1, color >> 8); }
    };
    palette(false, 0, {0x7FFF, 0x7C00, 0x03E0, 0x001F});
    palette(true, 0, {0, 0, 0x7C00, 0}); palette(true, 1, {0x001F, 0, 0, 0});
    for (unsigned row = 0; row < 8; ++row) {
        video.write(0x8000 + row * 2, 0xFF); // BG index 1.
        video.write(0x8011 + row * 2, 0xFF); // OBJ index 2.
        video.write(0x8020 + row * 2, 0xFF); video.write(0x8021 + row * 2, 0xFF); // OBJ index 3.
        video.write(0x8031 + row * 2, 0xFF); // Window index 2.
    }
    for (unsigned cell = 0; cell < 1024; ++cell) video.write(0x9C00 + cell, 3);
    video.write(0xFF4F, 1);
    for (unsigned cell = 0; cell < 1024; ++cell) video.write(0x9800 + cell, 0xEF);
    video.setDmgCompatibility(true); ppu.setDmgCompatibility(true, true);
    ppu.write(0xFF47, 0xD8); ppu.write(0xFF48, 0xE4); ppu.write(0xFF49, 0x1B);
    ppu.write(0xFF4A, 16); ppu.write(0xFF4B, 7);
    for (auto entry : {0u, 1u}) {
        ppu.write(0xFE00 + entry * 4, 16); ppu.write(0xFE01 + entry * 4, entry ? 12 : 16);
        ppu.write(0xFE02 + entry * 4, entry ? 2 : 1); ppu.write(0xFE03 + entry * 4, entry ? 0x1F : 0x0F);
    }
    ppu.write(0xFF40, 0xF3); ppu.advanceTCycles(2 * 70224);
    const auto &frame = ppu.getCompletedColorFrameBuffer();
    expect(frame[160 + 8] == 0x001F && frame[160 + 12] == 0x7C00,
           "compatibility uses X priority and OBP0/OBP1 remapped CGB object palettes, ignoring native bank bits");
    expect(frame[160 + 40] == 0x03E0 && frame[16 * 160 + 40] == 0x7C00,
           "background/window use BGP remapping, bank zero and no native attribute-map effects");
    ppu.setDmgCompatibility(true, false); ppu.advanceTCycles(2 * 70224);
    expect(ppu.getCompletedColorFrameBuffer()[160 + 8] == 0x7C00, "bootstrap OPRI independently selects OAM-order conflicts");
    ppu.setDmgCompatibility(true, true); ppu.write(0xFF40, 0xF2); ppu.advanceTCycles(2 * 70224);
    expect(ppu.getCompletedColorFrameBuffer()[160 + 40] == 0x7FFF &&
               ppu.getCompletedColorFrameBuffer()[16 * 160 + 40] == 0x7FFF,
           "compatibility LCDC bit zero disables background and window");
    ppu.write(0xFF40, 0); ppu.write(0xFF45, 200);
    std::array<uint8_t, 160> oam{};
    for (unsigned i = 0; i < oam.size(); ++i) { oam[i] = static_cast<uint8_t>(i * 17 + 3); ppu.write(0xFE00 + i, oam[i]); }
    ppu.write(0xFF40, 0x93);
    for (unsigned dot = 0; dot < 456 && (ppu.read(0xFF41) & 3) != 2; ++dot) ppu.advanceTCycles(1);
    ppu.advanceTCycles(16);
    irq.write(0xFF0F, 0); ppu.write(0xFF41, 0); ppu.onCpuBusAccess(0xFE10, CpuBusEvent::Write);
    ppu.advanceTCycles(4);
    expect(!(irq.read(0xFF0F) & 2), "CGB compatibility does not acquire the DMG STAT-write glitch");
    ppu.write(0xFF40, 0);
    for (unsigned i = 0; i < oam.size(); ++i) expect(ppu.read(0xFE00 + i) == oam[i], "CGB compatibility does not acquire DMG OAM corruption");
}
}
int main() { testBootstrapLockAndState(); testCompatibilityRenderer(); std::cout << "CGB compatibility bootstrap, rendering and snapshot tests passed\n"; }
