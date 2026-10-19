#include <iostream>
#include <cstddef>
#include <cstdint>
#include "include/instSubFunc.hpp"
#include "include/instSubFuncCommon.hpp"
#include "include/instruction.hpp"
#include "include/instSubFuncMacro.hpp"
#include "include/gb_reg.hpp"
#include "include/instSubFuncInlineFunc.hpp"

namespace LOTUSGB {

static uint8_t* getRegLHS(const uint8_t &op, Reg *pReg) {
    constexpr uint8_t MASK = 0x3F;
    const int num = ((op&MASK)>>3U);
    auto ret = _getReg(num, pReg);
    if (!ret)
        std::cerr << "invalid OP code, reg not support" << (int)op << std::endl;
    return ret;
}

SUB_FUNC_IMPL(NOT_SUPPORT) {
    // TDOO: throw exception
    std::cerr << "Op code " << std::hex << pInstState->opcode
              << "is not supported" << std::endl;
}

SUB_FUNC_IMPL(LDRR) {
    // TODO: error handling
    const uint8_t &op = pInstState->opcode;
    uint8_t *lhs = getRegLHS(op, pReg);
    uint8_t *rhs = getRegRHS(op, pReg);
    if (lhs && rhs)
        *lhs = *rhs;
}

SUB_FUNC_IMPL(LD_SP_HL) {
    pInstState->memMode = MEM_MODE_SLEEP;
    pReg->getRefSP() = pReg->getRefHL();
}

static uint16_t& _getPushPopReg16(uint8_t op, Reg *pReg) {
    switch((op >> 4) & 0x03) {
        case 0: return pReg->getRefBC();
        case 1: return pReg->getRefDE();
        case 2: return pReg->getRefHL();
        case 3:
        default:
            return pReg->getRefAF();
    }
}

SUB_FUNC_IMPL(PUSH_LD_A16_RR) {
    pInstState->memMode = MEM_MODE_SLEEP;
    pInstState->a16Addr = _getPushPopReg16(pInstState->opcode, pReg);
}

SUB_FUNC_IMPL(POP_LD_R16_MEM16) {
    pInstState->memMode = MEM_MODE_FETCH;
    _getPushPopReg16(pInstState->opcode, pReg) = pInstState->a16Addr;
    // The low, unused bits of F are always zero on the Game Boy CPU.
    pReg->getRefF() = pReg->getRefF() & 0xF0;
}

SUB_FUNC_IMPL(MemReadPC) {
    // notify CPU to read immediate value
    pInstState->memMode = MEM_MODE_READ;
    pInstState->memAddr = pReg->getRefPC()++;
}

static uint16_t& _getIndreictRegBC_DE(InstState *pInstState, Reg *pReg) {
    switch(pInstState->opcode) {
        case 0x02:
        case 0x0A:
            return pReg->getRefBC();
        case 0x12:
        case 0x1A:
        default:
            return pReg->getRefDE();
    }
}

static uint16_t& _getCommonReg16(uint8_t opcode, Reg *pReg) {
    switch((opcode >> 4)&0x3) {
        case 0: return pReg->getRefBC();
        case 1: return pReg->getRefDE();
        case 2: return pReg->getRefHL();
        case 3:
        default:
            return pReg->getRefSP();
    }
}



SUB_FUNC_IMPL(MemReadIndirectHLDec) {
    pInstState->memMode = MEM_MODE_READ;
    pInstState->memAddr = pReg->getRefHL()--;
}

SUB_FUNC_IMPL(MemReadIndirectHLInc) {
    pInstState->memMode = MEM_MODE_READ;
    pInstState->memAddr = pReg->getRefHL()++;
}

SUB_FUNC_IMPL(MemReadIndirectBC_DE) {
    pInstState->memMode = MEM_MODE_READ;
    pInstState->memAddr = _getIndreictRegBC_DE(pInstState, pReg);
}

SUB_FUNC_IMPL(MemReadIndirectA16) {
    pInstState->memMode = MEM_MODE_READ;
    pInstState->memAddr = pInstState->a16Addr;
}

SUB_FUNC_IMPL(ReadHighC) {
    pInstState->memMode = MEM_MODE_READ;
    pInstState->memAddr = 0xF0 | pReg->getRefC();
}

SUB_FUNC_IMPL(ReadA16LSB) {
    pInstState->memMode = MEM_MODE_READ_ADDR_LSB;
    pInstState->memAddr = pReg->getRefPC()++;
}

SUB_FUNC_IMPL(ReadA16MSB) {
    pInstState->memMode = MEM_MODE_READ_ADDR_MSB;
    pInstState->memAddr = pReg->getRefPC()++;
}

SUB_FUNC_IMPL(MemWriteIndirectHL) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = pReg->getRefHL();
    uint8_t *rhs = getRegRHS(pInstState->opcode, pReg);
    if (rhs)
        pInstState->memValue = *rhs;
}

SUB_FUNC_IMPL(MemWriteIndirectHLDec) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = pReg->getRefHL()--;
    pInstState->memValue = pReg->getRefA();
}

SUB_FUNC_IMPL(MemWriteIndirectHLInc) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = pReg->getRefHL()++;
    pInstState->memValue = pReg->getRefA();
}

SUB_FUNC_IMPL(MemWriteIndirectBC_DE) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = _getIndreictRegBC_DE(pInstState, pReg);
    pInstState->memValue = pReg->getRefA();
}

SUB_FUNC_IMPL(MemWriteIndirectA16) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = pInstState->a16Addr;
    pInstState->memValue = pReg->getRefA();
}

SUB_FUNC_IMPL(MemWriteSPA16LSB) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = pInstState->a16Addr;
    pInstState->memValue = (pReg->getRefSP()) & 0xFF;
}

SUB_FUNC_IMPL(MemWriteSPA16MSB) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = pInstState->a16Addr + 1;
    pInstState->memValue = (pReg->getRefSP()>>8) & 0xFF;
}

SUB_FUNC_IMPL(WriteHighC) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = HIGH_MEM_ADDR | pReg->getRefC();
    pInstState->memValue = pReg->getRefA();
}

SUB_FUNC_IMPL(LD_R_MEMVAL) {
    uint8_t *lhs = getRegLHS(pInstState->opcode, pReg);
    if (lhs)
        *lhs = pInstState->memValue;
}

SUB_FUNC_IMPL(LD_A_MEMVAL) {
    pReg->getRefA() = pInstState->memValue;
}

SUB_FUNC_IMPL(LD_R16_MEM16) {
    _getCommonReg16(pInstState->opcode, pReg) = pInstState->a16Addr;
}

SUB_FUNC_IMPL(PUSH_A16_MSB) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = --pReg->getRefSP();
    pInstState->memValue = (pInstState->a16Addr>>8)&0xFF;
}

SUB_FUNC_IMPL(PUSH_A16_LSB) {
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = --pReg->getRefSP();
    pInstState->memValue = (pInstState->a16Addr)&0xFF;
}

SUB_FUNC_IMPL(POP_LD_A16_LSB) {
    pInstState->memMode = MEM_MODE_READ_ADDR_LSB;
    pInstState->memAddr = pReg->getRefSP()++;
}

SUB_FUNC_IMPL(POP_LD_A16_MSB) {
    pInstState->memMode = MEM_MODE_READ_ADDR_MSB;
    pInstState->memAddr = pReg->getRefSP()++;
}

SUB_FUNC_IMPL(AddA_R) {
    uint8_t *rhs = getRegRHS(pInstState->opcode, pReg);
    if (!rhs)
        return;
    const int result = pReg->getRefA() + *rhs;
    GB_Flag flag = {};

    flag.Z = (!GET_LSB(result));
    flag.N = false;
    flag.H = (HALF_OP_HELPER(pReg->getRefA(), +, *rhs) >
             HALF_UPPER_BOUND);
    flag.C = result > BYTE_UPPER_BOUND;
    pReg->setFlag(flag);
    pReg->getRefA() = GET_LSB(result);
}

SUB_FUNC_IMPL(AddA_MEMVAL) {
    const int result = pReg->getRefA() + pInstState->memValue;
    GB_Flag flag = {};

    flag.Z = (!GET_LSB(result));
    flag.N = false;
    flag.H = (HALF_OP_HELPER(pReg->getRefA(), +, pInstState->memValue) >
             HALF_UPPER_BOUND);
    flag.C = result > BYTE_UPPER_BOUND;
    pReg->setFlag(flag);
    pReg->getRefA() = GET_LSB(result);
}

// TODO: refactor?
SUB_FUNC_IMPL(AddA_R_Carry) {
    uint8_t *rhs = getRegRHS(pInstState->opcode, pReg);
    if (!rhs)
        return;
    GB_Flag flag = {};
    pReg->getFlag(flag);

    const int vc = flag.C ? 1 : 0;
    const int result = pReg->getRefA() + *rhs + vc;
    flag.Z = (!GET_LSB(result));
    flag.N = false;
    flag.H = (HALF_OP_HELPER(pReg->getRefA(), +, *rhs) + vc >
             HALF_UPPER_BOUND);
    flag.C = result > BYTE_UPPER_BOUND;
    pReg->setFlag(flag);
    pReg->getRefA() = GET_LSB(result);
}

SUB_FUNC_IMPL(AddA_MEMVAL_Carry) {
    GB_Flag flag = {};
    pReg->getFlag(flag);

    const int vc = flag.C ? 1 : 0;
    const int result = pReg->getRefA() + pInstState->memValue + vc;
    flag.Z = (!GET_LSB(result));
    flag.N = false;
    flag.H = (HALF_OP_HELPER(pReg->getRefA(), +, pInstState->memValue) + vc >
             HALF_UPPER_BOUND);
    flag.C = result > BYTE_UPPER_BOUND;
    pReg->setFlag(flag);
    pReg->getRefA() = GET_LSB(result);
}

struct SubArgs {
    int rhs = 0U;
    bool useCarry = false;
    bool updateA = true;
    SubArgs() {};
    SubArgs(int iRhs, bool iUseCarry, bool iUpdateA)
    : rhs(iRhs), useCarry(iUseCarry), updateA(iUpdateA) {};
};

static inline void _subCommon(const SubArgs &args, SUB_FUNC_PARAMS) {
    GB_Flag flag = {};
    pReg->getFlag(flag);

    const int vc = args.useCarry && flag.C ? 1 : 0;
    const int result = pReg->getRefA() - (args.rhs + vc);
    flag.Z = (!GET_LSB(result));
    flag.N = true;
    flag.H = GET_HALF_INT(args.rhs) + vc > GET_HALF_INT(pReg->getRefA());
    flag.C = result < 0;
    pReg->setFlag(flag);
    pReg->getRefA() = (args.updateA ? GET_LSB(result) : pReg->getRefA());
}

SUB_FUNC_IMPL(SubA_R) {
    if (uint8_t *rhs = getRegRHS(pInstState->opcode, pReg))
        _subCommon(SubArgs(*rhs, false, true), SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(SubA_MEMVAL) {
    _subCommon(SubArgs(pInstState->memValue, false, true), SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(SubA_R_Carry) {
    if (uint8_t *rhs = getRegRHS(pInstState->opcode, pReg))
        _subCommon(SubArgs(*rhs, true, true), SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(SubA_MEMVAL_Carry) {
    _subCommon(SubArgs(pInstState->memValue, true, true), SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(CmpA_R) {
    if (uint8_t *rhs = getRegRHS(pInstState->opcode, pReg))
        _subCommon(SubArgs(*rhs, false, false), SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(CmpA_MEMVAL) {
    _subCommon(SubArgs(pInstState->memValue, false, false), SUB_FUNC_ARGS);
}

static inline void _incUint8Common(uint8_t *lhs, SUB_FUNC_PARAMS) {
    GB_Flag flag = {};
    pReg->getFlag(flag);
    const uint8_t tmp = (*lhs)++;
    flag.Z = (!(*lhs));
    flag.N = false;
    flag.H = (HALF_OP_HELPER(tmp, +, 1) > HALF_UPPER_BOUND);
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(IncR) {
    if (uint8_t *lhs = getRegLHS(pInstState->opcode, pReg))
        _incUint8Common(lhs, SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(MemIncWriteIndirectHL) {
    _incUint8Common(&pInstState->memValue, SUB_FUNC_ARGS);
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = pReg->getRefHL();
}

static inline void _decUint8Common(uint8_t *lhs, SUB_FUNC_PARAMS) {
    GB_Flag flag = {};
    pReg->getFlag(flag);
    const uint8_t tmp = (*lhs)--;
    flag.Z = (!(*lhs));
    flag.N = true;
    flag.H = (!GET_HALF_INT(tmp)); // lower 4bit < 1
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(DecR) {
    if (uint8_t *lhs = getRegLHS(pInstState->opcode, pReg))
        _decUint8Common(lhs, SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(MemDecWriteIndirectHL) {
    _decUint8Common(&pInstState->memValue, SUB_FUNC_ARGS);
    pInstState->memMode = MEM_MODE_WRITE;
    pInstState->memAddr = pReg->getRefHL();
}


static inline void _andUint8Common(uint8_t *rhs, SUB_FUNC_PARAMS) {
    pReg->getRefA() = GET_LSB(pReg->getRefA() & (*rhs));
    GB_Flag flag;
    flag.Z = (!pReg->getRefA());
    flag.N = false;
    flag.H = true;
    flag.C = false;
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(AndA_R) {
    if (uint8_t *rhs = getRegRHS(pInstState->opcode, pReg))
        _andUint8Common(rhs, SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(AndA_MEMVAL) {
    _andUint8Common(&pInstState->memValue, SUB_FUNC_ARGS);
}


static inline void _orUint8Common(uint8_t *rhs, SUB_FUNC_PARAMS) {
    pReg->getRefA() = GET_LSB(pReg->getRefA() | (*rhs));
    GB_Flag flag;
    flag.Z = (!pReg->getRefA());
    flag.N = false;
    flag.H = false;
    flag.C = false;
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(OrA_R) {
    if (uint8_t *rhs = getRegRHS(pInstState->opcode, pReg))
        _orUint8Common(rhs, SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(OrA_MEMVAL) {
    _orUint8Common(&pInstState->memValue, SUB_FUNC_ARGS);
}

static inline void _xorUint8Common(uint8_t *rhs, SUB_FUNC_PARAMS) {
    pReg->getRefA() = GET_LSB(pReg->getRefA() ^ (*rhs));
    GB_Flag flag;
    flag.Z = (!pReg->getRefA());
    flag.N = false;
    flag.H = false;
    flag.C = false;
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(XorA_R) {
    if (uint8_t *rhs = getRegRHS(pInstState->opcode, pReg))
        _xorUint8Common(rhs, SUB_FUNC_ARGS);
}
SUB_FUNC_IMPL(XorA_MEMVAL){
    _xorUint8Common(&pInstState->memValue, SUB_FUNC_ARGS);
}

SUB_FUNC_IMPL(CCF) {
    GB_Flag flag;
    pReg->getFlag(flag);
    flag.N = false;
    flag.H = false;
    flag.C = !flag.C;
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(SCF) {
    GB_Flag flag;
    pReg->getFlag(flag);
    flag.N = false;
    flag.H = false;
    flag.C = true;
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(DAA) {
    GB_Flag flag;
    pReg->getFlag(flag);

    uint8_t adjustment = 0;
    if (flag.N) {
        if (flag.H)
            adjustment |= 0x06;
        if (flag.C)
            adjustment |= 0x60;
        pReg->getRefA() = static_cast<uint8_t>(pReg->getRefA() - adjustment);
    } else {
        if (flag.H || (pReg->getRefA() & 0x0F) > 0x09)
            adjustment |= 0x06;
        if (flag.C || pReg->getRefA() > 0x99) {
            adjustment |= 0x60;
            flag.C = true;
        }
        pReg->getRefA() = static_cast<uint8_t>(pReg->getRefA() + adjustment);
    }

    flag.Z = !pReg->getRefA();
    flag.H = false;
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(CPL) {
    GB_Flag flag;
    pReg->getFlag(flag);
    flag.N = true;
    flag.H = true;
    pReg->setFlag(flag);
    pReg->getRefA() = ~pReg->getRefA();
}

SUB_FUNC_IMPL(LD_PC_MEM16) {
    pReg->getRefPC() = pInstState->a16Addr;
    pInstState->memMode = MEM_MODE_SLEEP;
}

SUB_FUNC_IMPL(LD_PC_HL) {
    // TODO: handle timing if execute/fetch are parallel
    pReg->getRefPC() = pReg->getRefHL();
}

static inline bool _jpConditionTrue(const uint8_t &op, const GB_Flag &flag) {
    switch((op>>3)&3) {
        case 0: return !flag.Z;
        case 1: return flag.Z;
        case 2: return !flag.C;
        case 3:
        default:
            return flag.C;
    }
}

SUB_FUNC_IMPL(CondLD_PC_MEM16) {
    GB_Flag flag;
    pReg->getFlag(flag);
    if (_jpConditionTrue(pInstState->opcode, flag)) {
        pReg->getRefPC() = pInstState->a16Addr;
        pInstState->memMode = MEM_MODE_SLEEP;
    }
}

SUB_FUNC_IMPL(JR) {
    pReg->getRefPC() += (int8_t)(pInstState->memValue);
    pInstState->memMode = MEM_MODE_SLEEP;
}

SUB_FUNC_IMPL(CondJR) {
    GB_Flag flag;
    pReg->getFlag(flag);
    if (_jpConditionTrue(pInstState->opcode, flag)) {
        pReg->getRefPC() += (int8_t)(pInstState->memValue);
        pInstState->memMode = MEM_MODE_SLEEP;
    }
}

SUB_FUNC_IMPL(SwapPC_MEM16) {
    std::swap(pReg->getRefPC(),pInstState->a16Addr);
    pInstState->memMode = MEM_MODE_SLEEP;
}

SUB_FUNC_IMPL(CondSwapPC_MEM16) {
    GB_Flag flag;
    pReg->getFlag(flag);
    if (_jpConditionTrue(pInstState->opcode, flag)) {
        std::swap(pReg->getRefPC(), pInstState->a16Addr);
        pInstState->memMode = MEM_MODE_SLEEP;
    }
}

SUB_FUNC_IMPL(Bubble) {
    pInstState->memMode = MEM_MODE_SLEEP;
}

SUB_FUNC_IMPL(CondPOP_LD_A16_LSB) {
    GB_Flag flag;
    pReg->getFlag(flag);
    if (_jpConditionTrue(pInstState->opcode, flag)) {
        pInstState->memMode = MEM_MODE_READ_ADDR_LSB;
        pInstState->memAddr = pReg->getRefSP()++;
    }
}

SUB_FUNC_IMPL(LD_PC_MEM16_RETI) {
    pReg->getRefPC() = pInstState->a16Addr;
    pInstState->memMode = MEM_MODE_SLEEP;
    pInstState->imeAction = IME_ACTION_ENABLE;
}

SUB_FUNC_IMPL(LD_MEM16_PC) {
    pInstState->a16Addr = pReg->getRefPC();
    pInstState->memMode = MEM_MODE_SLEEP;
}

static inline uint8_t __getRstAddr(const uint8_t &op) {
    return GET_LSB(((op>>3)&7)<<3);// id * 8, id = bit[3 ... 5]
}

SUB_FUNC_IMPL(RST) {
    pReg->getRefPC() = __getRstAddr(pInstState->opcode);
}

SUB_FUNC_IMPL(Halt) {
    pInstState->clockAction = CLOCK_ACTION_HALT;
}

SUB_FUNC_IMPL(Stop) {
    // The byte following STOP is consumed as part of the instruction, but its
    // value is ignored by the SM83 instruction definition.
    pInstState->clockAction = CLOCK_ACTION_STOP;
    pInstState->memMode = MEM_MODE_SLEEP;
}

SUB_FUNC_IMPL(DI) {
    pInstState->imeAction = IME_ACTION_DISABLE;
}

SUB_FUNC_IMPL(EI) {
    pInstState->imeAction = IME_ACTION_ENABLE_DELAYED;
}

SUB_FUNC_IMPL(IncRR) {
    // TODO: emu 8 bit cpu
    ++_getCommonReg16(pInstState->opcode, pReg);
}

SUB_FUNC_IMPL(DecRR) {
    // TODO: emu 8 bit cpu
    --_getCommonReg16(pInstState->opcode, pReg);
}

SUB_FUNC_IMPL(Add_HL_RR) {
    // The instruction's M-cycle sequence is modeled in the decode table; the
    // internal 8-bit ALU phases are not modeled separately.
    const int lhs = pReg->getRefHL();
    const int rhs = _getCommonReg16(pInstState->opcode, pReg);
    const int  result = lhs + rhs;
    GB_Flag flag;
    pReg->getFlag(flag);
    flag.N = false;
    flag.C = result > 0xFFFF;
    flag.H = (lhs&0xFFF) + (rhs&0xFFF) > 0xFFF;
    pReg->getRefHL() = (uint16_t)(result & 0xFFFF);
    pReg->setFlag(flag);
}

static inline uint16_t _add_SP_R8(const uint16_t lhs, const uint8_t offset,
                                  GB_Flag &flag) {
    const int signedOffset = offset < 0x80 ? offset : (int)offset - 0x100;
    flag.Z = false;
    flag.N = false;
    flag.H = (lhs & 0x0F) + (offset & 0x0F) > 0x0F;
    flag.C = (lhs & 0xFF) + offset > 0xFF;
    return static_cast<uint16_t>(lhs + signedOffset);
}

SUB_FUNC_IMPL(Add_SP_R8) {
    // The decode table preserves the instruction's M-cycle count. The
    // intermediate ALU substeps are not modeled; the result is committed here.
    GB_Flag flag;
    pReg->getFlag(flag);
    const uint16_t result = _add_SP_R8(pReg->getRefSP(), pInstState->memValue, flag);
    pReg->getRefSP() = result;
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(Add_HL_SP_R8) {
    // The decode table preserves the instruction's M-cycle count. The
    // intermediate ALU substeps are not modeled; the result is committed here.
    GB_Flag flag;
    pReg->getFlag(flag);
    const uint16_t result = _add_SP_R8(pReg->getRefSP(), pInstState->memValue, flag);
    pReg->getRefHL() = result;
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(RLCA) {
    auto &A = pReg->getRefA();
    int result = A;
    GB_Flag flag = {};
    flag.C = (1<<7)&result;
    result = (result << 1) | (flag.C ? 1 : 0);
    pReg->getRefA() = GET_LSB(result);
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(RLA) {
    auto &A = pReg->getRefA();
    int result = A;
    GB_Flag flag;
    pReg->getFlag(flag);
    result = (result << 1) | (flag.C ? 1 : 0);
    flag.C = (1<<8)&result;
    flag.Z = false;
    flag.N = false;
    flag.H = false;
    pReg->getRefA() = GET_LSB(result);
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(RRCA) {
    auto &A = pReg->getRefA();
    int result = A;
    GB_Flag flag = {};
    pReg->getFlag(flag);
    flag.C = (1)&result;
    result = (result) | (flag.C ? 1<<8 : 0);
    result >>= 1;
    flag.N = false;
    flag.Z = false;
    flag.H = false;
    pReg->getRefA() = GET_LSB(result);
    pReg->setFlag(flag);
}

SUB_FUNC_IMPL(RRA) {
    auto &A = pReg->getRefA();
    int result = A;
    GB_Flag flag;
    pReg->getFlag(flag);
    result = (result) | (flag.C ? 1<<8 : 0);
    flag.C = (1)&result;
    result >>= 1;
    flag.Z = false;
    flag.N = false;
    flag.H = false;
    pReg->getRefA() = GET_LSB(result);
    pReg->setFlag(flag);
}

}
