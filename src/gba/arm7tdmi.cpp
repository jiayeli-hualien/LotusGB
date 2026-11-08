#include "arm7tdmi.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace LOTUSGBA {
namespace {

uint32_t rotateRight(uint32_t value, unsigned amount) {
    amount &= 31;
    if (amount == 0)
        return value;
    return (value >> amount) | (value << (32 - amount));
}

int32_t signExtend24(uint32_t value) {
    value &= 0x00FFFFFF;
    if (value & 0x00800000)
        value |= 0xFF000000;
    return static_cast<int32_t>(value);
}

} // namespace

void Arm7Tdmi::reset(uint32_t entryPoint, uint32_t stackPointer,
                     uint32_t initialCpsr, uint32_t irqStackPointer,
                     uint32_t supervisorStackPointer) {
    registers_.fill(0);
    userBank_.fill(0);
    fiqBank_.fill(0);
    for (auto &bank : exceptionRegisterBanks_)
        bank.fill(0);
    exceptionRegisterBanks_[0][0] = irqStackPointer;
    exceptionRegisterBanks_[1][0] = supervisorStackPointer;
    savedStatusBanks_.fill(0);
    irqSavedRegisters_.fill(0);
    registers_[13] = stackPointer;
    registers_[14] = 0;
    userBank_[5] = stackPointer;
    pc_ = entryPoint & ~uint32_t{3};
    cpsr_ = initialCpsr;
    irqReturnProgramCounter_ = 0;
    irqReturnStatus_ = 0;
    irqActive_ = false;
    halted_ = false;
    haltOnAnyEnabledInterrupt_ = false;
    haltInterruptMask_ = 0;
    fault_.clear();
}

uint32_t Arm7Tdmi::registerValue(unsigned index) const {
    if (index >= registers_.size())
        return 0;
    return index == 15 ? pc_ + 8 : registers_[index];
}

uint32_t Arm7Tdmi::readRegister(unsigned index,
                                uint32_t instructionAddress) const {
    if (index == 15)
        return instructionAddress + 8;
    return registers_[index];
}

bool Arm7Tdmi::conditionPassed(unsigned condition) const {
    const bool n = (cpsr_ & CPSR_N) != 0;
    const bool z = (cpsr_ & CPSR_Z) != 0;
    const bool c = (cpsr_ & CPSR_C) != 0;
    const bool v = (cpsr_ & CPSR_V) != 0;
    switch (condition) {
    case 0: return z;
    case 1: return !z;
    case 2: return c;
    case 3: return !c;
    case 4: return n;
    case 5: return !n;
    case 6: return v;
    case 7: return !v;
    case 8: return c && !z;
    case 9: return !c || z;
    case 10: return n == v;
    case 11: return n != v;
    case 12: return !z && n == v;
    case 13: return z || n != v;
    case 14: return true;
    default: return false;
    }
}

void Arm7Tdmi::setNz(uint32_t result) {
    cpsr_ &= ~(CPSR_N | CPSR_Z);
    if (result & 0x80000000)
        cpsr_ |= CPSR_N;
    if (result == 0)
        cpsr_ |= CPSR_Z;
}

void Arm7Tdmi::setAddFlags(uint32_t left, uint32_t right, uint64_t result) {
    const uint32_t low = static_cast<uint32_t>(result);
    cpsr_ &= ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V);
    if (low & 0x80000000)
        cpsr_ |= CPSR_N;
    if (low == 0)
        cpsr_ |= CPSR_Z;
    if (result >> 32)
        cpsr_ |= CPSR_C;
    if (((~(left ^ right) & (left ^ low)) & 0x80000000) != 0)
        cpsr_ |= CPSR_V;
}

void Arm7Tdmi::setSubFlags(uint32_t left, uint32_t right, uint32_t result) {
    cpsr_ &= ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V);
    if (result & 0x80000000)
        cpsr_ |= CPSR_N;
    if (result == 0)
        cpsr_ |= CPSR_Z;
    if (left >= right)
        cpsr_ |= CPSR_C;
    if ((((left ^ right) & (left ^ result)) & 0x80000000) != 0)
        cpsr_ |= CPSR_V;
}

void Arm7Tdmi::switchMode(uint32_t mode) {
    constexpr uint32_t MODE_MASK = 0x1F;
    const uint32_t oldMode = cpsr_ & MODE_MASK;
    mode &= MODE_MASK;
    if (oldMode == mode)
        return;

    if (oldMode == 0x11) { // Save the FIQ bank.
        for (unsigned index = 0; index < 7; ++index) {
            fiqBank_[index] = registers_[index + 8];
        }
    } else if (const int exception = exceptionModeIndex(oldMode); exception >= 0) {
        exceptionRegisterBanks_[static_cast<size_t>(exception)] = {
            registers_[13], registers_[14]};
        for (unsigned index = 0; index < 5; ++index)
            userBank_[index] = registers_[index + 8];
    } else {
        for (unsigned index = 0; index < 5; ++index)
            userBank_[index] = registers_[index + 8];
        userBank_[5] = registers_[13];
        userBank_[6] = registers_[14];
    }

    if (mode == 0x11) { // shared User/System bank -> FIQ
        for (unsigned index = 0; index < 7; ++index)
            registers_[index + 8] = fiqBank_[index];
    } else if (const int exception = exceptionModeIndex(mode); exception >= 0) {
        if (oldMode == 0x11) {
            for (unsigned index = 0; index < 5; ++index)
                registers_[index + 8] = userBank_[index];
        }
        const auto &bank = exceptionRegisterBanks_[static_cast<size_t>(exception)];
        registers_[13] = bank[0];
        registers_[14] = bank[1];
    } else {
        if (oldMode == 0x11) {
            for (unsigned index = 0; index < 5; ++index)
                registers_[index + 8] = userBank_[index];
        }
        registers_[13] = userBank_[5];
        registers_[14] = userBank_[6];
    }
    cpsr_ = (cpsr_ & ~MODE_MASK) | mode;
}

int Arm7Tdmi::exceptionModeIndex(uint32_t mode) {
    switch (mode & 0x1F) {
    case 0x12: return 0; // IRQ
    case 0x13: return 1; // Supervisor
    case 0x17: return 2; // Abort
    case 0x1B: return 3; // Undefined
    default: return -1;
    }
}

uint32_t *Arm7Tdmi::savedStatusForMode(uint32_t mode) {
    mode &= 0x1F;
    if (mode == 0x11)
        return &savedStatusBanks_[0];
    const int exception = exceptionModeIndex(mode);
    if (exception < 0)
        return nullptr;
    return &savedStatusBanks_[static_cast<size_t>(exception + 1)];
}

void Arm7Tdmi::wakeFromInterrupts(uint16_t flags, uint16_t enable) {
    if (!halted_)
        return;
    if (haltOnAnyEnabledInterrupt_ ? (flags & enable) != 0
                                   : (flags & haltInterruptMask_) != 0) {
        halted_ = false;
        haltOnAnyEnabledInterrupt_ = false;
        haltInterruptMask_ = 0;
    }
}

bool Arm7Tdmi::enterIrq(uint32_t handlerAddress) {
    if (irqDisabled() || irqActive_)
        return false;
    uint32_t *savedStatus = savedStatusForMode(0x12);
    if (!savedStatus)
        return false;
    *savedStatus = cpsr_;
    irqSavedRegisters_ = {registers_[0], registers_[1], registers_[2],
                          registers_[3], registers_[12]};
    irqReturnProgramCounter_ = pc_;
    irqReturnStatus_ = cpsr_;
    switchMode(0x12);
    // The HLE BIOS vector saves the caller context, invokes the pointer at
    // 0x03007FFC, then restores that context and returns from the exception.
    registers_[14] = IRQ_RETURN_SENTINEL;
    cpsr_ |= CPSR_I;
    cpsr_ &= ~CPSR_T;
    pc_ = handlerAddress == 0 ? IRQ_RETURN_SENTINEL
                              : handlerAddress & ~uint32_t{3};
    irqActive_ = true;
    halted_ = false;
    haltOnAnyEnabledInterrupt_ = false;
    haltInterruptMask_ = 0;
    return true;
}

bool Arm7Tdmi::enterIrqVector() {
    if (irqDisabled() || irqActive_)
        return false;
    uint32_t *savedStatus = savedStatusForMode(0x12);
    if (!savedStatus)
        return false;
    *savedStatus = cpsr_;
    switchMode(0x12);
    registers_[14] = pc_ + 4;
    cpsr_ |= CPSR_I;
    cpsr_ &= ~CPSR_T;
    pc_ = 0x00000018;
    halted_ = false;
    haltOnAnyEnabledInterrupt_ = false;
    haltInterruptMask_ = 0;
    return true;
}

bool Arm7Tdmi::enterSwiVector(uint32_t returnAddress) {
    uint32_t *savedStatus = savedStatusForMode(0x13);
    if (!savedStatus)
        return false;
    *savedStatus = cpsr_;
    switchMode(0x13);
    registers_[14] = returnAddress;
    cpsr_ |= CPSR_I;
    cpsr_ &= ~CPSR_T;
    pc_ = 0x00000008;
    halted_ = false;
    haltOnAnyEnabledInterrupt_ = false;
    haltInterruptMask_ = 0;
    return true;
}

void Arm7Tdmi::finishIrqReturn() {
    if (!irqActive_ || (cpsr_ & 0x1F) != 0x12)
        return;
    switchMode(irqReturnStatus_ & 0x1F);
    registers_[0] = irqSavedRegisters_[0];
    registers_[1] = irqSavedRegisters_[1];
    registers_[2] = irqSavedRegisters_[2];
    registers_[3] = irqSavedRegisters_[3];
    registers_[12] = irqSavedRegisters_[4];
    cpsr_ = irqReturnStatus_;
    pc_ = irqReturnProgramCounter_;
    irqActive_ = false;
}

uint32_t Arm7Tdmi::readUserRegister(unsigned index) const {
    if (index >= 8 && index <= 14 && (cpsr_ & 0x1F) == 0x11)
        return userBank_[index - 8];
    return index < 16 ? registers_[index] : 0;
}

void Arm7Tdmi::writeUserRegister(unsigned index, uint32_t value) {
    if (index >= 8 && index <= 14 && (cpsr_ & 0x1F) == 0x11)
        userBank_[index - 8] = value;
    else if (index < 16)
        registers_[index] = value;
}

bool Arm7Tdmi::executeStatusTransfer(uint32_t instruction,
                                     uint32_t instructionAddress) {
    if ((instruction & 0x0FBF0FFF) == 0x010F0000) { // MRS
        const bool saved = (instruction & (1u << 22)) != 0;
        const unsigned rd = (instruction >> 12) & 0xF;
        if (saved) {
            const uint32_t *status = savedStatusForMode(cpsr_ & 0x1F);
            if (!status)
                return false;
            registers_[rd] = *status;
        } else {
            registers_[rd] = cpsr_;
        }
        return true;
    }

    const bool registerForm =
        (instruction & 0x0FB0FFF0) == 0x0120F000;
    const bool immediateForm =
        (instruction & 0x0FB0F000) == 0x0320F000;
    if (!registerForm && !immediateForm)
        return false;

    const bool saved = (instruction & (1u << 22)) != 0;
    const unsigned fieldMask = (instruction >> 16) & 0xF;
    uint32_t value = 0;
    if (immediateForm) {
        const unsigned rotate = ((instruction >> 8) & 0xF) * 2;
        value = rotateRight(instruction & 0xFF, rotate);
    } else {
        value = readRegister(instruction & 0xF, instructionAddress);
    }

    uint32_t mask = 0;
    if (fieldMask & 1) mask |= 0x000000FF;
    if (fieldMask & 2) mask |= 0x0000FF00;
    if (fieldMask & 4) mask |= 0x00FF0000;
    if (fieldMask & 8) mask |= 0xFF000000;

    if (saved) {
        uint32_t *status = savedStatusForMode(cpsr_ & 0x1F);
        if (!status)
            return false;
        *status = (*status & ~mask) | (value & mask);
    } else {
        // User mode may update only condition flags. This prototype runs in
        // System mode and supports the control and flag fields used by tests.
        if ((cpsr_ & 0x1F) == 0x10)
            mask &= 0xF0000000;
        const uint32_t newCpsr = (cpsr_ & ~mask) | (value & mask);
        const uint32_t mode = newCpsr & 0x1F;
        if (mode != 0x10 && mode != 0x11 && mode != 0x12 &&
            mode != 0x13 && mode != 0x17 && mode != 0x1B && mode != 0x1F)
            return false;
        const uint32_t nonModeMask = mask & ~uint32_t{0x1F};
        cpsr_ = (cpsr_ & ~nonModeMask) | (value & nonModeMask);
        if ((mask & 0x1F) != 0)
            switchMode(mode);
    }
    return true;
}

bool Arm7Tdmi::executeSoftwareInterrupt(uint32_t service, GbaBus &bus,
                                        uint32_t returnAddress) {
    if (bus.hasBios())
        return enterSwiVector(returnAddress);
    if (service == 0x000001) { // BIOS RegisterRamReset
        bus.registerRamReset(static_cast<uint8_t>(registers_[0]));
        return true;
    }
    if (service == 0x00000B || service == 0x00000C) { // CpuSet / CpuFastSet
        const uint32_t source = registers_[0];
        uint32_t destination = registers_[1];
        const uint32_t control = registers_[2];
        const bool fixedSource = (control & (1u << 24)) != 0;
        if (service == 0x00000B) {
            const bool wordTransfer = (control & (1u << 26)) != 0;
            const uint32_t unitSize = wordTransfer ? 4 : 2;
            const uint32_t count = control & 0x001FFFFF;
            const uint32_t fillValue = fixedSource
                ? (wordTransfer ? bus.read32(source) : bus.read16(source))
                : 0;
            for (uint32_t index = 0; index < count; ++index) {
                const uint32_t value = fixedSource
                    ? fillValue
                    : (wordTransfer
                        ? bus.read32(source + index * unitSize)
                        : bus.read16(source + index * unitSize));
                if (wordTransfer)
                    bus.write32(destination, value);
                else
                    bus.write16(destination, static_cast<uint16_t>(value));
                destination += unitSize;
            }
        } else {
            const uint32_t count = ((control & 0x001FFFFF) + 7u) & ~7u;
            const uint32_t fillValue = fixedSource ? bus.read32(source) : 0;
            for (uint32_t index = 0; index < count; ++index) {
                const uint32_t value = fixedSource
                    ? fillValue
                    : bus.read32(source + index * 4);
                bus.write32(destination, value);
                destination += 4;
            }
        }
        return true;
    }
    if (service == 0x000011 || service == 0x000012) { // LZ77UnCompWRAM / VRAM
        const uint32_t sourceStart = registers_[0];
        uint32_t source = sourceStart + 4;
        const uint32_t destination = registers_[1];
        const uint32_t outputSize = (bus.read32(sourceStart) >> 8) & 0x00FFFFFF;
        std::vector<uint8_t> output;
        output.reserve(outputSize);

        while (output.size() < outputSize) {
            const uint8_t flags = bus.read8(source++);
            for (unsigned bit = 0; bit < 8 && output.size() < outputSize; ++bit) {
                if ((flags & (0x80u >> bit)) == 0) {
                    output.push_back(bus.read8(source++));
                    continue;
                }

                const uint8_t first = bus.read8(source++);
                const uint8_t second = bus.read8(source++);
                const unsigned length = (first >> 4) + 3;
                const unsigned displacement =
                    ((static_cast<unsigned>(first & 0x0F) << 8) |
                     static_cast<unsigned>(second)) + 1u;
                for (unsigned index = 0;
                     index < length && output.size() < outputSize; ++index) {
                    output.push_back(output.size() >= displacement
                        ? output[output.size() - displacement]
                        : 0);
                }
            }
        }

        if (service == 0x000011) {
            for (size_t index = 0; index < output.size(); ++index)
                bus.write8(destination + static_cast<uint32_t>(index), output[index]);
        } else {
            uint32_t outputAddress = destination;
            size_t outputOffset = 0;
            while (outputOffset < output.size()) {
                const uint32_t halfwordAddress = outputAddress & ~uint32_t{1};
                uint16_t value = bus.read16(halfwordAddress);
                if ((outputAddress & 1) != 0) {
                    value = static_cast<uint16_t>((value & 0x00FF) |
                        (static_cast<uint16_t>(output[outputOffset++]) << 8));
                    ++outputAddress;
                } else {
                    value = static_cast<uint16_t>((value & 0xFF00) |
                        output[outputOffset++]);
                    ++outputAddress;
                    if (outputOffset < output.size()) {
                        value = static_cast<uint16_t>((value & 0x00FF) |
                            (static_cast<uint16_t>(output[outputOffset++]) << 8));
                        ++outputAddress;
                    }
                }
                bus.write16(halfwordAddress, value);
            }
        }

        registers_[0] = source;
        registers_[1] = destination + outputSize;
        registers_[3] = 0;
        return true;
    }
    if (service == 0x00000E || service == 0x00000F) { // BgAffineSet / ObjAffineSet
        const bool background = service == 0x00000E;
        uint32_t source = registers_[0];
        uint32_t destination = registers_[1];
        const uint32_t count = registers_[2];
        const uint32_t stride = registers_[3];
        constexpr float PI = 3.14159265358979323846f;

        for (uint32_t item = 0; item < count; ++item) {
            float originX = 0;
            float originY = 0;
            int16_t centerX = 0;
            int16_t centerY = 0;
            const float scaleX = static_cast<int16_t>(bus.read16(source +
                (background ? 12 : 0))) / 256.0f;
            const float scaleY = static_cast<int16_t>(bus.read16(source +
                (background ? 14 : 2))) / 256.0f;
            const uint32_t rotationOffset = background ? 16 : 4;
            const uint8_t rotation = static_cast<uint8_t>(
                bus.read16(source + rotationOffset) >> 8);
            if (background) {
                originX = static_cast<float>(static_cast<int32_t>(
                    bus.read32(source))) / 256.0f;
                originY = static_cast<float>(static_cast<int32_t>(
                    bus.read32(source + 4))) / 256.0f;
                centerX = static_cast<int16_t>(bus.read16(source + 8));
                centerY = static_cast<int16_t>(bus.read16(source + 10));
            }

            const float theta = static_cast<float>(rotation) / 128.0f * PI;
            const float sine = std::sin(theta);
            const float cosine = std::cos(theta);
            const float pa = cosine * scaleX;
            const float pb = -sine * scaleX;
            const float pc = sine * scaleY;
            const float pd = cosine * scaleY;
            bus.write16(destination, static_cast<uint16_t>(
                static_cast<int16_t>(pa * 256.0f)));
            if (background) {
                bus.write16(destination + 2, static_cast<uint16_t>(
                    static_cast<int16_t>(pb * 256.0f)));
                bus.write16(destination + 4, static_cast<uint16_t>(
                    static_cast<int16_t>(pc * 256.0f)));
                bus.write16(destination + 6, static_cast<uint16_t>(
                    static_cast<int16_t>(pd * 256.0f)));
                const float referenceX = originX - (pa * centerX + pb * centerY);
                const float referenceY = originY - (pc * centerX + pd * centerY);
                bus.write32(destination + 8, static_cast<uint32_t>(
                    static_cast<int32_t>(referenceX * 256.0f)));
                bus.write32(destination + 12, static_cast<uint32_t>(
                    static_cast<int32_t>(referenceY * 256.0f)));
                source += 20;
                destination += 16;
            } else {
                bus.write16(destination + stride, static_cast<uint16_t>(
                    static_cast<int16_t>(pb * 256.0f)));
                bus.write16(destination + stride * 2, static_cast<uint16_t>(
                    static_cast<int16_t>(pc * 256.0f)));
                bus.write16(destination + stride * 3, static_cast<uint16_t>(
                    static_cast<int16_t>(pd * 256.0f)));
                source += 8;
                destination += stride * 4;
            }
        }
        return true;
    }
    if (service == 0x000006) { // BIOS Div
        const int32_t numerator = static_cast<int32_t>(registers_[0]);
        const int32_t denominator = static_cast<int32_t>(registers_[1]);
        int32_t quotient = 0;
        int32_t remainder = numerator;
        if (denominator != 0) {
            if (numerator == std::numeric_limits<int32_t>::min() &&
                denominator == -1) {
                quotient = std::numeric_limits<int32_t>::min();
                remainder = 0;
            } else {
                quotient = numerator / denominator;
                remainder = numerator % denominator;
            }
        }
        registers_[0] = static_cast<uint32_t>(quotient);
        registers_[1] = static_cast<uint32_t>(remainder);
        registers_[3] = quotient < 0
            ? static_cast<uint32_t>(-static_cast<int64_t>(quotient))
            : static_cast<uint32_t>(quotient);
        return true;
    }
    if (service == 0x000002) { // BIOS Halt
        halted_ = true;
        haltOnAnyEnabledInterrupt_ = true;
        haltInterruptMask_ = 0;
        return true;
    }
    if (service == 0x000004 || service == 0x000005) { // IntrWait / VBlankIntrWait
        const bool discard = service == 0x000005 || registers_[0] != 0;
        const uint16_t mask = static_cast<uint16_t>(
            (service == 0x000005 ? 1u : registers_[1]) & 0x3FFFu);
        bus.write16(0x04000208, 1); // BIOS wait services force IME on.
        if (discard) {
            bus.write16(0x04000202, mask);
            bus.write16(0x03007FF8, mask);
        } else {
            const uint16_t pending = static_cast<uint16_t>(
                (bus.read16(0x04000202) | bus.read16(0x03007FF8)) & mask);
            if (pending != 0) {
                bus.write16(0x04000202, pending);
                bus.write16(0x03007FF8,
                            static_cast<uint16_t>(bus.read16(0x03007FF8) &
                                                  ~mask));
                return true;
            }
        }
        halted_ = true;
        haltOnAnyEnabledInterrupt_ = false;
        haltInterruptMask_ = mask;
        return true;
    }
    return false;
}

bool Arm7Tdmi::executeMultiply(uint32_t instruction) {
    const bool longMultiply = (instruction & 0x0F8000F0) == 0x00800090;
    const bool accumulate = (instruction & (1u << 21)) != 0;
    const bool setFlags = (instruction & (1u << 20)) != 0;
    const unsigned rd = (instruction >> 16) & 0xF;
    const unsigned rn = (instruction >> 12) & 0xF;
    const unsigned rs = (instruction >> 8) & 0xF;
    const unsigned rm = instruction & 0xF;
    if (rm == 15 || rs == 15 || rn == 15 ||
        (longMultiply && (rd == 15 || rd == rn)))
        return false;

    if (!longMultiply) {
        const uint32_t product = static_cast<uint32_t>(
            static_cast<uint64_t>(registers_[rm]) * registers_[rs]);
        const uint32_t result = accumulate
            ? product + registers_[rn]
            : product;
        registers_[rd] = result;
        if (setFlags)
            setNz(result);
        return true;
    }

    const bool signedMultiply = (instruction & (1u << 22)) != 0;
    uint64_t result = 0;
    if (signedMultiply) {
        const int64_t product = static_cast<int64_t>(
            static_cast<int32_t>(registers_[rm])) *
            static_cast<int64_t>(static_cast<int32_t>(registers_[rs]));
        result = static_cast<uint64_t>(product);
    } else {
        result = static_cast<uint64_t>(registers_[rm]) * registers_[rs];
    }
    if (accumulate) {
        const uint64_t accumulator =
            (static_cast<uint64_t>(registers_[rd]) << 32) | registers_[rn];
        result += accumulator;
    }
    registers_[rn] = static_cast<uint32_t>(result);
    registers_[rd] = static_cast<uint32_t>(result >> 32);
    if (setFlags) {
        cpsr_ &= ~(CPSR_N | CPSR_Z);
        if (result & (uint64_t{1} << 63)) cpsr_ |= CPSR_N;
        if (result == 0) cpsr_ |= CPSR_Z;
    }
    return true;
}

bool Arm7Tdmi::executeSwap(uint32_t instruction, GbaBus &bus,
                           uint32_t instructionAddress) {
    const bool byteTransfer = (instruction & (1u << 22)) != 0;
    const unsigned rn = (instruction >> 16) & 0xF;
    const unsigned rd = (instruction >> 12) & 0xF;
    const unsigned rm = instruction & 0xF;
    if (rn == 15 || rd == 15 || rm == 15)
        return false;

    const uint32_t address = readRegister(rn, instructionAddress);
    const uint32_t source = registers_[rm];
    if (byteTransfer) {
        const uint8_t oldValue = bus.read8(address);
        bus.write8(address, static_cast<uint8_t>(source));
        registers_[rd] = oldValue;
    } else {
        const uint32_t oldValue = rotateRight(
            bus.read32(address & ~uint32_t{3}), (address & 3) * 8);
        bus.write32(address, source);
        registers_[rd] = oldValue;
    }
    return true;
}

Arm7Tdmi::Operand Arm7Tdmi::decodeOperand2(uint32_t instruction,
                                           uint32_t instructionAddress) const {
    Operand operand{};
    if (instruction & (1u << 25)) {
        const unsigned rotate = ((instruction >> 8) & 0xF) * 2;
        operand.value = rotateRight(instruction & 0xFF, rotate);
        operand.carry = rotate == 0 ? (cpsr_ & CPSR_C) != 0
                                    : (operand.value & 0x80000000) != 0;
        return operand;
    }

    const unsigned rm = instruction & 0xF;
    const unsigned shiftType = (instruction >> 5) & 0x3;
    const bool registerShift = (instruction & (1u << 4)) != 0;
    unsigned amount = registerShift
                          ? (readRegister((instruction >> 8) & 0xF,
                                          instructionAddress) & 0xFF)
                          : ((instruction >> 7) & 0x1F);
    const uint32_t value = rm == 15 && registerShift
        ? instructionAddress + 12
        : readRegister(rm, instructionAddress);
    operand.value = value;
    operand.carry = (cpsr_ & CPSR_C) != 0;

    if (registerShift && amount == 0)
        return operand;
    if (!registerShift && amount == 0) {
        if (shiftType == 1) { // LSR #0 encodes LSR #32
            operand.value = 0;
            operand.carry = (value & 0x80000000) != 0;
        } else if (shiftType == 2) { // ASR #0 encodes ASR #32
            operand.carry = (value & 0x80000000) != 0;
            operand.value = operand.carry ? 0xFFFFFFFF : 0;
        } else if (shiftType == 3) { // RRX
            operand.value = ((cpsr_ & CPSR_C) ? 0x80000000 : 0) | (value >> 1);
            operand.carry = (value & 1) != 0;
        }
        return operand;
    }

    switch (shiftType) {
    case 0: // LSL
        if (amount < 32) {
            operand.carry = ((value >> (32 - amount)) & 1) != 0;
            operand.value = value << amount;
        } else if (amount == 32) {
            operand.carry = (value & 1) != 0;
            operand.value = 0;
        } else {
            operand.carry = false;
            operand.value = 0;
        }
        break;
    case 1: // LSR
        if (amount < 32) {
            operand.carry = ((value >> (amount - 1)) & 1) != 0;
            operand.value = value >> amount;
        } else if (amount == 32) {
            operand.carry = (value & 0x80000000) != 0;
            operand.value = 0;
        } else {
            operand.carry = false;
            operand.value = 0;
        }
        break;
    case 2: // ASR
        if (amount >= 32) {
            operand.carry = (value & 0x80000000) != 0;
            operand.value = operand.carry ? 0xFFFFFFFF : 0;
        } else {
            operand.carry = ((value >> (amount - 1)) & 1) != 0;
            operand.value = static_cast<uint32_t>(
                static_cast<int32_t>(value) >> amount);
        }
        break;
    case 3: // ROR
        if (registerShift && (amount & 31) == 0) {
            operand.value = value;
            operand.carry = (value & 0x80000000) != 0;
        } else {
            amount &= 31;
            operand.value = rotateRight(value, amount);
            operand.carry = (operand.value & 0x80000000) != 0;
        }
        break;
    }
    return operand;
}

void Arm7Tdmi::setFault(const char *reason, uint32_t instructionAddress,
                        uint32_t instruction) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "%s at 0x%08X (instruction 0x%08X)", reason,
                  instructionAddress, instruction);
    fault_ = message;
}

bool Arm7Tdmi::executeDataProcessing(uint32_t instruction,
                                     uint32_t instructionAddress) {
    const unsigned opcode = (instruction >> 21) & 0xF;
    const bool setFlags = (instruction & (1u << 20)) != 0;
    const unsigned rn = (instruction >> 16) & 0xF;
    const unsigned rd = (instruction >> 12) & 0xF;
    const bool registerSpecifiedShift =
        (instruction & (1u << 25)) == 0 &&
        (instruction & (1u << 4)) != 0;
    const uint32_t left = rn == 15 && registerSpecifiedShift
        ? instructionAddress + 12
        : readRegister(rn, instructionAddress);
    const Operand operand = decodeOperand2(instruction, instructionAddress);
    uint32_t result = 0;
    bool writesResult = true;
    bool arithmetic = false;
    switch (opcode) {
    case 0x0: result = left & operand.value; break; // AND
    case 0x1: result = left ^ operand.value; break; // EOR
    case 0x2: result = left - operand.value; arithmetic = true; break; // SUB
    case 0x3: result = operand.value - left; arithmetic = true; break; // RSB
    case 0x4: result = left + operand.value; arithmetic = true; break; // ADD
    case 0x5: { // ADC
        const uint32_t carryIn = (cpsr_ & CPSR_C) ? 1u : 0u;
        result = static_cast<uint32_t>(static_cast<uint64_t>(left) +
                                       operand.value + carryIn);
        arithmetic = true;
        break;
    }
    case 0x6: { // SBC
        const uint32_t borrow = (cpsr_ & CPSR_C) ? 0u : 1u;
        const uint64_t subtrahend = static_cast<uint64_t>(operand.value) + borrow;
        result = static_cast<uint32_t>(static_cast<uint64_t>(left) - subtrahend);
        arithmetic = true;
        break;
    }
    case 0x7: { // RSC
        const uint32_t borrow = (cpsr_ & CPSR_C) ? 0u : 1u;
        const uint64_t subtrahend = static_cast<uint64_t>(left) + borrow;
        result = static_cast<uint32_t>(static_cast<uint64_t>(operand.value) - subtrahend);
        arithmetic = true;
        break;
    }
    case 0x8: result = left & operand.value; writesResult = false; break; // TST
    case 0x9: result = left ^ operand.value; writesResult = false; break; // TEQ
    case 0xA: result = left - operand.value; arithmetic = true; writesResult = false; break; // CMP
    case 0xB: result = left + operand.value; arithmetic = true; writesResult = false; break; // CMN
    case 0xC: result = left | operand.value; break; // ORR
    case 0xD: result = operand.value; break; // MOV
    case 0xE: result = left & ~operand.value; break; // BIC
    case 0xF: result = ~operand.value; break; // MVN
    default: return false;
    }

    const bool exceptionReturn = rd == 15 && setFlags &&
        savedStatusForMode(cpsr_ & 0x1F) != nullptr;
    if (writesResult && rd == 15 && setFlags && !exceptionReturn)
        return false;

    if (writesResult) {
        if (rd == 15)
            pc_ = result & ~uint32_t{3};
        else
            registers_[rd] = result;
    }
    if (setFlags || !writesResult) {
        if (arithmetic) {
            if (opcode == 0x2 || opcode == 0xA)
                setSubFlags(left, operand.value, result);
            else if (opcode == 0x3)
                setSubFlags(operand.value, left, result);
            else if (opcode == 0x5)
                setAddFlags(left, operand.value,
                    static_cast<uint64_t>(left) + operand.value +
                    ((cpsr_ & CPSR_C) ? 1u : 0u));
            else if (opcode == 0x6 || opcode == 0x7) {
                const uint32_t borrow = (cpsr_ & CPSR_C) ? 0u : 1u;
                const uint32_t minuend = opcode == 0x6 ? left : operand.value;
                const uint32_t subtrahendValue = opcode == 0x6
                    ? operand.value : left;
                const uint64_t subtrahend =
                    static_cast<uint64_t>(subtrahendValue) + borrow;
                const int64_t signedResult =
                    static_cast<int64_t>(static_cast<int32_t>(minuend)) -
                    static_cast<int64_t>(static_cast<int32_t>(subtrahendValue)) -
                    borrow;
                cpsr_ &= ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V);
                if (result & 0x80000000) cpsr_ |= CPSR_N;
                if (result == 0) cpsr_ |= CPSR_Z;
                if (static_cast<uint64_t>(minuend) >= subtrahend)
                    cpsr_ |= CPSR_C;
                if (signedResult < std::numeric_limits<int32_t>::min() ||
                    signedResult > std::numeric_limits<int32_t>::max())
                    cpsr_ |= CPSR_V;
            }
            else
                setAddFlags(left, operand.value, static_cast<uint64_t>(left) + operand.value);
        } else {
            setNz(result);
            cpsr_ = (cpsr_ & ~CPSR_C) | (operand.carry ? CPSR_C : 0);
        }
    }
    if (exceptionReturn) {
        const uint32_t *savedStatusPointer =
            savedStatusForMode(cpsr_ & 0x1F);
        if (!savedStatusPointer)
            return false;
        const uint32_t savedStatus = *savedStatusPointer;
        switchMode(savedStatus & 0x1F);
        cpsr_ = savedStatus;
        if (writesResult)
            pc_ = result & ((savedStatus & CPSR_T) ? ~uint32_t{1}
                                                   : ~uint32_t{3});
    }
    return true;
}

bool Arm7Tdmi::executeSingleTransfer(uint32_t instruction, GbaBus &bus,
                                     uint32_t instructionAddress) {
    const bool registerOffset = (instruction & (1u << 25)) != 0;
    const bool preIndex = (instruction & (1u << 24)) != 0;
    const bool addOffset = (instruction & (1u << 23)) != 0;
    const bool byteTransfer = (instruction & (1u << 22)) != 0;
    const bool writeBack = (instruction & (1u << 21)) != 0;
    const bool load = (instruction & (1u << 20)) != 0;
    const unsigned rn = (instruction >> 16) & 0xF;
    const unsigned rd = (instruction >> 12) & 0xF;
    const uint32_t base = readRegister(rn, instructionAddress);
    uint32_t offset = instruction & 0xFFF;
    if (registerOffset) {
        const uint32_t rm = readRegister(instruction & 0xF, instructionAddress);
        const unsigned shift = (instruction >> 7) & 0x1F;
        switch ((instruction >> 5) & 3) {
        case 0: offset = rm << shift; break;
        case 1: offset = shift == 0 ? 0 : rm >> shift; break;
        case 2: offset = shift == 0 ? ((rm & 0x80000000) ? 0xFFFFFFFF : 0)
                                    : static_cast<uint32_t>(static_cast<int32_t>(rm) >> shift); break;
        case 3: offset = shift == 0
            ? (((cpsr_ & CPSR_C) ? 0x80000000u : 0u) | (rm >> 1))
            : rotateRight(rm, shift); break;
        }
    }
    const uint32_t adjusted = addOffset ? base + offset : base - offset;
    const uint32_t address = preIndex ? adjusted : base;
    if (load) {
        uint32_t value = byteTransfer ? bus.read8(address) : bus.read32(address);
        if (!byteTransfer)
            value = rotateRight(value, (address & 3) * 8);
        if (rd == 15)
            pc_ = value & ~uint32_t{3};
        else
            registers_[rd] = value;
    } else {
        const uint32_t value = rd == 15
            ? instructionAddress + 12
            : registers_[rd];
        if (byteTransfer)
            bus.write8(address, static_cast<uint8_t>(value));
        else
            bus.write32(address, value);
    }
    if ((!preIndex || writeBack) && !(load && rn == rd)) {
        if (rn == 15)
            return false;
        registers_[rn] = adjusted;
    }
    return true;
}

bool Arm7Tdmi::executeHalfTransfer(uint32_t instruction, GbaBus &bus,
                                   uint32_t instructionAddress) {
    const bool immediateOffset = (instruction & (1u << 22)) != 0;
    const bool preIndex = (instruction & (1u << 24)) != 0;
    const bool addOffset = (instruction & (1u << 23)) != 0;
    const bool writeBack = (instruction & (1u << 21)) != 0;
    const bool load = (instruction & (1u << 20)) != 0;
    const unsigned rn = (instruction >> 16) & 0xF;
    const unsigned rd = (instruction >> 12) & 0xF;
    const unsigned transferType = (instruction >> 5) & 3;
    uint32_t offset = 0;
    if (immediateOffset)
        offset = ((instruction >> 4) & 0xF0) | (instruction & 0xF);
    else
        offset = readRegister(instruction & 0xF, instructionAddress);
    const uint32_t base = readRegister(rn, instructionAddress);
    const uint32_t adjusted = addOffset ? base + offset : base - offset;
    const uint32_t address = preIndex ? adjusted : base;

    uint32_t value = 0;
    if (load) {
        if (transferType == 1) {
            if (address & 1)
                value = rotateRight(bus.read32(address & ~uint32_t{3}), 8);
            else
                value = bus.read16(address);
        }
        else if (transferType == 2)
            value = static_cast<uint32_t>(static_cast<int8_t>(bus.read8(address)));
        else if (transferType == 3) {
            if (address & 1)
                value = static_cast<uint32_t>(static_cast<int8_t>(bus.read8(address)));
            else
                value = static_cast<uint32_t>(static_cast<int16_t>(bus.read16(address)));
        }
        else
            return false;
        if (rd == 15)
            pc_ = value & ~uint32_t{3};
        else
            registers_[rd] = value;
    } else {
        if (transferType != 1)
            return false;
        bus.write16(address,
                    static_cast<uint16_t>(readRegister(rd, instructionAddress)));
    }
    if ((!preIndex || writeBack) && !(load && rn == rd)) {
        if (rn == 15)
            return false;
        registers_[rn] = adjusted;
    }
    return true;
}

bool Arm7Tdmi::executeBlockTransfer(uint32_t instruction, GbaBus &bus,
                                    uint32_t instructionAddress) {
    const bool preIndex = (instruction & (1u << 24)) != 0;
    const bool increment = (instruction & (1u << 23)) != 0;
    const bool userBank = (instruction & (1u << 22)) != 0;
    const bool writeBack = (instruction & (1u << 21)) != 0;
    const bool load = (instruction & (1u << 20)) != 0;
    const unsigned baseRegister = (instruction >> 16) & 0xF;
    const uint32_t registerList = instruction & 0xFFFF;

    // This HLE core starts in System mode, whose unbanked registers match the
    // User bank. Exception-return forms need banked SPSR state that is not
    // implemented yet, so reject them explicitly instead of silently losing it.
    if (userBank && load && (registerList & (1u << 15)) != 0)
        return false;

    unsigned transferCount = 0;
    for (unsigned reg = 0; reg < 16; ++reg)
        transferCount += (registerList >> reg) & 1u;
    const bool emptyList = transferCount == 0;
    if (emptyList)
        transferCount = 16;

    const uint32_t base = readRegister(baseRegister, instructionAddress);
    const uint32_t adjustedBase = increment
        ? base + 4u * transferCount
        : base - 4u * transferCount;
    uint32_t address = 0;
    if (increment)
        address = base + (preIndex ? 4u : 0u);
    else
        address = base - (4u * transferCount) + (preIndex ? 0u : 4u);

    if (emptyList) {
        if (load)
            pc_ = bus.read32(address) & ~uint32_t{3};
        else
            bus.write32(address, instructionAddress + 12);
    } else {
        unsigned firstRegister = 0;
        while ((registerList & (1u << firstRegister)) == 0)
            ++firstRegister;
        for (unsigned reg = 0; reg < 16; ++reg) {
            if ((registerList & (1u << reg)) == 0)
                continue;
            if (load) {
                const uint32_t value = bus.read32(address);
                if (reg == 15)
                    pc_ = value & ~uint32_t{3};
                else if (userBank)
                    writeUserRegister(reg, value);
                else
                    registers_[reg] = value;
            } else {
                uint32_t value = reg == 15 ? instructionAddress + 12
                    : userBank ? readUserRegister(reg) : registers_[reg];
                if (writeBack && reg == baseRegister &&
                    reg != firstRegister)
                    value = adjustedBase;
                bus.write32(address, value);
            }
            address += 4;
        }
    }

    if (writeBack && baseRegister != 15 &&
        !(load && (registerList & (1u << baseRegister)) != 0)) {
        registers_[baseRegister] = adjustedBase;
    }
    return true;
}

bool Arm7Tdmi::executeArm(uint32_t instruction, GbaBus &bus,
                          uint32_t instructionAddress) {
    if ((instruction & 0x0F000000) == 0x0F000000)
        return executeSoftwareInterrupt(instruction & 0x00FFFFFF, bus, pc_);
    if ((instruction & 0x0FB00FF0) == 0x01000090)
        return executeSwap(instruction, bus, instructionAddress);
    if ((instruction & 0x0FC000F0) == 0x00000090 ||
        (instruction & 0x0F8000F0) == 0x00800090)
        return executeMultiply(instruction);
    if ((instruction & 0x0FBF0FFF) == 0x010F0000 ||
        (instruction & 0x0FB0FFF0) == 0x0120F000 ||
        (instruction & 0x0FB0F000) == 0x0320F000)
        return executeStatusTransfer(instruction, instructionAddress);
    if ((instruction & 0x0FFFFFF0) == 0x012FFF10) { // BX
        const uint32_t target = readRegister(instruction & 0xF, instructionAddress);
        if (target & 1) {
            cpsr_ |= CPSR_T;
            pc_ = target & ~uint32_t{1};
        } else {
            cpsr_ &= ~CPSR_T;
            pc_ = target & ~uint32_t{3};
        }
        return true;
    }
    if ((instruction & 0x0E000000) == 0x0A000000) { // B / BL
        if (instruction & (1u << 24))
            registers_[14] = instructionAddress + 4;
        const int64_t offset = static_cast<int64_t>(signExtend24(instruction)) * 4;
        pc_ = static_cast<uint32_t>(static_cast<int64_t>(instructionAddress) +
                                    8 + offset) & ~uint32_t{3};
        return true;
    }
    if ((instruction & 0x0E000090) == 0x00000090 &&
        (instruction & 0x60) != 0)
        return executeHalfTransfer(instruction, bus, instructionAddress);
    if ((instruction & 0x0E000000) == 0x08000000)
        return executeBlockTransfer(instruction, bus, instructionAddress);
    if ((instruction & 0x0C000000) == 0x04000000)
        return executeSingleTransfer(instruction, bus, instructionAddress);
    if ((instruction & 0x0C000000) == 0)
        return executeDataProcessing(instruction, instructionAddress);
    return false;
}

bool Arm7Tdmi::executeThumb(uint16_t instruction, GbaBus &bus,
                            uint32_t instructionAddress) {
    const auto readReg = [&](unsigned index) {
        if (index == 15)
            return instructionAddress + 4;
        return registers_[index];
    };
    const auto writeReg = [&](unsigned index, uint32_t value) {
        if (index == 15)
            pc_ = value & ~uint32_t{1};
        else
            registers_[index] = value;
    };
    const auto setLogicalFlags = [&](uint32_t value, bool carry) {
        setNz(value);
        cpsr_ = (cpsr_ & ~CPSR_C) | (carry ? CPSR_C : 0);
    };
    const auto loadWord = [&](uint32_t address) {
        return rotateRight(bus.read32(address & ~uint32_t{3}),
                           (address & 3) * 8);
    };
    const auto loadUnsignedHalf = [&](uint32_t address) {
        return (address & 1)
            ? rotateRight(bus.read32(address & ~uint32_t{3}), 8)
            : static_cast<uint32_t>(bus.read16(address));
    };
    const auto loadSignedHalf = [&](uint32_t address) {
        return (address & 1)
            ? static_cast<uint32_t>(static_cast<int8_t>(bus.read8(address)))
            : static_cast<uint32_t>(static_cast<int16_t>(bus.read16(address)));
    };

    if ((instruction & 0xF800) == 0x4800) { // LDR Rd, [PC, #imm]
        const unsigned rd = (instruction >> 8) & 7;
        const uint32_t base = (instructionAddress + 4) & ~uint32_t{3};
        const uint32_t address = base +
            (static_cast<uint32_t>(instruction & 0xFF) << 2);
        registers_[rd] = loadWord(address);
        return true;
    }

    if ((instruction & 0xFC00) == 0x4400) { // High-register ops and BX
        const unsigned operation = (instruction >> 8) & 3;
        const unsigned rd = (instruction & 7) | ((instruction >> 4) & 8);
        const unsigned rs = ((instruction >> 3) & 7) | ((instruction >> 3) & 8);
        const uint32_t left = readReg(rd);
        const uint32_t right = readReg(rs);
        if (operation == 3) {
            if ((instruction & 0xFF87) != 0x4700)
                return false;
            if (right & 1) {
                cpsr_ |= CPSR_T;
                pc_ = right & ~uint32_t{1};
            } else {
                cpsr_ &= ~CPSR_T;
                pc_ = right & ~uint32_t{3};
            }
            return true;
        }
        if (operation == 0) {
            const uint32_t result = left + right;
            writeReg(rd, result);
        } else if (operation == 1) {
            setSubFlags(left, right, left - right);
        } else {
            writeReg(rd, right);
        }
        return true;
    }

    if ((instruction & 0xE000) == 0x0000) {
        const unsigned shiftType = (instruction >> 11) & 3;
        const unsigned rd = instruction & 7;
        const unsigned rs = (instruction >> 3) & 7;
        if (shiftType != 3) { // LSL/LSR/ASR immediate
            unsigned amount = (instruction >> 6) & 0x1F;
            const uint32_t value = registers_[rs];
            uint32_t result = value;
            bool carry = (cpsr_ & CPSR_C) != 0;
            if (shiftType == 0) {
                if (amount != 0) {
                    carry = ((value >> (32 - amount)) & 1) != 0;
                    result = value << amount;
                }
            } else if (shiftType == 1) {
                if (amount == 0)
                    amount = 32;
                carry = ((value >> (amount - 1)) & 1) != 0;
                result = amount == 32 ? 0 : value >> amount;
            } else {
                if (amount == 0)
                    amount = 32;
                carry = ((value >> (amount - 1)) & 1) != 0;
                result = static_cast<uint32_t>(
                    static_cast<int32_t>(value) >> (amount == 32 ? 31 : amount));
            }
            registers_[rd] = result;
            setLogicalFlags(result, carry);
            return true;
        }

        const bool immediate = (instruction & (1u << 10)) != 0;
        const bool subtract = (instruction & (1u << 9)) != 0;
        const uint32_t left = registers_[rs];
        const uint32_t operand = immediate ? ((instruction >> 6) & 7)
                                           : registers_[(instruction >> 6) & 7];
        const uint32_t result = subtract ? left - operand : left + operand;
        registers_[rd] = result;
        if (subtract)
            setSubFlags(left, operand, result);
        else
            setAddFlags(left, operand, static_cast<uint64_t>(left) + operand);
        return true;
    }

    if ((instruction & 0xE000) == 0x2000) { // MOV/CMP/ADD/SUB immediate
        const unsigned operation = (instruction >> 11) & 3;
        const unsigned rd = (instruction >> 8) & 7;
        const uint32_t immediate = instruction & 0xFF;
        const uint32_t left = registers_[rd];
        if (operation == 0) {
            registers_[rd] = immediate;
            setNz(immediate);
        } else if (operation == 1) {
            setSubFlags(left, immediate, left - immediate);
        } else if (operation == 2) {
            const uint32_t result = left + immediate;
            registers_[rd] = result;
            setAddFlags(left, immediate, static_cast<uint64_t>(left) + immediate);
        } else {
            const uint32_t result = left - immediate;
            registers_[rd] = result;
            setSubFlags(left, immediate, result);
        }
        return true;
    }

    if ((instruction & 0xFC00) == 0x4000) { // Thumb ALU operations
        const unsigned operation = (instruction >> 6) & 0xF;
        const unsigned rs = (instruction >> 3) & 7;
        const unsigned rd = instruction & 7;
        const uint32_t left = registers_[rd];
        const uint32_t right = registers_[rs];
        uint32_t result = left;
        bool writesResult = true;
        bool logical = false;
        switch (operation) {
        case 0x0: result = left & right; logical = true; break; // AND
        case 0x1: result = left ^ right; logical = true; break; // EOR
        case 0x2: { // LSL register
            const unsigned amount = right & 0xFF;
            bool carry = (cpsr_ & CPSR_C) != 0;
            if (amount > 0 && amount < 32) {
                carry = ((left >> (32 - amount)) & 1) != 0;
                result = left << amount;
            } else if (amount == 32) {
                carry = (left & 1) != 0;
                result = 0;
            } else if (amount > 32) {
                carry = false;
                result = 0;
            }
            registers_[rd] = result;
            setLogicalFlags(result, carry);
            return true;
        }
        case 0x3: { // LSR register
            const unsigned amount = right & 0xFF;
            bool carry = (cpsr_ & CPSR_C) != 0;
            if (amount > 0 && amount < 32) {
                carry = ((left >> (amount - 1)) & 1) != 0;
                result = left >> amount;
            } else if (amount == 32) {
                carry = (left & 0x80000000) != 0;
                result = 0;
            } else if (amount > 32) {
                carry = false;
                result = 0;
            }
            registers_[rd] = result;
            setLogicalFlags(result, carry);
            return true;
        }
        case 0x4: { // ASR register
            const unsigned amount = right & 0xFF;
            bool carry = (cpsr_ & CPSR_C) != 0;
            if (amount >= 32) {
                carry = (left & 0x80000000) != 0;
                result = carry ? 0xFFFFFFFF : 0;
            } else if (amount != 0) {
                carry = ((left >> (amount - 1)) & 1) != 0;
                result = static_cast<uint32_t>(static_cast<int32_t>(left) >> amount);
            }
            registers_[rd] = result;
            setLogicalFlags(result, carry);
            return true;
        }
        case 0x5: { // ADC
            const uint32_t carryIn = (cpsr_ & CPSR_C) ? 1 : 0;
            const uint64_t wide = static_cast<uint64_t>(left) + right + carryIn;
            result = static_cast<uint32_t>(wide);
            registers_[rd] = result;
            setAddFlags(left, right + carryIn, wide);
            return true;
        }
        case 0x6: { // SBC
            const uint32_t borrow = (cpsr_ & CPSR_C) ? 0 : 1;
            const uint32_t operand = right + borrow;
            result = left - operand;
            registers_[rd] = result;
            setSubFlags(left, operand, result);
            return true;
        }
        case 0x7: { // ROR register
            const unsigned amount = right & 0xFF;
            bool carry = (cpsr_ & CPSR_C) != 0;
            if (amount != 0) {
                result = rotateRight(left, amount);
                carry = (result & 0x80000000) != 0;
            }
            registers_[rd] = result;
            setLogicalFlags(result, carry);
            return true;
        }
        case 0x8: result = left & right; writesResult = false; logical = true; break; // TST
        case 0x9: result = 0 - right; setSubFlags(0, right, result); break; // NEG
        case 0xA: setSubFlags(left, right, left - right); return true; // CMP
        case 0xB: setAddFlags(left, right, static_cast<uint64_t>(left) + right); return true; // CMN
        case 0xC: result = left | right; logical = true; break; // ORR
        case 0xD: result = static_cast<uint32_t>(static_cast<uint64_t>(left) * right); logical = true; break; // MUL
        case 0xE: result = left & ~right; logical = true; break; // BIC
        case 0xF: result = ~right; logical = true; break; // MVN
        }
        if (writesResult)
            registers_[rd] = result;
        if (logical)
            setNz(result);
        return true;
    }

    if ((instruction & 0xF000) == 0x5000) { // Register-offset memory ops
        const unsigned operation = (instruction >> 9) & 7;
        const unsigned ro = (instruction >> 6) & 7;
        const unsigned rb = (instruction >> 3) & 7;
        const unsigned rd = instruction & 7;
        const uint32_t address = registers_[rb] + registers_[ro];
        switch (operation) {
        case 0: bus.write32(address, registers_[rd]); break;
        case 1: bus.write16(address, static_cast<uint16_t>(registers_[rd])); break;
        case 2: bus.write8(address, static_cast<uint8_t>(registers_[rd])); break;
        case 3: registers_[rd] = static_cast<uint32_t>(static_cast<int8_t>(bus.read8(address))); break;
        case 4: registers_[rd] = loadWord(address); break;
        case 5: registers_[rd] = loadUnsignedHalf(address); break;
        case 6: registers_[rd] = bus.read8(address); break;
        case 7: registers_[rd] = loadSignedHalf(address); break;
        }
        return true;
    }

    if ((instruction & 0xE000) == 0x6000) { // Immediate word/byte transfer
        const bool byteTransfer = (instruction & (1u << 12)) != 0;
        const bool load = (instruction & (1u << 11)) != 0;
        const unsigned offset = (instruction >> 6) & 0x1F;
        const unsigned rb = (instruction >> 3) & 7;
        const unsigned rd = instruction & 7;
        const uint32_t address = registers_[rb] +
            (byteTransfer ? offset : (offset << 2));
        if (load)
            registers_[rd] = byteTransfer ? bus.read8(address) : loadWord(address);
        else if (byteTransfer)
            bus.write8(address, static_cast<uint8_t>(registers_[rd]));
        else
            bus.write32(address, registers_[rd]);
        return true;
    }

    if ((instruction & 0xF000) == 0x8000) { // Immediate halfword transfer
        const bool load = (instruction & (1u << 11)) != 0;
        const unsigned offset = static_cast<unsigned>(
            ((instruction >> 6) & 0x1F) << 1);
        const unsigned rb = (instruction >> 3) & 7;
        const unsigned rd = instruction & 7;
        const uint32_t address = registers_[rb] + offset;
        if (load)
            registers_[rd] = loadUnsignedHalf(address);
        else
            bus.write16(address, static_cast<uint16_t>(registers_[rd]));
        return true;
    }

    if ((instruction & 0xF000) == 0x9000) { // SP-relative word transfer
        const bool load = (instruction & (1u << 11)) != 0;
        const unsigned rd = (instruction >> 8) & 7;
        const uint32_t address = registers_[13] +
            (static_cast<uint32_t>(instruction & 0xFF) << 2);
        if (load)
            registers_[rd] = loadWord(address);
        else
            bus.write32(address, registers_[rd]);
        return true;
    }

    if ((instruction & 0xF000) == 0xA000) { // ADD Rd, PC/SP, #imm
        const unsigned rd = (instruction >> 8) & 7;
        const uint32_t base = (instruction & (1u << 11))
                                  ? registers_[13]
                                  : ((instructionAddress + 4) & ~uint32_t{3});
        registers_[rd] = base +
            (static_cast<uint32_t>(instruction & 0xFF) << 2);
        return true;
    }

    if ((instruction & 0xFF00) == 0xB000) { // ADD/SUB SP, #imm
        const uint32_t offset = static_cast<uint32_t>(instruction & 0x7F) << 2;
        if (instruction & (1u << 7))
            registers_[13] -= offset;
        else
            registers_[13] += offset;
        return true;
    }

    const uint16_t stackOpcode = instruction & 0xFE00;
    if (stackOpcode == 0xB400 || stackOpcode == 0xBC00) { // PUSH / POP
        const bool pop = stackOpcode == 0xBC00;
        const bool extra = (instruction & (1u << 8)) != 0;
        const uint8_t list = static_cast<uint8_t>(instruction);
        uint32_t address = registers_[13];
        if (!pop) {
            unsigned count = extra ? 1 : 0;
            for (unsigned i = 0; i < 8; ++i)
                count += (list >> i) & 1;
            address -= count * 4;
            uint32_t cursor = address;
            for (unsigned i = 0; i < 8; ++i) {
                if (list & (1u << i)) {
                    bus.write32(cursor, registers_[i]);
                    cursor += 4;
                }
            }
            if (extra) {
                bus.write32(cursor, registers_[14]);
                cursor += 4;
            }
            registers_[13] = address;
        } else {
            uint32_t cursor = address;
            for (unsigned i = 0; i < 8; ++i) {
                if (list & (1u << i)) {
                    registers_[i] = loadWord(cursor);
                    cursor += 4;
                }
            }
            if (extra) {
                const uint32_t target = loadWord(cursor);
                cursor += 4;
                cpsr_ |= CPSR_T;
                pc_ = target & ~uint32_t{1};
            }
            registers_[13] = cursor;
        }
        return true;
    }

    if ((instruction & 0xF000) == 0xC000) { // STMIA / LDMIA
        const bool load = (instruction & (1u << 11)) != 0;
        const unsigned rb = (instruction >> 8) & 7;
        const uint8_t list = static_cast<uint8_t>(instruction);
        uint32_t address = registers_[rb];
        if (list == 0) {
            if (load) {
                cpsr_ |= CPSR_T;
                pc_ = loadWord(address) & ~uint32_t{1};
            } else {
                bus.write32(address, instructionAddress + 6);
            }
            registers_[rb] += 0x40;
        } else {
            unsigned count = 0;
            for (unsigned i = 0; i < 8; ++i)
                count += (list >> i) & 1u;
            unsigned firstRegister = 0;
            while ((list & (1u << firstRegister)) == 0)
                ++firstRegister;
            const uint32_t adjustedBase = registers_[rb] + 4 * count;
            for (unsigned i = 0; i < 8; ++i) {
                if (!(list & (1u << i)))
                    continue;
                if (load)
                    registers_[i] = loadWord(address);
                else {
                    const uint32_t value = i == rb && i != firstRegister
                        ? adjustedBase
                        : registers_[i];
                    bus.write32(address, value);
                }
                address += 4;
            }
            if (!(load && (list & (1u << rb)) != 0))
                registers_[rb] = address;
        }
        return true;
    }

    if ((instruction & 0xF000) == 0xD000) { // Conditional branch / SWI
        const unsigned condition = (instruction >> 8) & 0xF;
        if (condition == 0xF)
            return executeSoftwareInterrupt(instruction & 0xFF, bus, pc_);
        if (condition == 0xE)
            return false;
        if (conditionPassed(condition)) {
            int32_t offset = static_cast<int8_t>(instruction & 0xFF);
            pc_ = static_cast<uint32_t>(static_cast<int64_t>(instructionAddress) +
                                        4 + static_cast<int64_t>(offset) * 2) &
                  ~uint32_t{1};
        }
        return true;
    }

    if ((instruction & 0xF800) == 0xE000) { // Unconditional branch
        int32_t offset = static_cast<int32_t>(instruction & 0x7FF);
        if (offset & 0x400)
            offset |= ~0x7FF;
        pc_ = static_cast<uint32_t>(static_cast<int64_t>(instructionAddress) +
                                    4 + static_cast<int64_t>(offset) * 2) &
              ~uint32_t{1};
        return true;
    }

    if ((instruction & 0xF800) == 0xF000) { // First half of BL
        int32_t offset = static_cast<int32_t>(instruction & 0x7FF);
        if (offset & 0x400)
            offset |= ~0x7FF;
        registers_[14] = static_cast<uint32_t>(
            static_cast<int64_t>(instructionAddress + 4) +
            static_cast<int64_t>(offset) * 4096);
        return true;
    }
    if ((instruction & 0xF800) == 0xF800) { // Second half of BL
        const uint32_t target = registers_[14] +
            (static_cast<uint32_t>(instruction & 0x7FF) << 1);
        registers_[14] = (instructionAddress + 2) | 1;
        pc_ = target & ~uint32_t{1};
        return true;
    }

    return false;
}

Arm7Tdmi::StepResult Arm7Tdmi::step(GbaBus &bus) {
    if (!fault_.empty())
        return {};
    if (irqActive_ && pc_ == IRQ_RETURN_SENTINEL) {
        finishIrqReturn();
        return {true, 1};
    }
    if (halted_)
        return {true, 1};

    const uint32_t instructionAddress = pc_;
    if (thumbState()) {
        const uint16_t instruction = bus.read16(instructionAddress);
        pc_ += 2;
        if (!executeThumb(instruction, bus, instructionAddress)) {
            setFault("Unsupported Thumb instruction", instructionAddress,
                     instruction);
            return {};
        }
    } else {
        const uint32_t instruction = bus.read32(instructionAddress);
        const unsigned condition = instruction >> 28;
        pc_ += 4;
        if (condition == 0xF) {
            setFault("Unconditional ARM extension is not implemented",
                     instructionAddress, instruction);
            return {};
        }
        if (conditionPassed(condition)) {
            if (!executeArm(instruction, bus, instructionAddress)) {
                setFault("Unsupported ARM instruction", instructionAddress,
                         instruction);
                return {};
            }
        }
    }
    return {true, 1};
}

} // namespace LOTUSGBA
