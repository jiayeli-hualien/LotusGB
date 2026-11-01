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
    const auto extra=dmg.saveState().size();
    expect(extra+32768<bad.size(),"CGB extension includes full banked WRAM");
    bad[extra+32768]=8; // SVBK's raw selector can only contain 0..7.
    const auto hash=StateInternal::sha256(bad.data()+116,bad.size()-116);
    std::copy(hash.begin(),hash.end(),bad.begin()+84);
    try { cgb.loadState(bad.data(),bad.size()); expect(false,"invalid typed CGB bank must fail after checksum validation"); }
    catch(const std::invalid_argument &) {}
    expect(cgb.saveState()==before,"invalid CGB payload leaves live state unchanged");
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
        expect(state[8]==2 && state[12]==2,"CGB states identify version 2/model 2");
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
    testAcid2(); testInvalidCgbBankStateIsRejectedAtomically(); testSpeedDmaAudioInputAndSnapshots();
    std::cout<<"CGB acid2, speed/DMA/audio/input and snapshot tests passed\n";
}
