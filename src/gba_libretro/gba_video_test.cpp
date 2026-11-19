#include <cassert>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "gba_system.hpp"
#include "gba_boot_rom.hpp"

namespace {

using LOTUSGBA::GbaSystem;

constexpr uint32_t VRAM = 0x06000000;
constexpr uint32_t PALETTE = 0x05000000;
constexpr uint32_t OBJ_PALETTE = 0x05000200;
constexpr uint32_t OAM = 0x07000000;
constexpr uint32_t DISPCNT = 0x04000000;
constexpr uint32_t BG0CNT = 0x04000008;
constexpr uint32_t BG2CNT = 0x0400000C;

std::vector<uint8_t> loopingRom() {
    std::vector<uint8_t> rom(0xC4, 0);
    const uint32_t entryBranch = 0xEA00002E; // Branch from the header to 0x080000C0.
    const uint32_t waitForever = 0xEAFFFFFE;
    for (unsigned byte = 0; byte < 4; ++byte) {
        rom[byte] = static_cast<uint8_t>(entryBranch >> (byte * 8));
        rom[0xC0 + byte] = static_cast<uint8_t>(waitForever >> (byte * 8));
    }
    return rom;
}

std::vector<uint8_t> waitcntRomLoopCounter() {
    std::vector<uint8_t> rom(0xDC, 0);
    const uint32_t entryBranch = 0xEA00002E;
    const uint32_t code[] = {
        0xE59F0010, // LDR r0, [pc, #16] ; EWRAM counter address at 0xD8.
        0xE5901000, // LDR r1, [r0]
        0xE2811001, // ADD r1, r1, #1
        0xE5801000, // STR r1, [r0]
        0xEAFFFFFB, // B back to the loop's LDR.
        0xE1A00000, // NOP
        0x02000000, // EWRAM counter address.
    };
    for (unsigned byte = 0; byte < 4; ++byte)
        rom[byte] = static_cast<uint8_t>(entryBranch >> (byte * 8));
    for (size_t instruction = 0;
         instruction < sizeof(code) / sizeof(code[0]); ++instruction) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + instruction * 4 + byte] = static_cast<uint8_t>(
                code[instruction] >> (byte * 8));
    }
    return rom;
}

std::vector<uint8_t> waitcntRomSramReadLoop() {
    std::vector<uint8_t> rom(0xE0, 0);
    const uint32_t entryBranch = 0xEA00002E;
    const uint32_t code[] = {
        0xE59F0014, // LDR r0, [pc, #20] ; Game Pak RAM address at 0xDC.
        0xE3A01000, // MOV r1, #0
        0xE5D02000, // LDRB r2, [r0]
        0xE2811001, // ADD r1, r1, #1
        0xEAFFFFFC, // B back to the loop's LDRB.
        0xE1A00000, // NOP
        0xE1A00000, // NOP
        0x0E000000, // Game Pak RAM address.
    };
    for (unsigned byte = 0; byte < 4; ++byte)
        rom[byte] = static_cast<uint8_t>(entryBranch >> (byte * 8));
    for (size_t instruction = 0;
         instruction < sizeof(code) / sizeof(code[0]); ++instruction) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + instruction * 4 + byte] = static_cast<uint8_t>(
                code[instruction] >> (byte * 8));
    }
    return rom;
}

std::vector<uint8_t> postBiosStackProbeRom() {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0xF0, 0);
    const uint32_t instructions[] = {
        0xE59F0024, // ldr r0, [pc, #0x24] (probe address)
        0xE1A0100D, // mov r1, sp (System stack)
        0xE5801000, // str r1, [r0]
        0xE321F0D2, // msr cpsr_c, #IRQ mode with IRQ/FIQ disabled
        0xE1A0100D, // mov r1, sp (IRQ stack)
        0xE5801004, // str r1, [r0, #4]
        0xE321F0D3, // msr cpsr_c, #Supervisor mode with IRQ/FIQ disabled
        0xE1A0100D, // mov r1, sp (Supervisor stack)
        0xE5801008, // str r1, [r0, #8]
        0xE321F0DF, // msr cpsr_c, #System mode with IRQ/FIQ disabled
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < sizeof(instructions) / sizeof(instructions[0]); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                instructions[index] >> (byte * 8));
    }
    constexpr uint32_t probeAddress = 0x02000000;
    for (unsigned byte = 0; byte < 4; ++byte)
        rom[0xEC + byte] = static_cast<uint8_t>(probeAddress >> (byte * 8));
    return rom;
}

std::vector<uint8_t> flash1mRom() {
    std::vector<uint8_t> rom = loopingRom();
    constexpr char marker[] = "FLASH1M_V103";
    rom.insert(rom.end(), marker, marker + sizeof(marker) - 1);
    return rom;
}

std::vector<uint8_t> saveMarkerRom(const char *marker) {
    std::vector<uint8_t> rom = loopingRom();
    const size_t length = std::char_traits<char>::length(marker);
    rom.insert(rom.end(), marker, marker + length);
    return rom;
}

void flashUnlockCommand(GbaSystem &system, uint8_t command) {
    constexpr uint32_t flash = 0x0E000000;
    system.write8(flash + 0x5555, 0xAA);
    system.write8(flash + 0x2AAA, 0x55);
    system.write8(flash + 0x5555, command);
}

void dma3Transfer(GbaSystem &system, uint32_t source, uint32_t destination,
                  uint16_t count) {
    constexpr uint32_t dma3 = 0x040000D4;
    system.write32(dma3, source);
    system.write32(dma3 + 4, destination);
    system.write16(dma3 + 8, count);
    system.write16(dma3 + 10, 0x8000); // Immediate 16-bit transfer.
}

std::vector<uint16_t> eepromCommandBits(unsigned command, uint16_t address,
                                        unsigned addressBits,
                                        const std::array<uint8_t, 8> *data) {
    std::vector<uint16_t> bits;
    bits.push_back(1); // Start.
    bits.push_back(static_cast<uint16_t>((command >> 1) & 1));
    bits.push_back(static_cast<uint16_t>(command & 1));
    for (unsigned bit = addressBits; bit > 0; --bit)
        bits.push_back(static_cast<uint16_t>((address >> (bit - 1)) & 1));
    if (data) {
        for (uint8_t byte : *data) {
            for (int bit = 7; bit >= 0; --bit)
                bits.push_back(static_cast<uint16_t>((byte >> bit) & 1));
        }
    }
    return bits;
}

void eepromDmaWrite(GbaSystem &system, uint16_t address,
                    unsigned addressBits, const std::array<uint8_t, 8> &data) {
    const auto bits = eepromCommandBits(2, address, addressBits, &data);
    assert(bits.size() == (addressBits == 6 ? 73u : 81u));
    constexpr uint32_t source = 0x02000000;
    for (size_t index = 0; index < bits.size(); ++index)
        system.write16(source + static_cast<uint32_t>(index * 2), bits[index]);
    dma3Transfer(system, source, 0x0D000000,
                 static_cast<uint16_t>(bits.size()));
}

std::array<uint8_t, 8> eepromDmaRead(GbaSystem &system, uint16_t address,
                                    unsigned addressBits) {
    const auto bits = eepromCommandBits(3, address, addressBits, nullptr);
    assert(bits.size() == (addressBits == 6 ? 9u : 17u));
    constexpr uint32_t commandSource = 0x02000000;
    for (size_t index = 0; index < bits.size(); ++index)
        system.write16(commandSource + static_cast<uint32_t>(index * 2), bits[index]);
    dma3Transfer(system, commandSource, 0x0D000000,
                 static_cast<uint16_t>(bits.size()));

    constexpr uint32_t resultDestination = 0x02000400;
    dma3Transfer(system, 0x0D000000, resultDestination, 68);
    for (unsigned bit = 0; bit < 4; ++bit)
        assert(system.read16(resultDestination + bit * 2) == 0);
    std::array<uint8_t, 8> result{};
    for (unsigned bit = 0; bit < 64; ++bit) {
        const uint16_t value = system.read16(resultDestination + (bit + 4) * 2);
        result[bit / 8] = static_cast<uint8_t>((result[bit / 8] << 1) | value);
    }
    return result;
}

std::vector<uint8_t> registerRamResetRom() {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0xCC, 0);
    const uint32_t instructions[] = {
        0xE3A000FF, // mov r0, #0xFF
        0xEF000001, // swi 0x01 (RegisterRamReset)
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < 3; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                instructions[index] >> (byte * 8));
    }
    return rom;
}

std::vector<uint8_t> byteWriteVideoRom() {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0xEC, 0);
    const uint32_t instructions[] = {
        0xE3A000A5, // mov r0, #0xA5
        0xE59F1014, // ldr r1, =0x06000200
        0xE5C10000, // strb r0, [r1] (BG VRAM)
        0xE59F1010, // ldr r1, =0x05000020
        0xE5C10000, // strb r0, [r1] (palette RAM)
        0xE59F100C, // ldr r1, =0x07000004
        0xE5C10000, // strb r0, [r1] (OAM, ignored)
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < sizeof(instructions) / sizeof(instructions[0]); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                instructions[index] >> (byte * 8));
    }
    const uint32_t addresses[] = {0x06000200, 0x05000020, 0x07000004};
    for (unsigned index = 0; index < sizeof(addresses) / sizeof(addresses[0]); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xE0 + index * 4 + byte] = static_cast<uint8_t>(
                addresses[index] >> (byte * 8));
    }
    return rom;
}

std::vector<uint8_t> midScanlineDisplayDisableRom() {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0xE4, 0);
    const uint32_t instructions[] = {
        0xE59F1018, // ldr r1, [pc, #0x18] (DISPCNT)
        0xE3A00000, // mov r0, #0
        0xE3A02012, // mov r2, #18; account for default Game Pak wait states.
        0xE2522001, // subs r2, r2, #1
        0x1AFFFFFD, // bne back to subs
        0xE5810000, // str r0, [r1] (disable BG0 mid-scanline)
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < sizeof(instructions) / sizeof(instructions[0]); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                instructions[index] >> (byte * 8));
    }
    for (unsigned byte = 0; byte < 4; ++byte)
        rom[0xE0 + byte] = static_cast<uint8_t>(DISPCNT >> (byte * 8));
    return rom;
}

std::vector<uint8_t> branchToCartridgeBios() {
    std::vector<uint8_t> bios(0x4000, 0);
    constexpr uint32_t loadPcFromLiteral = 0xE51FF004; // ldr pc, [pc, #-4]
    constexpr uint32_t cartridgeEntry = 0x08000000;
    for (unsigned byte = 0; byte < 4; ++byte) {
        bios[byte] = static_cast<uint8_t>(loadPcFromLiteral >> (byte * 8));
        bios[4 + byte] = static_cast<uint8_t>(cartridgeEntry >> (byte * 8));
    }
    return bios;
}

std::vector<uint8_t> biosWithIrqVector() {
    std::vector<uint8_t> bios = branchToCartridgeBios();
    const uint32_t vectorInstructions[] = {
        0xE59F0000, // ldr r0, [pc, #0x00]
        0xE590F000, // ldr pc, [r0]
        0x03007FFC, // BIOS user IRQ callback pointer
    };
    for (unsigned index = 0; index < 3; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            bios[0x18 + index * 4 + byte] = static_cast<uint8_t>(
                vectorInstructions[index] >> (byte * 8));
    }
    return bios;
}

std::vector<uint8_t> biosWithSwiReturn() {
    std::vector<uint8_t> bios = branchToCartridgeBios();
    constexpr uint32_t returnFromSwi = 0xE1B0F00E; // movs pc, lr
    for (unsigned byte = 0; byte < 4; ++byte)
        bios[8 + byte] = static_cast<uint8_t>(returnFromSwi >> (byte * 8));
    return bios;
}

std::vector<uint8_t> biosSwiTestRom() {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0xE4, 0);
    const uint32_t instructions[] = {
        0xE321F05F, // msr cpsr_c, #System mode with IRQ enabled
        0xEF000000, // swi 0x00, routed through the BIOS vector.
        0xE3A0005A, // mov r0, #0x5A
        0xE59F100C, // ldr r1, [pc, #0x0C] (marker address)
        0xE5810000, // str r0, [r1]
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < sizeof(instructions) / sizeof(instructions[0]); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                instructions[index] >> (byte * 8));
    }
    constexpr uint32_t markerAddress = 0x02000000;
    for (unsigned byte = 0; byte < 4; ++byte)
        rom[0xE0 + byte] = static_cast<uint8_t>(markerAddress >> (byte * 8));
    return rom;
}

std::vector<uint8_t> biosIrqTestRom() {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0x128, 0);
    const uint32_t program[] = {
        0xE59F0020, // ldr r0, [pc, #0x20] (IRQ handler)
        0xE59F1020, // ldr r1, [pc, #0x20] (IRQ callback slot)
        0xE5810000, // str r0, [r1]
        0xE3A00001, // mov r0, #1
        0xE59F1018, // ldr r1, [pc, #0x18] (IE)
        0xE1C100B0, // strh r0, [r1]
        0xE59F1014, // ldr r1, [pc, #0x14] (IME)
        0xE1C100B0, // strh r0, [r1]
        0xE321F05F, // msr cpsr_c, #System mode with IRQ enabled
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < sizeof(program) / sizeof(program[0]); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                program[index] >> (byte * 8));
    }
    const uint32_t addresses[] = {
        0x08000100, 0x03007FFC, 0x04000200, 0x04000208,
    };
    for (unsigned index = 0; index < sizeof(addresses) / sizeof(addresses[0]); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xE8 + index * 4 + byte] = static_cast<uint8_t>(
                addresses[index] >> (byte * 8));
    }
    const uint32_t handler[] = {
        0xE59F1018, // ldr r1, [pc, #0x18] (IRQ counter)
        0xE5910000, // ldr r0, [r1]
        0xE2800001, // add r0, r0, #1
        0xE5810000, // str r0, [r1]
        0xE3A01001, // mov r1, #1 (acknowledge VBlank)
        0xE59F2008, // ldr r2, [pc, #0x08] (IF)
        0xE1C210B0, // strh r1, [r2]
        0xE25EF004, // subs pc, lr, #4
    };
    for (unsigned index = 0; index < sizeof(handler) / sizeof(handler[0]); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0x100 + index * 4 + byte] = static_cast<uint8_t>(
                handler[index] >> (byte * 8));
    }
    const uint32_t handlerAddresses[] = {0x02000000, 0x04000202};
    for (unsigned index = 0; index < 2; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0x120 + index * 4 + byte] = static_cast<uint8_t>(
                handlerAddresses[index] >> (byte * 8));
    }
    return rom;
}

std::vector<uint8_t> memoryCopyRom(unsigned service, uint32_t control) {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0x124, 0);
    const uint32_t instructions[] = {
        0xE59F0010, // ldr r0, [pc, #0x10]
        0xE59F1010, // ldr r1, [pc, #0x10]
        0xE59F2010, // ldr r2, [pc, #0x10]
        0xEF000000 | service,
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < 5; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                instructions[index] >> (byte * 8));
    }
    const uint32_t literals[] = {0x080000E4, 0x02000000, control};
    for (unsigned index = 0; index < 3; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xD8 + index * 4 + byte] = static_cast<uint8_t>(
                literals[index] >> (byte * 8));
    }
    const uint32_t sourceWords[] = {
        0xAABBCCDD, 0x11223344, 0x55667788, 0x99AABBCC,
        0xDEADBEEF, 0x12345678, 0x87654321, 0x0BADF00D,
        0xCAFEBABE, 0xF00DFACE, 0x13579BDF, 0x2468ACE0,
        0xABCDEF01, 0x10203040, 0x40506070, 0xFEEDC0DE,
    };
    for (unsigned index = 0; index < 16; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xE4 + index * 4 + byte] = static_cast<uint8_t>(
                sourceWords[index] >> (byte * 8));
    }
    return rom;
}

std::vector<uint8_t> dmaTransferRom() {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0xA48, 0);
    for (unsigned index = 0; index < 0x200; ++index) {
        const uint32_t value = 0x10203040u + index;
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0x248 + index * 4 + byte] = static_cast<uint8_t>(
                value >> (byte * 8));
    }
    return rom;
}

std::vector<uint8_t> irqRegisterBankRom() {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0xF4, 0);
    const uint32_t userProgram[] = {
        0xE3A0C011, // mov r12, #0x11
        0xE321F05F, // msr cpsr_c, #System mode with IRQ enabled
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < 3; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                userProgram[index] >> (byte * 8));
    }
    const uint32_t irqHandler[] = {
        0xE3A0C02A, // mov r12, #0x2A
        0xE321F0DF, // msr cpsr_c, #System mode, keep unacknowledged IRQ masked
        0xE1A0000C, // mov r0, r12
        0xE59F100C, // ldr r1, [pc, #0x0C]
        0xE5810000, // str r0, [r1]
        0xE321F0D2, // msr cpsr_c, #IRQ mode, IRQ still masked
        0xE12FFF1E, // bx lr
    };
    for (unsigned index = 0; index < 7; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xD0 + index * 4 + byte] = static_cast<uint8_t>(
                irqHandler[index] >> (byte * 8));
    }
    constexpr uint32_t resultAddress = 0x02000000;
    for (unsigned byte = 0; byte < 4; ++byte)
        rom[0xF0 + byte] = static_cast<uint8_t>(resultAddress >> (byte * 8));
    return rom;
}

std::vector<uint8_t> lz77Rom(unsigned service, uint32_t destination) {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0xF0, 0);
    const uint32_t instructions[] = {
        0xE59F0010, // ldr r0, [pc, #0x10]
        0xE59F1010, // ldr r1, [pc, #0x10]
        0xEF000000 | service,
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < 4; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                instructions[index] >> (byte * 8));
    }
    constexpr uint32_t compressedSource = 0x080000E4;
    const uint32_t literals[] = {compressedSource, destination};
    for (unsigned index = 0; index < 2; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xD8 + index * 4 + byte] = static_cast<uint8_t>(
                literals[index] >> (byte * 8));
    }
    const uint8_t compressed[] = {
        0x10, 0x06, 0x00, 0x00, // LZ77 header: six output bytes.
        0x10, 'A', 'B', 'C', 0x00, 0x02, // Three literals plus a 3-byte backref.
    };
    for (unsigned index = 0; index < sizeof(compressed); ++index)
        rom[0xE4 + index] = compressed[index];
    return rom;
}

std::vector<uint8_t> affineSetRom(unsigned service) {
    std::vector<uint8_t> rom = loopingRom();
    rom.resize(0xF8, 0);
    const uint32_t instructions[] = {
        0xE59F0020, // ldr r0, [pc, #0x20]
        0xE59F1020, // ldr r1, [pc, #0x20]
        0xE59F2020, // ldr r2, [pc, #0x20]
        0xE59F3020, // ldr r3, [pc, #0x20]
        0xEF000000 | service,
        0xEAFFFFFE, // b .
    };
    for (unsigned index = 0; index < 6; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xC0 + index * 4 + byte] = static_cast<uint8_t>(
                instructions[index] >> (byte * 8));
    }
    const uint32_t literals[] = {0x02000000, 0x02000020, 1, 2};
    for (unsigned index = 0; index < 4; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[0xE8 + index * 4 + byte] = static_cast<uint8_t>(
                literals[index] >> (byte * 8));
    }
    return rom;
}

void write4bppTile(GbaSystem &system, uint32_t base, unsigned tile,
                   const uint8_t (&rows)[8][8]) {
    const uint32_t address = base + tile * 32;
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; x += 4) {
            const uint8_t first = static_cast<uint8_t>(
                (rows[y][x] & 0x0F) | ((rows[y][x + 1] & 0x0F) << 4));
            const uint8_t second = static_cast<uint8_t>(
                (rows[y][x + 2] & 0x0F) | ((rows[y][x + 3] & 0x0F) << 4));
            system.write16(address + y * 4 + x / 2,
                           static_cast<uint16_t>(first |
                               (static_cast<uint16_t>(second) << 8)));
        }
    }
}

void writeSolid4bppTile(GbaSystem &system, uint32_t base, unsigned tile,
                        unsigned colorIndex) {
    uint8_t rows[8][8]{};
    for (auto &row : rows)
        for (uint8_t &pixel : row)
            pixel = static_cast<uint8_t>(colorIndex);
    write4bppTile(system, base, tile, rows);
}

void writeSolid8bppTile(GbaSystem &system, uint32_t base, unsigned tile,
                        unsigned colorIndex) {
    const uint32_t address = base + tile * 64;
    const uint16_t pair = static_cast<uint16_t>(colorIndex & 0xFF) |
                          static_cast<uint16_t>((colorIndex & 0xFF) << 8);
    for (unsigned pixel = 0; pixel < 64; pixel += 2)
        system.write16(address + pixel, pair);
}

void setPalette(GbaSystem &system, uint32_t base, unsigned index, uint16_t color) {
    system.write16(base + index * 2, color);
}

uint16_t pixel(const GbaSystem &system, unsigned x, unsigned y) {
    return system.frameBuffer()[y * GbaSystem::SCREEN_WIDTH + x];
}

void testRegisterRamResetSoftwareInterrupt() {
    GbaSystem system(registerRamResetRom());
    system.write8(0x02000000, 0xA1);
    system.write8(0x03000000, 0xB2);
    system.write8(0x03007DFF, 0xC3);
    system.write8(0x03007E00, 0xD4); // The BIOS preserves the top 0x200 bytes.
    system.write8(PALETTE, 0xE5);
    system.write8(VRAM, 0xF6);
    system.write8(OAM, 0x17);
    system.write16(DISPCNT, 0x0400);
    system.write16(0x04000020, 0x1234);
    system.write16(0x04000200, 0x3FFF);
    system.write16(0x04000208, 1);
    system.write16(0x04000088, 0x1234);
    system.write16(0x04000128, 0x1234);

    assert(system.runFrame().success);
    assert(system.read8(0x02000000) == 0);
    assert(system.read8(0x03000000) == 0);
    assert(system.read8(0x03007DFF) == 0);
    assert(system.read8(0x03007E00) == 0xD4);
    assert(system.read8(PALETTE) == 0);
    assert(system.read8(VRAM) == 0);
    assert(system.read8(OAM) == 0);
    assert(system.read16(DISPCNT) == 0x0080);
    assert(system.read16(0x04000020) == 0x0100);
    assert(system.read16(0x04000200) == 0);
    assert(system.read16(0x04000208) == 0);
    assert(system.read16(0x04000088) == 0x0200);
    assert(system.read16(0x04000128) == 0);
}

void testVideoMemoryAccessWidths() {
    GbaSystem system(byteWriteVideoRom());
    assert(system.runFrame().success);
    assert(system.read16(VRAM + 0x0200) == 0xA5A5);
    assert(system.read16(PALETTE + 0x20) == 0xA5A5);
    assert(system.read16(OAM + 4) == 0);

    system.write16(VRAM + 0x0200, 0x1234);
    assert(system.read16(VRAM + 0x0200) == 0x1234);

    system.write8(PALETTE + 0x20, 0x5A);
    assert(system.read16(PALETTE + 0x20) == 0x5A5A);
    system.write16(PALETTE + 0x20, 0xBEEF);
    assert(system.read16(PALETTE + 0x20) == 0xBEEF);

    system.write16(VRAM + 0x10000, 0xCAFE);
    system.write8(VRAM + 0x10000, 0x12);
    assert(system.read16(VRAM + 0x10000) == 0xCAFE);
    system.write16(OAM + 4, 0x9876);
    system.write8(OAM + 4, 0x43);
    assert(system.read16(OAM + 4) == 0x9876);

    system.write16(DISPCNT, 3);
    system.write16(VRAM + 0x14000, 0x1357);
    system.write8(VRAM + 0x14000, 0x24);
    assert(system.read16(VRAM + 0x14000) == 0x1357);
    system.write8(VRAM + 0x13FFE, 0x69);
    assert(system.read16(VRAM + 0x13FFE) == 0x6969);
}

void testOptionalBiosEntry() {
    GbaSystem system(byteWriteVideoRom(), branchToCartridgeBios());
    assert(system.cpu().programCounter() == 0);
    assert(system.cpu().currentMode() == 0x13);
    assert(system.read8(0x04000300) == 0);
    assert(system.read32(0) == 0xE51FF004);
    assert(system.read32(4) == 0x08000000);
    assert(system.runFrame().success);
    assert(system.read16(VRAM + 0x0200) == 0xA5A5);
    assert(system.read16(PALETTE + 0x20) == 0xA5A5);
    assert(system.read16(OAM + 4) == 0);
}

void testSkipBiosStartsWithInitializedCartridgeState() {
    GbaSystem system(postBiosStackProbeRom());
    assert(system.cpu().programCounter() == 0x08000000);
    assert(system.cpu().currentMode() == 0x1F); // System mode.
    assert(!system.cpu().thumbState());
    assert(system.cpu().registerValue(13) == 0x03007F00);
    assert(system.read8(0x04000300) == 1); // POSTFLG.
    assert(system.read16(0x04000020) == 0x0100); // BG2PA.
    assert(system.read16(0x04000022) == 0); // BG2PB.
    assert(system.read16(0x04000024) == 0); // BG2PC.
    assert(system.read16(0x04000026) == 0x0100); // BG2PD.
    assert(system.read16(0x04000030) == 0x0100); // BG3PA.
    assert(system.read16(0x04000032) == 0); // BG3PB.
    assert(system.read16(0x04000034) == 0); // BG3PC.
    assert(system.read16(0x04000036) == 0x0100); // BG3PD.
    assert(system.read32(0x02000000) == 0);
    assert(system.read32(0x03000000) == 0);
    assert(system.read16(OAM) == 0x0200); // Every regular OBJ starts hidden.
    assert(system.read16(OAM + 127 * 8) == 0x0200);

    assert(system.runFrame().success);
    assert(system.read32(0x02000000) == 0x03007F00); // System SP.
    assert(system.read32(0x02000004) == 0x03007FA0); // IRQ SP.
    assert(system.read32(0x02000008) == 0x03007FE0); // Supervisor SP.
    assert(system.cpu().currentMode() == 0x1F);
    assert(system.cpu().programCounter() >= 0x08000000);
}

void testFlash1mIdentificationCommandsBankingAndPersistence() {
    constexpr uint32_t flash = 0x0E000000;
    GbaSystem system(flash1mRom());
    assert(system.getSaveRamSize() == GbaSystem::FLASH1M_SIZE);
    assert(system.getSaveRamData() != nullptr);
    assert(system.read8(flash) == 0xFF);

    flashUnlockCommand(system, 0x90); // Read chip ID.
    assert(system.read8(flash) == 0xC2);
    assert(system.read8(flash + 1) == 0x09);
    assert(system.read16(flash) == 0x09C2);
    system.write8(flash + 0x5555, 0xF0);

    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x1234, 0xA5);
    assert(system.read8(flash + 0x1234) == 0xA5);
    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x1234, 0xFF);
    assert(system.read8(flash + 0x1234) == 0xA5); // Flash only clears bits.
    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x1300, 0xF0); // F0 is valid program data here.
    assert(system.read8(flash + 0x1300) == 0xF0);

    flashUnlockCommand(system, 0xB0); // Select the upper 64 KiB bank.
    system.write8(flash, 1);
    assert(system.read8(flash + 0x1234) == 0xFF);
    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x1234, 0x00);
    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x1F00, 0x00);
    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x2345, 0x00);

    system.write8(flash + 0x5555, 0xAA);
    system.write8(flash + 0x2AAA, 0x55);
    system.write8(flash + 0x5555, 0x80);
    system.write8(flash + 0x5555, 0xAA);
    system.write8(flash + 0x2AAA, 0x55);
    system.write8(flash + 0x1234, 0x30); // Erase one 4 KiB sector.
    assert(system.read8(flash + 0x1234) == 0xFF);
    assert(system.read8(flash + 0x1F00) == 0xFF);
    assert(system.read8(flash + 0x2345) == 0x00);

    flashUnlockCommand(system, 0xB0);
    system.write8(flash, 0);
    assert(system.read8(flash + 0x1234) == 0xA5);

    system.write8(flash + 0x5555, 0xAA);
    system.write8(flash + 0x2AAA, 0x55);
    system.write8(flash + 0x5555, 0x80);
    system.write8(flash + 0x5555, 0xAA);
    system.write8(flash + 0x2AAA, 0x55);
    system.write8(flash + 0x5555, 0x10); // Erase both banks.
    assert(system.read8(flash + 0x1234) == 0xFF);
    flashUnlockCommand(system, 0xB0);
    system.write8(flash, 1);
    assert(system.read8(flash + 0x2345) == 0xFF);

    flashUnlockCommand(system, 0xB0);
    system.write8(flash, 0);
    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x3333, 0x5A);
    const auto storage = system.getSaveRamStorage();
    GbaSystem reloaded(flash1mRom(), {}, storage);
    assert(reloaded.read8(flash + 0x3333) == 0x5A);
    assert(reloaded.getSaveRamData() == storage->data());

    GbaSystem noSaveRom(loopingRom());
    assert(noSaveRom.getSaveRamData() == nullptr);
    assert(noSaveRom.getSaveRamSize() == 0);
    assert(noSaveRom.read8(flash) == 0xFF);
}

void testSramMappingAndPersistence() {
    const auto rom = saveMarkerRom("SRAM_V");
    GbaSystem system(rom);
    assert(system.getSaveRamSize() == GbaSystem::SRAM_SIZE);
    assert(system.read8(0x0E000123) == 0xFF);
    system.write8(0x0E000123, 0x5A);
    assert(system.read8(0x0E008123) == 0x5A); // 32 KiB SRAM mirrors.
    const auto storage = system.getSaveRamStorage();
    GbaSystem reloaded(rom, {}, storage);
    assert(reloaded.read8(0x0E000123) == 0x5A);
    assert(reloaded.getSaveRamData() == storage->data());
}

void testFlash512CommandsAndPersistence() {
    constexpr uint32_t flash = 0x0E000000;
    const auto rom = saveMarkerRom("FLASH_V123");
    GbaSystem system(rom);
    assert(system.getSaveRamSize() == GbaSystem::FLASH_SIZE);
    assert(system.read8(flash) == 0xFF);

    flashUnlockCommand(system, 0x90);
    assert(system.read8(flash) == 0x32); // Panasonic MN63F805 maker ID.
    assert(system.read8(flash + 1) == 0x1B);
    system.write8(flash, 0xF0);

    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x2345, 0xA6);
    assert(system.read8(flash + 0x2345) == 0xA6);
    flashUnlockCommand(system, 0xB0);
    system.write8(flash, 1); // 64 KiB flash ignores the 128 KiB bank command.
    assert(system.read8(flash + 0x2345) == 0xA6);
    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x2350, 0x00);
    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x3345, 0x00);
    system.write8(flash + 0x5555, 0xAA);
    system.write8(flash + 0x2AAA, 0x55);
    system.write8(flash + 0x5555, 0x80);
    system.write8(flash + 0x5555, 0xAA);
    system.write8(flash + 0x2AAA, 0x55);
    system.write8(flash + 0x2345, 0x30); // Erase one 4 KiB sector.
    assert(system.read8(flash + 0x2345) == 0xFF);
    assert(system.read8(flash + 0x2350) == 0xFF);
    assert(system.read8(flash + 0x3345) == 0x00);
    flashUnlockCommand(system, 0xA0);
    system.write8(flash + 0x2345, 0xA6);
    const auto storage = system.getSaveRamStorage();
    GbaSystem reloaded(rom, {}, storage);
    assert(reloaded.read8(flash + 0x2345) == 0xA6);
}

void testEeprom512DmaReadWriteAndPersistence() {
    const auto rom = saveMarkerRom("EEPROM_V");
    GbaSystem system(rom);
    assert(system.getSaveRamSize() == GbaSystem::EEPROM512_SIZE);
    assert(system.read16(0x0D000000) == 1); // Idle serial data line.
    const std::array<uint8_t, 8> expected =
        {{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0}};
    eepromDmaWrite(system, 17, 6, expected);
    assert(system.getSaveRamSize() == GbaSystem::EEPROM512_SIZE);
    assert(std::equal(expected.begin(), expected.end(), system.getSaveRamData() + 17 * 8));
    assert(eepromDmaRead(system, 17, 6) == expected);
    assert(system.read16(0x0D000000) == 1);

    const auto storage = system.getSaveRamStorage();
    system.reset();
    GbaSystem reloaded(rom, {}, storage);
    assert(reloaded.getSaveRamSize() == GbaSystem::EEPROM512_SIZE);
    assert(reloaded.getSaveRamData() == storage->data());
    assert(eepromDmaRead(reloaded, 17, 6) == expected);
}

void testEeprom8kDmaReadWriteAndExpansion() {
    const auto rom = saveMarkerRom("EEPROM_V");
    GbaSystem system(rom);
    uint8_t *publishedSaveData = system.getSaveRamData();
    const std::array<uint8_t, 8> expected =
        {{0xA5, 0x5A, 0xC3, 0x3C, 0x96, 0x69, 0xF0, 0x0F}};
    eepromDmaWrite(system, 1023, 14, expected);
    assert(system.getSaveRamSize() == GbaSystem::EEPROM_SIZE);
    assert(system.getSaveRamData() == publishedSaveData);
    assert(std::equal(expected.begin(), expected.end(),
                      system.getSaveRamData() + GbaSystem::EEPROM_SIZE - 8));
    assert(eepromDmaRead(system, 1023, 14) == expected);
    const auto storage = system.getSaveRamStorage();
    GbaSystem reloaded(rom, {}, storage);
    assert(reloaded.getSaveRamSize() == GbaSystem::EEPROM_SIZE);
    assert(eepromDmaRead(reloaded, 1023, 14) == expected);
}

void testBiosIrqVectorAndExceptionReturn() {
    GbaSystem system(biosIrqTestRom(), biosWithIrqVector());
    system.write16(0x04000004, 1u << 3); // Enable the VBlank IRQ source.
    assert(system.runFrame().success);
    assert(system.read32(0x02000000) != 0);
    assert(system.cpu().currentMode() == 0x1F);
    assert(!system.cpu().thumbState());
}

void testBiosSwiVectorAndExceptionReturn() {
    GbaSystem system(biosSwiTestRom(), biosWithSwiReturn());
    assert(system.runFrame().success);
    assert(system.read32(0x02000000) == 0x5A);
    assert(system.cpu().currentMode() == 0x1F);
}

void testCpuSetAndCpuFastSetSoftwareInterrupts() {
    {
        GbaSystem system(memoryCopyRom(0x0B, (1u << 26) | 4));
        assert(system.runFrame().success);
        assert(system.read32(0x02000000) == 0xAABBCCDD);
        assert(system.read32(0x02000004) == 0x11223344);
        assert(system.read32(0x02000008) == 0x55667788);
        assert(system.read32(0x0200000C) == 0x99AABBCC);
    }
    {
        GbaSystem system(memoryCopyRom(0x0C, (1u << 24) | 9));
        assert(system.runFrame().success);
        for (unsigned index = 0; index < 16; ++index)
            assert(system.read32(0x02000000 + index * 4) == 0xAABBCCDD);
    }
}

void testImmediateAndBlankingDma() {
    {
        GbaSystem system(dmaTransferRom());
        system.write32(0x040000D4, 0x08000248); // DMA3 source
        system.write32(0x040000D8, 0x03002750); // DMA3 destination
        system.write32(0x040000DC, 0x84000200); // Immediate, 32-bit, 512 words
        assert(system.read32(0x03002750) == 0x10203040);
        assert(system.read32(0x03002754) == 0x10203041);
        assert(system.read32(0x03002F4C) == 0x1020323F);
        assert((system.read16(0x040000DE) & 0x8000) == 0);
    }
    {
        GbaSystem system(dmaTransferRom());
        system.write32(0x040000D4, 0x08000248);
        system.write32(0x040000D8, 0x02000000);
        system.write32(0x040000DC, 0x90000004); // VBlank, 4 halfwords
        assert(system.read16(0x02000000) == 0);
        assert(system.runFrame().success);
        assert(system.read16(0x02000000) == 0x3040);
        assert(system.read16(0x02000002) == 0x1020);
        assert(system.read16(0x02000004) == 0x3041);
        assert(system.read16(0x02000006) == 0x1020);
        assert((system.read16(0x040000DE) & 0x8000) == 0);
    }
    {
        GbaSystem system(dmaTransferRom());
        system.write32(0x040000D4, 0x08000248);
        system.write32(0x040000D8, 0x02000000);
        system.write32(0x040000DC, 0xA0000002); // HBlank, 2 halfwords
        assert(system.runFrame().success);
        assert(system.read16(0x02000000) == 0x3040);
        assert(system.read16(0x02000002) == 0x1020);
        assert((system.read16(0x040000DE) & 0x8000) == 0);
    }
    {
        GbaSystem system(dmaTransferRom());
        system.write32(0x040000D4, 0x08000248);
        system.write32(0x040000D8, 0x02000000);
        system.write32(0x040000DC, 0x92000004); // Repeating VBlank DMA.
        assert(system.runFrame().success);
        assert((system.read16(0x040000DE) & 0x8000) != 0);
        assert(system.runFrame().success);
        assert(system.read16(0x02000000) == 0x3040);
        assert(system.read16(0x02000006) == 0x1020);
        assert(system.read16(0x02000008) == 0x3042);
        assert(system.read16(0x0200000A) == 0x1020);
    }
}

void testIrqAndSystemShareR8ThroughR12() {
    GbaSystem system(irqRegisterBankRom());
    system.write32(0x03007FFC, 0x080000D0);
    assert(system.runFrame().success);
    assert(system.read32(0x02000000) == 0);

    system.write16(0x04000200, 1); // Enable VBlank IRQ.
    system.write16(0x04000208, 1);
    system.write16(0x04000004, 1u << 3); // Enable the VBlank IRQ source.
    assert(system.read16(0x04000200) == 1);
    assert(system.read16(0x04000202) == 0);
    assert(system.read16(0x04000208) == 1);
    assert(system.runFrame().success);
    assert(system.read32(0x02000000) == 0x2A);
}

void testDisplayInterruptEnableBits() {
    {
        GbaSystem system(loopingRom());
        assert((system.read16(0x04000004) & (1u << 2)) != 0);
        assert(system.runFrame().success);
        assert((system.read16(0x04000202) & 0x0007) == 0);
        assert((system.read16(0x04000004) & (1u << 2)) != 0);
    }
    {
        GbaSystem system(loopingRom());
        system.write16(0x04000004, (160u << 8) | (1u << 3) |
                                    (1u << 4) | (1u << 5));
        assert(system.runFrame().success);
        assert((system.read16(0x04000202) & 0x0007) == 0x0007);
    }
}

void testHBlankDmaUpdatesFollowingScanlines() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 1, 0x001F); // First row is red.
    writeSolid4bppTile(system, VRAM + 0x4000, 0, 1);
    system.write16(VRAM + 31 * 0x800, 0);
    system.write16(BG0CNT, (1u << 2) | (31u << 8));
    system.write16(DISPCNT, 1u << 8);
    system.write32(0x02000000, 0x7C0003E0); // HBlank DMA: green, then blue.
    system.write32(0x040000D4, 0x02000000); // DMA3 source.
    system.write32(0x040000D8, PALETTE + 2); // Fixed BG palette destination.
    system.write16(0x040000DC, 1); // One halfword per HBlank.
    system.write16(0x040000DE, 0xA240); // Enable, HBlank, repeat, fixed dest.

    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 0x001F);
    assert(pixel(system, 0, 1) == 0x03E0);
    assert(pixel(system, 0, 2) == 0x7C00);
}

void testGbaTimersPrescalersCascadeAndIrq() {
    {
        GbaSystem system(loopingRom());
        system.write16(0x04000100, 0xFFFE);
        system.write16(0x04000102, 0x00C0); // Enable timer 0 and its IRQ.
        assert(system.runFrame().success);
        assert(system.read16(0x04000100) == 0xFFFE);
        assert((system.read16(0x04000202) & (1u << 3)) != 0);
    }
    {
        GbaSystem system(loopingRom());
        system.write16(0x0400010C, 0);
        system.write16(0x0400010E, 0x0081); // Timer 3 at CPU/64.
        assert(system.runFrame().success);
        assert(system.read16(0x0400010C) == GbaSystem::CYCLES_PER_FRAME / 64);
    }
    {
        GbaSystem system(loopingRom());
        system.write16(0x04000100, 0xFFFF);
        system.write16(0x04000104, 0xFFFC);
        system.write16(0x04000106, 0x00C4); // Timer 1 cascades from timer 0.
        system.write16(0x04000102, 0x00C0);
        assert(system.runFrame().success);
        assert(system.read16(0x04000104) == 0xFFFC);
        assert((system.read16(0x04000202) & ((1u << 3) | (1u << 4))) ==
               ((1u << 3) | (1u << 4)));
    }
}

void testWaitcntRegisterMasksAndReset() {
    GbaSystem system(loopingRom());
    constexpr uint32_t WAITCNT = 0x04000204;
    assert(system.read16(WAITCNT) == 0);

    system.write16(WAITCNT, 0xFFFF);
    assert(system.read16(WAITCNT) == 0x5FFF);
    assert(system.read8(WAITCNT + 1) == 0x5F);

    system.write8(WAITCNT + 1, 0xFF);
    assert(system.read16(WAITCNT) == 0x5FFF);
    system.write8(WAITCNT + 1, 0x20); // Reserved bit 13 is read as zero.
    assert(system.read16(WAITCNT) == 0x00FF);

    system.reset();
    assert(system.read16(WAITCNT) == 0);
}

uint32_t runRomLoopCounter(uint16_t waitcnt) {
    GbaSystem system(waitcntRomLoopCounter());
    system.write16(0x04000204, waitcnt);
    system.write32(0x02000000, 0);
    assert(system.runFrame().success);
    return system.read32(0x02000000);
}

uint32_t runSramReadLoop(uint16_t waitcnt) {
    GbaSystem system(waitcntRomSramReadLoop());
    system.write16(0x04000204, waitcnt);
    assert(system.runFrame().success);
    return system.cpu().registerValue(1);
}

uint32_t measureGamePakAccessCycles(uint16_t waitcnt, uint32_t address,
                                   unsigned width) {
    GbaSystem system(loopingRom());
    system.write16(0x04000204, waitcnt);
    system.beginCpuStep();
    if (width == 1)
        (void)system.read8(address);
    else if (width == 2)
        (void)system.read16(address);
    else {
        assert(width == 4);
        (void)system.read32(address);
    }
    return system.endCpuStep(1);
}

uint32_t measureSequentialRomAccess(uint16_t waitcnt, uint32_t firstAddress,
                                    uint32_t secondAddress) {
    GbaSystem system(loopingRom());
    system.write16(0x04000204, waitcnt);
    system.beginCpuStep();
    (void)system.read16(firstAddress);
    (void)system.endCpuStep(1);
    system.beginCpuStep();
    (void)system.read16(secondAddress);
    return system.endCpuStep(1);
}

void testWaitcntBusCycleTotals() {
    // The WAITCNT tables specify wait clocks. Each transfer also spends its
    // ordinary Game Pak bus cycle, so the first default WS0 access is 5 clocks.
    assert(measureGamePakAccessCycles(0x0000, 0x08000000, 2) == 5);
    assert(measureSequentialRomAccess(0x0000, 0x08000000, 0x08000002) == 3);
    assert(measureSequentialRomAccess(0x0010, 0x08000000, 0x08000002) == 2);
    assert(measureGamePakAccessCycles(0x0008, 0x08000000, 2) == 3);
    assert(measureGamePakAccessCycles(0x000C, 0x08000000, 2) == 9);

    // A word transfer occupies the 16-bit Game Pak bus twice: N then S.
    assert(measureGamePakAccessCycles(0x0000, 0x08000000, 4) == 8);
    assert(measureGamePakAccessCycles(0x0000, 0x0A000000, 2) == 5);
    assert(measureGamePakAccessCycles(0x0060, 0x0A000000, 2) == 9);
    assert(measureSequentialRomAccess(0x0080, 0x0A000000,
                                      0x0A000002) == 2);
    assert(measureGamePakAccessCycles(0x0000, 0x0C000000, 2) == 5);
    assert(measureGamePakAccessCycles(0x0200, 0x0C000000, 2) == 3);
    assert(measureSequentialRomAccess(0x0400, 0x0C000000,
                                      0x0C000002) == 2);

    // Game Pak RAM is 8-bit wide; the default selector is 4 waits + 1 bus
    // cycle, and the selector value 2 is 2 waits + 1 bus cycle.
    assert(measureGamePakAccessCycles(0x0000, 0x0E000000, 1) == 5);
    assert(measureGamePakAccessCycles(0x0002, 0x0E000000, 1) == 3);
}

void testWaitcntGamePakRomAndRamWaitCycles() {
    // WAITCNT WS0 first-access encodings: 000=4, 010=2, 011=8 wait clocks.
    const uint32_t defaultRomSpeed = runRomLoopCounter(0x0000);
    const uint32_t fasterFirstAccess = runRomLoopCounter(0x0008);
    const uint32_t slowerFirstAccess = runRomLoopCounter(0x000C);
    assert(fasterFirstAccess > defaultRomSpeed);
    assert(defaultRomSpeed > slowerFirstAccess);

    // WS0 second-access bit 4 selects two or one wait clocks for sequential fetches.
    const uint32_t fasterSequentialAccess = runRomLoopCounter(0x0010);
    assert(fasterSequentialAccess > defaultRomSpeed);

    // The low two bits control Game Pak RAM access speed independently.
    const uint16_t fastRamWaitcnt = 0x0018 | 0x0002;
    const uint16_t defaultRamWaitcnt = 0x0018;
    const uint16_t slowRamWaitcnt = 0x0018 | 0x0003;
    const uint32_t fastRamCount = runSramReadLoop(fastRamWaitcnt);
    const uint32_t defaultRamCount = runSramReadLoop(defaultRamWaitcnt);
    const uint32_t slowRamCount = runSramReadLoop(slowRamWaitcnt);
    assert(fastRamCount > defaultRamCount);
    assert(defaultRamCount > slowRamCount);
}

void testGbaPsgProducesStereoAudio() {
    GbaSystem system(loopingRom());
    system.write8(0x04000084, 0x80); // Master sound enable.
    system.write16(0x04000080, 0x7777); // Full PSG volume, both sides.
    system.write16(0x04000062, 0xF080); // Square 1 duty and envelope.
    system.write16(0x04000064, 0x86D6); // Trigger square 1.

    assert(system.runFrame().success);
    const std::vector<int16_t> samples = system.takeAudioSamples();
    assert(samples.size() >= 800 * 2 && samples.size() <= 804 * 2);
    bool leftHasSignal = false;
    bool rightHasSignal = false;
    for (size_t index = 0; index + 1 < samples.size(); index += 2) {
        leftHasSignal |= samples[index] != 0;
        rightHasSignal |= samples[index + 1] != 0;
    }
    assert(leftHasSignal && rightHasSignal);
    assert(system.takeAudioSamples().empty());
}

void testGbaDirectSoundFifoUsesTimerAndSpecialDma() {
    GbaSystem system(loopingRom());
    system.write8(0x04000084, 0x80); // Master sound enable.
    system.write16(0x04000082, 0x0B00); // FIFO A, both sides, reset FIFO.
    system.write16(0x04000082, 0x0300); // Release FIFO reset.
    for (unsigned word = 0; word < 64; ++word)
        system.write32(0x02000000 + word * 4, 0x20202020);
    system.write32(0x040000BC, 0x02000000); // DMA1 source.
    system.write32(0x040000C0, 0x040000A0); // FIFO A destination.
    system.write16(0x040000C4, 1); // Sound DMA always transfers four words.
    system.write16(0x040000C6, 0xB640); // Enable, FIFO timing, repeat, 32-bit.
    system.write16(0x04000100, 0xF000);
    system.write16(0x04000102, 0x0080); // Timer 0 clocks FIFO A.

    assert(system.runFrame().success);
    const std::vector<int16_t> samples = system.takeAudioSamples();
    bool leftHasSignal = false;
    bool rightHasSignal = false;
    for (size_t index = 0; index + 1 < samples.size(); index += 2) {
        leftHasSignal |= samples[index] != 0;
        rightHasSignal |= samples[index + 1] != 0;
    }
    assert(leftHasSignal && rightHasSignal);
}

void testLz77SoftwareInterrupts() {
    {
        GbaSystem system(lz77Rom(0x12, VRAM));
        assert(system.runFrame().success);
        const char expected[] = "ABCABC";
        for (unsigned index = 0; index < sizeof(expected) - 1; ++index)
            assert(system.read8(VRAM + index) ==
                   static_cast<uint8_t>(expected[index]));
    }
    {
        GbaSystem system(lz77Rom(0x11, 0x02000000));
        assert(system.runFrame().success);
        const char expected[] = "ABCABC";
        for (unsigned index = 0; index < sizeof(expected) - 1; ++index)
            assert(system.read8(0x02000000 + index) ==
                   static_cast<uint8_t>(expected[index]));
    }
}

void testAffineSetSoftwareInterrupts() {
    {
        GbaSystem system(affineSetRom(0x0F));
        system.write16(0x02000000, 0x0200); // X scale 2.0
        system.write16(0x02000002, 0x0100); // Y scale 1.0
        system.write16(0x02000004, 0); // Rotation
        assert(system.runFrame().success);
        assert(system.read16(0x02000020) == 0x0200);
        assert(system.read16(0x02000022) == 0);
        assert(system.read16(0x02000024) == 0);
        assert(system.read16(0x02000026) == 0x0100);
    }
    {
        GbaSystem system(affineSetRom(0x0E));
        system.write32(0x02000000, 0x00000300); // Origin X = 3.0
        system.write32(0x02000004, 0x00000400); // Origin Y = 4.0
        system.write16(0x02000008, 10); // Center X
        system.write16(0x0200000A, 20); // Center Y
        system.write16(0x0200000C, 0x0100); // X scale = 1.0
        system.write16(0x0200000E, 0x0100); // Y scale = 1.0
        system.write16(0x02000010, 0); // Rotation
        assert(system.runFrame().success);
        assert(system.read16(0x02000020) == 0x0100);
        assert(system.read16(0x02000022) == 0);
        assert(system.read16(0x02000024) == 0);
        assert(system.read16(0x02000026) == 0x0100);
        assert(system.read32(0x02000028) == 0xFFFFF900);
        assert(system.read32(0x0200002C) == 0xFFFFF000);
    }
}

void testMode0TextBackgroundPixelsAndScroll() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F); // Red backdrop.
    setPalette(system, PALETTE, 2, 0x7C00);
    setPalette(system, PALETTE, 17, 0x03E0);
    setPalette(system, PALETTE, 18, 0x7C00);
    setPalette(system, PALETTE, 24, 0x4210);
    setPalette(system, PALETTE, 25, 0x2529);

    uint8_t rows[8][8]{};
    const uint8_t firstRow[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (unsigned x = 0; x < 8; ++x)
        rows[0][x] = firstRow[x];
    for (unsigned x = 0; x < 8; ++x) {
        rows[1][x] = 9;
        rows[7][x] = 2;
    }
    write4bppTile(system, VRAM, 1, rows);

    constexpr uint32_t map = VRAM + 31 * 0x800;
    system.write16(map, 1 | (1u << 10) | (1u << 12)); // H-flip, palette 1.
    system.write16(map + 64, 1 | (1u << 11)); // V-flip on the next map row.
    system.write16(BG0CNT, (31u << 8));
    system.write16(DISPCNT, 1u << 8); // Mode 0, BG0 enabled.

    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 0x4210); // Nibble order, H-flip, palette bank.
    assert(pixel(system, 7, 0) == 0x03E0);
    assert(pixel(system, 8, 0) == 0x001F); // Color index zero is transparent.
    assert(pixel(system, 0, 1) == 0x2529);
    assert(pixel(system, 0, 8) == 0x7C00); // V-flip.

    system.write16(map + 2, 1 | (1u << 12)); // Unflipped tile in palette 1.
    system.write16(0x04000010, 8); // BG0HOFS.
    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 0x03E0);
    assert(pixel(system, 7, 0) == 0x4210);
}

void testBackgroundMosaic() {
    GbaSystem system(loopingRom());
    uint8_t rows[8][8]{};
    const uint8_t firstRow[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t thirdRow[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    for (unsigned x = 0; x < 8; ++x) {
        rows[0][x] = firstRow[x];
        rows[1][x] = static_cast<uint8_t>(9 + x);
        rows[2][x] = thirdRow[x];
    }
    for (unsigned index = 1; index < 16; ++index)
        setPalette(system, PALETTE, index, static_cast<uint16_t>(index));

    write4bppTile(system, VRAM + 0x4000, 1, rows);
    constexpr uint32_t screenBlock = VRAM + 31 * 0x800;
    system.write16(screenBlock, 1);
    system.write16(BG0CNT, (1u << 2) | (31u << 8) | (1u << 6));
    system.write16(0x0400004C, 0x0011); // Two-by-two BG mosaic pixels.
    system.write16(DISPCNT, 1u << 8);

    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 1);
    assert(pixel(system, 1, 0) == 1);
    assert(pixel(system, 2, 0) == 3);
    assert(pixel(system, 3, 0) == 3);
    assert(pixel(system, 0, 1) == 1); // The second scanline reuses row zero.
    assert(pixel(system, 0, 2) == 8);
    assert(pixel(system, 1, 3) == 8);
    assert(pixel(system, 2, 2) == 6);
}

void testTextBackgroundSizesAndEightBitTiles() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F);
    setPalette(system, PALETTE, 9, 0x7C00);
    writeSolid8bppTile(system, VRAM + 0x4000, 1, 9);

    // A 512x256 8-bit BG uses the second screen block for tile column 32.
    constexpr uint32_t screenBlock = VRAM + 20 * 0x800;
    system.write16(screenBlock + 0x800, 1);
    system.write16(BG0CNT, (1u << 2) | (1u << 7) | (20u << 8) | (1u << 14));
    system.write16(DISPCNT, 1u << 8);
    system.write16(0x04000010, 256);

    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 0x7C00);
    assert(pixel(system, 8, 0) == 0x001F);
}

void testObjectsPrioritiesShapesAndMapping() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F);
    setPalette(system, OBJ_PALETTE, 1, 0x03E0);
    setPalette(system, OBJ_PALETTE, 2, 0x7C00);
    setPalette(system, OBJ_PALETTE, 3, 0x4210);
    writeSolid4bppTile(system, VRAM + 0x10000, 0, 1);
    writeSolid4bppTile(system, VRAM + 0x10000, 1, 2);
    writeSolid4bppTile(system, VRAM + 0x10000, 2, 3);
    writeSolid4bppTile(system, VRAM + 0x10000, 32, 2);

    // Object 0 is a 16x16 sprite in 1D mapping. Object 1 overlaps it.
    system.write16(OAM, 4);
    system.write16(OAM + 2, 5 | (1u << 14));
    system.write16(OAM + 4, 0 | (1u << 10));
    system.write16(OAM + 8, 4);
    system.write16(OAM + 10, 5 | (1u << 14));
    system.write16(OAM + 12, 1 | (1u << 10));
    system.write16(DISPCNT, (1u << 6) | (1u << 12)); // OBJ, 1D mapping.

    assert(system.runFrame().success);
    assert(pixel(system, 5, 5) == 0x03E0); // Lower OAM index wins a priority tie.
    assert(pixel(system, 13, 5) == 0x7C00); // Second 1D tile follows the first.
    assert(pixel(system, 5, 13) == 0x4210); // Third tile is on the next 1D row.

    system.write16(OAM + 4, 1u << 10); // OBJ0 priority 1.
    system.write16(OAM + 12, 1); // OBJ1 priority 0.
    assert(system.runFrame().success);
    assert(pixel(system, 5, 5) == 0x7C00); // Higher OBJ priority wins.
    system.write16(OAM + 4, 0);
    system.write16(OAM + 12, 1 | (1u << 10));

    // The same 16x16 object uses a 32-tile row stride in 2D mapping.
    system.write16(DISPCNT, 1u << 12);
    assert(system.runFrame().success);
    assert(pixel(system, 5, 13) == 0x7C00);

    // A background wins when its priority number is lower than the OBJ's.
    system.write16(BG0CNT, 31u << 8);
    writeSolid4bppTile(system, VRAM, 1, 1);
    setPalette(system, PALETTE, 1, 0x2529);
    system.write16(VRAM + 31 * 0x800, 1);
    system.write16(OAM + 4, 0 | (2u << 10));
    system.write16(DISPCNT, (1u << 8) | (1u << 12));
    assert(system.runFrame().success);
    assert(pixel(system, 5, 5) == 0x2529);
}

void testObjectVerticalCoordinateWrapsAtScreenHeight() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F);
    setPalette(system, OBJ_PALETTE, 1, 0x03E0);
    writeSolid4bppTile(system, VRAM + 0x10000, 0, 1);

    // Y values 128..159 are visible; wrapping starts at the 160-pixel
    // screen boundary, not at the midpoint of the 8-bit OAM coordinate.
    system.write16(OAM, 128);
    system.write16(OAM + 2, 0);
    system.write16(OAM + 4, 0);
    system.write16(OAM + 8, 159);
    system.write16(OAM + 10, 16);
    system.write16(OAM + 12, 0);
    system.write16(OAM + 16, 160);
    system.write16(OAM + 18, 32);
    system.write16(OAM + 20, 0);
    system.write16(OAM + 24, 255);
    system.write16(OAM + 26, 48);
    system.write16(OAM + 28, 0);
    system.write16(DISPCNT, (1u << 6) | (1u << 12));

    assert(system.runFrame().success);
    assert(pixel(system, 0, 127) == 0x001F);
    assert(pixel(system, 0, 128) == 0x03E0);
    assert(pixel(system, 16, 158) == 0x001F);
    assert(pixel(system, 16, 159) == 0x03E0);
    assert(pixel(system, 32, 0) == 0x001F); // Y=160 wraps above the screen.
    assert(pixel(system, 48, 0) == 0x03E0); // Y=255 wraps to -1.
}

void testRegularObjectFlipAndShapeSizeCombinations() {
    constexpr unsigned dimensions[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    };

    for (unsigned shape = 0; shape < 3; ++shape) {
        for (unsigned size = 0; size < 4; ++size) {
            GbaSystem system(loopingRom());
            setPalette(system, PALETTE, 0, 0x001F);
            setPalette(system, OBJ_PALETTE, 1, 0x03E0);
            for (unsigned tile = 0; tile < 64; ++tile)
                writeSolid4bppTile(system, VRAM + 0x10000, tile, 1);

            constexpr unsigned x = 8;
            constexpr unsigned y = 8;
            const unsigned width = dimensions[shape][size][0];
            const unsigned height = dimensions[shape][size][1];
            system.write16(OAM, static_cast<uint16_t>(y | (shape << 14)));
            system.write16(OAM + 2,
                           static_cast<uint16_t>(x | (size << 14)));
            system.write16(OAM + 4, 0);
            system.write16(DISPCNT, (1u << 6) | (1u << 12));

            assert(system.runFrame().success);
            assert(pixel(system, x, y) == 0x03E0);
            assert(pixel(system, x + width - 1, y + height - 1) == 0x03E0);
            if (width < GbaSystem::SCREEN_WIDTH)
                assert(pixel(system, x + width, y) == 0x001F);
            assert(pixel(system, x, y + height) == 0x001F);
        }
    }

    const uint8_t tileRows[8][8] = {
        {1, 2, 3, 4, 5, 6, 7, 8},
        {2, 3, 4, 5, 6, 7, 8, 9},
        {3, 4, 5, 6, 7, 8, 9, 10},
        {4, 5, 6, 7, 8, 9, 10, 11},
        {5, 6, 7, 8, 9, 10, 11, 12},
        {6, 7, 8, 9, 10, 11, 12, 13},
        {7, 8, 9, 10, 11, 12, 13, 14},
        {8, 9, 10, 11, 12, 13, 14, 15},
    };
    struct FlipCase {
        uint16_t flags;
        uint16_t topLeft;
        uint16_t topRight;
        uint16_t bottomLeft;
    };
    constexpr FlipCase flips[] = {
        {0, 1, 8, 8},
        {1u << 12, 8, 1, 15},
        {1u << 13, 8, 15, 1},
        {(1u << 12) | (1u << 13), 15, 8, 8},
    };
    for (const FlipCase &flip : flips) {
        GbaSystem system(loopingRom());
        setPalette(system, PALETTE, 0, 0x001F);
        for (unsigned color = 1; color <= 15; ++color)
            setPalette(system, OBJ_PALETTE, color,
                       static_cast<uint16_t>(color));
        write4bppTile(system, VRAM + 0x10000, 0, tileRows);
        system.write16(OAM, 8);
        system.write16(OAM + 2, static_cast<uint16_t>(8 | flip.flags));
        system.write16(OAM + 4, 0);
        system.write16(DISPCNT, (1u << 6) | (1u << 12));

        assert(system.runFrame().success);
        assert(pixel(system, 8, 8) == flip.topLeft);
        assert(pixel(system, 15, 8) == flip.topRight);
        assert(pixel(system, 8, 15) == flip.bottomLeft);
    }
}

void testEightBitObjectsInOneDimensionalAndTwoDimensionalMapping() {
    {
        GbaSystem system(loopingRom());
        setPalette(system, PALETTE, 0, 0x001F);
        setPalette(system, OBJ_PALETTE, 1, 0x03E0);
        setPalette(system, OBJ_PALETTE, 2, 0x7C00);
        setPalette(system, OBJ_PALETTE, 3, 0x4210);
        setPalette(system, OBJ_PALETTE, 4, 0x7FE0);
        writeSolid8bppTile(system, VRAM + 0x10000, 0, 1);
        writeSolid8bppTile(system, VRAM + 0x10000, 1, 2);
        writeSolid8bppTile(system, VRAM + 0x10000, 2, 3);
        writeSolid8bppTile(system, VRAM + 0x10000, 3, 4);
        // The low tile-number bit is ignored for 8bpp objects.
        system.write16(OAM, 4 | (1u << 13));
        system.write16(OAM + 2, 5 | (1u << 14));
        system.write16(OAM + 4, 1);
        // Make the top-left source pixel transparent; adjacent 8bpp pixels
        // still use independent palette indices.
        system.write16(VRAM + 0x10000, 0x0100);
        system.write16(DISPCNT, (1u << 6) | (1u << 12));

        assert(system.runFrame().success);
        assert(pixel(system, 5, 4) == 0x001F);
        assert(pixel(system, 6, 4) == 0x03E0);
        assert(pixel(system, 13, 5) == 0x7C00);
        assert(pixel(system, 5, 13) == 0x4210);
        assert(pixel(system, 13, 13) == 0x7FE0);
    }

    {
        GbaSystem system(loopingRom());
        setPalette(system, PALETTE, 0, 0x001F);
        setPalette(system, OBJ_PALETTE, 5, 0x03E0);
        setPalette(system, OBJ_PALETTE, 6, 0x7C00);
        setPalette(system, OBJ_PALETTE, 7, 0x4210);
        setPalette(system, OBJ_PALETTE, 8, 0x7FE0);
        writeSolid8bppTile(system, VRAM + 0x10000, 0, 5);
        writeSolid8bppTile(system, VRAM + 0x10000, 1, 6);
        writeSolid8bppTile(system, VRAM + 0x10000, 16, 7);
        writeSolid8bppTile(system, VRAM + 0x10000, 17, 8);
        system.write16(OAM, 4 | (1u << 13));
        system.write16(OAM + 2, 5 | (1u << 14));
        system.write16(OAM + 4, 0);
        system.write16(DISPCNT, 1u << 12); // OBJ, 2D mapping.

        assert(system.runFrame().success);
        assert(pixel(system, 5, 5) == 0x03E0);
        assert(pixel(system, 13, 5) == 0x7C00);
        assert(pixel(system, 5, 13) == 0x4210);
        assert(pixel(system, 13, 13) == 0x7FE0);
    }
}

void testEightBitObjectsUseBitmapModeTileBase() {
    GbaSystem system(loopingRom());
    setPalette(system, OBJ_PALETTE, 9, 0x03E0);
    writeSolid8bppTile(system, VRAM + 0x14000, 0, 9);
    system.write16(VRAM + (32 * GbaSystem::SCREEN_WIDTH + 32) * 2,
                   0x7C00);
    system.write16(OAM, 32 | (1u << 13));
    system.write16(OAM + 2, 32);
    system.write16(OAM + 4, 510);
    system.write16(DISPCNT, 3u | (1u << 10) | (1u << 12));

    assert(system.runFrame().success);
    assert(pixel(system, 32, 32) == 0x7C00); // Tile numbers below 512 are reserved.

    system.write16(OAM + 4, 512);
    assert(system.runFrame().success);
    assert(pixel(system, 32, 32) == 0x03E0);
}

void testAffineEightBitObject() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F);
    setPalette(system, OBJ_PALETTE, 9, 0x03E0);
    writeSolid8bppTile(system, VRAM + 0x10000, 0, 9);

    // Affine matrix 0 is interleaved through the high halves of OAM entries.
    system.write16(OAM + 6, 0x0100);
    system.write16(OAM + 14, 0);
    system.write16(OAM + 22, 0);
    system.write16(OAM + 30, 0x0100);
    system.write16(OAM, 32 | (1u << 8) | (1u << 13));
    system.write16(OAM + 2, 32);
    system.write16(OAM + 4, 0);
    system.write16(DISPCNT, (1u << 6) | (1u << 12));

    assert(system.runFrame().success);
    assert(pixel(system, 32, 32) == 0x03E0);
    assert(pixel(system, 39, 39) == 0x03E0);
    assert(pixel(system, 40, 39) == 0x001F);
}

void testAffineObjectScaleAndDoubleSizeBounds() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F);
    uint8_t rows[8][8]{};
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            rows[y][x] = static_cast<uint8_t>(x + 1);
            setPalette(system, OBJ_PALETTE, x + 1,
                       static_cast<uint16_t>(x + 1));
        }
    }
    write4bppTile(system, VRAM + 0x10000, 0, rows);

    // Screen-to-source PA=0.5 doubles the horizontal size. The double-size
    // flag expands both bounds to 16x16, centering the unchanged vertical
    // source in the taller field.
    system.write16(OAM + 6, 0x0080);
    system.write16(OAM + 14, 0);
    system.write16(OAM + 22, 0);
    system.write16(OAM + 30, 0x0100);
    system.write16(OAM, 32 | (1u << 8) | (1u << 9));
    system.write16(OAM + 2, 32);
    system.write16(OAM + 4, 0);
    system.write16(DISPCNT, (1u << 6) | (1u << 12));

    assert(system.runFrame().success);
    assert(pixel(system, 32, 35) == 0x001F);
    assert(pixel(system, 32, 36) == 1);
    assert(pixel(system, 33, 36) == 1);
    assert(pixel(system, 34, 36) == 2);
    assert(pixel(system, 46, 43) == 8);
    assert(pixel(system, 47, 43) == 8);
    assert(pixel(system, 48, 43) == 0x001F);
    assert(pixel(system, 32, 44) == 0x001F);
}

void testObjectMosaic() {
    GbaSystem system(loopingRom());
    uint8_t rows[8][8]{};
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            rows[y][x] = static_cast<uint8_t>(1 + (x + y * 3) % 15);
    for (unsigned index = 1; index < 16; ++index)
        setPalette(system, OBJ_PALETTE, index, static_cast<uint16_t>(index));
    write4bppTile(system, VRAM + 0x10000, 0, rows);

    system.write16(OAM, (1u << 12)); // Enable mosaic for OBJ0.
    system.write16(OAM + 2, 0);
    system.write16(OAM + 4, 0);
    system.write16(0x0400004C, 0x1100); // 2x2 OBJ mosaic pixels.
    system.write16(DISPCNT, 1u << 12);

    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 1);
    assert(pixel(system, 1, 0) == 1);
    assert(pixel(system, 2, 0) == 3);
    assert(pixel(system, 3, 0) == 3);
    assert(pixel(system, 0, 1) == 1);
    assert(pixel(system, 0, 2) == 7);
}

void testTransparentObjectPixelsPromoteSpritePriority() {
    const auto prepareScene = [](GbaSystem &system) {
        setPalette(system, PALETTE, 0, 0x0000);
        setPalette(system, PALETTE, 1, 0x03E0);
        setPalette(system, OBJ_PALETTE, 1, 0x7C00);
        writeSolid4bppTile(system, VRAM + 0x4000, 1, 1);
        writeSolid4bppTile(system, VRAM + 0x10000, 0, 1);
        system.write16(VRAM + 31 * 0x800, 1);
        system.write16(BG0CNT, (1u << 2) | (31u << 8));
    };

    {
        GbaSystem system(loopingRom());
        prepareScene(system);
        system.write16(OAM, 0);
        system.write16(OAM + 2, 0);
        system.write16(OAM + 4, 1u << 10); // Red OBJ0 has BG priority 1.
        system.write16(OAM + 8, 0);
        system.write16(OAM + 10, 0);
        system.write16(OAM + 12, 1); // Transparent OBJ1 has priority 0.
        system.write16(DISPCNT, (1u << 8) | (1u << 12));

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x7C00);
        assert(pixel(system, 1, 0) == 0x7C00);
    }

    {
        GbaSystem system(loopingRom());
        prepareScene(system);
        uint8_t maskTile[8][8]{};
        for (unsigned y = 0; y < 8; ++y)
            maskTile[y][1] = 1; // x=0 is transparent; x=1 marks OBJ window.
        write4bppTile(system, VRAM + 0x10000, 1, maskTile);
        system.write16(OAM, 0);
        system.write16(OAM + 2, 0);
        system.write16(OAM + 4, 1u << 10); // Red OBJ0 has BG priority 1.
        system.write16(OAM + 8, 2u << 10); // OBJ1 is an OBJ-window sprite.
        system.write16(OAM + 10, 0);
        system.write16(OAM + 12, 1); // Its priority is higher than BG0.
        system.write16(0x0400004A, (1u << 8) | (1u << 4) | 1u);
        system.write16(DISPCNT, (1u << 8) | (1u << 12) | (1u << 15));

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x7C00); // Transparent OBJWIN pixel promotes priority.
        assert(pixel(system, 1, 0) == 0x03E0); // Opaque OBJWIN pixel selects BG-only mask.
    }
}

void testWindowsAndColorEffects() {
    {
        GbaSystem system(loopingRom());
        setPalette(system, PALETTE, 0, 0x0000);
        setPalette(system, PALETTE, 1, 0x03E0);
        setPalette(system, OBJ_PALETTE, 1, 0x001F);
        writeSolid4bppTile(system, VRAM + 0x4000, 0, 1);
        writeSolid4bppTile(system, VRAM + 0x10000, 0, 1);
        system.write16(VRAM + 31 * 0x800, 0);
        system.write16(BG0CNT, (1u << 2) | (31u << 8));
        system.write16(OAM, 32);
        system.write16(OAM + 2, 88);
        system.write16(OAM + 4, 0);
        system.write16(0x04000040, 80); // WIN0: x=[0,80)
        system.write16(0x04000044, 160); // y=[0,160)
        system.write16(0x04000048, 1); // Inside WIN0: BG0 only.
        system.write16(0x0400004A, 1u << 4); // Outside: OBJ only.
        system.write16(DISPCNT, (1u << 8) | (1u << 12) | (1u << 13));

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x03E0);
        assert(pixel(system, 100, 0) == 0x0000);
        assert(pixel(system, 88, 32) == 0x001F);
    }
    {
        GbaSystem system(loopingRom());
        setPalette(system, PALETTE, 0, 0x0000);
        setPalette(system, PALETTE, 1, 0x03E0);
        writeSolid4bppTile(system, VRAM + 0x4000, 0, 1);
        system.write16(VRAM + 31 * 0x800, 0);
        system.write16(BG0CNT, (1u << 2) | (31u << 8));
        system.write16(0x04000040, (200u << 8) | 40u);
        system.write16(0x04000044, (120u << 8) | 40u);
        system.write16(0x04000048, 1); // Inside: BG0 only.
        system.write16(0x0400004A, 0); // Outside: backdrop only.
        system.write16(DISPCNT, (1u << 8) | (1u << 13));

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x03E0);     // Both axes wrap at zero.
        assert(pixel(system, 100, 0) == 0x0000);   // Horizontal outside.
        assert(pixel(system, 10, 80) == 0x0000);   // Vertical outside.
        assert(pixel(system, 220, 130) == 0x03E0); // Both wrapped segments.
    }
    {
        GbaSystem system(loopingRom());
        setPalette(system, PALETTE, 0, 0x0000);
        setPalette(system, PALETTE, 1, 0x03E0);
        writeSolid4bppTile(system, VRAM + 0x4000, 0, 1);
        uint8_t windowTile[8][8]{};
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 4; ++x)
                windowTile[y][x] = 1;
        write4bppTile(system, VRAM + 0x10000, 0, windowTile);
        system.write16(VRAM + 31 * 0x800, 0);
        system.write16(BG0CNT, (1u << 2) | (31u << 8));
        system.write16(OAM, 32 | (2u << 10)); // OBJ window, 8x8.
        system.write16(OAM + 2, 40);
        system.write16(OAM + 4, 0);
        system.write16(0x0400004A, 1u << 8); // OBJ window exposes BG0.
        system.write16(DISPCNT, (1u << 8) | (1u << 12) | (1u << 15));

        assert(system.runFrame().success);
        assert(pixel(system, 40, 32) == 0x03E0);
        assert(pixel(system, 43, 32) == 0x03E0);
        assert(pixel(system, 44, 32) == 0x0000);
    }
    {
        GbaSystem system(loopingRom());
        setPalette(system, PALETTE, 0, 0x0000);
        setPalette(system, PALETTE, 1, 0x03E0);
        setPalette(system, OBJ_PALETTE, 1, 0x001F);
        writeSolid4bppTile(system, VRAM + 0x4000, 0, 1);
        writeSolid4bppTile(system, VRAM + 0x10000, 0, 1);
        system.write16(VRAM + 31 * 0x800, 0);
        system.write16(BG0CNT, (1u << 2) | (31u << 8));
        system.write16(OAM, 32 | (1u << 10)); // Semi-transparent OBJ.
        system.write16(OAM + 2, 40);
        system.write16(OAM + 4, 0);
        system.write16(0x04000050, (3u << 6) | (1u << 8));
        system.write16(0x04000052, 8 | (8u << 8));
        system.write16(DISPCNT, (1u << 8) | (1u << 12));

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x03E0);
        assert(pixel(system, 40, 32) == 0x01EF); // Forced OBJ alpha blend.

        setPalette(system, OBJ_PALETTE, 2, 0x7C00);
        writeSolid4bppTile(system, VRAM + 0x10000, 1, 2);
        system.write16(BG0CNT, (1u << 2) | (31u << 8) | 3);
        system.write16(OAM, 32 | (1u << 10));
        system.write16(OAM + 2, 40);
        system.write16(OAM + 4, 0);
        system.write16(OAM + 8, 32); // OBJ1 under OBJ0, above BG0.
        system.write16(OAM + 10, 40);
        system.write16(OAM + 12, 1 | (1u << 10));
        system.write16(0x04000050, (1u << 6) | (1u << 12));
        system.write16(DISPCNT, (1u << 8) | (1u << 12));
        assert(system.runFrame().success);
        assert(pixel(system, 40, 32) == 0x001F); // OBJ1 is not a blend target.

        system.write16(0x04000050, (1u << 6) | (1u << 8));
        assert(system.runFrame().success);
        assert(pixel(system, 40, 32) == 0x01EF); // Blend OBJ0 with BG0.

        system.write16(OAM + 8, 1u << 9); // Disable OBJ1 for the next case.
        system.write16(BG0CNT, (1u << 2) | (31u << 8));
        system.write16(OAM, 32); // Normal OBJ uses BLDCNT first-target mask.
        system.write16(0x04000050, (1u << 6) | (1u << 4) | (1u << 8));
        assert(system.runFrame().success);
        assert(pixel(system, 40, 32) == 0x01EF);

        system.write16(0x04000050, (2u << 6) | (1u << 0));
        system.write16(0x04000054, 16);
        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x7FFF);
        system.write16(0x04000050, (3u << 6) | (1u << 0));
        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x0000);
    }
}

void testMidScanlineDisplayRegisterWrite() {
    GbaSystem system(midScanlineDisplayDisableRom());
    setPalette(system, PALETTE, 0, 0x0000);
    setPalette(system, PALETTE, 1, 0x03E0);
    writeSolid4bppTile(system, VRAM + 0x4000, 0, 1);
    system.write16(VRAM + 31 * 0x800, 0);
    system.write16(BG0CNT, (1u << 2) | (31u << 8));
    system.write16(DISPCNT, 1u << 8);

    assert(system.runFrame().success);
    unsigned splitPixel = 0;
    while (splitPixel < GbaSystem::SCREEN_WIDTH &&
           pixel(system, splitPixel, 0) == 0x03E0)
        ++splitPixel;
    assert(splitPixel >= 64 && splitPixel <= 80);
    for (unsigned x = 0; x < splitPixel; ++x)
        assert(pixel(system, x, 0) == 0x03E0);
    for (unsigned x = splitPixel; x < GbaSystem::SCREEN_WIDTH; ++x)
        assert(pixel(system, x, 0) == 0x0000);
    assert(pixel(system, 0, 1) == 0x0000);
}

void testMode1AffineBackground() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F);
    setPalette(system, PALETTE, 5, 0x7C00);
    writeSolid8bppTile(system, VRAM + 0x4000, 1, 5);
    system.write16(VRAM + 24 * 0x800, 1); // BG2 affine map, tile (0, 0).
    system.write16(BG2CNT, (1u << 2) | (1u << 7) | (24u << 8));
    system.write16(DISPCNT, (1u << 0) | (1u << 10)); // Mode 1, BG2 enabled.
    system.write16(0x04000020, 0x0100); // BG2PA = 1.0
    system.write16(0x04000022, 0);
    system.write16(0x04000024, 0);
    system.write16(0x04000026, 0x0100); // BG2PD = 1.0
    system.write32(0x04000028, 0);
    system.write32(0x0400002C, 0);

    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 0x7C00);
    assert(pixel(system, 7, 7) == 0x7C00);
    assert(pixel(system, 8, 0) == 0x001F);

    // Non-wrapping affine coordinates outside the map reveal the backdrop.
    system.write16(BG2CNT, (1u << 2) | (1u << 7) | (24u << 8) | (1u << 13));
    system.write32(0x04000028, 0x00010000); // Reference X = 256 pixels.
    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 0x7C00); // Wraparound enabled.
    system.write16(BG2CNT, (1u << 2) | (1u << 7) | (24u << 8));
    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 0x001F);
}

void testMode2AffineBackgroundComposition() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F);
    setPalette(system, PALETTE, 1, 0x03E0);
    setPalette(system, PALETTE, 2, 0x7C00);
    writeSolid8bppTile(system, VRAM, 1, 1);
    writeSolid8bppTile(system, VRAM + 0x4000, 1, 2);
    system.write16(VRAM + 24 * 0x800, 0x0101); // BG2 starts with tile 1 twice.
    system.write16(VRAM + 25 * 0x800, 1); // BG3 map begins with tile 1.

    system.write16(BG2CNT, 1 | (24u << 8));
    system.write16(0x0400000E, (1u << 2) | (25u << 8)); // BG3CNT.
    system.write16(DISPCNT, 2u | (1u << 10) | (1u << 11));
    system.write16(0x04000020, 0x0100); // BG2 identity matrix.
    system.write16(0x04000026, 0x0100);
    system.write16(0x04000030, 0x0100); // BG3 identity matrix.
    system.write16(0x04000036, 0x0100);

    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 0x7C00); // BG3 priority 0 covers BG2.
    assert(pixel(system, 7, 7) == 0x7C00);
    assert(pixel(system, 8, 0) == 0x03E0); // Transparent BG3 tile reveals BG2.

    system.write16(0x04000030, 0x0200); // BG3 reads two source pixels per x.
    assert(system.runFrame().success);
    assert(pixel(system, 0, 0) == 0x7C00);
    assert(pixel(system, 3, 0) == 0x7C00);
    assert(pixel(system, 4, 0) == 0x03E0); // BG3 advances into its zero map tile.
}

void testAffineObjectsAndBitmapObjectMemory() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F);
    setPalette(system, OBJ_PALETTE, 1, 0x03E0);
    writeSolid4bppTile(system, VRAM + 0x14000, 0, 1);

    // Affine matrix 0 is stored in the high halfwords of four OAM slots.
    system.write16(OAM + 6, 0x0100);
    system.write16(OAM + 14, 0);
    system.write16(OAM + 22, 0);
    system.write16(OAM + 30, 0x0100);
    system.write16(OAM, 32 | (1u << 8));
    system.write16(OAM + 2, 32);
    system.write16(OAM + 4, 0);
    system.write16(DISPCNT, (1u << 12) | (1u << 8)); // Mode 0 OBJ + BG0.

    // Switch to bitmap mode, where OBJ tile memory begins at VRAM + 0x14000.
    system.write16(DISPCNT, (3u << 0) | (1u << 10) | (1u << 12));
    system.write16(0x0400000C, 2);
    for (unsigned y = 0; y < GbaSystem::SCREEN_HEIGHT; ++y)
        for (unsigned x = 0; x < GbaSystem::SCREEN_WIDTH; ++x)
            system.write16(VRAM + (y * GbaSystem::SCREEN_WIDTH + x) * 2, 0x7C00);
    assert(system.runFrame().success);

    // Bitmap modes reserve OBJ tile numbers 0..511 for the bitmap area.
    assert(pixel(system, 32, 32) == 0x7C00);
    system.write16(OAM + 4, 512);
    assert(system.runFrame().success);
    assert(pixel(system, 32, 32) == 0x03E0);
    assert(pixel(system, 0, 0) == 0x7C00);
}

void testObjectScanlineCycleLimit() {
    const auto prepareScene = [](GbaSystem &system) {
        setPalette(system, PALETTE, 0, 0x001F);
        setPalette(system, OBJ_PALETTE, 1, 0x03E0);
        writeSolid4bppTile(system, VRAM + 0x10000, 0, 1);
    };
    const auto setObject = [](GbaSystem &system, unsigned object,
                              uint16_t attribute0, uint16_t x) {
        const uint32_t address = OAM + object * 8;
        system.write16(address, attribute0);
        system.write16(address + 2, x);
        system.write16(address + 4, 0);
    };

    {
        GbaSystem system(loopingRom());
        prepareScene(system);
        for (unsigned object = 0; object < 119; ++object)
            setObject(system, object, 0, 300); // Offscreen: OAM scan only.
        for (unsigned object = 119; object < 128; ++object)
            setObject(system, object, 0, static_cast<uint16_t>((object - 119) * 8));

        system.write16(DISPCNT, (1u << 12) | (1u << 5)); // OBJ + H-Blank free.
        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x03E0); // Hidden sprites use only scan cycles.
        assert(pixel(system, 64, 0) == 0x03E0);

        system.write16(DISPCNT, 1u << 12); // Full 1210-cycle OBJ budget.
        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x03E0);
        assert(pixel(system, 64, 0) == 0x03E0);
    }

    {
        GbaSystem system(loopingRom());
        prepareScene(system);
        for (unsigned object = 0; object < 36; ++object)
            setObject(system, object, 1u << 8, 200); // Affine 8x8: 26 cycles each.
        setObject(system, 36, 0, 0);
        setObject(system, 37, 0, 8);
        setObject(system, 38, 0, 16);
        system.write16(DISPCNT, (1u << 12) | (1u << 5));
        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x03E0);
        assert(pixel(system, 8, 0) == 0x03E0);
        assert(pixel(system, 16, 0) == 0x001F);
    }

    {
        GbaSystem system(loopingRom());
        prepareScene(system);
        writeSolid4bppTile(system, VRAM + 0x10000, 1, 1);
        for (unsigned object = 0; object < 117; ++object)
            setObject(system, object, 0, 100);
        setObject(system, 117, 1u << 14, 500); // 16x8 OBJ at x=-12.
        setObject(system, 118, 0, 8);
        system.write16(DISPCNT, (1u << 12) | (1u << 5)); // OBJ + H-Blank free.

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x03E0); // Left-clipped OBJ is visible.
        assert(pixel(system, 8, 0) == 0x03E0); // Its cycle saving admits OBJ 118.
    }

    {
        GbaSystem system(loopingRom());
        prepareScene(system);
        writeSolid4bppTile(system, VRAM + 0x10000, 1, 1);
        for (unsigned object = 0; object < 114; ++object)
            setObject(system, object, 0, 100);
        setObject(system, 114, (1u << 8) | (1u << 14),
                  504 | (31u << 9)); // Affine 16x8 OBJ at x=-8.
        setObject(system, 115, 0, 8);
        const uint32_t matrix = OAM + 31 * 32;
        system.write16(matrix + 6, 0x0100);
        system.write16(matrix + 14, 0);
        system.write16(matrix + 22, 0);
        system.write16(matrix + 30, 0x0100);
        system.write16(DISPCNT, (1u << 12) | (1u << 5)); // OBJ + H-Blank free.

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x03E0); // Affine OBJ is left-clipped.
        assert(pixel(system, 8, 0) == 0x03E0); // Clipping admits the next OBJ.
    }

    {
        GbaSystem system(loopingRom());
        prepareScene(system);
        for (unsigned object = 0; object < 119; ++object)
            setObject(system, object, 2u << 10, 100); // OBJ windows use draw cycles.
        setObject(system, 119, 0, 0);
        system.write16(0x0400004A, 1u << 4); // OBJ visible outside the OBJ window.
        system.write16(DISPCNT, (1u << 12) | (1u << 15) | (1u << 5));
        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x001F); // OBJ windows share the line budget.
    }
}

void testMode4IndexZeroTransparency() {
    {
        GbaSystem system(loopingRom());
        setPalette(system, PALETTE, 0, 0x7C00);
        setPalette(system, PALETTE, 1, 0x001F);
        setPalette(system, OBJ_PALETTE, 1, 0x03E0);
        system.write16(VRAM, 0x0100); // Pixel 0 is index 0; pixel 1 is index 1.
        writeSolid4bppTile(system, VRAM + 0x14000, 0, 1);
        system.write16(OAM, 0);
        system.write16(OAM + 2, 0);
        system.write16(OAM + 4, 512 | (1u << 10)); // OBJ priority 1, behind BG2.
        system.write16(DISPCNT, (4u << 0) | (1u << 10) | (1u << 12));

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x03E0); // Index 0 reveals the lower-priority OBJ.
        assert(pixel(system, 1, 0) == 0x001F); // Nonzero bitmap pixels stay opaque.
    }
    {
        GbaSystem system(loopingRom());
        setPalette(system, OBJ_PALETTE, 1, 0x03E0);
        writeSolid4bppTile(system, VRAM + 0x14000, 0, 1);
        system.write16(OAM, 0);
        system.write16(OAM + 2, 0);
        system.write16(OAM + 4, 512 | (1u << 10));
        system.write16(DISPCNT, (3u << 0) | (1u << 10) | (1u << 12));

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x0000); // Mode 3 black is still opaque.
    }
}

void testHBlankDmaObjectScanlineStaging() {
    GbaSystem system(loopingRom());
    setPalette(system, PALETTE, 0, 0x001F);
    setPalette(system, OBJ_PALETTE, 1, 0x03E0);
    writeSolid4bppTile(system, VRAM + 0x10000, 0, 1);
    system.write16(OAM, 0); // Visible 8x8 OBJ at the top of the screen.
    system.write16(OAM + 2, 8);
    system.write16(OAM + 4, 0);
    system.write16(DISPCNT, 1u << 12);

    // A one-shot H-Blank DMA hides OBJ0 after scanline zero is rendered.
    system.write16(0x02000000, 1u << 9);
    system.write32(0x040000D4, 0x02000000); // DMA3 source.
    system.write32(0x040000D8, OAM); // DMA3 fixed destination.
    system.write16(0x040000DC, 1); // One halfword.
    system.write16(0x040000DE,
                   0x8000 | (2u << 12) | (2u << 5)); // Enable, H-Blank, fixed.

    assert(system.runFrame().success);
    assert(pixel(system, 8, 0) == 0x03E0);
    assert(pixel(system, 8, 1) == 0x03E0); // H-Blank DMA follows line-1 staging.
    assert(pixel(system, 8, 2) == 0x001F); // New OAM appears after the staged line.
}

void testBitmapAffineTransformAndMode5() {
    {
        GbaSystem system(loopingRom());
        for (unsigned x = 0; x < GbaSystem::SCREEN_WIDTH; ++x)
            for (unsigned y = 0; y < GbaSystem::SCREEN_HEIGHT; ++y)
                system.write16(VRAM + (y * GbaSystem::SCREEN_WIDTH + x) * 2,
                    static_cast<uint16_t>(x));
        system.write16(0x0400004C, 3); // Four-pixel BG mosaic groups.
        system.write16(0x0400000C, 1u << 6); // Enable BG2 mosaic.
        system.write16(DISPCNT, (3u << 0) | (1u << 10));
        system.write32(0x04000028, 0x00000100); // Start at source X = 1.

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 1);
        assert(pixel(system, 3, 0) == 1);
        assert(pixel(system, 4, 0) == 5);
        assert(pixel(system, 20, 0) == 21);
    }
    {
        GbaSystem system(loopingRom());
        setPalette(system, PALETTE, 0, 0x001F);
        system.write16(VRAM + 0xA000, 0x7C00);
        system.write16(VRAM + 0xA000 + (127 * 160 + 159) * 2, 0x03E0);
        system.write16(DISPCNT, (5u << 0) | (1u << 4) | (1u << 10));

        assert(system.runFrame().success);
        assert(pixel(system, 0, 0) == 0x7C00);
        assert(pixel(system, 159, 127) == 0x03E0);
        assert(pixel(system, 160, 0) == 0x001F); // Outside bitmap is transparent.
        assert(pixel(system, 0, 128) == 0x001F);
    }
}

void testTileAndSpriteHomebrewScreen() {
    GbaSystem system(makeGbaMode0TileSpriteHomebrewRom());
    assert(system.runFrame().success);
    assert(system.runFrame().success);

    // The first frame executes its setup while scanlines are being drawn.
    // Check the stable frame after the Mode 0 scene is fully initialized.
    for (unsigned y = 0; y < GbaSystem::SCREEN_HEIGHT; ++y) {
        for (unsigned x = 0; x < GbaSystem::SCREEN_WIDTH; ++x) {
            const bool inSprite =
                (x >= 40 && x < 56 && y >= 32 && y < 48) ||
                (x >= 64 && x < 80 && y >= 128 && y < 144) ||
                (x >= 88 && x < 104 && y == 159) ||
                (x >= 136 && x < 152 && y < 15);
            const uint16_t expected = inSprite ? 0x03E0 : 0x001F;
            const uint16_t actual = pixel(system, x, y);
            if (actual != expected) {
                std::fprintf(stderr,
                    "Homebrew tile/sprite frame mismatch at (%u,%u): expected %04x, got %04x\n",
                    x, y, expected, actual);
                std::fprintf(stderr,
                    "DISPCNT=%04x BG0CNT=%04x palette=%04x/%04x tile=%02x map=%04x OAM=%04x/%04x/%04x PC=%08x R0=%08x R1=%08x R2=%08x lit=%08x/%08x/%08x\n",
                    system.read16(DISPCNT), system.read16(BG0CNT),
                    system.read16(PALETTE), system.read16(PALETTE + 2),
                    system.read8(VRAM + 0x4000), system.read16(VRAM),
                    system.read16(OAM), system.read16(OAM + 2),
                    system.read16(OAM + 4), system.cpu().programCounter(),
                    system.cpu().registerValue(0), system.cpu().registerValue(1),
                    system.cpu().registerValue(2), system.read32(0x08000134),
                    system.read32(0x08000138), system.read32(0x0800013C));
                assert(actual == expected);
            }
        }
    }
}

} // namespace

int main() {
    testBiosSwiVectorAndExceptionReturn();
    testBiosIrqVectorAndExceptionReturn();
    testOptionalBiosEntry();
    testSkipBiosStartsWithInitializedCartridgeState();
    testFlash1mIdentificationCommandsBankingAndPersistence();
    testSramMappingAndPersistence();
    testFlash512CommandsAndPersistence();
    testEeprom512DmaReadWriteAndPersistence();
    testEeprom8kDmaReadWriteAndExpansion();
    testVideoMemoryAccessWidths();
    testRegisterRamResetSoftwareInterrupt();
    testCpuSetAndCpuFastSetSoftwareInterrupts();
    testImmediateAndBlankingDma();
    testIrqAndSystemShareR8ThroughR12();
    testDisplayInterruptEnableBits();
    testGbaTimersPrescalersCascadeAndIrq();
    testWaitcntRegisterMasksAndReset();
    testWaitcntBusCycleTotals();
    testWaitcntGamePakRomAndRamWaitCycles();
    testGbaPsgProducesStereoAudio();
    testGbaDirectSoundFifoUsesTimerAndSpecialDma();
    testHBlankDmaUpdatesFollowingScanlines();
    testLz77SoftwareInterrupts();
    testAffineSetSoftwareInterrupts();
    testMode0TextBackgroundPixelsAndScroll();
    testBackgroundMosaic();
    testTextBackgroundSizesAndEightBitTiles();
    testObjectsPrioritiesShapesAndMapping();
    testObjectVerticalCoordinateWrapsAtScreenHeight();
    testRegularObjectFlipAndShapeSizeCombinations();
    testEightBitObjectsInOneDimensionalAndTwoDimensionalMapping();
    testEightBitObjectsUseBitmapModeTileBase();
    testAffineEightBitObject();
    testAffineObjectScaleAndDoubleSizeBounds();
    testObjectMosaic();
    testTransparentObjectPixelsPromoteSpritePriority();
    testWindowsAndColorEffects();
    testMidScanlineDisplayRegisterWrite();
    testMode1AffineBackground();
    testMode2AffineBackgroundComposition();
    testAffineObjectsAndBitmapObjectMemory();
    testObjectScanlineCycleLimit();
    testHBlankDmaObjectScanlineStaging();
    testMode4IndexZeroTransparency();
    testBitmapAffineTransformAndMode5();
    testTileAndSpriteHomebrewScreen();
}
