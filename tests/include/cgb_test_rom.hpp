#ifndef LOTUSGB_CGB_TEST_ROM_HPP
#define LOTUSGB_CGB_TEST_ROM_HPP
#include <cstdint>
#include <initializer_list>
#include <vector>
namespace LOTUSGB::Test {
// Original MIT fixture. Native CGB detection, palette RAM, banked memory,
// general DMA, KEY1/STOP, audible tone and a button-controlled red/blue field.
inline std::vector<uint8_t> makeCgbExerciseRom() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100]=0xC3; rom[0x101]=0x50; rom[0x102]=1;
    rom[0x143]=0xC0; rom[0x147]=3; rom[0x149]=2;
    size_t at=0x150;
    auto emit=[&](std::initializer_list<uint8_t> b) { for(auto x:b) rom[at++]=x; };
    auto io=[&](uint8_t reg,uint8_t value) { emit({0x3E,value,0xE0,reg}); };
    auto jump=[&](size_t target) { emit({0xC3,static_cast<uint8_t>(target),static_cast<uint8_t>(target>>8)}); };
    emit({0xF3,0x31,0xFE,0xFF,0x47,0x3E,0x0A,0xEA,0,0,0x78,0xEA,0,0xA0});
    io(0x40,0);
    emit({0x21,0,0x80,0x06,8,0x3E,0xFF,0x22,0xAF,0x22,0x05,0x20,0xF8});
    io(0x68,0x80);
    for(uint8_t byte : {0xFF,0x7F,0x1F,0,0xE0,3,0,0x7C}) io(0x69,byte);
    emit({0x21,0,0xC0,0x06,16,0x3E,0x33,0x22,0x3C,0x05,0x20,0xFB});
    io(0x51,0xC0); io(0x52,0x0F); io(0x53,0xE0); io(0x54,0x1F); io(0x55,0x1F);
    emit({0xFA,0x10,0x80,0xEA,1,0xA0});
    io(0x4D,1); emit({0x10,0});
    emit({0xF0,0x4D,0xEA,2,0xA0});
    io(0x26,0x80); io(0x24,0x77); io(0x25,0x11);
    io(0x11,0x80); io(0x12,0xF3); io(0x13,0x40); io(0x14,0x87);
    io(0x00,0x20); io(0x40,0x91);
    io(0x51,0xC0); io(0x52,0); io(0x53,4); io(0x54,0); io(0x55,0x8F);
    size_t loop=at;
    emit({0xF0,0x44,0xFE,0x90,0x38,0xFA}); // Wait VBlank
    io(0x68,0x82);
    emit({0xF0,0x00,0xE6,1,0x28,0}); size_t rightPatch=at-1;
    io(0x69,0x1F); io(0x69,0); // Neutral: red
    size_t endJump=at; jump(0);
    rom[rightPatch]=static_cast<uint8_t>(at-rightPatch-1);
    io(0x69,0); io(0x69,0x7C); // Right: blue
    rom[endJump+1]=static_cast<uint8_t>(at); rom[endJump+2]=static_cast<uint8_t>(at>>8);
    emit({0x21,0x10,0xA0,0x34});
    emit({0xF0,0x44,0xFE,0x90,0x30,0xFA}); // Wait until next visible frame
    jump(loop);
    return rom;
}
// Original CGB-compatible MBC1/MBC5 battery game fixtures. They render a color
// field, change it on Right, and advance SRAM progress each frame.
inline std::vector<uint8_t> makeCgbMbc1BatteryExerciseRom() {
    auto rom = makeCgbExerciseRom();
    rom[0x143] = 0x80; // CGB compatible; header selects native CGB hardware.
    rom[0x147] = 0x03; // MBC1 + RAM + battery.
    rom[0x148] = 0x00; // 32 KiB ROM.
    rom[0x149] = 0x02; // 8 KiB external SRAM.
    return rom;
}
inline std::vector<uint8_t> makeCgbMbc5BatteryExerciseRom() {
    auto rom = makeCgbExerciseRom();
    rom[0x143] = 0x80;
    rom[0x147] = 0x1B; // MBC5 + RAM + battery.
    rom[0x148] = 0x00; // 32 KiB ROM.
    rom[0x149] = 0x02; // 8 KiB external SRAM.
    return rom;
}
// Original 8-MiB MBC5 game: execute a routine above ROM bank $FF every
// frame, advancing SRAM bank 15 independently of the bank-0 game progress.
inline std::vector<uint8_t> makeCgbMbc5HighBankExerciseRom(uint16_t bank) {
    auto rom = makeCgbMbc5BatteryExerciseRom();
    rom.resize(0x800000, 0);
    rom[0x148] = 8; // 512 ROM banks.
    rom[0x149] = 4; // Sixteen 8-KiB SRAM banks.
    rom[0x101] = 0; rom[0x102] = 3;
    const uint8_t setup[] = {
        0xF5, // Preserve the native CGB startup A/flags for the game probe.
        0x3E, static_cast<uint8_t>(bank), 0xEA, 0, 0x20,
        0x3E, static_cast<uint8_t>(bank >> 8), 0xEA, 0, 0x30,
        0xF1, 0xC3, 0x50, 1
    };
    for (size_t i = 0; i < sizeof(setup); ++i) rom[0x300 + i] = setup[i];
    const uint8_t routine[] = {
        0xF5, 0xE5,                         // Preserve AF/HL.
        0x3E, 15, 0xEA, 0, 0x40,          // Select SRAM bank 15.
        0x21, 0x10, 0xA0, 0x34,            // Advance imported progress.
        0x3E, static_cast<uint8_t>(bank), 0xEA, 0x11, 0xA0,
        0x3E, static_cast<uint8_t>(bank >> 8), 0xEA, 0x12, 0xA0,
        0xAF, 0xEA, 0, 0x40,               // Return to SRAM bank 0.
        0x21, 0x10, 0xA0, 0x34,            // Original game progress.
        0xE1, 0xF1, 0xC9
    };
    for (size_t i = 0; i < sizeof(routine); ++i) rom[bank * 0x4000 + i] = routine[i];
    // Replace the original frame-loop increment with a call into banked ROM.
    for (size_t at = 0x150; at < 0x2FC; ++at) {
        if (rom[at] == 0x21 && rom[at + 1] == 0x10 &&
            rom[at + 2] == 0xA0 && rom[at + 3] == 0x34) {
            rom[at] = 0xCD; rom[at + 1] = 0; rom[at + 2] = 0x40; rom[at + 3] = 0;
            break;
        }
    }
    return rom;
}
// Original CGB-compatible MBC3+RTC game fixture. Its startup reads the
// latched seconds register, increments it, writes it back, then exposes the
// observed value in battery SRAM at offset $30.
inline std::vector<uint8_t> makeCgbMbc3RtcExerciseRom() {
    auto rom = makeCgbExerciseRom();
    rom[0x143] = 0x80; // CGB compatible.
    rom[0x147] = 0x10; // MBC3 + timer + RAM + battery.
    rom[0x148] = 0x00; // 32 KiB ROM.
    rom[0x149] = 0x03; // 32 KiB external SRAM.
    rom[0x101] = 0x00;
    rom[0x102] = 0x03; // Run RTC exercise before the normal CGB game loop.

    const uint8_t rtcExercise[] = {
        0xF3,                         // DI
        0x3E, 0x0A, 0xEA, 0x00, 0x00, // Enable cartridge RAM/RTC.
        0x3E, 0x0C, 0xEA, 0x00, 0x40, // Select RTC day-high register.
        0x3E, 0x40, 0xEA, 0x00, 0xA0, // Halt clock for deterministic reads.
        0xAF, 0xEA, 0x00, 0x60,       // Begin RTC latch sequence (0).
        0x3C, 0xEA, 0x00, 0x60,       // Complete RTC latch sequence (1).
        0x3E, 0x08, 0xEA, 0x00, 0x40, // Select RTC seconds register.
        0xFA, 0x00, 0xA0,             // Read latched seconds.
        0x3C, 0xEA, 0x00, 0xA0,       // Increment and write seconds.
        0xAF, 0xEA, 0x00, 0x60,       // Latch the updated clock (0, then 1).
        0x3C, 0xEA, 0x00, 0x60,
        0x3E, 0x08, 0xEA, 0x00, 0x40, // Re-select seconds and read them back.
        0xFA, 0x00, 0xA0,
        0x47,                         // Preserve observed seconds in B.
        0xAF, 0xEA, 0x00, 0x40,       // Select SRAM bank 0.
        0x78, 0xEA, 0x30, 0xA0,       // Store observed seconds at SRAM[$30].
        0xC3, 0x50, 0x01              // Continue into the CGB game fixture.
    };
    for (size_t i = 0; i < sizeof(rtcExercise); ++i)
        rom[0x0300 + i] = rtcExercise[i];
    return rom;
}
// Freeze an imported running clock, then copy all five guest-visible RTC
// registers to SRAM $30-$34 before entering the color/input game loop.
inline std::vector<uint8_t> makeCgbRtcOfflineResumeRom() {
    auto rom = makeCgbMbc3RtcExerciseRom();
    size_t at = 0x300;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) rom[at++] = byte;
    };
    emit({0xF3, 0x3E, 0x0A, 0xEA, 0, 0}); // Enable RAM/RTC.
    emit({0xAF, 0xEA, 0, 0x60, 0x3C, 0xEA, 0, 0x60}); // Latch current flags.
    emit({0x3E, 0x0C, 0xEA, 0, 0x40, 0xFA, 0, 0xA0});
    emit({0xF6, 0x40, 0xEA, 0, 0xA0}); // Halt, preserving day bit and carry.
    emit({0xAF, 0xEA, 0, 0x60, 0x3C, 0xEA, 0, 0x60}); // Latch.
    for (uint8_t reg = 0; reg < 5; ++reg) {
        emit({0x3E, static_cast<uint8_t>(8 + reg), 0xEA, 0, 0x40});
        emit({0xFA, 0, 0xA0, 0x47, 0xAF, 0xEA, 0, 0x40});
        emit({0x78, 0xEA, static_cast<uint8_t>(0x30 + reg), 0xA0});
    }
    emit({0xC3, 0x50, 1});
    return rom;
}
// Original MBC30 setup for expanded ROM/SRAM, frozen RTC and portable state.
inline std::vector<uint8_t> makeCgbMbc30ExerciseRom() {
    auto rom=makeCgbExerciseRom();rom.resize(0x400000,0);
    rom[0x147]=0x10;rom[0x148]=7;rom[0x149]=5;
    rom[0x101]=0;rom[0x102]=0x30;
    rom[0x80*0x4000]=0x80;
    const uint8_t setup[]={0xF3,0xF5,0x3E,0x0A,0xEA,0,0,0x3E,0x80,0xEA,0,0x20,
        0xFA,0,0x40,0xEA,4,0xA0,0x3E,7,0xEA,0,0x40,0x3E,0x77,0xEA,0,0xA0,
        0x3E,0x0C,0xEA,0,0x40,0x3E,0x40,0xEA,0,0xA0,0xAF,0xEA,0,0x40,0xF1,0xC3,0x50,1};
    for(size_t n=0;n<sizeof(setup);++n)rom[0x3000+n]=setup[n];
    return rom;
}

}
#endif
