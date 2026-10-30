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
}
}
int main() {
    testHashVectors();testRoundTripsAtCycleBoundaries();testMapperStorage();
    testCpuAtEveryMachineCycle();testRtcAndRamRestore();
    testStopAndBootState();testRejectedSnapshots();
    std::cout << "Save-state tests passed\n";
}
