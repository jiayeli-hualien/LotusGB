#ifndef LOTUSGBA_ARM7TDMI_HPP
#define LOTUSGBA_ARM7TDMI_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace LOTUSGBA {

class GbaBus {
public:
    virtual ~GbaBus() = default;
    virtual uint8_t read8(uint32_t address) const = 0;
    virtual uint16_t read16(uint32_t address) const = 0;
    virtual uint32_t read32(uint32_t address) const = 0;
    virtual void write8(uint32_t address, uint8_t value) = 0;
    virtual void write16(uint32_t address, uint16_t value) = 0;
    virtual void write32(uint32_t address, uint32_t value) = 0;
    virtual void registerRamReset(uint8_t flags) = 0;
    virtual bool hasBios() const { return false; }
    virtual void beginCpuStep() {}
    virtual uint32_t endCpuStep(uint32_t baseCycles) { return baseCycles; }
};

class Arm7Tdmi {
public:
    struct StepResult {
        bool success = false;
        uint32_t cycles = 0;
    };

    void reset(uint32_t entryPoint = 0x08000000,
               uint32_t stackPointer = 0x03007F00,
               uint32_t initialCpsr = 0xDF,
               uint32_t irqStackPointer = 0,
               uint32_t supervisorStackPointer = 0);
    StepResult step(GbaBus &bus);
    bool irqDisabled() const { return (cpsr_ & CPSR_I) != 0; }
    bool halted() const { return halted_; }
    void wakeFromInterrupts(uint16_t flags, uint16_t enable);
    bool enterIrq(uint32_t handlerAddress);
    bool enterIrqVector();
    bool enterSwiVector(uint32_t returnAddress);

    uint32_t programCounter() const { return pc_; }
    uint32_t registerValue(unsigned index) const;
    uint32_t currentMode() const { return cpsr_ & 0x1F; }
    const std::string &fault() const { return fault_; }
    bool thumbState() const { return (cpsr_ & CPSR_T) != 0; }

private:
    static constexpr uint32_t CPSR_N = 1u << 31;
    static constexpr uint32_t CPSR_Z = 1u << 30;
    static constexpr uint32_t CPSR_C = 1u << 29;
    static constexpr uint32_t CPSR_V = 1u << 28;
    static constexpr uint32_t CPSR_T = 1u << 5;
    static constexpr uint32_t CPSR_I = 1u << 7;
    static constexpr uint32_t IRQ_RETURN_SENTINEL = 0x00000018;

    struct Operand {
        uint32_t value = 0;
        bool carry = false;
    };

    struct HleIrqFrame {
        std::array<uint32_t, 5> registers;
        uint32_t returnPc;
        uint32_t returnStatus;
        std::optional<uint16_t> suspendedWaitMask;
    };

    uint32_t readRegister(unsigned index, uint32_t instructionAddress) const;
    Operand decodeOperand2(uint32_t instruction, uint32_t instructionAddress) const;
    bool conditionPassed(unsigned condition) const;
    void setNz(uint32_t result);
    void setAddFlags(uint32_t left, uint32_t right, uint64_t result);
    void setSubFlags(uint32_t left, uint32_t right, uint32_t result);
    void switchMode(uint32_t mode);
    uint32_t readUserRegister(unsigned index) const;
    void writeUserRegister(unsigned index, uint32_t value);
    static int exceptionModeIndex(uint32_t mode);
    uint32_t *savedStatusForMode(uint32_t mode);
    void finishIrqReturn();
    bool executeStatusTransfer(uint32_t instruction,
                               uint32_t instructionAddress);
    bool executeSoftwareInterrupt(uint32_t service, GbaBus &bus,
                                  uint32_t returnAddress);
    bool executeMultiply(uint32_t instruction);
    bool executeSwap(uint32_t instruction, GbaBus &bus,
                     uint32_t instructionAddress);
    bool executeArm(uint32_t instruction, GbaBus &bus,
                    uint32_t instructionAddress);
    bool executeThumb(uint16_t instruction, GbaBus &bus,
                      uint32_t instructionAddress);
    bool executeDataProcessing(uint32_t instruction,
                               uint32_t instructionAddress);
    bool executeSingleTransfer(uint32_t instruction, GbaBus &bus,
                               uint32_t instructionAddress);
    bool executeHalfTransfer(uint32_t instruction, GbaBus &bus,
                             uint32_t instructionAddress);
    bool executeBlockTransfer(uint32_t instruction, GbaBus &bus,
                              uint32_t instructionAddress);
    void setFault(const char *reason, uint32_t instructionAddress,
                  uint32_t instruction);

    std::array<uint32_t, 16> registers_{};
    std::array<uint32_t, 7> userBank_{};
    std::array<uint32_t, 7> fiqBank_{};
    std::array<std::array<uint32_t, 2>, 4> exceptionRegisterBanks_{};
    std::array<uint32_t, 5> savedStatusBanks_{};
    std::vector<HleIrqFrame> hleIrqFrames_;
    uint32_t pc_ = 0x08000000;
    uint32_t cpsr_ = 0x1F;
    bool halted_ = false;
    bool haltOnAnyEnabledInterrupt_ = false;
    uint16_t haltInterruptMask_ = 0;
    std::string fault_;
};

} // namespace LOTUSGBA

#endif // LOTUSGBA_ARM7TDMI_HPP
