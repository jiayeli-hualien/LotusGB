#include "game_boy_system.hpp"
#include "state_hash.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace LOTUSGB;
namespace {
void expect(bool condition, const char *message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::string digest(const std::string &input) {
    const auto hash = StateInternal::sha256(reinterpret_cast<const uint8_t *>(input.data()), input.size());
    std::string out;
    for (auto b : hash) { out += "0123456789abcdef"[b >> 4]; out += "0123456789abcdef"[b & 15]; }
    return out;
}
void testHashVectors() {
    expect(digest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty input");
    expect(digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc");
    expect(digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 padding crosses block");
    expect(digest(std::string(1000000, 'a')) ==
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 multiple blocks");
}
void cycles(GameBoySystem &s, unsigned count) {
    for (unsigned i=0;i<count;++i) expect(s.stepOneCycle().success, "CPU advances after snapshot");
}
// Original synthetic ROM exercises live audio, LCD fetches, timer/serial IRQs,
// mapper RAM, CB memory instructions and DMA from an HRAM routine.
std::vector<uint8_t> makeRom(uint8_t type=0x1b, uint8_t ram=4) {
    std::vector<uint8_t> rom(0x10000, 0);
    rom[0x147]=type; rom[0x149]=ram;
    rom[0x100]=0xc3; rom[0x101]=0x50; rom[0x102]=1;
    for (unsigned vector : {0x40,0x48,0x50,0x58,0x60}) rom[vector]=0xd9;
    size_t at=0x150;
    auto emit=[&](std::initializer_list<uint8_t> bytes) { for (auto b:bytes) rom[at++]=b; };
    auto io=[&](uint8_t reg,uint8_t value) { emit({0x3e,value,0xe0,reg}); };
    emit({0xf3,0x31,0xfe,0xff}); io(0x40,0);
    emit({0x3e,0x0a,0xea,0,0,0x3e,2,0xea,0,0x20});
    emit({0x21,0,0x80,0x06,16,0x3e,0xaa,0x22,0x2f,0x05,0x20,0xfb});
    emit({0x21,0,0xc0,0x06,0xa0,0x3e,0x10,0x22,0x3c,0x05,0x20,0xfb});
    io(0x26,0x80); io(0x24,0x77); io(0x25,0xff);
    io(0x10,0x16); io(0x11,0x80); io(0x12,0xf3); io(0x13,0x40); io(0x14,0x87);
    io(0x16,0x40); io(0x17,0xa2); io(0x18,0x60); io(0x19,0x87);
    for (unsigned i=0;i<16;++i) io(0x30+i, static_cast<uint8_t>(i*17));
    io(0x1a,0x80); io(0x1c,0x20); io(0x1d,0x80); io(0x1e,0x87);
    io(0x21,0xf2); io(0x22,0x13); io(0x23,0x80);
    io(0x47,0xe4); io(0x48,0xe4); io(0x4a,30); io(0x4b,57); io(0x40,0xf3);
    // DMA, wait, and return, entirely from HRAM.
    const uint8_t routine[]={0x3e,0xc0,0xe0,0x46,0x06,40,0x05,0x20,0xfd,0xc3,0,0};
    const uint16_t resume=static_cast<uint16_t>(at+sizeof(routine)*4+3);
    for (size_t i=0;i<sizeof(routine);++i)
        io(0x80+i,i==10?static_cast<uint8_t>(resume):i==11?static_cast<uint8_t>(resume>>8):routine[i]);
    emit({0xc3,0x80,0xff});
    io(0x04,0); io(0x05,0xfc); io(0x06,0xe0); io(0x07,5); io(0x01,0xa5); io(0x02,0x81);
    io(0xff,0x0d); emit({0xfb});
    const uint16_t loop=at;
    emit({0x21,0,0xa0,0x34,0xcb,0x06,0x7e,0xe0,0x43,0x76,0xc3,
          static_cast<uint8_t>(loop),static_cast<uint8_t>(loop>>8)});
    return rom;
}

std::vector<uint8_t> makeMode2OamSnapshotRom(uint16_t &loopAddress) {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes) rom[at++] = byte;
    };
    const auto io = [&](uint8_t reg, uint8_t value) {
        emit({0x3E, value, 0xE0, reg});
    };

    io(0x40, 0x00); // Set up OAM and tile data with the LCD disabled.
    io(0x47, 0xE4);
    io(0x48, 0xE4);
    emit({0x21, 0x10, 0x80, 0x3E, 0xFF, 0x22, 0xAF, 0x77});
    emit({0x21, 0x00, 0xFE, 0x3E, 16, 0x22, 0x3E, 8, 0x22,
          0x3E, 1, 0x22, 0xAF, 0x77});
    io(0x40, 0x93);
    loopAddress = static_cast<uint16_t>(at);
    emit({0x18, 0xFE});
    return rom;
}

void testMode2OamSnapshotReplay() {
    uint16_t loopAddress = 0;
    const auto rom = makeMode2OamSnapshotRom(loopAddress);
    GameBoySystem original(rom), restored(rom);
    unsigned steps = 0;
    for (; steps < 2000 &&
           original.getCpuRegisters().getRefPC() != loopAddress; ++steps)
        expect(original.stepOneCycle().success,
               "guest prepares an object and enables the LCD");
    expect(steps < 2000, "guest reaches the loop immediately after LCD enable");

    const auto pendingScan = original.saveState();
    cycles(original, 500);
    const auto expectedFrame = original.getFrameBuffer();
    const auto expectedState = original.saveState();
    expect(expectedFrame[0] == 1,
           "the Mode 2-selected object is rendered after the saved scan completes");

    restored.loadState(pendingScan.data(), pendingScan.size());
    cycles(restored, 500);
    expect(restored.getFrameBuffer() == expectedFrame &&
               restored.saveState() == expectedState,
           "a mid-Mode-2 object scan restores its candidate data and replays exactly");
}
std::vector<uint8_t> makeMbc7SerialResumeRom(uint16_t &waitAddress,
                                             uint16_t &busyWaitAddress) {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x0143] = 0x80;
    rom[0x0147] = 0x22;
    rom[0x0148] = 0;
    rom[0x0149] = 0;
    rom[0x0100] = 0xC3; rom[0x0101] = 0x50; rom[0x0102] = 0x01;
    size_t at = 0x0150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes) rom[at++] = byte;
    };
    const auto write = [&](uint16_t address, uint8_t value) {
        emit({0x3E, value, 0xEA, static_cast<uint8_t>(address),
              static_cast<uint8_t>(address >> 8)});
    };
    const auto sendBit = [&](bool one) {
        const uint8_t low = static_cast<uint8_t>(0x80 | (one ? 0x02 : 0));
        const uint8_t high = static_cast<uint8_t>(0xC0 | (one ? 0x02 : 0));
        write(0xA080, low);
        write(0xA080, high);
    };
    write(0x0000, 0x0A);
    write(0x4000, 0x48);
    write(0xA080, 0x00);
    write(0xA080, 0x80);
    constexpr uint16_t ewen = 0x260;
    for (unsigned bit = 10; bit > 6; --bit)
        sendBit((ewen & (uint16_t{1} << (bit - 1))) != 0);
    write(0xFF00, 0x10); // Select buttons and wait for A after the snapshot.
    waitAddress = static_cast<uint16_t>(at);
    emit({0xF0, 0x00, 0xE6, 0x01, 0x20, 0xFA}); // JR NZ back to the P1 poll.
    for (unsigned bit = 6; bit > 0; --bit)
        sendBit((ewen & (uint16_t{1} << (bit - 1))) != 0);
    write(0xA080, 0x00);
    write(0xA080, 0x80);
    constexpr uint16_t writeWord = 0x280 | 0x35;
    for (unsigned bit = 10; bit > 0; --bit)
        sendBit((writeWord & (uint16_t{1} << (bit - 1))) != 0);
    constexpr uint16_t data = 0xA55A;
    for (unsigned bit = 16; bit > 0; --bit)
        sendBit((data & (uint16_t{1} << (bit - 1))) != 0);
    write(0xA080, 0x00);
    write(0xFF00, 0x10); // Wait for B while the EEPROM programs the word.
    busyWaitAddress = static_cast<uint16_t>(at);
    emit({0xF0, 0x00, 0xE6, 0x02, 0x20, 0xFA});
    const size_t loop = at;
    emit({0x18, 0xFE});
    (void)loop;
    return rom;
}
void compareFuture(GameBoySystem &a, GameBoySystem &b, unsigned count) {
    a.setButtonState(JoypadButton::A,true); b.setButtonState(JoypadButton::A,true);
    cycles(a,count); cycles(b,count);
    expect(a.getElapsedTCycles()==b.getElapsedTCycles(), "restored clock matches");
    expect(a.getFrameBuffer()==b.getFrameBuffer(), "restored in-flight pixels match");
    expect(a.getCompletedFrameBuffer()==b.getCompletedFrameBuffer(), "restored published pixels match");
    expect(a.takeAudioSamples()==b.takeAudioSamples(), "restored PCM and filter history match");
    expect(a.saveState()==b.saveState(), "all component state matches after execution");
}
void testRoundTripsAtCycleBoundaries() {
    const auto rom=makeRom();
    GameBoySystem original(rom), restored(rom);
    // Cover instruction fetch, CB operations, DMA startup/transfer and first
    // Mode 3 pipeline. Later checkpoints cross scanlines and frame boundaries.
    for (unsigned i=0;i<300;++i) {
        cycles(original,i<220?13:241);
        auto state=original.saveState();
        auto *ram=restored.getSaveRamData();
        restored.loadState(state.data(),state.size());
        expect(restored.getSaveRamData()==ram, "SRAM pointer stable across restore");
        expect(restored.saveState()==state, "fresh system preserves every saved field");
        compareFuture(original,restored,97);
    }
    auto state=original.saveState();
    restored.loadState(state.data(),state.size());
    compareFuture(original,restored,40000);
    expect(original.getSaveRamSize()==128*1024, "largest MBC5 RAM fits snapshot");
}
void testMapperStorage() {
    for (uint8_t type : {0x03,0x06,0x13,0x1b,0x1e}) {
        auto rom=makeRom(type,3);
        GameBoySystem a(rom), b(rom);
        cycles(a,10000);
        const auto state=a.saveState();
        auto *ram=b.getSaveRamData();
        b.loadState(state.data(),state.size());
        expect(b.getSaveRamData()==ram,"mapper RAM storage remains published");
        compareFuture(a,b,18000);
    }
}
void testCpuAtEveryMachineCycle() {
    std::vector<uint8_t> rom(0x8000,0);
    const uint8_t program[]={
        0xf3,0x31,0xfe,0xff,0x21,0,0xc0,0x36,0x81,
        0xcb,0x06,0xcb,0xcb,0x3e,1,0xe0,0xff,0xe0,0x0f,
        0x76, // IME off with pending interrupt: HALT bug
        0x04,0xfb,0x00,0x76,0x18,0xfe};
    std::copy(std::begin(program),std::end(program),rom.begin());
    rom[0x40]=0xd9;
    auto a=GameBoySystem::createForTest(rom);
    auto b=GameBoySystem::createForTest(rom);
    for (unsigned i=0;i<100;++i) {
        auto state=a.saveState();b.loadState(state.data(),state.size());
        cycles(a,1);cycles(b,1);
        expect(a.saveState()==b.saveState(),"CPU fetch, CB, EI, HALT bug and interrupt-entry round trip");
    }
}
void testRtcAndRamRestore() {
    auto rom=makeRom(0x10,3);
    const uint8_t program[]={
        0xf3,0x3e,0x0a,0xea,0,0, // enable SRAM/RTC
        0x3e,0x0c,0xea,0,0x40,0x3e,0x40,0xea,0,0xa0, // halt clock
        0x3e,8,0xea,0,0x40,0x3e,23,0xea,0,0xa0, // seconds=23
        0xaf,0xea,0,0x60,0x3c,0xea,0,0x60, // latch
        0xfa,0,0xa0,0x47, // B=latched seconds
        0x3e,3,0xea,0,0x40,0x3e,0x5a,0xea,0,0xa0, // RAM bank3
        0x18,0xfe};
    std::copy(std::begin(program),std::end(program),rom.begin()+0x150);
    GameBoySystem a(rom),b(rom);
    auto initial=a.saveState(); b.loadState(initial.data(),initial.size());
    expect(b.saveState()==initial,"lazy RTC storage snapshot restored before first access");
    cycles(a,200);
    expect(a.getCpuRegisters().getRefB()==23,"RTC latched register observable through CPU");
    auto state=a.saveState(); auto *rtc=b.getRtcData();auto *ram=b.getSaveRamData();
    std::fill_n(rtc,b.getRtcSize(),0);std::fill_n(ram,b.getSaveRamSize(),0);
    b.loadState(state.data(),state.size());
    expect(b.getRtcData()==rtc && b.getSaveRamData()==ram,"RTC and SRAM addresses stable");
    expect(std::equal(rtc,rtc+b.getRtcSize(),a.getRtcData()),"RTC persistence block restored");
    expect(ram[3*8192]==0x5a,"banked SRAM restored with clock");
    compareFuture(a,b,2000);
}
void testStopAndBootState() {
    std::vector<uint8_t> rom(0x8000,0);
    const uint8_t stopped[]={0xaf,0xe0,0x00,0x10,0,0x3e,0x42,0x18,0xfe};
    std::copy(std::begin(stopped),std::end(stopped),rom.begin());
    auto a=GameBoySystem::createForTest(rom);
    auto b=GameBoySystem::createForTest(rom);
    cycles(a,10);
    auto state=a.saveState(); b.loadState(state.data(),state.size());
    expect(b.stepOneCycle().tCyclesElapsed==0,"STOP remains stopped after restore");
    compareFuture(a,b,10);
    expect(b.getCpuRegisters().getRefA()==0x42,"restored STOP wakes on input");

    std::vector<uint8_t> boot(256,0);
    boot[0]=0x3e;boot[1]=1;boot[2]=0xe0;boot[3]=0x50;
    rom[4]=0x18;rom[5]=0xfe;
    GameBoySystem c(rom,nullptr,nullptr,boot), d(rom,nullptr,nullptr,boot);
    cycles(c,2);state=c.saveState();
    cycles(d,20);d.loadState(state.data(),state.size());
    compareFuture(c,d,20);
}
void testIllegalOpcodeLockupSnapshotReplay() {
    for (const auto model : {HardwareModel::Dmg, HardwareModel::CgbE}) {
        std::vector<uint8_t> rom(0x8000, 0);
        rom[0x0143] = model == HardwareModel::CgbE ? 0x80 : 0;
        rom[0x0100] = 0xD3; // Invalid opcodes hard-lock both CPU models.
        GameBoySystem original(rom, model), restored(rom, model);
        cycles(original, 2); // Fetch the opcode, then enter lock-up.
        const uint16_t lockedPc = original.getCpuRegisters().getRefPC();
        const auto lockedState = original.saveState();
        const uint8_t expectedVersion = model == HardwareModel::CgbE ? 13 : 11;
        expect(lockedState[8] == expectedVersion,
               "DMG/CGB snapshots use the lock-state format version");
        restored.loadState(lockedState.data(), lockedState.size());
        expect(restored.saveState() == lockedState,
               "locked CPU state survives snapshot load exactly");

        const uint64_t startTCycles = original.getElapsedTCycles();
        cycles(original, 128);
        cycles(restored, 128);
        expect(original.getElapsedTCycles() - startTCycles == 128 * 4 &&
                   original.getCpuRegisters().getRefPC() == lockedPc &&
                   restored.getCpuRegisters().getRefPC() == lockedPc,
               "system clocks continue while the locked CPU stops fetching");
        expect(original.saveState() == restored.saveState(),
               "DMG/CGB lock-up replay keeps peripherals and complete state identical");
    }
}
void rejectedUnchanged(GameBoySystem &s, const std::vector<uint8_t> &bad, size_t size) {
    const auto before=s.saveState(); auto *ram=s.getSaveRamData();
    bool rejected=false;
    try { s.loadState(bad.data(),size); } catch(const std::invalid_argument &) { rejected=true; }
    expect(rejected,"invalid snapshot rejected");
    expect(s.saveState()==before && s.getSaveRamData()==ram,"rejected load leaves live state untouched");
}
void testRejectedSnapshots() {
    GameBoySystem a(makeRom());cycles(a,6000); const auto good=a.saveState();
    for (size_t offset : {size_t(0),size_t(8),size_t(12),size_t(16),size_t(20),size_t(52),size_t(100),good.size()-1}) {
        auto bad=good;bad[offset]^=0x80;rejectedUnchanged(a,bad,bad.size());
    }
    for (size_t size : {size_t(0),size_t(16),good.size()-1}) rejectedUnchanged(a,good,size);
    auto other=makeRom(); other[0x7000]=1;GameBoySystem b(other);
    rejectedUnchanged(b,good,good.size());
    // A recomputed checksum must not allow invalid typed fields or lengths.
    for (size_t offset : {size_t(116),size_t(117),size_t(117+4+12)}) {
        auto bad=good;bad[offset]=0xff;
        const auto hash=StateInternal::sha256(bad.data()+116,bad.size()-116);
        std::copy(hash.begin(),hash.end(),bad.begin()+84);
        rejectedUnchanged(a,bad,bad.size());
    }
    for (uint8_t invalidLockState : {uint8_t(1), uint8_t(2)}) {
        auto bad = good;
        bad.back() = invalidLockState;
        const auto hash = StateInternal::sha256(bad.data() + 116,
                                                bad.size() - 116);
        std::copy(hash.begin(), hash.end(), bad.begin() + 84);
        rejectedUnchanged(a, bad, bad.size());
    }
}
void testLegacySnapshotMigrationAndPulseFlags() {
    for (auto model : {HardwareModel::Dmg, HardwareModel::CgbE}) {
        auto rom = makeRom(); rom[0x143] = 0x80;
        GameBoySystem system(rom, model);
        auto current = system.saveState();
        const bool cgb = model == HardwareModel::CgbE;
        const size_t suffix = current.size() - (cgb ? 56 : 58);
        expect(current[8] == (cgb ? 13 : 11), "models publish DMG v11/CGB v13");
        auto legacy = current;
        legacy.resize(suffix); // Remove extensions after the original model payload.
        legacy[8] = model == HardwareModel::Dmg ? 1 : 2;
        const auto writeLengthAndHash = [](std::vector<uint8_t> &bytes) {
            const uint32_t length = static_cast<uint32_t>(bytes.size());
            for (unsigned i = 0; i < 4; ++i) bytes[16 + i] = static_cast<uint8_t>(length >> (8 * i));
            const auto hash = StateInternal::sha256(bytes.data() + 116, bytes.size() - 116);
            std::copy(hash.begin(), hash.end(), bytes.begin() + 84);
        };
        writeLengthAndHash(legacy);
        cycles(system, 30);
        system.loadState(legacy.data(), legacy.size());
        const auto migrated = system.saveState();
        expect(migrated[8] == current[8] && migrated.size() == current.size() &&
                   migrated[suffix] == 0 && migrated[suffix + 1] == 0 && migrated[suffix + 2] == 0 &&
                   (!cgb || migrated[suffix + 6] == 3),
               "legacy DMG v1/CGB v2 states migrate with their unsuppressed output semantics");
        expect(std::equal(legacy.begin() + 116, legacy.end(), migrated.begin() + 116),
               "migration retains the complete legacy component payload");
        auto previousCurrent = current;
        previousCurrent.resize(previousCurrent.size() - 1);
        previousCurrent[8] = cgb ? 12 : 10;
        writeLengthAndHash(previousCurrent);
        system.loadState(previousCurrent.data(), previousCurrent.size());
        expect(system.saveState() == current,
               "previous current snapshot version migrates across the OAM-fetch state extension");
        current = migrated; // Legacy envelopes resume without unavailable stop history.
        auto invalid = migrated; invalid[suffix + 1] = 2;
        writeLengthAndHash(invalid);
        rejectedUnchanged(system, invalid, invalid.size());
        // Both new booleans, including true values, must commit during restore.
        current[suffix] = 1; current[suffix + 1] = 1;
        writeLengthAndHash(current);
        system.loadState(current.data(), current.size());
        expect(system.saveState() == current, "v4 preserves both first-duty flags exactly");
        auto version3 = current; version3.resize(suffix + 2); version3[8] = 3;
        writeLengthAndHash(version3);
        system.loadState(version3.data(), version3.size());
        expect(system.saveState() == current, "v3 state imports existing pulse flags and an inactive STAT override");
        invalid = current; invalid[suffix + 2] = 5; writeLengthAndHash(invalid);
        rejectedUnchanged(system, invalid, invalid.size());
        auto version4Common = current; version4Common.resize(suffix + 3); version4Common[8] = 4;
        writeLengthAndHash(version4Common);
        system.loadState(version4Common.data(), version4Common.size());
        expect(system.saveState() == current, "both models migrate v4 envelope state");
        for (size_t flag = suffix + 3; flag < suffix + 6; ++flag) {
            invalid = current; invalid[flag] = 2; writeLengthAndHash(invalid);
            rejectedUnchanged(system, invalid, invalid.size());
        }
        auto allRunning = current;
        std::fill(allRunning.begin() + suffix + 3, allRunning.begin() + suffix + 6, 1);
        writeLengthAndHash(allRunning);
        system.loadState(allRunning.data(), allRunning.size());
        expect(system.saveState() == allRunning, "current states commit each envelope-running flag");
        if (model == HardwareModel::Dmg) {
            auto version7 = current; version7.resize(version7.size() - 23); version7[8] = 7;
            writeLengthAndHash(version7);
            system.loadState(version7.data(), version7.size());
            expect(system.saveState() == current,
                   "DMG v7 migrates MBC7 protocol state with an idle EEPROM timer");
            auto pending = current; pending[suffix + 2] = 4; writeLengthAndHash(pending);
            system.loadState(pending.data(), pending.size());
            expect(system.saveState() == pending, "v4 preserves pending one-M-cycle STAT override");
        } else {
            auto version4 = current; version4.resize(suffix + 3); version4[8] = 4;
            writeLengthAndHash(version4);
            system.loadState(version4.data(), version4.size());
            expect(system.saveState() == current, "CGB v4 migrates with unrestricted legacy PPU permissions");
            auto version6 = current; version6.resize(version6.size() - 49); version6[8] = 6;
            writeLengthAndHash(version6);
            system.loadState(version6.data(), version6.size());
            expect(system.saveState() == current, "CGB v6 migrates its envelope flags into v13");
            auto version7 = current; version7.resize(version7.size() - 47); version7[8] = 7;
            writeLengthAndHash(version7);
            system.loadState(version7.data(), version7.size());
            expect(system.saveState() == current, "CGB v7 migrates its boot selections with no active IR warm-up");
            auto version8 = current; version8.resize(version8.size() - 42); version8[8] = 8;
            writeLengthAndHash(version8);
            system.loadState(version8.data(), version8.size());
            expect(system.saveState() == current, "CGB v8 migrates IR state without cartridge protocol progress");
            auto version9 = current; version9.resize(version9.size() - 13); version9[8] = 9;
            writeLengthAndHash(version9);
            system.loadState(version9.data(), version9.size());
            expect(system.saveState() == current, "CGB v9 migrates MBC7 protocol state without EEPROM busy timing");
            for (size_t field = suffix + 7; field < suffix + 9; ++field) {
                invalid = current; invalid[field] = 2; writeLengthAndHash(invalid);
                rejectedUnchanged(system, invalid, invalid.size());
            }
            auto version5 = version4; version5.push_back(3); version5[8] = 5;
            writeLengthAndHash(version5);
            system.loadState(version5.data(), version5.size());
            expect(system.saveState() == current, "CGB v5 preserves its PPU latch while migrating envelope flags");
            for (uint8_t mode : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(4)}) {
                invalid = current; invalid[suffix + 6] = mode; writeLengthAndHash(invalid);
                rejectedUnchanged(system, invalid, invalid.size());
            }
            invalid = current;
            std::fill(invalid.begin() + suffix + 9, invalid.begin() + suffix + 13, 0xff);
            writeLengthAndHash(invalid);
            rejectedUnchanged(system, invalid, invalid.size());
            invalid = current; invalid[suffix + 13] = 2; writeLengthAndHash(invalid);
            rejectedUnchanged(system, invalid, invalid.size());
            invalid = current; invalid[suffix + 2] = 1; writeLengthAndHash(invalid);
            rejectedUnchanged(system, invalid, invalid.size());
            const size_t mbc7Timing = current.size() - 13;
            invalid = current; invalid[mbc7Timing] = 0xFF; writeLengthAndHash(invalid);
            rejectedUnchanged(system, invalid, invalid.size());
            std::fill(invalid.begin() + mbc7Timing + 6,
                      invalid.begin() + mbc7Timing + 10, 0xFF);
            writeLengthAndHash(invalid);
            rejectedUnchanged(system, invalid, invalid.size());
            invalid = current; invalid[mbc7Timing + 11] = 2;
            writeLengthAndHash(invalid);
            rejectedUnchanged(system, invalid, invalid.size());
        }
    }
}

std::vector<uint8_t> makeMode3OamDmaSnapshotRom() {
    auto rom = makeRom();
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) rom[at++] = byte;
    };
    const auto io = [&](uint8_t port, uint8_t value) {
        emit({0x3E, value, 0xE0, port});
    };
    const auto fillBytes = [&](uint16_t address, uint8_t value, uint8_t count) {
        emit({0x21, static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8),
              0x3E, value, 0x06, count});
        const uint16_t loopAddress = static_cast<uint16_t>(0x150 + at);
        emit({0x22, 0x05, 0x20, 0});
        rom[at - 1] = static_cast<uint8_t>(loopAddress - (0x150 + at));
    };
    const auto fillBitplane = [&](uint16_t address, uint8_t value) {
        emit({0x21, static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8),
              0x3E, value, 0x06, 8});
        const uint16_t loopAddress = static_cast<uint16_t>(0x150 + at);
        emit({0x22, 0x23, 0x05, 0x20, 0});
        rom[at - 1] = static_cast<uint8_t>(loopAddress - (0x150 + at));
    };

    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
    emit({0xF3, 0x31, 0xFE, 0xFF}); // Disable interrupts and set HRAM stack.
    io(0x40, 0);                    // LCD off for tile/OAM setup.
    emit({0x3E, 0x0A, 0xEA, 0x00, 0x00}); // Enable MBC5 battery RAM as a valid system image.
    fillBitplane(0x8010, 0xFF);    // Tile 1 low plane makes object color 1.
    fillBitplane(0x8021, 0xFF);    // Tile 2 high plane makes object color 2.

    emit({0x21, 0x00, 0xFE, 0x3E, 0x10, 0x22, // OAM Y = 16.
          0x3E, 0x98, 0x22,                    // OAM X = 152.
          0x3E, 0x01, 0x22,                    // Original tile = 1.
          0x3E, 0x00, 0x77});                   // Attributes = 0.
    fillBytes(0xC000, 0x02, 0xA0); // Every DMA word selects tile 2, attributes 2.
    io(0x47, 0xE4); io(0x48, 0xE4);

    const uint8_t hramDmaRoutine[] = {0x3E, 0xC0, 0xE0, 0x46, 0x18, 0xFE};
    for (size_t i = 0; i < sizeof(hramDmaRoutine); ++i)
        emit({0x3E, hramDmaRoutine[i], 0xE0, static_cast<uint8_t>(0x80 + i)});

    io(0x40, 0x93);
    uint16_t loopAddress = static_cast<uint16_t>(0x150 + at);
    emit({0xF0, 0x44, 0xFE, 0x01, 0x20, 0}); // Wait for visible line 1.
    rom[at - 1] = static_cast<uint8_t>(loopAddress - (0x150 + at));
    loopAddress = static_cast<uint16_t>(0x150 + at);
    emit({0xF0, 0x41, 0xE6, 0x03, 0xFE, 0x03, 0x20, 0}); // Wait for Mode 3.
    rom[at - 1] = static_cast<uint8_t>(loopAddress - (0x150 + at));
    emit({0xCD, 0x80, 0xFF}); // Start DMA from an HRAM routine and remain there.
    return rom;
}

void testMode3OamDmaSnapshotReplay() {
    const auto rom = makeMode3OamDmaSnapshotRom();
    GameBoySystem original(rom), restored(rom);
    unsigned steps = 0;
    for (; steps < 20000 && original.getCpuRegisters().getRefPC() != 0xFF84; ++steps)
        expect(original.stepOneCycle().success,
               "guest reaches the HRAM loop after starting Mode-3 OAM DMA");
    expect(steps < 20000, "guest starts OAM DMA during Mode 3 within the startup bound");
    cycles(original, 10); // Let the DMA start while the far-right OBJ fetch is pending.

    const auto pending = original.saveState();
    expect(pending[8] == 11 && pending[pending.size() - 11] == 0,
           "DMG v11 snapshot retains the selected OAM index for a pending Mode-3 fetch");
    cycles(original, 100);
    const auto expectedFrame = original.getFrameBuffer();
    const auto expectedState = original.saveState();
    expect(expectedFrame[160 + 144] == 2,
           "restored DMA word supplies tile 2 when the selected object is fetched");

    restored.loadState(pending.data(), pending.size());
    cycles(restored, 100);
    expect(restored.getFrameBuffer() == expectedFrame &&
               restored.saveState() == expectedState,
           "in-flight Mode-3 OAM DMA fetch replays identical pixels and complete state");
}

void testStoppedEnvelopeCpuSnapshot() {
    for (auto model : {HardwareModel::Dmg, HardwareModel::CgbE}) {
        auto rom = makeRom(); rom[0x143] = 0x80;
        std::fill(rom.begin() + 0x150, rom.end(), 0);
        size_t at = 0x150;
        const auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) rom[at++] = b; };
        const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
        emit({0xF3, 0x3E, 0x0A, 0xEA, 0, 0});
        io(0x26, 0); io(0x26, 0x80);
        io(0x11, 0x40); io(0x16, 0x40); io(0x13, 0xFF); io(0x18, 0xFF); io(0x22, 8);
        for (auto port : {0x12, 0x17, 0x21}) io(port, 0xE9);
        io(0x14, 0x87); io(0x19, 0x87); io(0x23, 0x80);
        // Sixteen controlled DIV falling edges supply two envelope clocks:
        // volume 14 -> 15, then attempted overflow stops automatic changes.
        io(4, 0); emit({0x06, 16});
        const auto loop = at;
        emit({0xF0, 4, 0xE6, 0x10, 0x28, 0xFA, 0xE0, 4, 0x05, 0x20, 0});
        rom[at - 1] = static_cast<uint8_t>(loop - at);
        emit({0x3E, 1, 0xEA, 0x40, 0xA0, 0x18, 0xFE});
        GameBoySystem original(rom, model), restored(rom, model);
        unsigned steps = 0;
        for (; steps < 50000 && original.getSaveRamData()[0x40] != 1; ++steps)
            expect(original.stepOneCycle().success, "CPU clocks envelope to its terminal state through DIV");
        expect(steps < 50000, "CPU reaches the stopped-envelope snapshot checkpoint");
        const auto saved = original.saveState();
        const auto flags = saved.size() - (model == HardwareModel::CgbE ? 47 : 54);
        expect(saved[flags] == 0 && saved[flags + 1] == 0 && saved[flags + 2] == 0,
               "real CPU execution records all three stopped envelopes in current snapshots");
        restored.loadState(saved.data(), saved.size());
        expect(restored.saveState() == saved, "stopped envelopes restore at the CPU M-cycle boundary");
        for (unsigned n = 0; n < 10000; ++n)
            expect(original.stepOneCycle().success && restored.stepOneCycle().success,
                   "CPU and stopped envelopes continue after restore");
        expect(original.takeAudioSamples() == restored.takeAudioSamples() && original.saveState() == restored.saveState(),
               "stopped-envelope divider phase and analog filter charge replay exactly on both models");
    }
}

void testCpuStatWritePendingSnapshot() {
    auto rom = makeRom();
    std::fill(rom.begin() + 0x150, rom.end(), 0);
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) rom[at++] = b; };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    emit({0xF3, 0x3E, 0x0A, 0xEA, 0, 0});
    io(0x40, 0); io(0x41, 0); io(0x45, 200); io(0x40, 0x91);
    emit({0xF0, 0x44, 0xFE, 1, 0x20, 0xFA});
    emit({0xF0, 0x41, 0xE6, 3, 0x20, 0xFA}); // Visible-line HBlank.
    io(0x0F, 0); emit({0xE0, 0x41}); // A=0; spurious STAT edge with no enabled sources.
    emit({0xF0, 0x0F, 0xE6, 2, 0xEA, 0x30, 0xA0, 0x18, 0xFE});
    GameBoySystem original(rom), restored(rom);
    std::vector<uint8_t> pending;
    for (unsigned n = 0; n < 1000; ++n) {
        expect(original.stepOneCycle().success, "CPU advances to a STAT zero write in HBlank");
        pending = original.saveState();
        if (pending[pending.size() - 56] == 4) break;
    }
    expect(pending[pending.size() - 56] == 4, "snapshot captures actual CPU STAT write with all four override clocks pending");
    restored.loadState(pending.data(), pending.size());
    expect(restored.saveState() == pending, "pending STAT override restores before another CPU bus access");
    for (unsigned n = 0; n < 100; ++n)
        expect(original.stepOneCycle().success && restored.stepOneCycle().success, "CPU continues pending STAT write replay");
    expect(original.getSaveRamData()[0x30] == 2 && restored.getSaveRamData()[0x30] == 2,
           "CPU observes spurious STAT IF after original and restored writes");
    expect(original.takeAudioSamples() == restored.takeAudioSamples() && original.saveState() == restored.saveState(),
           "STAT write transient and interrupt continuation replay exact PCM and complete state");
}

void testMbc7SerialCommandSnapshotReplay() {
    uint16_t waitAddress = 0, busyWaitAddress = 0;
    const auto rom = makeMbc7SerialResumeRom(waitAddress, busyWaitAddress);
    for (const auto model : {HardwareModel::Dmg, HardwareModel::CgbE}) {
        GameBoySystem original(rom, model), restored(rom, model);
        auto *originalSaveRam = original.getSaveRamData();
        auto *restoredSaveRam = restored.getSaveRamData();
        original.setCartridgeAccelerometerData(0x8234, 0x7F90);
        unsigned steps = 0;
        for (; steps < 10000 && original.getCpuRegisters().getRefPC() != waitAddress;
             ++steps)
            expect(original.stepOneCycle().success,
                   "guest CPU reaches the middle of an MBC7 EEPROM command");
        expect(steps < 10000,
               "guest reaches the EEPROM command checkpoint within the startup bound");
        const auto pending = original.saveState();
        expect(pending[8] == (model == HardwareModel::Dmg ? 11 : 13),
               "MBC7 snapshots use the current model-specific format");
        const size_t extraOamIndices = model == HardwareModel::Dmg ? 10 : 0;
        const size_t mbc7Control = pending.size() - 41 - extraOamIndices;
        auto legacyGate = pending;
        if (extraOamIndices)
            legacyGate.resize(legacyGate.size() - extraOamIndices - 1);
        else
            legacyGate.resize(legacyGate.size() - 1);
        legacyGate[8] = model == HardwareModel::Dmg ? 8 : 10;
        legacyGate[mbc7Control] = 1; // The old serialized boolean meant gate enabled.
        const uint32_t legacyLength = static_cast<uint32_t>(legacyGate.size());
        for (unsigned i = 0; i < 4; ++i)
            legacyGate[16 + i] = static_cast<uint8_t>(legacyLength >> (8 * i));
        const auto legacyHash = StateInternal::sha256(
            legacyGate.data() + 116, legacyGate.size() - 116);
        std::copy(legacyHash.begin(), legacyHash.end(), legacyGate.begin() + 84);
        GameBoySystem legacyRestored(rom, model);
        legacyRestored.loadState(legacyGate.data(), legacyGate.size());
        expect(!legacyRestored.isCartridgeRumbleActive() &&
                   legacyRestored.saveState()[legacyRestored.saveState().size() - 41 - extraOamIndices] == 0x40,
               "legacy MBC7 gate state migrates without inventing an active rumble motor");
        restored.loadState(pending.data(), pending.size());
        expect(restored.saveState() == pending,
               "snapshot restores partial MBC7 command, pin phase and accelerometer input");

        original.setButtonState(JoypadButton::A, true);
        restored.setButtonState(JoypadButton::A, true);
        unsigned resumeSteps = 0;
        for (; resumeSteps < 10000 &&
               original.getCpuRegisters().getRefPC() != busyWaitAddress;
             ++resumeSteps) {
            expect(original.stepOneCycle().success && restored.stepOneCycle().success,
                   "both guest CPUs finish the command and enter EEPROM busy time");
        }
        expect(resumeSteps < 10000 &&
                   original.getCpuRegisters().getRefPC() == busyWaitAddress &&
                   original.getSaveRamData()[0x6A] == 0xFF &&
                   original.getSaveRamData()[0x6B] == 0xFF,
               "guest reaches its wait loop while the EEPROM word remains busy and uncommitted");
        const auto busy = original.saveState();
        restored.loadState(busy.data(), busy.size());
        expect(restored.saveState() == busy,
               "snapshot preserves the pending EEPROM operation and remaining busy time");
        original.setButtonState(JoypadButton::A, false);
        restored.setButtonState(JoypadButton::A, false);
        original.setButtonState(JoypadButton::B, true);
        restored.setButtonState(JoypadButton::B, true);
        unsigned busySteps = 0;
        for (; busySteps < 10000 &&
               (original.getSaveRamData()[0x6A] != 0xA5 ||
                original.getSaveRamData()[0x6B] != 0x5A); ++busySteps) {
            expect(original.stepOneCycle().success && restored.stepOneCycle().success,
                   "both guest CPUs advance the restored EEPROM programming cycle");
        }
        expect(busySteps < 10000 && original.getSaveRamData()[0x6A] == 0xA5 &&
                   original.getSaveRamData()[0x6B] == 0x5A,
               "EEPROM contents commit only after the guest-clocked busy interval");
        expect(original.getSaveRamSize() == 0x100 &&
                   original.getSaveRamData() == originalSaveRam &&
                   restored.getSaveRamData() == restoredSaveRam &&
                   original.saveState() == restored.saveState(),
               "MBC7 EEPROM storage identity and complete state remain stable across replay");
    }
}

}
int main() {
    testStoppedEnvelopeCpuSnapshot();
    testHashVectors();testRoundTripsAtCycleBoundaries();testMapperStorage();
    testCpuAtEveryMachineCycle();testRtcAndRamRestore();
    testStopAndBootState();testIllegalOpcodeLockupSnapshotReplay();
    testRejectedSnapshots();
    testLegacySnapshotMigrationAndPulseFlags();
    testMode2OamSnapshotReplay();
    testMode3OamDmaSnapshotReplay();
    testCpuStatWritePendingSnapshot();
    testMbc7SerialCommandSnapshotReplay();
    std::cout << "Save-state tests passed\n";
}
