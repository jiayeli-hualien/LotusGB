#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "cartridge.hpp"
#include "game_boy_system.hpp"
#include "../../tests/include/cgb_test_rom.hpp"
namespace {
using namespace LOTUSGB;
void expect(bool value,const char *message) {
    if(!value) { std::cerr<<"FAIL: "<<message<<'\n';std::exit(1); }
}
std::vector<uint8_t> makeRom(size_t bytes,uint8_t ram=5,uint8_t type=0x10) {
    std::vector<uint8_t> rom(bytes,0);
    for(size_t bank=0;bank<bytes/0x4000;++bank)
        rom[bank*0x4000]=static_cast<uint8_t>(bank);
    rom[0x143]=0xC0; rom[0x147]=type; rom[0x149]=ram;
    return rom;
}
void testVariantGeometryAndRomBanks() {
    Cartridge classic(makeRom(0x200000,3));
    classic.write(0x2000,0x80);
    expect(classic.read(0x4000)==1,"classic MBC3 retains seven-bit bank masking and bank-zero alias");
    Cartridge expandedRom(makeRom(0x400000,3));
    for(unsigned bank : {0u,1u,0x7Fu,0x80u,0x81u,0xFFu}) {
        expandedRom.write(0x2000,static_cast<uint8_t>(bank));
        expect(expandedRom.read(0x4000)==(bank?bank:1),"MBC30 exposes all eight ROM bank bits, including bank $80");
        expect(expandedRom.read(0)==0,"MBC30 switchable bank never changes fixed ROM bank zero");
    }
    Cartridge expandedRam(makeRom(0x200000));
    expect(expandedRam.getExternalRamSize()==0x10000,"RAM header $05 allocates 64 KiB");
    expandedRam.write(0x2000,0x80);
    expect(expandedRam.read(0x4000)==0,"64-KiB geometry selects MBC30 even with a 2-MiB ROM; absent ROM address line wraps");
    for(uint8_t type : {uint8_t(0x10),uint8_t(0x13)}) {
        bool rejected=false;
        try { Cartridge invalid(makeRom(0x200000,4,type)); } catch(const std::invalid_argument &) { rejected=true; }
        expect(rejected,"128-KiB SRAM remains outside MBC30 capacity");
    }
}
void testEightRamBanksRtcAndBatteryReload() {
    int64_t now=1000000;
    auto rom=makeRom(0x200000);
    Cartridge cart(rom,nullptr,nullptr,[&]{return now;});
    cart.write(0x0000,0x0A);
    for(unsigned bank=0;bank<8;++bank) {
        cart.write(0x4000,static_cast<uint8_t>(bank));
        for(unsigned offset=0;offset<0x2000;++offset)
            cart.write(static_cast<uint16_t>(0xA000+offset),static_cast<uint8_t>(bank*29+offset));
    }
    for(unsigned bank=0;bank<8;++bank) {
        cart.write(0x4000,static_cast<uint8_t>(bank));
        for(unsigned offset=0;offset<0x2000;++offset)
            expect(cart.read(static_cast<uint16_t>(0xA000+offset))==static_cast<uint8_t>(bank*29+offset),"all 65536 SRAM bytes retain independent bank contents");
    }
    auto reg=[&](uint8_t select,uint8_t value){cart.write(0x4000,select);cart.write(0xA000,value);};
    reg(0x0C,0x41); reg(8,58); reg(9,59); reg(10,23); reg(11,255);
    now+=100; cart.write(0x6000,0);cart.write(0x6000,1);
    cart.write(0x4000,8);expect(cart.read(0xA000)==58,"MBC30 RTC stays halted independently of SRAM bank 7");
    reg(0x0C,1); now+=2;cart.write(0x6000,0);cart.write(0x6000,1);
    cart.write(0x4000,8);expect(cart.read(0xA000)==0,"MBC30 RTC ticks after clearing halt");
    cart.write(0x4000,0x0C);expect(cart.read(0xA000)==0x80,"MBC30 RTC preserves day overflow and carry");
    auto ram=std::make_shared<std::vector<uint8_t>>(*cart.getBatteryRamStorage());
    auto rtc=std::make_shared<std::vector<uint8_t>>(*cart.getBatteryRtcStorage());
    Cartridge reloaded(rom,ram,rtc,[&]{return now;});
    reloaded.write(0,0x0A); reloaded.write(0x4000,7);
    expect(reloaded.read(0xA000)==static_cast<uint8_t>(7*29) && reloaded.getExternalRamData()==ram->data(),"fresh cartridge reload retains upper SRAM banks and shared storage pointer");
    reloaded.write(0x4000,0x0C);expect(reloaded.read(0xA000)==0x80,"fresh cartridge reload restores RTC alongside 64-KiB SRAM");
    reloaded.write(0x4000,0x0D);reloaded.write(0xA000,0);
    expect(reloaded.read(0xA000)==0xFF,"invalid RTC/RAM selector does not wrap into SRAM");
}
void testCgbCpuAndHighBankSnapshotReplay() {
    auto rom=Test::makeCgbMbc30ExerciseRom();
    GameBoySystem a(rom,HardwareModel::CgbE), b(rom,HardwareModel::CgbE);
    for(unsigned checkpoint=0;checkpoint<24;++checkpoint) {
        for(unsigned i=0;i<1001;++i)expect(a.stepOneCycle().success,"MBC30 CGB CPU program advances");
        auto state=a.saveState();auto *pointer=b.getSaveRamData();
        b.loadState(state.data(),state.size());
        expect(pointer==b.getSaveRamData(),"MBC30 state restore preserves frontend SRAM pointer");
        for(unsigned i=0;i<509;++i)expect(a.stepOneCycle().success&&b.stepOneCycle().success,"MBC30 restored CPU continues");
        expect(a.takeAudioSamples()==b.takeAudioSamples(),"MBC30 replay preserves PCM");
        expect(a.getCompletedColorFrameBuffer()==b.getCompletedColorFrameBuffer()&&a.saveState()==b.saveState(),"MBC30 ROM bank $80, eight SRAM banks, RTC and color hardware replay exactly");
    }
    expect(a.getSaveRamSize()==0x10000&&a.getSaveRamData()[4]==0x80&&a.getSaveRamData()[7*0x2000]==0x77,"CPU reaches high ROM bank and upper SRAM through public system interface");
}
}
int main() {
    testVariantGeometryAndRomBanks();testEightRamBanksRtcAndBatteryReload();testCgbCpuAndHighBankSnapshotReplay();
    std::cout<<"MBC30 ROM/RAM/RTC, battery reload and CGB snapshot tests passed\n";
}
