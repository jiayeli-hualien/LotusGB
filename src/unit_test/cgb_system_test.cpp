#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include "game_boy_system.hpp"
#include "state_hash.hpp"
#include <algorithm>
#include "../../tests/include/cgb_test_rom.hpp"
#ifndef LOTUSGB_CGB_ACID2_ASSET_DIR
#error missing asset directory
#endif
namespace {
using namespace LOTUSGB;
void expect(bool ok,const char *message) {
    if(!ok) { std::cerr<<"FAIL: "<<message<<'\n'; std::exit(EXIT_FAILURE); }
}
std::vector<uint8_t> read(const std::string &file) {
    std::ifstream in(file,std::ios::binary);
    if(!in) throw std::runtime_error("cannot read "+file);
    return {std::istreambuf_iterator<char>(in),{}};
}
void testAcid2() {
    const std::string dir=LOTUSGB_CGB_ACID2_ASSET_DIR;
    GameBoySystem system(read(dir+"/cgb-acid2.gbc"),HardwareModel::CgbE);
    for(unsigned frame=0;frame<20;++frame) {
        const auto reason=system.stepForPresentation().reason;
        expect(reason==PresentationReason::VBlank || reason==PresentationReason::Repeat,
               "CGB acid2 advances through LCD-off setup and completes frames");
    }
    auto expected=read(dir+"/reference-rgb555.bin");
    const auto &actual=system.getCompletedColorFrameBuffer();
    expect(expected.size()==actual.size()*2,"reference contains full RGB555 frame");
    size_t mismatch=0;
    for(size_t i=0;i<actual.size();++i)
        mismatch += actual[i] != (expected[i*2] | (expected[i*2+1]<<8));
    if(mismatch) std::cerr<<"cgb-acid2 mismatches: "<<mismatch<<'\n';
    expect(mismatch==0,"cgb-acid2 v1.1 matches all 23040 RGB555 pixels");
}
void testInvalidCgbBankStateIsRejectedAtomically() {
    const auto rom=Test::makeCgbExerciseRom();
    GameBoySystem cgb(rom,HardwareModel::CgbE), dmg(rom);
    const auto before=cgb.saveState();
    auto bad=before;
    // v2 extends the same fixed v1 sections. Fresh machines have empty PCM
    // queues, so this offset identifies the first appended WRAM bank selector.
    const auto extra=dmg.saveState().size() - 58; // DMG v11 common extensions follow model-specific fields.
    expect(extra+32768<bad.size(),"CGB extension includes full banked WRAM");
    bad[extra+32768]=8; // SVBK's raw selector can only contain 0..7.
    const auto hash=StateInternal::sha256(bad.data()+116,bad.size()-116);
    std::copy(hash.begin(),hash.end(),bad.begin()+84);
    try { cgb.loadState(bad.data(),bad.size()); expect(false,"invalid typed CGB bank must fail after checksum validation"); }
    catch(const std::invalid_argument &) {}
    expect(cgb.saveState()==before,"invalid CGB payload leaves live state unchanged");
}

void testCgbRomExecutionDuringWramOamDma() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) rom[at++] = byte;
    };
    emit({0xF3, 0x31, 0xFE, 0xFF, 0x3E, 0x0A, 0xEA, 0, 0});
    emit({0xAF, 0xE0, 0x40}); // LCD off for an unrestricted OAM read later.
    emit({0x3E, 0x5A, 0xEA, 0, 0xC0, 0x3E, 0xC0, 0xE0, 0x46});
    for (unsigned i = 0; i < 12; ++i) emit({0});
    emit({0x3E, 0x77, 0xEA, 0x30, 0xA0}); // Cartridge write during WRAM DMA.
    emit({0x06, 40, 0x05, 0x20, 0xFD}); // Wait for DMA to finish.
    emit({0xFA, 0, 0xFE, 0xEA, 0x31, 0xA0, 0x18, 0xFE});
    GameBoySystem original(rom, HardwareModel::CgbE), restored(rom, HardwareModel::CgbE);
    for (unsigned i = 0; i < 48; ++i)
        expect(original.stepOneCycle().success, "ROM executes while WRAM OAM DMA starts");
    auto saved = original.saveState(); // DMA is in flight, before the SRAM marker.
    auto *ram = restored.getSaveRamData();
    restored.loadState(saved.data(), saved.size());
    expect(restored.getSaveRamData() == ram, "mid-OAM-DMA restore preserves SRAM pointer");
    for (unsigned i = 0; i < 20; ++i)
        expect(original.stepOneCycle().success && restored.stepOneCycle().success,
               "both CGB CPUs continue ROM fetches during WRAM DMA");
    expect(original.getSaveRamData()[0x30] == 0x77 && ram[0x30] == 0x77,
           "CGB ROM code writes cartridge SRAM before 160-M-cycle DMA completes");
    for (unsigned i = 0; i < 220; ++i)
        expect(original.stepOneCycle().success && restored.stepOneCycle().success,
               "CGB guest completes OAM DMA after snapshot replay");
    expect(ram[0x31] == 0x5A, "guest reads the completed OAM copy through the public CPU path");
    expect(original.takeAudioSamples() == restored.takeAudioSamples() &&
               original.saveState() == restored.saveState(),
           "mid-OAM-DMA restore reproduces PCM and full CGB state");
}

void testCgbOamDmaReadsMbc1RomBank() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x148] = 0; rom[0x149] = 2;

    std::array<uint8_t, DmgPpu::OAM_SIZE> expected{};
    for (unsigned i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<uint8_t>(0x23 + i * 7);
        rom[0x4000 + i] = expected[i];
    }

    const std::vector<uint8_t> routine = {
        0x3E, 0x40,             // LD A,$40
        0xE0, 0x46,             // LDH ($FF46),A: DMA from MBC1 ROM bank 1
        0x06, 0x50,             // LD B,$50 (leave enough time for 160 bytes)
        0x05, 0x20, 0xFD,       // wait: DEC B; JR NZ,wait
        0x21, 0x00, 0xFE,       // LD HL,$FE00
        0x11, 0x00, 0xA0,       // LD DE,$A000
        0x06, 0xA0,             // LD B,160
        0x2A, 0x12, 0x13, 0x05, // copy: LD A,(HL+); LD (DE),A; INC DE; DEC B
        0x20, 0xFA,             // JR NZ,copy
        0x18, 0xFE              // stop in a local loop after copying OAM to SRAM
    };
    std::copy(routine.begin(), routine.end(), rom.begin() + 0x0300);

    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes) rom[at++] = byte;
    };
    emit({0xF3, 0x31, 0xFE, 0xFF}); // DI; stack in HRAM.
    emit({0x3E, 0x0A, 0xEA, 0x00, 0x00}); // Enable MBC1 SRAM.
    emit({0xAF, 0xE0, 0x40}); // Turn LCD off for OAM readback.
    emit({0x21, 0x00, 0x03, 0x11, 0x00, 0xC0,
          0x06, static_cast<uint8_t>(routine.size())}); // Copy routine into WRAM.
    const size_t copyLoop = at;
    emit({0x2A, 0x12, 0x13, 0x05, 0x20, 0x00});
    rom[at - 1] = static_cast<uint8_t>(copyLoop - at);
    emit({0xC3, 0x00, 0xC0}); // Execute from the free CGB WRAM bus.

    GameBoySystem system(std::move(rom), HardwareModel::CgbE);
    auto *saveRam = system.getSaveRamData();
    expect(saveRam, "MBC1 OAM-source probe exposes cartridge SRAM for readback");
    bool copied = false;
    for (unsigned cycle = 0; cycle < 10000 && !copied; ++cycle) {
        expect(system.stepOneCycle().success,
               "CGB guest executes while OAM DMA reads the cartridge ROM bus");
        copied = std::equal(expected.begin(), expected.end(), saveRam);
    }
    expect(copied,
           "OAM receives all 160 bytes from the selected MBC1 ROM bank and CPU reads them back");
}

std::vector<uint8_t> makeOamDmaSpeedProbeRom(bool doubleSpeed) {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes) rom[at++] = byte;
    };
    emit({0xF3, 0x31, 0xFE, 0xFF}); // DI; stack stays outside the source page.
    emit({0x3E, 0x0A, 0xEA, 0, 0}); // Enable MBC1 RAM for the host start marker.
    if (doubleSpeed)
        emit({0x3E, 1, 0xE0, 0x4D, 0x10, 0}); // Prepare KEY1 and switch speed.

    emit({0xAF, 0xE0, 0x40}); // LCD off while preparing tile and OAM source.
    emit({0x21, 0x10, 0x80, 0x06, 8}); // Tile 1: color index 1 across every row.
    const size_t tileLoop = at;
    emit({0x3E, 0xFF, 0x22, 0xAF, 0x22, 0x05, 0x20,
          static_cast<uint8_t>(static_cast<int8_t>(tileLoop - (at + 8)))});
    emit({0x3E, 18, 0xEA, 0x00, 0xC0, // Sprite Y: visible on scanline 2.
          0x3E, 8, 0xEA, 0x01, 0xC0,   // Sprite X: first screen pixel.
          0x3E, 1, 0xEA, 0x02, 0xC0,   // Tile 1.
          0xAF, 0xEA, 0x03, 0xC0});    // Front priority, no palette flags.
    emit({0x3E, 0x93, 0xE0, 0x40}); // LCD on, background and objects enabled.

    const size_t lineWait = at;
    emit({0xF0, 0x44, 0xFE, 1, 0x20,
          static_cast<uint8_t>(static_cast<int8_t>(lineWait - (at + 6)))});
    const size_t modeWait = at;
    emit({0xF0, 0x41, 0xE6, 3, 0xFE, 3, 0x20,
          static_cast<uint8_t>(static_cast<int8_t>(modeWait - (at + 8)))});
    emit({0x3E, 0xC0, 0xE0, 0x46}); // Start WRAM OAM DMA during line 1 Mode 3.
    emit({0x3E, 0xA5, 0xEA, 0, 0xA0, 0x18, 0xFE}); // Mark start, then wait.
    return rom;
}

void testCgbOamDmaCompletesInFewerDotsAtDoubleSpeed() {
    for (bool doubleSpeed : {false, true}) {
        GameBoySystem system(makeOamDmaSpeedProbeRom(doubleSpeed), HardwareModel::CgbE);
        auto *ram = system.getSaveRamData();
        expect(ram != nullptr, "OAM DMA speed fixture exposes its SRAM start marker");
        bool sawDoubleSpeed = false;
        for (unsigned cycle = 0; cycle < 10000 && ram[0] != 0xA5; ++cycle) {
            const auto step = system.stepOneCycle();
            expect(step.success, "CGB OAM DMA speed fixture reaches its scanline trigger");
            sawDoubleSpeed |= step.tCyclesElapsed == 2;
        }
        expect(ram[0] == 0xA5, "guest starts OAM DMA in line 1 Mode 3");
        expect(sawDoubleSpeed == doubleSpeed,
               "the OAM DMA probe is running at the selected CPU clock rate");

        const auto frame = system.stepForPresentation();
        expect(frame.reason == PresentationReason::VBlank,
               "the first post-DMA scanline fixture frame completes");
        const bool firstLineTwoObjectPixel =
            system.getCompletedFrameBuffer()[2 * DmgPpu::FRAME_WIDTH] == 1;
        expect(firstLineTwoObjectPixel == doubleSpeed,
               "CGB OAM DMA finishes before line 2 only at double speed");
    }
}

std::vector<uint8_t> makeVramDmaClockProbeRom(bool doubleSpeed, bool hblankMode,
                                               uint16_t &triggerAddress) {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x148] = 0; rom[0x149] = 2;
    for (unsigned i = 0; i < 16; ++i)
        rom[0x0200 + i] = static_cast<uint8_t>(0xA0 + i);

    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes) rom[at++] = byte;
    };
    const auto writeByte = [&](uint16_t address, uint8_t value) {
        emit({0x3E, value, 0xEA, static_cast<uint8_t>(address),
              static_cast<uint8_t>(address >> 8)});
    };
    const auto writeIo = [&](uint8_t address, uint8_t value) {
        emit({0x3E, value, 0xE0, address});
    };

    emit({0xF3, 0x31, 0xFE, 0xFF}); // DI; stack remains outside the DMA source.
    writeIo(0x40, 0); // Keep LCD off so one HBlank-DMA block starts immediately.
    writeIo(0x51, 0x02); writeIo(0x52, 0x00);
    writeIo(0x53, 0x00); writeIo(0x54, 0x00);
    emit({0x3E, 0x0A, 0xEA, 0, 0}); // Enable MBC1 RAM for the guest-visible result.
    writeByte(0xA030, 0);

    if (doubleSpeed)
        emit({0x3E, 1, 0xE0, 0x4D, 0x10, 0}); // Prepare KEY1, then switch speed.

    emit({0x3E, static_cast<uint8_t>(hblankMode ? 0x80 : 0x00),
          0x21, 0x55, 0xFF}); // Set DMA mode/value and point HL at FF55.
    triggerAddress = static_cast<uint16_t>(at);
    emit({0x77}); // LD (HL),A starts DMA on its second M-cycle.
    emit({0x21, 0x00, 0x80, 0x7E, 0xEA, 0x30, 0xA0, 0x18, 0xFE});
    return rom;
}

void testVramDmaKeepsBaseClockRateAtDoubleSpeed() {
    for (const bool doubleSpeed : {false, true}) {
        for (const bool hblankMode : {false, true}) {
            uint16_t triggerAddress = 0;
            auto rom = makeVramDmaClockProbeRom(doubleSpeed, hblankMode,
                                                triggerAddress);
            GameBoySystem system(std::move(rom), HardwareModel::CgbE);
            auto *ram = system.getSaveRamData();
            expect(ram, "CGB VRAM DMA clock probe exposes its SRAM result byte");
            bool resultCleared = ram[0x30] == 0;
            for (unsigned cycle = 0; cycle < 100 && !resultCleared; ++cycle) {
                const auto step = system.stepOneCycle();
                expect(step.success, "guest enables SRAM and clears the result byte");
                resultCleared = ram[0x30] == 0;
            }
            expect(resultCleared, "guest clears its SRAM result byte before DMA setup");

            bool triggerFetched = false;
            for (unsigned cycle = 0; cycle < 30000 && !triggerFetched; ++cycle) {
                const auto step = system.stepOneCycle();
                expect(step.success, "guest reaches the VRAM DMA trigger");
                triggerFetched = step.fetchedNewInst &&
                    system.getCpuRegisters().getRefPC() == triggerAddress + 1;
            }
            expect(triggerFetched, "guest fetches the one-cycle VRAM DMA trigger opcode");

            const auto start = system.stepOneCycle();
            expect(start.success && !start.fetchedNewInst &&
                       start.tCyclesElapsed == (doubleSpeed ? 2 : 4),
                   "LD (HL),A writes FF55 at the selected CPU clock rate");

            unsigned stalledMCycles = 0;
            uint64_t stalledBaseTCycles = 0;
            bool resumed = false;
            for (unsigned cycle = 0; cycle < 20; ++cycle) {
                const auto step = system.stepOneCycle();
                expect(step.success, "CPU remains healthy during VRAM DMA stall");
                if (step.fetchedNewInst) {
                    resumed = true;
                    break;
                }
                ++stalledMCycles;
                stalledBaseTCycles += step.tCyclesElapsed;
                expect(system.getCpuRegisters().getRefPC() == triggerAddress + 1,
                       "GDMA keeps the CPU at the next opcode until its block completes");
            }
            expect(resumed && stalledBaseTCycles == 32 &&
                       stalledMCycles == (doubleSpeed ? 16 : 8),
                   "integrated GDMA/HBlank DMA consumes 32 base clocks at both CPU speeds");

            for (unsigned cycle = 0; cycle < 12 && ram[0x30] == 0; ++cycle)
                expect(system.stepOneCycle().success,
                       "guest reads VRAM and stores the transferred byte after DMA");
            expect(ram[0x30] == 0xA0,
                   "guest observes the expected byte after the fixed-rate VRAM DMA block");
        }
    }
}

void testCgbWramOamDmaFeedsCpuFetchAndLeavesCartridgeBusUsable() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x148] = 0; rom[0x149] = 2;

    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes) rom[at++] = byte;
    };
    const auto writeByte = [&](uint16_t address, uint8_t value) {
        emit({0x3E, value, 0xEA, static_cast<uint8_t>(address),
              static_cast<uint8_t>(address >> 8)});
    };

    emit({0xF3, 0x31, 0xFE, 0xFF}); // DI; stack in HRAM.
    emit({0x3E, 0x0A, 0xEA, 0, 0}); // Enable MBC1 cartridge RAM.
    writeByte(0xA000, 0); // Clear the guest-visible completion marker.

    // The DMA source becomes an instruction stream: LD A,$5A; LD ($A000),A.
    // Remaining source bytes are NOPs so they cannot trigger a second DMA.
    writeByte(0xC000, 0x3E);
    writeByte(0xC001, 0x5A);
    writeByte(0xC002, 0xEA);
    writeByte(0xC003, 0x00);
    writeByte(0xC004, 0xA0);

    // Start WRAM-sourced DMA from a different WRAM address. Once the transfer
    // starts, the CPU's same-bus opcode fetches must see bytes from $C000.
    writeByte(0xD000, 0x3E);
    writeByte(0xD001, 0xC0);
    writeByte(0xD002, 0xE0);
    writeByte(0xD003, 0x46);
    emit({0xC3, 0x00, 0xD0});

    GameBoySystem cgb(rom, HardwareModel::CgbE);
    GameBoySystem dmg(rom, HardwareModel::Dmg);
    auto *cgbRam = cgb.getSaveRamData();
    auto *dmgRam = dmg.getSaveRamData();
    expect(cgbRam && dmgRam, "DMA conflict probe exposes cartridge RAM on both models");

    bool cgbMarkerWritten = false;
    for (unsigned cycle = 0; cycle < 200 && !cgbMarkerWritten; ++cycle) {
        expect(cgb.stepOneCycle().success, "CGB executes DMA-conflict probe");
        cgbMarkerWritten = cgbRam[0] == 0x5A;
    }
    if (!cgbMarkerWritten) {
        auto registers = cgb.getCpuRegisters();
        std::cerr << "DMA conflict probe stopped at PC=$" << std::hex
                  << registers.getRefPC() << " A=$"
                  << static_cast<unsigned>(registers.getRefA()) << " SRAM[0]=$"
                  << static_cast<unsigned>(cgbRam[0]) << std::dec << '\n';
    }
    expect(cgbMarkerWritten,
           "CGB CPU executes the byte being transferred and writes through the free cartridge bus");

    for (unsigned cycle = 0; cycle < 200; ++cycle)
        expect(dmg.stepOneCycle().success, "DMG executes the matching DMA-conflict control");
    expect(dmgRam[0] == 0,
           "DMG OAM DMA blocks the cartridge write while its source bus is occupied");
}

void testCgbRomOamDmaFetchConflictAndCartridgeWrite() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x148] = 0; rom[0x149] = 2;

    const std::vector<uint8_t> startDma = {
        0x3E, 0x41,             // LD A,$41
        0xE0, 0x46,             // LDH ($FF46),A: DMA from ROM page $41
        0xC3, 0x00, 0x40        // JP $4000 on the still-free WRAM-to-ROM path
    };
    const std::vector<uint8_t> retrySramWrite = {
        0x3E, 0x5A,             // LD A,$5A
        0xEA, 0x00, 0xA0,       // LD ($A000),A
        0xC3, 0x00, 0xC1        // Retry from WRAM while cartridge bus is occupied
    };
    std::copy(startDma.begin(), startDma.end(), rom.begin() + 0x0300);
    std::copy(retrySramWrite.begin(), retrySramWrite.end(), rom.begin() + 0x0310);

    // The actual ROM at $4000 is HALT. During ROM-sourced OAM DMA, the CPU
    // fetches from that cartridge bus must instead receive the current DMA
    // byte from $4100. A run of NOPs absorbs startup alignment before the
    // DMA-supplied instruction stream jumps to the WRAM retry routine.
    std::fill(rom.begin() + 0x4000, rom.begin() + 0x4100, 0x76);
    std::fill(rom.begin() + 0x4100, rom.begin() + 0x41A0, 0x00);
    rom[0x4100 + 16] = 0xC3;
    rom[0x4100 + 17] = 0x00;
    rom[0x4100 + 18] = 0xC1;

    size_t at = 0x0150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes) rom[at++] = byte;
    };
    const auto emitCopy = [&](uint16_t source, uint16_t destination,
                              uint8_t length) {
        emit({0x21, static_cast<uint8_t>(source),
              static_cast<uint8_t>(source >> 8),
              0x11, static_cast<uint8_t>(destination),
              static_cast<uint8_t>(destination >> 8), 0x06, length});
        const size_t loop = at;
        emit({0x2A, 0x12, 0x13, 0x05, 0x20, 0x00});
        rom[at - 1] = static_cast<uint8_t>(loop - at);
    };
    emit({0xF3, 0x31, 0xFE, 0xFF}); // DI; use no stack during DMA.
    emit({0x3E, 0x0A, 0xEA, 0x00, 0x00}); // Enable MBC1 SRAM.
    emit({0xAF, 0xEA, 0x00, 0xA0}); // Clear the visible completion marker.
    emitCopy(0x0300, 0xC000, static_cast<uint8_t>(startDma.size()));
    emitCopy(0x0310, 0xC100, static_cast<uint8_t>(retrySramWrite.size()));
    emit({0xC3, 0x00, 0xC0});

    GameBoySystem system(std::move(rom), HardwareModel::CgbE);
    auto *saveRam = system.getSaveRamData();
    expect(saveRam, "ROM-source conflict probe exposes cartridge SRAM");
    bool enteredRetryRoutine = false;
    for (unsigned cycle = 0; cycle < 400; ++cycle) {
        expect(system.stepOneCycle().success,
               "CGB executes DMA-supplied ROM opcodes and the WRAM retry routine");
        const uint16_t pc = system.getCpuRegisters().getRefPC();
        enteredRetryRoutine |= pc >= 0xC100 && pc < 0xC10A;
        if (enteredRetryRoutine && cycle > 60) {
            expect(saveRam[0] == 0,
                   "ROM-source DMA blocks SRAM writes while the retry code runs from WRAM");
            break;
        }
    }
    expect(enteredRetryRoutine,
           "DMA bus bytes replace the HALT ROM fetches with a WRAM jump");
    for (unsigned cycle = 0; cycle < 40; ++cycle)
        expect(system.stepOneCycle().success,
               "WRAM retry code continues while the cartridge bus is occupied");
    expect(saveRam[0] == 0,
           "SRAM remains blocked while ROM-source DMA is still active");
    bool writeCompleted = false;
    for (unsigned cycle = 0; cycle < 400 && !writeCompleted; ++cycle) {
        expect(system.stepOneCycle().success,
               "CGB releases the cartridge bus after ROM-source OAM DMA");
        writeCompleted = saveRam[0] == 0x5A;
    }
    expect(writeCompleted,
           "the WRAM retry routine writes SRAM after ROM-source DMA releases the bus");
}

void testCgbRomOamDmaLeavesWramStackAccessible() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x148] = 0; rom[0x149] = 2;

    // Start a cartridge-ROM-sourced DMA, then execute from WRAM while the
    // cartridge bus is occupied. PUSH/POP must still use the free WRAM bus.
    const std::vector<uint8_t> routine = {
        0x3E, 0x41,             // LD A,$41
        0xE0, 0x46,             // LDH ($FF46),A
        0xC3, 0x07, 0xC0,       // JP $C007
        0x31, 0xFF, 0xC2,       // LD SP,$C2FF
        0x01, 0x34, 0x12,       // LD BC,$1234
        0xC5,                   // PUSH BC to free WRAM bus
        0xD1,                   // POP DE from free WRAM bus
        0x7A, 0xFE, 0x12,       // LD A,D; CP $12
        0x20, 0x09,             // JR NZ,fail
        0x7B, 0xFE, 0x34,       // LD A,E; CP $34
        0x20, 0x04,             // JR NZ,fail
        0x3E, 0x5A, 0x18, 0x02, // success: LD A,$5A; JR write
        0x3E, 0xE0,             // fail: LD A,$E0
        0xEA, 0x00, 0xA0,       // write: LD ($A000),A (blocked during DMA)
        0xC3, 0x1F, 0xC0        // JP write; retry until cartridge bus is free
    };
    std::copy(routine.begin(), routine.end(), rom.begin() + 0x0300);

    size_t at = 0x0150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t byte : bytes) rom[at++] = byte;
    };
    emit({0xF3, 0x31, 0xFE, 0xFF}); // DI; initialize a safe setup stack.
    emit({0x3E, 0x0A, 0xEA, 0x00, 0x00}); // Enable MBC1 SRAM.
    emit({0xAF, 0xEA, 0x00, 0xA0}); // Clear the completion marker.
    emit({0x21, 0x00, 0x03, 0x11, 0x00, 0xC0,
          0x06, static_cast<uint8_t>(routine.size())});
    const size_t copyLoop = at;
    emit({0x2A, 0x12, 0x13, 0x05, 0x20, 0x00});
    rom[at - 1] = static_cast<uint8_t>(copyLoop - at);
    emit({0xC3, 0x00, 0xC0}); // Execute DMA setup from the free WRAM bus.

    GameBoySystem system(std::move(rom), HardwareModel::CgbE);
    auto *saveRam = system.getSaveRamData();
    expect(saveRam, "ROM-source stack probe exposes cartridge SRAM");
    saveRam[0] = 0; // Give the probe a deterministic value different from both markers.
    bool markerWritten = false;
    for (unsigned cycle = 0; cycle < 600 && !markerWritten; ++cycle) {
        expect(system.stepOneCycle().success,
               "CGB executes WRAM code and stack operations during ROM-source OAM DMA");
        markerWritten = saveRam[0] != 0;
    }
    if (!markerWritten || saveRam[0] != 0x5A) {
        auto registers = system.getCpuRegisters();
        std::cerr << "ROM-source stack probe: PC=$" << std::hex
                  << registers.getRefPC() << " SP=$" << registers.getRefSP()
                  << " A=$" << static_cast<unsigned>(registers.getRefA())
                  << " SRAM[0]=$" << static_cast<unsigned>(saveRam[0])
                  << std::dec << '\n';
    }
    expect(markerWritten && saveRam[0] == 0x5A,
           "CGB ROM-source DMA leaves the WRAM stack usable for PUSH/POP");
}

void testHblankDmaWaitsForHaltWake() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) rom[at++] = byte;
    };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    emit({0xF3, 0x31, 0xFE, 0xFF, 0x3E, 0x0A, 0xEA, 0, 0});
    io(0x40, 0);
    emit({0x3E, 0x5A, 0xEA, 0, 0xC0});
    io(0x51, 0xC0); io(0x52, 0); io(0x53, 0); io(0x54, 0);
    io(0x0F, 0);
    emit({0x3E, 1, 0xEA, 0xFF, 0xFF}); // IE=VBlank; IME stays off for HALT wake.
    io(0x40, 0x91);
    emit({0xF0, 0x44, 0xFE, 1, 0x20, 0xFA}); // Wait for visible line 1.
    io(0x55, 0x82); // Three HBlank blocks; arm before this line's HBlank.
    emit({0x3E, 0x11, 0xEA, 0x33, 0xA0, 0x76});
    emit({0xF0, 0x55, 0xEA, 0x30, 0xA0}); // Remaining blocks on VBlank wake.
    emit({0xFA, 0, 0x80, 0xEA, 0x31, 0xA0}); // VRAM is readable in VBlank.
    emit({0xF0, 0x55, 0xFE, 0xFF, 0x20, 0xFA}); // Await resumed HDMA completion.
    emit({0xF0, 0x41, 0xE6, 3, 0x20, 0xFA}); // Read VRAM in HBlank.
    emit({0xFA, 0, 0x80, 0xEA, 0x32, 0xA0, 0x18, 0xFE});
    GameBoySystem original(rom, HardwareModel::CgbE), restored(rom, HardwareModel::CgbE);
    for (unsigned n = 0; n < 600; ++n)
        expect(original.stepOneCycle().success, "CGB enters HALT with armed HBlank DMA");
    expect(original.getSaveRamData()[0x33] == 0x11 && original.getSaveRamData()[0x30] == 0xFF,
           "HALT waits for VBlank while PPU clocks continue");
    const auto saved = original.saveState();
    restored.loadState(saved.data(), saved.size());
    for (unsigned n = 0; n < 20000; ++n)
        expect(original.stepOneCycle().success && restored.stepOneCycle().success,
               "VBlank wakes HALT and HBlank DMA resumes in both branches");
    const auto *ram = original.getSaveRamData();
    expect(ram[0x30] == 2 && ram[0x31] == 0,
           "HALT suppresses HBlank blocks without cancelling or copying the pending transfer");
    expect(ram[0x32] == 0x5A, "HBlank DMA copies the pending blocks after CPU wake");
    expect(original.takeAudioSamples() == restored.takeAudioSamples() &&
               original.getCompletedColorFrameBuffer() == restored.getCompletedColorFrameBuffer() &&
               original.saveState() == restored.saveState(),
           "HALT plus pending HBlank DMA restores exact video, PCM and complete state");
}

void testHblankDmaTransfersOneBlockWhenLcdIsDisabled() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) rom[at++] = byte;
    };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    const auto waitForHdma5 = [&](uint8_t value) {
        emit({0xF0, 0x55, 0xFE, value, 0x20, 0xFA});
    };
    emit({0xF3, 0x31, 0xFE, 0xFF, 0x3E, 0x0A, 0xEA, 0, 0}); // SRAM on.
    io(0x40, 0);
    emit({0x21, 0x00, 0xC0, 0x06, 48, 0x3E, 0x11});
    emit({0x22, 0x3C, 0x05, 0x20, 0xFB}); // Fill C000-C02F with 11-40.
    io(0x51, 0xC0); io(0x52, 0); io(0x53, 0); io(0x54, 0);
    io(0x40, 0x91);
    io(0x55, 0x82); // Three HBlank blocks, started while LCD is on.
    waitForHdma5(1); // First block done; two remain.
    emit({0x3E, 0x55, 0xEA, 0x34, 0xA0});
    io(0x40, 0); // The active transfer must immediately run one more block.
    waitForHdma5(0); // Second block done; one remains.
    emit({0x3E, 0x66, 0xEA, 0x35, 0xA0});
    io(0x40, 0x91); // Last block waits for a visible HBlank.
    waitForHdma5(0xFF);
    emit({0x3E, 0x77, 0xEA, 0x36, 0xA0});
    io(0x40, 0);
    emit({0xFA, 0x00, 0x80, 0xEA, 0x30, 0xA0});
    emit({0xFA, 0x10, 0x80, 0xEA, 0x31, 0xA0});
    emit({0xFA, 0x20, 0x80, 0xEA, 0x32, 0xA0});
    emit({0xF0, 0x55, 0xEA, 0x33, 0xA0});
    emit({0x18, 0xFE});

    GameBoySystem original(rom, HardwareModel::CgbE);
    GameBoySystem restored(rom, HardwareModel::CgbE);
    unsigned cycles = 0;
    for (; cycles < 10000 && original.getSaveRamData()[0x34] != 0x55; ++cycles)
        expect(original.stepOneCycle().success,
               "CGB reaches the first HBlank DMA checkpoint");
    expect(cycles < 10000 && original.getSaveRamData()[0x35] != 0x66,
           "guest observes one HBlank block before the LCD-off checkpoint");

    const auto state = original.saveState();
    auto *restoredRam = restored.getSaveRamData();
    restored.loadState(state.data(), state.size());
    for (unsigned n = 0; n < 30000; ++n)
        expect(original.stepOneCycle().success && restored.stepOneCycle().success,
               "CGB HBlank DMA resumes across LCD-off and LCD-on transitions");

    const auto *ram = original.getSaveRamData();
    expect(ram[0x35] == 0x66 && ram[0x36] == 0x77 && ram[0x33] == 0xFF,
           "guest observes exactly one LCD-off block then completion at the next HBlank");
    expect(ram[0x30] == 0x11 && ram[0x31] == 0x21 && ram[0x32] == 0x31,
           "the three HBlank blocks copy consecutive source data to VRAM");
    expect(std::equal(ram, ram + 0x38, restoredRam) &&
               original.takeAudioSamples() == restored.takeAudioSamples() &&
               original.getCompletedColorFrameBuffer() == restored.getCompletedColorFrameBuffer() &&
               original.saveState() == restored.saveState(),
           "snapshot during an active HBlank transfer replays LCD transitions and exact system state");
}

void testCgbWaveRamCpuAccessAndSnapshot() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) rom[at++] = byte;
    };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    emit({0xF3, 0x3E, 0x0A, 0xEA, 0, 0});
    io(0x26, 0); io(0x26, 0x80);
    for (uint8_t port = 0x30; port < 0x40; ++port) io(port, 0x55);
    io(0x24, 0x77); io(0x25, 0x44);
    io(0x1A, 0x80); io(0x1C, 0x20); io(0x1D, 0); io(0x1E, 0x80);
    emit({0x3E, 0x10, 0xEA, 0x33, 0xA0}); // Snapshot handshake with CH3 active.
    for (unsigned n = 0; n < 64; ++n) emit({0});
    emit({0xF0, 0x3F, 0xEA, 0x30, 0xA0}); // Active read aliases wave byte 0.
    io(0x3F, 0xAB); // Active write aliases wave byte 0 too.
    io(0x1A, 0); // Stop before the first 4096-T-cycle sample advance.
    emit({0xF0, 0x30, 0xEA, 0x31, 0xA0, 0xF0, 0x3F, 0xEA, 0x32, 0xA0});
    emit({0x18, 0xFE});
    GameBoySystem original(rom, HardwareModel::CgbE), restored(rom, HardwareModel::CgbE);
    unsigned n = 0;
    for (; n < 300 && original.getSaveRamData()[0x33] != 0x10; ++n)
        expect(original.stepOneCycle().success, "CGB CPU initializes active wave channel");
    expect(n < 300, "wave CPU probe reaches its active-channel checkpoint");
    const auto saved = original.saveState();
    auto *ram = restored.getSaveRamData();
    restored.loadState(saved.data(), saved.size());
    expect(ram == restored.getSaveRamData(), "active CGB APU restore preserves SRAM pointer");
    for (unsigned cycle = 0; cycle < 300; ++cycle)
        expect(original.stepOneCycle().success && restored.stepOneCycle().success,
               "CGB wave CPU probe continues after active-channel restore");
    expect(ram[0x30] == 0x55 && ram[0x31] == 0xAB && ram[0x32] == 0x55,
           "CPU active CGB wave accesses alias channel address and stopped accesses use CPU address");
    expect(original.takeAudioSamples() == restored.takeAudioSamples() &&
               original.saveState() == restored.saveState(),
           "CGB APU model and active wave access survive exact snapshot replay");
}

void testPcmRegistersThroughCpuBusAndSnapshot() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0x80; rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) rom[at++] = byte;
    };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    emit({0xF3, 0x3E, 0x0A, 0xEA, 0, 0});
    io(0x26, 0); io(0x26, 0x80);
    for (uint8_t port = 0x30; port < 0x40; ++port) io(port, 0xAA);
    io(0x1A, 0x80); io(0x1C, 0x20); io(0x1D, 0xFE); io(0x1E, 0x87);
    emit({0x3E, 0x10, 0xEA, 0x33, 0xA0});
    const size_t loop = at;
    emit({0xF0, 0x76, 0xEA, 0x30, 0xA0, 0xF0, 0x77, 0xEA, 0x31, 0xA0});
    io(0x77, 0xFF); // Writes must not replace the live generator output.
    emit({0xF0, 0x77, 0xEA, 0x32, 0xA0});
    emit({0xC3, static_cast<uint8_t>(loop), static_cast<uint8_t>(loop >> 8)});
    for (auto model : {HardwareModel::Dmg, HardwareModel::CgbE}) {
        GameBoySystem original(rom, model), restored(rom, model);
        unsigned n = 0;
        for (; n < 300 && original.getSaveRamData()[0x33] != 0x10; ++n)
            expect(original.stepOneCycle().success, "PCM CPU probe initializes CH3");
        expect(n < 300, "PCM CPU probe reaches active-channel snapshot checkpoint");
        const auto saved = original.saveState();
        restored.loadState(saved.data(), saved.size());
        for (unsigned cycle = 0; cycle < 500; ++cycle)
            expect(original.stepOneCycle().success && restored.stepOneCycle().success,
                   "PCM CPU probe replays live reads after restore");
        const auto *ram = original.getSaveRamData();
        const bool cgb = model == HardwareModel::CgbE;
        expect(ram[0x30] == (cgb ? 0 : 0xFF) &&
                   ram[0x31] == (cgb ? 10 : 0xFF) && ram[0x32] == ram[0x31],
               "CPU sees CGB read-only PCM levels while DMG addresses stay unmapped");
        expect(original.takeAudioSamples() == restored.takeAudioSamples() &&
                   original.saveState() == restored.saveState(),
               "PCM observations and active sound generator replay with exact full state");
    }
}

void testLengthEnableEdgesThroughCpuAndDiv() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0x80; rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) rom[at++] = byte;
    };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    const auto dividerEdge = [&] {
        emit({0xF0, 4, 0xE6, 0x10, 0x28, 0xFA}); // Wait for DIV-APU input high.
        io(4, 0); // Force its falling edge with a CPU write.
    };
    const auto lengths = [&] {
        io(0x11, 63); io(0x16, 63); io(0x1B, 255); io(0x20, 63);
        for (uint8_t port : {0x14, 0x19, 0x1E, 0x23}) io(port, 0x80);
    };
    const auto observe = [&](uint8_t offset) {
        emit({0xF0, 0x26, 0xE6, 15, 0xEA, offset, 0xA0});
    };
    emit({0xF3, 0x3E, 0x0A, 0xEA, 0, 0});
    io(0x26, 0); io(0x26, 0x80);
    io(0x12, 0xF0); io(0x17, 0xF0); io(0x1A, 0x80); io(0x21, 0xF0);
    lengths(); dividerEdge(); // Step 0; next step 1 does not clock lengths.
    emit({0x3E, 0x10, 0xEA, 0x33, 0xA0}); // Snapshot before enable edge.
    for (uint8_t port : {0x14, 0x19, 0x1E, 0x23}) io(port, 0x40);
    observe(0x30);
    lengths(); dividerEdge(); // Step 1; next step 2 does clock lengths.
    for (uint8_t port : {0x14, 0x19, 0x1E, 0x23}) io(port, 0x40);
    observe(0x31);
    dividerEdge(); observe(0x32); // Regular step 2 expires all four counters.
    emit({0x18, 0xFE});
    for (auto model : {HardwareModel::Dmg, HardwareModel::CgbE}) {
        GameBoySystem original(rom, model), restored(rom, model);
        unsigned n = 0;
        for (; n < 5000 && original.getSaveRamData()[0x33] != 0x10; ++n)
            expect(original.stepOneCycle().success, "CPU length probe reaches odd sequencer phase");
        expect(n < 5000, "CPU length probe reaches pre-enable snapshot checkpoint");
        const auto saved = original.saveState();
        restored.loadState(saved.data(), saved.size());
        for (unsigned cycle = 0; cycle < 12000; ++cycle)
            expect(original.stepOneCycle().success && restored.stepOneCycle().success,
                   "CPU length probe runs both enable edges and regular DIV clock");
        const auto *ram = original.getSaveRamData();
        expect(ram[0x30] == 0 && ram[0x31] == 15 && ram[0x32] == 0,
               "CPU-driven DIV/NRx4 writes observe odd extra clock, even no-extra clock and regular expiration");
        expect(original.takeAudioSamples() == restored.takeAudioSamples() &&
                   original.saveState() == restored.saveState(),
               "length-enable boundary snapshot replays exact PCM and complete state on both models");
    }
}

void testEnvelopeTriggerPhaseThroughCpuAndSnapshot() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) rom[at++] = byte;
    };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    const auto dividerEdge = [&] {
        emit({0xF0, 4, 0xE6, 0x10, 0x28, 0xFA}); io(4, 0);
    };
    const auto observe = [&](uint8_t offset) {
        // NOP makes successive polls visit different duty steps; a fixed
        // eight-M-cycle loop could remain phase-locked to digital zero.
        emit({0, 0xF0, 0x76, 0xE6, 15, 0x28, 0xF9, 0xEA, offset, 0xA0});
    };
    emit({0xF3, 0x3E, 0x0A, 0xEA, 0, 0});
    io(0x26, 0); io(0x26, 0x80);
    io(0x11, 0x40); io(0x12, 0x71); io(0x13, 0xFF);
    for (unsigned n = 0; n < 7; ++n) dividerEdge();
    io(0x14, 0x87); // Trigger immediately before envelope step 7.
    emit({0x3E, 0x10, 0xEA, 0x33, 0xA0});
    dividerEdge(); observe(0x30); // Extra trigger delay retains initial volume 7.
    for (unsigned n = 0; n < 8; ++n) dividerEdge();
    observe(0x31); // Then normal period-one changes to 6 and 5.
    for (unsigned n = 0; n < 8; ++n) dividerEdge();
    observe(0x32); emit({0x18, 0xFE});
    GameBoySystem original(rom, HardwareModel::CgbE), restored(rom, HardwareModel::CgbE);
    unsigned n = 0;
    for (; n < 12000 && original.getSaveRamData()[0x33] != 0x10; ++n)
        expect(original.stepOneCycle().success, "CPU envelope probe reaches phase-7 trigger");
    expect(n < 12000, "CPU envelope probe reaches extra-delay snapshot checkpoint");
    const auto saved = original.saveState();
    restored.loadState(saved.data(), saved.size());
    for (unsigned cycle = 0; cycle < 30000; ++cycle)
        expect(original.stepOneCycle().success && restored.stepOneCycle().success,
               "CPU envelope probe runs delayed first update and subsequent normal updates");
    const auto *ram = original.getSaveRamData();
    expect(ram[0x30] == 7 && ram[0x31] == 6 && ram[0x32] == 5,
           "CPU PCM/DIV observations distinguish trigger delay from subsequent envelope pace");
    expect(original.takeAudioSamples() == restored.takeAudioSamples() &&
               original.saveState() == restored.saveState(),
           "pending extra envelope tick survives exact PCM and full-state replay");
}

void testCpuSpeedSwitchPpuLatchAndReplay() {
    for (uint8_t mode = 0; mode < 4; ++mode) {
        std::vector<uint8_t> rom(0x8000, 0);
        rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
        rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x149] = 2;
        size_t at = 0x150;
        const auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) rom[at++] = b; };
        emit({0xF3, 0x3E, 0x0A, 0xEA, 0, 0});
        std::array<uint16_t, 2> stoppedPcs{};
        for (uint8_t direction = 0; direction < 2; ++direction) {
            emit({0x3E, 1, 0xE0, 0x4D});
            // Synchronize to a fresh line before selecting the target mode.
            emit({0xF0, 0x44, 0xFE, 2, 0x28, 0xFA});
            emit({0xF0, 0x44, 0xFE, 2, 0x20, 0xFA});
            emit({0xF0, 0x41, 0xE6, 3, 0xFE, mode, 0x28, 0xF8});
            emit({0xF0, 0x41, 0xE6, 3, 0xFE, mode, 0x20, 0xF8});
            stoppedPcs[direction] = static_cast<uint16_t>(at + 2);
            emit({0x10, 0, 0xF0, 0x4D, 0xEA, direction, 0xA0});
        }
        emit({0x18, 0xFE});
        GameBoySystem original(rom, HardwareModel::CgbE), restored(rom, HardwareModel::CgbE);
        for (unsigned direction = 0; direction < 2; ++direction) {
            unsigned steps = 0;
            // STOP holds PC immediately after its two-byte opcode.
            const auto stoppedPc = stoppedPcs[direction];
            for (; steps < 80000 && original.getCpuRegisters().getRefPC() != stoppedPc; ++steps)
                expect(original.stepOneCycle().success, "CPU reaches selected-mode STOP");
            expect(steps < 80000, "selected-mode STOP executes within two bounded frames");
            expect(original.stepOneCycle().success, "STOP completes after consuming its padding byte");
            const auto saved = original.saveState();
            expect(saved[saved.size() - 50] == mode, "STOP latches its starting PPU mode into CGB v13");
            auto version6 = saved; version6.resize(version6.size() - 49);
            auto legacy = version6; legacy.resize(legacy.size() - 4); legacy[8] = 4;
            const uint32_t length = static_cast<uint32_t>(legacy.size());
            for (unsigned i = 0; i < 4; ++i) legacy[16 + i] = static_cast<uint8_t>(length >> (8 * i));
            const auto hash = StateInternal::sha256(legacy.data() + 116, legacy.size() - 116);
            std::copy(hash.begin(), hash.end(), legacy.begin() + 84);
            restored.loadState(legacy.data(), legacy.size());
            expect(restored.saveState()[restored.saveState().size() - 50] == 3, "legacy mid-pause state retains unrestricted PPU semantics");
            auto legacy5 = version6; legacy5.erase(legacy5.end() - 4, legacy5.end() - 1); legacy5[8] = 5;
            const uint32_t length5 = static_cast<uint32_t>(legacy5.size());
            for (unsigned i = 0; i < 4; ++i) legacy5[16 + i] = static_cast<uint8_t>(length5 >> (8 * i));
            const auto hash5 = StateInternal::sha256(legacy5.data() + 116, legacy5.size() - 116);
            std::copy(hash5.begin(), hash5.end(), legacy5.begin() + 84);
            restored.loadState(legacy5.data(), legacy5.size());
            expect(restored.saveState()[restored.saveState().size() - 50] == mode, "legacy CGB v5 retains its actual mid-pause PPU latch");
            restored.loadState(saved.data(), saved.size());
            const auto pc = original.getCpuRegisters().getRefPC();
            for (unsigned cycle = 0; cycle < 2050; ++cycle) {
                const auto a = original.stepOneCycle(), b = restored.stepOneCycle();
                expect(a.success && b.success && a.tCyclesElapsed == (direction ? 2u : 4u),
                       "speed pause preserves the old speed for all 2050 M-cycles");
                expect(original.getCpuRegisters().getRefPC() == pc, "CPU does not execute during speed pause");
                if (cycle == 1024) expect(original.saveState()[original.saveState().size() - 50] == mode,
                                         "PPU mode latch survives ongoing scanline transitions");
            }
            expect(original.saveState()[original.saveState().size() - 50] == 3, "speed-switch completion releases PPU locks");
            for (unsigned cycle = 0; cycle < 12; ++cycle)
                expect(original.stepOneCycle().success && restored.stepOneCycle().success,
                       "CPU resumes after speed switch");
            expect(original.getSaveRamData()[direction] == (direction ? 0x7E : 0xFE),
                   "guest observes completed KEY1 switch in each direction");
            expect(original.saveState() == restored.saveState(), "paused PPU, CPU, video and audio state replay exactly");
        }
    }
}

void testCpuMutedPulsePhaseAndSnapshot() {
    for (unsigned gap : {1u, 2u, 3u, 7u, 19u}) {
        const auto makeProbe = [gap](bool mute) {
            std::vector<uint8_t> rom(0x8000, 0);
            rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
            rom[0x143] = 0xC0; rom[0x147] = 3; rom[0x149] = 2;
            size_t at = 0x150;
            const auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) rom[at++] = b; };
            const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
            emit({0xF3, 0x3E, 0x0A, 0xEA, 0, 0});
            io(0x26, 0); io(0x26, 0x80);
            io(0x11, 0x40); io(0x16, 0x40); io(0x12, 0x50); io(0x17, 0x50);
            io(0x13, 0xF8); io(0x18, 0xF8); io(0x14, 0x87); io(0x19, 0x87);
            io(0x12, mute ? 0 : 0x50); io(0x17, mute ? 0 : 0x50);
            emit({0x3E, 1, 0xEA, 0x40, 0xA0}); // Inactive-divider snapshot checkpoint.
            for (unsigned n = 0; n < gap; ++n) emit({0});
            io(0x12, 0x50); io(0x17, 0x50); io(0x14, 0x87); io(0x19, 0x87);
            emit({0x21, 0, 0xA1, 0x06, 128});
            emit({0xF0, 0x76, 0x22, 0x05, 0x20, 0xFA});
            emit({0x3E, 1, 0xEA, 0x41, 0xA0, 0x18, 0xFE});
            return rom;
        };
        const auto rom = makeProbe(true);
        GameBoySystem original(rom, HardwareModel::CgbE), restored(rom, HardwareModel::CgbE);
        GameBoySystem continuous(makeProbe(false), HardwareModel::CgbE);
        unsigned steps = 0;
        for (; steps < 1000 && original.getSaveRamData()[0x40] != 1; ++steps)
            expect(original.stepOneCycle().success && continuous.stepOneCycle().success,
                   "CPU pulse probes reach equivalent muted and continuous checkpoints");
        expect(steps < 1000 && continuous.getSaveRamData()[0x40] == 1,
               "only DAC output gating differs between the guest probes");
        const auto saved = original.saveState();
        restored.loadState(saved.data(), saved.size());
        for (unsigned n = 0; n < 2000; ++n)
            expect(original.stepOneCycle().success && restored.stepOneCycle().success && continuous.stepOneCycle().success,
                   "CPU retriggers after a silent interval and captures digital pulse output");
        const auto *data = original.getSaveRamData();
        expect(data[0x41] == 1 && continuous.getSaveRamData()[0x41] == 1,
               "both CPU probes complete their 128-sample PCM observation");
        expect(std::equal(data + 0x100, data + 0x180, continuous.getSaveRamData() + 0x100),
               "guest PCM after retrigger matches the continuously clocked duty reference");
        expect(std::any_of(data + 0x101, data + 0x180, [&](uint8_t x) { return x != data[0x100]; }),
               "guest observations include actual duty transitions");
        expect(original.takeAudioSamples() == restored.takeAudioSamples() && original.saveState() == restored.saveState(),
               "snapshot during a muted pulse interval replays exact duty, PCM and complete system state");
    }
}

void testSpeedDmaAudioInputAndSnapshots() {
    auto rom=Test::makeCgbExerciseRom();
    GameBoySystem original(rom,HardwareModel::CgbE), restored(rom,HardwareModel::CgbE);
    bool fast=false;
    // Restore at instruction boundaries, DMA, speed-switch pause and color fetches.
    for(unsigned checkpoint=0;checkpoint<100;++checkpoint) {
        for(unsigned i=0;i<(checkpoint<80?37u:3001u);++i) {
            auto step=original.stepOneCycle();
            expect(step.success,"original CGB execution succeeds");
            fast |= step.tCyclesElapsed==2;
        }
        auto state=original.saveState();
        expect(state[8]==13 && state[12]==2,"CGB states identify version 13/model 2");
        auto *ram=restored.getSaveRamData();
        restored.loadState(state.data(),state.size());
        expect(ram==restored.getSaveRamData(),"CGB restore preserves frontend SRAM pointers");
        for(unsigned i=0;i<53;++i) {
            auto a=original.stepOneCycle(),b=restored.stepOneCycle();
            expect(a.success && b.success && a.tCyclesElapsed==b.tCyclesElapsed,"restored speed/CPU progress matches");
        }
        expect(original.takeAudioSamples()==restored.takeAudioSamples(),"CGB restored PCM is exact");
        expect(original.getCompletedColorFrameBuffer()==restored.getCompletedColorFrameBuffer(),"CGB restored color frame matches");
        expect(original.saveState()==restored.saveState(),"CGB bank, DMA, speed, FIFO and palette state replays exactly");
    }
    expect(fast,"KEY1 STOP resumes CPU at two base T-cycles per M-cycle");
    const auto *ram=original.getSaveRamData();
    expect(ram[0]==0x11 && ram[1]==0x33 && ram[2]==0xFE,"CGB handoff, GDMA signature and KEY1 readback match");
    auto before=original.saveState();
    auto invalid=before; invalid[12]=1;
    try { original.loadState(invalid.data(),invalid.size()); expect(false,"cross-model snapshot must fail"); }
    catch(const std::invalid_argument &) {}
    expect(original.saveState()==before,"failed CGB snapshot load is atomic");
    auto begin=original.getElapsedTCycles();
    size_t samples=0;
    for(unsigned i=0;i<60;++i) {
        expect(original.stepForPresentation().reason==PresentationReason::VBlank,"double-speed CPU still presents real VBlank");
        samples+=original.takeAudioSamples().size()/2;
    }
    auto elapsed=original.getElapsedTCycles()-begin;
    expect(samples==elapsed*48000/4194304 || samples==elapsed*48000/4194304+1,"double-speed audio follows base hardware time");
    expect(original.getCompletedColorFrameBuffer()[80*160+80]==0x001F,"neutral input renders red");
    original.setButtonState(JoypadButton::Right,true);
    for(unsigned i=0;i<4;++i) original.stepForPresentation();
    expect(original.getCompletedColorFrameBuffer()[80*160+80]==0x7C00,"input changes CGB palette and visible output to blue");
}
}
int main() {
    testCpuMutedPulsePhaseAndSnapshot(); testCpuSpeedSwitchPpuLatchAndReplay(); testAcid2(); testInvalidCgbBankStateIsRejectedAtomically(); testSpeedDmaAudioInputAndSnapshots();
    testCgbRomExecutionDuringWramOamDma();
    testCgbOamDmaReadsMbc1RomBank();
    testCgbOamDmaCompletesInFewerDotsAtDoubleSpeed();
    testVramDmaKeepsBaseClockRateAtDoubleSpeed();
    testCgbWramOamDmaFeedsCpuFetchAndLeavesCartridgeBusUsable();
    testCgbRomOamDmaFetchConflictAndCartridgeWrite();
    testCgbRomOamDmaLeavesWramStackAccessible();
    testHblankDmaWaitsForHaltWake();
    testHblankDmaTransfersOneBlockWhenLcdIsDisabled();
    testCgbWaveRamCpuAccessAndSnapshot();
    testPcmRegistersThroughCpuBusAndSnapshot();
    testLengthEnableEdgesThroughCpuAndDiv();
    testEnvelopeTriggerPhaseThroughCpuAndSnapshot();
    std::cout<<"CGB acid2, speed/DMA/audio/input and snapshot tests passed\n";
}
