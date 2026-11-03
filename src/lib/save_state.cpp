#include "include/game_boy_system.hpp"
#include "include/state_hash.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace LOTUSGB {
namespace {
void requireState(bool condition, const char *message) {
    if (!condition) throw std::invalid_argument(message);
}
constexpr uint32_t DMG_STATE_VERSION = 11;
constexpr uint32_t CGB_STATE_VERSION = 13;
constexpr uint32_t DMG_MODEL = 1;
constexpr size_t HEADER_SIZE = 8 + 4 + 4 + 4 + 32 + 32 + 32;
constexpr size_t MAX_AUDIO_SAMPLES = 96000;
constexpr uint8_t PPU_OAM_SCAN_IN_PROGRESS = 0x80;
}

// Explicit value-only schema: bus maps, references, callbacks, decoders, ROM
// bytes and host pointers belong to the live machine, never to a snapshot.
class StateCodec {
public:
    static auto fields(Reg &v) {
        return std::tie(v.getRefA(), v.getRefF(), v.getRefB(), v.getRefC(),
                        v.getRefD(), v.getRefE(), v.getRefH(), v.getRefL(),
                        v.getRefSP(), v.getRefPC());
    }
// Snapshot v1 field order. Changing the schema requires a format version bump.
    static auto fields(GBCPU &v) {
        return std::tie(v.reg,
                        v.interruptMasterEnable,
                        v.isHalt,
                        v.isStopped,
                        v.fetchAfterStopWake,
                        v.initialFetchPending,
                        v.imeEnablePending,
                        v.waitDecodeCB,
                        v.instructionBoundary,
                        v.haltBugPending,
                        v.interruptSequenceActive,
                        v.interruptSequenceCycle,
                        v.activeInterruptMask,
                        v.activeInterruptVector,
                        v.instStat);
    }
    static auto fields(InstState &v) {
        return std::tie(v.opcode,
                        v.memCycleCnt,
                        v.memMode,
                        v.memValue,
                        v.memAddr,
                        v.a16Addr,
                        v.setHalt,
                        v.nextHaltValue,
                        v.setIME,
                        v.nextIMEValue,
                        v.imeAction,
                        v.clockAction);
    }
    static auto fields(Timer &v) {
        return std::tie(v.systemCounter,
                        v.tima,
                        v.tma,
                        v.tac,
                        v.overflowReloadTCycles,
                        v.reloadWindowTCycles);
    }
    static auto fields(DmgSerial &v) {
        return std::tie(v.data,
                        v.control,
                        v.bitsTransferred,
                        v.clockPhaseTCycles);
    }
    static auto fields(Joypad &v) {
        return std::tie(v.selectionBits,
                        v.pressedButtons);
    }
    static auto fields(InterruptRegisters &v) {
        return std::tie(v.interruptFlags,
                        v.interruptEnable);
    }
    static auto fields(DmgOamDma &v) {
        return std::tie(v.sourcePage,
                        v.activePage,
                        v.lastByte,
                        v.byteOffset,
                        v.startDelayMCycles,
                        v.mCycleTCycles,
                        v.active);
    }
    static auto fields(RamRegion &v) {
        return std::tie(v.bytes);
    }
    static auto fields(BootRom &v) {
        return std::tie(v.mapped);
    }
    static auto fields(SystemClock &v) {
        return std::tie(v.elapsedTCycles);
    }
    static auto fields(Cartridge &v) {
        return std::tie(v.ramEnabled,
                        v.romBankLow,
                        v.bankHigh,
                        v.mbc3RomBank,
                        v.mbc3RamBank,
                        v.mbc3RtcRegisters,
                        v.mbc3RtcLatchedRegisters,
                        v.mbc3RtcLastUnixTime,
                        v.mbc3RtcLastLatchValue,
                        v.mbc3RtcLoaded,
                        v.mbc2RomBank,
                        v.mbc5RomBank,
                        v.mbc5RamBank,
                        v.ramBankingMode,
                        v.externalRam,
                        v.rtcStorage);
    }
    static auto mbc7Fields(Cartridge &v) {
        return std::tie(v.mbc7RamEnable1,
                        v.mbc7ControlRegister,
                        v.mbc7RomBank,
                        v.mbc7AccelerometerX,
                        v.mbc7AccelerometerY,
                        v.mbc7LatchedAccelerometerX,
                        v.mbc7LatchedAccelerometerY,
                        v.mbc7AccelerometerLatchArmed,
                        v.mbc7AccelerometerLatched,
                        v.mbc7EepromWriteEnabled,
                        v.mbc7EepromChipSelect,
                        v.mbc7EepromClock,
                        v.mbc7EepromDataIn,
                        v.mbc7EepromDataOut,
                        v.mbc7EepromPhase,
                        v.mbc7EepromShiftRegister,
                        v.mbc7EepromReadWord,
                        v.mbc7EepromBitCount,
                        v.mbc7EepromAddress,
                        v.mbc7EepromReadBitsRemaining);
    }
    static auto mbc7EepromTimingFields(Cartridge &v) {
        return std::tie(v.mbc7EepromPendingOperation,
                        v.mbc7EepromPendingWord,
                        v.mbc7EepromBusyTCycles,
                        v.mbc7EepromChipSelectLowTCycles,
                        v.mbc7EepromChipSelectQualified);
    }
    static auto fields(DmgPpu &v) {
        return std::tie(v.videoRam,
                        v.objectRam,
                        v.frameBuffer,
                        v.completedFrameBuffer,
                        v.mode3,
                        v.lcdControl,
                        v.statInterruptEnable,
                        v.scrollY,
                        v.scrollX,
                        v.ly,
                        v.lyCompare,
                        v.bgPalette,
                        v.objectPalette0,
                        v.objectPalette1,
                        v.windowY,
                        v.windowX,
                        v.windowLineCounter,
                        v.windowYTriggered,
                        v.dot,
                        v.startupScanline,
                        v.lycCoincidence,
                        v.statInterruptLine);
    }
    static auto fields(DmgPpu::BackgroundFetcherState &v) {
        return std::tie(v.tile,
                        v.low,
                        v.high,
                        v.tileMapAddress,
                        v.lowPlaneAddress,
                        v.highPlaneAddress,
                        v.tileColumn,
                        v.startupDotsRemaining,
                        v.phase);
    }
    static auto fields(DmgPpu::BackgroundFifo &v) {
        return std::tie(v.pixels,
                        v.readIndex,
                        v.size);
    }
    static auto fields(DmgPpu::ObjectPixel &v) {
        return std::tie(v.colorIndex,
                        v.usesPalette1,
                        v.behindBackground);
    }
    static auto fields(DmgPpu::ObjectFetchEvent &v) {
        return std::tie(v.pixels,
                        v.tileIndex,
                        v.tileRow,
                        v.attributes,
                        v.low,
                        v.high,
                        v.startX,
                        v.firstPixelIndex,
                        v.alignmentDots,
                        v.transferDots);
    }
    static auto fields(DmgPpu::ObjectFifo &v) {
        return std::tie(v.pixels,
                        v.readIndex);
    }
    static auto fields(DmgPpu::ObjectFetcherState &v) {
        return std::tie(v.phase,
                        v.alignmentDotsRemaining,
                        v.fetchSetupDotsRemaining,
                        v.dataTransferDotsRemaining);
    }
    static auto fields(DmgPpu::Mode3State &v) {
        return std::tie(v.windowColorIndices,
                        v.objectFetchEvents,
                        v.backgroundFifo,
                        v.objectFifo,
                        v.fetcher,
                        v.objectFetcher,
                        v.duration,
                        v.fineScroll,
                        v.outputX,
                        v.windowStartX,
                        v.windowPenaltyDotsRemaining,
                        v.objectFetchEventCount,
                        v.nextObjectFetchEvent,
                        v.activeObjectFetchEvent,
                        v.windowPenaltyPending,
                        v.windowStarted,
                        v.drewWindow);
    }
    static auto fields(DmgApu &v) {
        return std::tie(v.nr10,
                        v.nr11,
                        v.nr12,
                        v.nr13,
                        v.nr14,
                        v.nr21,
                        v.nr22,
                        v.nr23,
                        v.nr24,
                        v.nr30,
                        v.nr32,
                        v.nr33,
                        v.nr34,
                        v.nr42,
                        v.nr43,
                        v.nr44,
                        v.nr50,
                        v.nr51,
                        v.powered,
                        v.channel1Enabled,
                        v.channel1DacEnabled,
                        v.lengthEnabled,
                        v.lengthCounter,
                        v.dutyPosition,
                        v.frequencyTimer,
                        v.channel2Enabled,
                        v.channel2DacEnabled,
                        v.lengthEnabled2,
                        v.lengthCounter2,
                        v.dutyPosition2,
                        v.frequencyTimer2,
                        v.envelopeVolume,
                        v.envelopeTimer,
                        v.envelopeVolume2,
                        v.envelopeTimer2,
                        v.channel3Enabled,
                        v.channel3DacEnabled,
                        v.lengthEnabled3,
                        v.lengthCounter3,
                        v.waveSampleIndex,
                        v.waveSampleBuffer,
                        v.frequencyTimer3,
                        v.waveRam,
                        v.elapsedTCycles,
                        v.lastWaveRamFetchTCycle,
                        v.hasWaveRamFetch,
                        v.channel4Enabled,
                        v.channel4DacEnabled,
                        v.lengthEnabled4,
                        v.lengthCounter4,
                        v.envelopeVolume4,
                        v.envelopeTimer4,
                        v.channel4Lfsr,
                        v.frequencyTimer4,
                        v.sweepShadowFrequency,
                        v.sweepTimer,
                        v.sweepEnabled,
                        v.sweepNegateUsed,
                        v.frameSequencerStep,
                        v.sampleAccumulator,
                        v.previousLeftInput,
                        v.previousRightInput,
                        v.previousLeftOutputQ16,
                        v.previousRightOutputQ16,
                        v.audioSamples);
    }
    static auto fields(GameBoySystem &v) {
        return std::tie(v.cpu, v.interruptRegisters, v.workRam, v.highRam,
                        v.timer, v.serial, v.joypad, v.oamDma, v.ppu, v.apu,
                        v.cartridge, v.bootRom, v.systemClock);
    }
    static auto fields(CgbWorkRam &v) { return std::tie(v.banks, v.bankSelect); }
    static auto fields(CgbVideoRam &v) { return std::tie(v.banks, v.bankSelect); }
    static auto fields(CgbIo &v) {
        return std::tie(v.switchArmed, v.doubleSpeed, v.speedPauseMCycles, v.infrared, v.scratch);
    }
    static auto fields(CgbVramDma &v) {
        return std::tie(v.source, v.destination, v.remainingBlocks, v.blockBytes,
                        v.byteTCycles, v.active, v.hblank);
    }
    static auto cgbFields(GameBoySystem &v) {
        auto &p = v.ppu;
        return std::tie(*v.cgbWorkRam, *v.cgbVideoRam, *v.cgbIo, *v.cgbVramDma,
            p.colorFrameBuffer, p.completedColorFrameBuffer, p.cgbBgPalettes,
            p.cgbObjPalettes, p.cgbBgIndex, p.cgbObjIndex, p.cgbFetchAttributes, p.cgbFetchRow);
    }
    template<class F> static void visitCgbPixels(GameBoySystem &v, F action) {
        for (auto &pixel : v.ppu.mode3.objectFifo.pixels)
            action(pixel.cgbPalette, pixel.priorityIndex);
        for (auto &event : v.ppu.mode3.objectFetchEvents) {
            action(event.oamIndex);
            for (auto &pixel : event.pixels) action(pixel.cgbPalette, pixel.priorityIndex);
        }
    }
    static uint8_t instructionKind(GBCPU &v) {
        if (!v.pInst) return 0;
        if (v.pInst == v.decoder.decode(v.instStat.opcode)) return 1;
        if (v.pInst == v.decoderCB.decode(v.instStat.opcode)) return 2;
        throw std::logic_error("CPU instruction does not belong to its decoder");
    }
    static void bindInstruction(GBCPU &v, uint8_t kind) {
        requireState(kind <= 2, "Invalid CPU instruction table");
        v.pInst = kind == 0 ? nullptr : kind == 1
            ? v.decoder.decode(v.instStat.opcode) : v.decoderCB.decode(v.instStat.opcode);
        requireState(kind == 0 || v.pInst, "Invalid CPU opcode in snapshot");
    }
    static void validate(GameBoySystem &v);
    static std::vector<uint8_t> save(GameBoySystem &v);
    static void load(GameBoySystem &v, const void *data, size_t size);
};

namespace {
// Integer widths are explicit in the schema; int/unsigned/enums are 32-bit.
// Signed values are encoded as two's complement, without implementation-defined
// unsigned-to-signed conversion on input.
class Writer {
public:
    std::vector<uint8_t> bytes;
    template<class... T> void operator()(T &...v) { (field(v), ...); }
    template<class T> void field(T &v) {
        if constexpr (std::is_same_v<T, bool>) {
            bytes.push_back(v ? 1 : 0);
        } else if constexpr (std::is_enum_v<T>) {
            uint32_t n = static_cast<uint32_t>(v); field(n);
        } else if constexpr (std::is_integral_v<T>) {
            static_assert(sizeof(int) == 4 && sizeof(unsigned) == 4);
            using U = std::make_unsigned_t<T>;
            U n = static_cast<U>(v);
            for (size_t i = 0; i < sizeof(T); ++i)
                bytes.push_back(static_cast<uint8_t>(n >> (i * 8)));
        } else {
            std::apply(*this, StateCodec::fields(v));
        }
    }
    template<class T, size_t N> void field(std::array<T,N> &v) {
        for (auto &x : v) field(x);
    }
    template<class T> void field(std::vector<T> &v) {
        requireState(v.size() <= GameBoySystem::MAX_STATE_SIZE / sizeof(T), "Snapshot vector is too large");
        uint32_t count = static_cast<uint32_t>(v.size()); field(count);
        for (auto &x : v) field(x);
    }
    void field(std::shared_ptr<std::vector<uint8_t>> &v) {
        uint32_t count = v ? static_cast<uint32_t>(v->size()) : 0;
        field(count);
        if (v) for (auto &x : *v) field(x);
    }
};

class Reader {
public:
    const uint8_t *data;
    size_t size;
    size_t position = 0;
    template<class... T> void operator()(T &...v) { (field(v), ...); }
    template<class T> void field(T &v) {
        if constexpr (std::is_same_v<T,bool>) {
            uint8_t n = 0; field(n);
            requireState(n <= 1, "Invalid snapshot boolean"); v = n != 0;
        } else if constexpr (std::is_enum_v<T>) {
            uint32_t n = 0; field(n);
            using U = std::underlying_type_t<T>;
            requireState(n <= static_cast<uint64_t>(std::numeric_limits<U>::max()), "Invalid snapshot enum");
            v = static_cast<T>(n);
        } else if constexpr (std::is_integral_v<T>) {
            requireState(sizeof(T) <= size - position, "Truncated snapshot");
            using U = std::make_unsigned_t<T>;
            U n = 0;
            for (size_t i = 0; i < sizeof(T); ++i)
                n |= static_cast<U>(data[position++]) << (i * 8);
            if constexpr (std::is_signed_v<T>) {
                v = n <= static_cast<U>(std::numeric_limits<T>::max())
                    ? static_cast<T>(n) : static_cast<T>(-1 - static_cast<T>(static_cast<U>(~n)));
            } else v = n;
        } else std::apply(*this, StateCodec::fields(v));
    }
    template<class T, size_t N> void field(std::array<T,N> &v) {
        for (auto &x : v) field(x);
    }
    template<class T> void field(std::vector<T> &v) {
        uint32_t count = 0; field(count);
        requireState(count <= (size - position) / sizeof(T), "Invalid snapshot vector length");
        if constexpr (std::is_same_v<T,int16_t>) {
            requireState(count <= MAX_AUDIO_SAMPLES && count % 2 == 0, "Invalid audio queue length");
            v.resize(count);
        } else requireState(count == v.size(), "Snapshot RAM size mismatch");
        for (auto &x : v) field(x);
    }
    void field(std::shared_ptr<std::vector<uint8_t>> &v) {
        uint32_t count = 0; field(count);
        requireState(count == (v ? v->size() : 0), "Snapshot cartridge storage size mismatch");
        if (v) for (auto &x : *v) field(x);
    }
};

// Commit has no allocation and cannot fail. Preserve published SRAM/RTC data
// addresses while keeping all callbacks and borrowed references on the target.
template<class T> void commitValue(T &dst, T &src);
template<class T, size_t N> void commitValue(std::array<T,N> &dst, std::array<T,N> &src);
template<class T> void commitValue(std::vector<T> &dst, std::vector<T> &src);
void commitValue(std::shared_ptr<std::vector<uint8_t>> &dst, std::shared_ptr<std::vector<uint8_t>> &src);
template<class... T, size_t... I>
void commitTuple(std::tuple<T&...> dst, std::tuple<T&...> src, std::index_sequence<I...>) {
    (commitValue(std::get<I>(dst), std::get<I>(src)), ...);
}
template<class T> void commitValue(T &dst, T &src) {
    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) dst = src;
    else {
        auto d = StateCodec::fields(dst), s = StateCodec::fields(src);
        commitTuple(d, s, std::make_index_sequence<std::tuple_size_v<decltype(d)>>{});
    }
}
template<class T, size_t N> void commitValue(std::array<T,N> &dst, std::array<T,N> &src) { dst = src; }
template<class T> void commitValue(std::vector<T> &dst, std::vector<T> &src) { dst.swap(src); }
void commitValue(std::shared_ptr<std::vector<uint8_t>> &dst, std::shared_ptr<std::vector<uint8_t>> &src) {
    if (dst) std::copy(src->begin(), src->end(), dst->begin());
}
}

void StateCodec::validate(GameBoySystem &v) {
    auto &c = v.cpu;
    requireState(!v.systemClock.cpuStepInProgress, "Snapshot requires an M-cycle boundary");
    requireState((c.reg.getRefF() & 15) == 0 && c.interruptSequenceCycle <= 4 &&
                 c.instStat.memCycleCnt >= 0 && c.instStat.memCycleCnt <= 6 &&
                 c.instStat.memMode >= MEM_MODE_FETCH && c.instStat.memMode <= MEM_MODE_SLEEP &&
                 c.instStat.imeAction >= IME_ACTION_NONE && c.instStat.imeAction <= IME_ACTION_DISABLE &&
                 c.instStat.clockAction >= CLOCK_ACTION_NONE && c.instStat.clockAction <= CLOCK_ACTION_LOCKUP,
                 "Invalid CPU snapshot state");
    if (c.pInst) {
        const auto *inst = static_cast<Instruction *>(c.pInst);
        requireState(c.instStat.memCycleCnt < inst->memCycles ||
                     ((c.isStopped || c.fetchAfterStopWake || c.isLocked) &&
                      c.instStat.memCycleCnt == inst->memCycles),
                     "Invalid instruction cycle in snapshot");
    } else requireState(c.initialFetchPending || c.interruptSequenceActive,
                        "Snapshot has no executable CPU instruction");
    requireState(!c.isLocked ||
                     (c.pInst && !c.isHalt && !c.isStopped &&
                      !c.interruptSequenceActive &&
                      c.instStat.clockAction == CLOCK_ACTION_LOCKUP &&
                      c.instStat.memMode == MEM_MODE_SLEEP &&
                      static_cast<Instruction *>(c.pInst)->name == "ILLEGAL_OPCODE"),
                 "Invalid locked CPU snapshot state");
    requireState(c.isLocked || c.instStat.clockAction != CLOCK_ACTION_LOCKUP,
                 "CPU lock-up action without a locked CPU");
    requireState(v.timer.overflowReloadTCycles >= 0 && v.timer.overflowReloadTCycles <= 4 &&
                 v.timer.reloadWindowTCycles >= 0 && v.timer.reloadWindowTCycles <= 4 && v.timer.tac <= 7,
                 "Invalid timer snapshot state");
    requireState(v.serial.clockPhaseTCycles < 512 && v.serial.bitsTransferred < 8 &&
                 (v.serial.control & (v.model == HardwareModel::CgbE ? 0x7c : 0x7e)) == 0, "Invalid serial snapshot state");
    requireState(v.oamDma.byteOffset <= DmgPpu::OAM_SIZE && v.oamDma.startDelayMCycles <= 2 &&
                 v.oamDma.mCycleTCycles < 4, "Invalid DMA snapshot state");
    auto &p = v.ppu;
    auto &m = p.mode3;
    requireState(p.dot < 456 && p.ly < 154 && m.outputX <= 160 &&
                 m.duration >= 172 && m.duration <= 376 && m.fineScroll < 8 &&
                 (m.windowPenaltyDotsRemaining <= 5 ||
                  (m.windowPenaltyDotsRemaining == PPU_OAM_SCAN_IN_PROGRESS &&
                   p.ly < 144 && p.dot <= 80)) &&
                 m.backgroundFifo.readIndex < 16 && m.backgroundFifo.size <= 8 &&
                 m.objectFifo.readIndex < 16 && m.objectFetchEventCount <= 10 &&
                 m.nextObjectFetchEvent <= m.objectFetchEventCount && m.activeObjectFetchEvent < 10 &&
                 m.fetcher.tileMapAddress < 0x2000 && m.fetcher.lowPlaneAddress < 0x2000 &&
                 m.fetcher.highPlaneAddress < 0x2000 &&
                 static_cast<unsigned>(m.fetcher.phase) <= 7 &&
                 static_cast<unsigned>(m.objectFetcher.phase) <= 3,
                 "Invalid PPU snapshot state");
    for (const auto &event : m.objectFetchEvents) {
        const bool rawRow = (event.tileRow & 0x80) != 0;
        const uint8_t row = event.tileRow & 0x0F;
        requireState((rawRow ? (event.tileRow & 0x30) == 0 &&
                                 row < ((event.tileRow & 0x40) ? 16 : 8)
                              : event.tileRow < 16) &&
                         event.firstPixelIndex <= 8 &&
                         (!rawRow || event.oamIndex < 40),
                     "Invalid object fetch state");
    }
    const auto shadesValid = [](const auto &pixels) {
        return std::all_of(pixels.begin(), pixels.end(), [](uint8_t x) { return x < 4; });
    };
    requireState(shadesValid(p.frameBuffer) && shadesValid(p.completedFrameBuffer) &&
                 std::all_of(m.windowColorIndices.begin(), m.windowColorIndices.end(), [&](uint8_t x) {
                     return x < (p.cgbMode ? 64 : 4);
                 }) && std::all_of(m.backgroundFifo.pixels.begin(), m.backgroundFifo.pixels.end(), [&](uint8_t x) {
                     return x < (p.cgbMode ? 64 : 4);
                 }),
                 "Invalid framebuffer or FIFO shades");
    for (const auto &pixel : m.objectFifo.pixels)
        requireState(pixel.colorIndex < 4, "Invalid object FIFO color");
    for (const auto &event : m.objectFetchEvents)
        for (const auto &pixel : event.pixels)
            requireState(pixel.colorIndex < 4, "Invalid object fetch color");
    requireState(v.ppu.statWriteOverrideTCycles <= 4 &&
                 (v.model == HardwareModel::Dmg || v.ppu.statWriteOverrideTCycles == 0),
                 "Invalid STAT write override state");
    auto &a = v.apu;
    requireState(a.dutyPosition < 8 && a.dutyPosition2 < 8 && a.waveSampleIndex < 32 &&
                 a.frameSequencerStep < 8 && a.sampleAccumulator < 4194304 &&
                 a.envelopeVolume < 16 && a.envelopeVolume2 < 16 && a.envelopeVolume4 < 16 &&
                 a.envelopeTimer <= 9 && a.envelopeTimer2 <= 9 && a.envelopeTimer4 <= 9 &&
                 a.previousLeftInput >= -1024 && a.previousLeftInput <= 1024 &&
                 a.previousRightInput >= -1024 && a.previousRightInput <= 1024 &&
                 a.previousLeftOutputQ16 >= -(1LL << 32) && a.previousLeftOutputQ16 <= (1LL << 32) &&
                 a.previousRightOutputQ16 >= -(1LL << 32) && a.previousRightOutputQ16 <= (1LL << 32) &&
                 a.audioSamples.size() <= MAX_AUDIO_SAMPLES && a.audioSamples.size() % 2 == 0,
                 "Invalid APU snapshot state or undrained audio queue exceeds one second");
    if (v.model == HardwareModel::CgbE) {
        requireState(v.cgbWorkRam->bankSelect < 8 && v.cgbVideoRam->bankSelect < 2
            && (v.cgbIo->key0 & ~4) == 0 && v.cgbIo->opri <= 1
            && v.cgbIo->speedPauseMCycles <= 2050 && p.speedSwitchPpuMode <= 3
            && v.cgbIo->infraredSettleTCycles <= CgbIo::IR_RECEIVER_SETTLE_T_CYCLES
            && (v.cgbIo->infraredSettleTCycles == 0 || (v.cgbIo->infrared & 0xC0) == 0xC0)
            && ((v.cgbIo->infrared & 0xC0) == 0xC0 || !v.cgbIo->infraredInputLatched)
            && (v.cgbIo->infraredSettleTCycles == 0 || !v.cgbIo->infraredInputLatched)
            && (v.cgbIo->speedPauseMCycles || p.speedSwitchPpuMode == 3) && p.cgbFetchRow < 8
            && (p.cgbBgIndex & 0x40) == 0 && (p.cgbObjIndex & 0x40) == 0,
            "Invalid CGB banks, speed or palette index");
        const auto &d = *v.cgbVramDma;
        requireState(d.destination >= 0x8000 && d.destination <= 0x9FFF
            && d.remainingBlocks <= 128 && d.blockBytes <= 16 && d.byteTCycles < 2
            && (!d.active || (d.remainingBlocks && (d.hblank || d.blockBytes))),
            "Invalid CGB VRAM DMA state");
        for (const auto &buffer : {&p.colorFrameBuffer, &p.completedColorFrameBuffer})
            requireState(std::all_of(buffer->begin(), buffer->end(), [](uint16_t x) { return x <= 0x7FFF; }),
                "Invalid CGB color framebuffer");
        auto validPixel = [](const DmgPpu::ObjectPixel &pixel) {
            return pixel.cgbPalette < 8 && (pixel.priorityIndex < 40 || pixel.priorityIndex == 0xFF);
        };
        for (const auto &pixel : m.objectFifo.pixels) requireState(validPixel(pixel), "Invalid CGB object FIFO metadata");
        for (const auto &event : m.objectFetchEvents) {
            requireState(event.oamIndex < 40 || event.oamIndex == 0xFF, "Invalid CGB object index");
            for (const auto &pixel : event.pixels) requireState(validPixel(pixel), "Invalid CGB object metadata");
        }
    }
    requireState(!v.bootRom.mapped || !v.bootRom.bootRom.empty(), "Snapshot requires a boot ROM");
    auto &cart = v.cartridge;
    requireState(cart.mbc3RtcLastUnixTime >= 0 && cart.mbc5RomBank < 512 &&
                 cart.mbc5RamBank < 16 && cart.bankHigh < 4 && cart.romBankLow < 32 &&
                 cart.mbc2RomBank > 0 && cart.mbc2RomBank < 16 && cart.mbc3RomBank > 0 &&
                 (cart.mbc30 || cart.mbc3RomBank < 128) && cart.mbc7RomBank < 128 &&
                 static_cast<uint8_t>(cart.mbc7EepromPhase) <=
                     static_cast<uint8_t>(Cartridge::Mbc7EepromPhase::WriteAllData) &&
                 static_cast<uint8_t>(cart.mbc7EepromPendingOperation) <=
                     static_cast<uint8_t>(Cartridge::Mbc7EepromPendingOperation::WriteAll) &&
                 cart.mbc7EepromBitCount <= 15 && cart.mbc7EepromAddress < 128 &&
                 cart.mbc7EepromReadBitsRemaining <= 16 &&
                 cart.mbc7EepromBusyTCycles <= Cartridge::MBC7_WRITE_ALL_TCYCLES &&
                 cart.mbc7EepromChipSelectLowTCycles <= 2 &&
                 (cart.mbc7EepromBusyTCycles == 0 ||
                  cart.mbc7EepromPendingOperation !=
                      Cartridge::Mbc7EepromPendingOperation::None) &&
                 (cart.mbc7EepromChipSelect ||
                  !cart.mbc7EepromChipSelectQualified) &&
                 (cart.mapper != Cartridge::Mapper::Mbc7 ||
                  (cart.externalRam && cart.externalRam->size() == 0x100)),
                 "Invalid cartridge snapshot state");
}

std::vector<uint8_t> StateCodec::save(GameBoySystem &v) {
    validate(v);
    Writer payload;
    auto kind = instructionKind(v.cpu);
    payload(kind);
    // Numbered component sections give future versions a clear extension point.
    uint32_t section = 0;
    std::apply([&](auto &...component) {
        ((payload(++section), payload(component)), ...);
    }, fields(v));
    if (v.model == HardwareModel::CgbE) {
        std::apply(payload, cgbFields(v));
        visitCgbPixels(v, [&](auto &...x) { payload(x...); });
    }
    payload(v.apu.firstDutySuppressed, v.apu.firstDutySuppressed2, v.ppu.statWriteOverrideTCycles);
    payload(v.apu.envelopeRunning, v.apu.envelopeRunning2, v.apu.envelopeRunning4);
    if (v.model == HardwareModel::CgbE)
        payload(v.ppu.speedSwitchPpuMode, v.cgbIo->key0, v.cgbIo->opri,
                v.cgbIo->infraredSettleTCycles, v.cgbIo->infraredInputLatched);
    std::apply(payload, mbc7Fields(v.cartridge));
    std::apply(payload, mbc7EepromTimingFields(v.cartridge));
    if (v.model == HardwareModel::Dmg)
        for (const auto &event : v.ppu.mode3.objectFetchEvents)
            payload(event.oamIndex);
    payload(v.cpu.isLocked);
    Writer header;
    std::array<uint8_t,8> magic{'L','O','T','U','S','S','T',0};
    uint32_t version = v.model == HardwareModel::CgbE ? CGB_STATE_VERSION : DMG_STATE_VERSION;
    uint32_t model = v.model == HardwareModel::CgbE ? 2 : DMG_MODEL;
    uint32_t length = static_cast<uint32_t>(HEADER_SIZE + payload.bytes.size());
    auto bootHash = StateInternal::sha256(v.bootRom.bootRom.data(), v.bootRom.bootRom.size());
    auto payloadHash = StateInternal::sha256(payload.bytes.data(), payload.bytes.size());
    header(magic, version, model, length, v.cartridge.romHash, bootHash,
           payloadHash);
    header.bytes.insert(header.bytes.end(), payload.bytes.begin(), payload.bytes.end());
    requireState(header.bytes.size() <= GameBoySystem::MAX_STATE_SIZE, "Snapshot exceeds format size limit");
    return header.bytes;
}

void StateCodec::load(GameBoySystem &v, const void *data, size_t size) {
    requireState(data && size >= HEADER_SIZE,
                 "Invalid snapshot buffer size");
    requireState(!v.systemClock.cpuStepInProgress, "Snapshot requires an M-cycle boundary");
    Reader header{static_cast<const uint8_t *>(data), size};
    std::array<uint8_t,8> magic{};
    uint32_t version=0, model=0, length=0;
    std::array<uint8_t,32> romHash{}, bootHash{}, payloadHash{};
    header(magic, version, model, length, romHash, bootHash, payloadHash);
    requireState(magic == std::array<uint8_t,8>{'L','O','T','U','S','S','T',0}, "Not a LotusGB snapshot");
    requireState((v.model == HardwareModel::Dmg && (version == 1 || version == 3 || version == 4 || version == 6 || version == 7 || version == 8 || version == 9 || version == 10 || version == DMG_STATE_VERSION) && model == DMG_MODEL)
        || (v.model == HardwareModel::CgbE && (version == 2 || version == 3 || version == 4 || version == 5 || version == 6 || version == 7 || version == 8 || version == 9 || version == 10 || version == 11 || version == 12 || version == CGB_STATE_VERSION) && model == 2),
        "Unsupported snapshot version or model");
    requireState(length >= HEADER_SIZE && length <= size && length <= GameBoySystem::MAX_STATE_SIZE, "Truncated snapshot payload");
    requireState(romHash == v.cartridge.romHash, "Snapshot ROM mismatch");
    requireState(bootHash == StateInternal::sha256(v.bootRom.bootRom.data(), v.bootRom.bootRom.size()), "Snapshot boot ROM mismatch");
    requireState(payloadHash == StateInternal::sha256(header.data + HEADER_SIZE, length - HEADER_SIZE), "Snapshot checksum mismatch");
    requireState(std::all_of(header.data + length, header.data + size, [](uint8_t x) { return x == 0; }), "Invalid snapshot padding");

    // Parse and validate on an independently wired machine. Neither its storage
    // nor its callbacks reference the running system.
    GameBoySystem candidate(
        v.cartridge.rom,
        v.model == HardwareModel::CgbE
            ? GameBoySystem::StartupProfile::CgbPostBoot
            : GameBoySystem::StartupProfile::DmgPostBoot,
        nullptr, nullptr, v.bootRom.bootRom, v.model, &v.cartridge.romHash);
    Reader payload{header.data + HEADER_SIZE, length - HEADER_SIZE};
    uint8_t kind = 0;
    payload(kind);
    uint32_t expectedSection = 0;
    std::apply([&](auto &...component) {
        auto readSection = [&](auto &part) {
            uint32_t section = 0; payload(section);
            requireState(section == ++expectedSection, "Invalid snapshot component section");
            payload(part);
        };
        (readSection(component), ...);
    }, fields(candidate));
    if (v.model == HardwareModel::CgbE) {
        std::apply(payload, cgbFields(candidate));
        visitCgbPixels(candidate, [&](auto &...x) { payload(x...); });
        candidate.systemClock.setDoubleSpeed(candidate.cgbIo->doubleSpeed);
        candidate.timer.setDoubleSpeed(candidate.cgbIo->doubleSpeed);
    }
    if (version >= 3) {
        payload(candidate.apu.firstDutySuppressed, candidate.apu.firstDutySuppressed2);
    } else {
        // Legacy snapshots did not model first-step suppression. Preserve
        // their recorded observable output rather than guessing trigger history.
        candidate.apu.firstDutySuppressed = candidate.apu.firstDutySuppressed2 = false;
    }
    if (version >= 4) payload(candidate.ppu.statWriteOverrideTCycles);
    if (version >= 6) {
        payload(candidate.apu.envelopeRunning, candidate.apu.envelopeRunning2, candidate.apu.envelopeRunning4);
    } else {
        // Older versions kept attempting volume changes at the bounds. No
        // stop history is available, so retain their active-envelope semantics.
        candidate.apu.envelopeRunning = candidate.apu.channel1Enabled;
        candidate.apu.envelopeRunning2 = candidate.apu.channel2Enabled;
        candidate.apu.envelopeRunning4 = candidate.apu.channel4Enabled;
    }
    if (v.model == HardwareModel::CgbE && version >= 5) payload(candidate.ppu.speedSwitchPpuMode);
    if (v.model == HardwareModel::CgbE) {
        if (version >= 7) payload(candidate.cgbIo->key0, candidate.cgbIo->opri);
        else candidate.cgbIo->key0 = candidate.cgbIo->opri = 0;
        if (version >= 8)
            payload(candidate.cgbIo->infraredSettleTCycles, candidate.cgbIo->infraredInputLatched);
        else {
            candidate.cgbIo->infraredSettleTCycles = 0;
            candidate.cgbIo->infraredInputLatched = false;
        }
        candidate.applyCgbMode();
    }
    const bool hasMbc7State =
        (v.model == HardwareModel::Dmg && version >= 7) ||
        (v.model == HardwareModel::CgbE && version >= 9);
    if (hasMbc7State)
        std::apply(payload, mbc7Fields(candidate.cartridge));
    const bool hasMbc7EepromTimingState =
        (v.model == HardwareModel::Dmg && version >= 8) ||
        (v.model == HardwareModel::CgbE && version >= 10);
    if (hasMbc7EepromTimingState)
        std::apply(payload, mbc7EepromTimingFields(candidate.cartridge));
    const bool hasMbc7RumbleControlState =
        (v.model == HardwareModel::Dmg && version >= 9) ||
        (v.model == HardwareModel::CgbE && version >= 11);
    if (hasMbc7State && !hasMbc7RumbleControlState) {
        requireState(candidate.cartridge.mbc7ControlRegister <= 1,
                     "Invalid legacy MBC7 RAM gate state");
        candidate.cartridge.mbc7ControlRegister =
            candidate.cartridge.mbc7ControlRegister ? 0x40 : 0;
    }
    if (v.model == HardwareModel::Dmg && version >= 10)
        for (auto &event : candidate.ppu.mode3.objectFetchEvents)
            payload(event.oamIndex);
    const bool hasIllegalOpcodeLockState =
        (v.model == HardwareModel::Dmg && version >= 11) ||
        (v.model == HardwareModel::CgbE && version >= 13);
    if (hasIllegalOpcodeLockState)
        payload(candidate.cpu.isLocked);
    requireState(payload.position == payload.size, "Unexpected snapshot payload data");
    bindInstruction(candidate.cpu, kind);
    validate(candidate);

    // Resolve the destination instruction before committing (decoder tables are
    // immutable and identical); then all remaining operations are non-throwing.
    auto *instruction = kind == 0 ? nullptr : kind == 1
        ? v.cpu.decoder.decode(candidate.cpu.instStat.opcode)
        : v.cpu.decoderCB.decode(candidate.cpu.instStat.opcode);
    commitValue(v, candidate);
    v.cpu.isLocked = candidate.cpu.isLocked;
    auto dstMbc7 = mbc7Fields(v.cartridge);
    auto srcMbc7 = mbc7Fields(candidate.cartridge);
    commitTuple(dstMbc7, srcMbc7,
                std::make_index_sequence<std::tuple_size_v<decltype(dstMbc7)>>{});
    auto dstMbc7Timing = mbc7EepromTimingFields(v.cartridge);
    auto srcMbc7Timing = mbc7EepromTimingFields(candidate.cartridge);
    commitTuple(dstMbc7Timing, srcMbc7Timing,
                std::make_index_sequence<std::tuple_size_v<decltype(dstMbc7Timing)>>{});
    v.apu.firstDutySuppressed = candidate.apu.firstDutySuppressed;
    v.apu.firstDutySuppressed2 = candidate.apu.firstDutySuppressed2;
    v.ppu.statWriteOverrideTCycles = candidate.ppu.statWriteOverrideTCycles;
    v.ppu.speedSwitchPpuMode = candidate.ppu.speedSwitchPpuMode;
    v.apu.envelopeRunning = candidate.apu.envelopeRunning;
    v.apu.envelopeRunning2 = candidate.apu.envelopeRunning2;
    v.apu.envelopeRunning4 = candidate.apu.envelopeRunning4;
    if (v.model == HardwareModel::Dmg)
        for (size_t i = 0; i < v.ppu.mode3.objectFetchEvents.size(); ++i)
            v.ppu.mode3.objectFetchEvents[i].oamIndex =
                candidate.ppu.mode3.objectFetchEvents[i].oamIndex;
    if (v.model == HardwareModel::CgbE) {
        auto d = cgbFields(v), s = cgbFields(candidate);
        commitTuple(d, s, std::make_index_sequence<std::tuple_size_v<decltype(d)>>{});
        auto copyPixel = [](auto &dst, const auto &src) {
            dst.cgbPalette = src.cgbPalette; dst.priorityIndex = src.priorityIndex;
        };
        for (size_t i = 0; i < v.ppu.mode3.objectFifo.pixels.size(); ++i)
            copyPixel(v.ppu.mode3.objectFifo.pixels[i], candidate.ppu.mode3.objectFifo.pixels[i]);
        for (size_t i = 0; i < v.ppu.mode3.objectFetchEvents.size(); ++i) {
            auto &dst = v.ppu.mode3.objectFetchEvents[i];
            const auto &src = candidate.ppu.mode3.objectFetchEvents[i];
            dst.oamIndex = src.oamIndex;
            for (size_t j = 0; j < dst.pixels.size(); ++j) copyPixel(dst.pixels[j], src.pixels[j]);
        }
        v.systemClock.setDoubleSpeed(v.cgbIo->doubleSpeed);
        v.timer.setDoubleSpeed(v.cgbIo->doubleSpeed);
        v.cgbIo->key0 = candidate.cgbIo->key0;
        v.cgbIo->opri = candidate.cgbIo->opri;
        v.cgbIo->infraredSettleTCycles = candidate.cgbIo->infraredSettleTCycles;
        v.cgbIo->infraredInputLatched = candidate.cgbIo->infraredInputLatched;
        v.applyCgbMode();
    }
    v.cpu.pInst = instruction;
    v.systemClock.currentStepTCyclesAdvanced = 0;
    v.systemClock.cpuStepInProgress = false;
}

std::vector<uint8_t> GameBoySystem::saveState() {
    if (cgbBootProbe)
        throw std::logic_error("Synthetic CGB boot probes do not support snapshots");
    return StateCodec::save(*this);
}
void GameBoySystem::loadState(const void *data, size_t size) {
    if (cgbBootProbe)
        throw std::logic_error("Synthetic CGB boot probes do not support snapshots");
    StateCodec::load(*this, data, size);
}
}
