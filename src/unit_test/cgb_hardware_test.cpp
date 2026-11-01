#include <cstdlib>
#include <iostream>
#include "cgb_hardware.hpp"
#include "interrupt_registers.hpp"
#include "memory_region.hpp"
#include "dmg_serial.hpp"
#include "timer.hpp"
namespace {
using namespace LOTUSGB;
void expect(bool ok,const char *msg) { if(!ok) { std::cerr<<"FAIL: "<<msg<<'\n'; std::exit(1); } }
void testPalettePorts() {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq,&video,true);
    ppu.write(0xFF68,0xBF); ppu.write(0xFF69,0x12);
    expect(ppu.read(0xFF68)==0xC0,"palette auto-increment wraps 63 to 0 and masks reserved bit");
    ppu.write(0xFF6A,0xBF); ppu.write(0xFF6B,0x34);
    ppu.write(0xFF68,0x3F); ppu.write(0xFF6A,0x3F);
    expect(ppu.read(0xFF69)==0x12 && ppu.read(0xFF6B)==0x34,"BG and OBJ palette storage are independent");
    expect(ppu.read(0xFF68)==0x7F,"palette reads do not advance the index");
    ppu.write(0xFF40,0x91);
    for(unsigned i=0;i<456 && (ppu.read(0xFF41)&3)!=3;++i) ppu.advanceTCycles(1);
    expect((ppu.read(0xFF41)&3)==3,"palette test reaches Mode 3");
    ppu.write(0xFF68,0xBF); ppu.write(0xFF69,0x99);
    ppu.write(0xFF6A,0xBF); ppu.write(0xFF6B,0x99);
    expect(ppu.read(0xFF68)==0xC0 && ppu.read(0xFF6A)==0xC0,"blocked palette writes still increment both indices");
    expect(ppu.read(0xFF69)==0xFF && ppu.read(0xFF6B)==0xFF,"Mode 3 blocks palette reads");
    ppu.write(0xFF40,0); ppu.write(0xFF68,0x3F); ppu.write(0xFF6A,0x3F);
    expect(ppu.read(0xFF69)==0x12 && ppu.read(0xFF6B)==0x34,"blocked palette writes leave bytes unchanged");
}
void testVramDma() {
    InterruptRegisters irq; CgbVideoRam video; DmgPpu ppu(irq,&video,true);
    AddressBus bus; RamRegion work(0xC000,0x2000); bus.map(0xC000,0xDFFF,work);
    CgbVramDma dma(bus,video,ppu);
    for(uint16_t i=0;i<64;++i) work.write(0xC000+i,static_cast<uint8_t>(i+1));
    video.write(0xFF4F,1);
    dma.write(0xFF51,0xC0); dma.write(0xFF52,0x0F);
    dma.write(0xFF53,0xE0); dma.write(0xFF54,0x1F); dma.write(0xFF55,0);
    expect(dma.blocksCpu() && dma.read(0xFF55)==0,"GDMA blocks CPU and reports remaining block count");
    dma.advanceTCycles(31);
    expect(dma.blocksCpu() && video.readBank(1,0x801F)==0,"GDMA takes 32 base T-cycles per 16 bytes");
    dma.advanceTCycles(1);
    expect(!dma.blocksCpu() && dma.read(0xFF55)==0xFF,"GDMA completion releases CPU");
    for(unsigned i=0;i<16;++i)
        expect(video.readBank(1,0x8010+i)==i+1 && video.readBank(0,0x8010+i)==0,"GDMA masks addresses and copies into selected VRAM bank");
    ppu.write(0xFF40,0x91);
    dma.write(0xFF51,0xC0); dma.write(0xFF52,0x10); dma.write(0xFF53,0); dma.write(0xFF54,0x20);
    dma.write(0xFF55,0x82);
    expect(!dma.blocksCpu() && dma.read(0xFF55)==2,"HBlank DMA waits between blocks");
    dma.advanceTCycles(100); expect(video.readBank(1,0x8020)==0,"HBlank DMA cannot run before HBlank entry");
    dma.onHBlank(); expect(dma.blocksCpu(),"HBlank event starts a CPU-blocking block");
    dma.advanceTCycles(32);
    expect(!dma.blocksCpu() && dma.read(0xFF55)==1 && video.readBank(1,0x802F)==32,"HBlank DMA transfers one block then yields");
    dma.write(0xFF55,0);
    expect(dma.read(0xFF55)==0x81,"HBlank cancellation preserves remaining-count readback");
    dma.onHBlank(); dma.advanceTCycles(64);
    expect(video.readBank(1,0x8030)==0,"cancelled HBlank transfer does not resume on later events");
}
void testFastSerialAndDoubleSpeedDividerTap() {
    InterruptRegisters irq;
    DmgSerial serial(irq,true);
    serial.write(0xFF01,0); serial.write(0xFF02,0x83);
    serial.advanceTCycles(127);
    expect(serial.read(0xFF02)&0x80,"CGB fast serial remains busy before the eighth bit");
    serial.advanceTCycles(1);
    expect(!(serial.read(0xFF02)&0x80) && serial.read(0xFF01)==0xFF && (irq.read(0xFF0F)&8),
           "CGB fast serial completes eight incoming-one bits in 128 CPU clocks");
    Timer timer(irq); unsigned edges=0;
    timer.setDividerApuClockCallback([&] { ++edges; });
    timer.setDoubleSpeed(true);
    timer.advanceTCycles(8192);
    expect(edges==0,"double-speed DIV uses bit 13 for the APU frame sequencer");
    timer.advanceTCycles(8192);
    expect(edges==1,"double-speed CPU timer preserves the base-rate APU divider cadence");
}

}
int main() { testPalettePorts(); testVramDma(); testFastSerialAndDoubleSpeedDividerTap(); std::cout<<"CGB palette-port and VRAM-DMA tests passed\n"; }
