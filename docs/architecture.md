# Current architecture and development workflow

This guide describes the implemented runtime boundaries, not a target design.
Work priorities live in the [roadmap](roadmap.md); feature evidence and known
limits live in the [DMG validation matrix](dmg-validation.md).

## Current scope

LotusGB is a headless DMG system connected to a libretro adapter. The system
composes the CPU, address bus, cartridge and RAM, timer, interrupts, joypad,
unconnected serial port, PPU, OAM DMA and APU. The default startup profile
skips boot-ROM execution and begins from a DMG post-boot state; an optional
external DMG boot ROM can be selected for startup. ROM-only, MBC1, MBC2, MBC3
and MBC5 cartridges are supported within the limits listed in the validation
matrix.

The PPU renders background, window and object pixels and implements selected
mid-scanline register behavior. Its Mode 3 BG/OBJ fetch arbitration and some
raster timing remain approximate. The [PPU guide](ppu-scrolling.md) records the
current model and visual evidence. CGB behavior, additional mapper families,
connected serial peers and MBC5 rumble output are outside the current
implementation. The external boot ROM is user-supplied and is not included in
the repository.

## Runtime flow

```mermaid
flowchart TD
    RA[RetroArch frontend] -->|load ROM and run frames| LR[Libretro adapter<br/>libretro_core.cpp]
    RA -->|controller state| LR
    LR -->|poll buttons, stepFrame| SYS[GameBoySystem]

    SYS --> CLOCK[SystemClock]
    CLOCK -->|one M-cycle| CPU[CPU and opcode decoder]
    CPU <-->|synchronous reads and writes| BUS[AddressBus]

    BUS --> CART[ROM-only or MBC1/MBC2/MBC3/MBC5 cartridge, RAM and RTC]
    BUS --> RAM[WRAM and HRAM]
    BUS --> IO[Mapped hardware registers]
    IO --> JOY[Joypad and interrupts]
    IO --> TIMER[Timer]
    IO --> SERIAL[DMG serial SB/SC, no peer]
    IO --> APU[DMG APU, pulse channels 1 and 2, wave channel 3, noise channel 4]
    IO --> PPU[DMG PPU, VRAM and OAM]
    IO --> DMA[OAM DMA]

    CLOCK -->|elapsed T-cycles| SERIAL
    CLOCK -->|elapsed T-cycles| TIMER
    CLOCK -->|elapsed T-cycles| APU
    CLOCK -->|elapsed T-cycles| PPU
    CLOCK -->|elapsed T-cycles| DMA

    SYS -->|framebuffer and PCM samples| LR
    RA <-->|save RAM and RTC memory interfaces| LR
    LR -->|video and audio callbacks| RA
```

`retro_run()` polls input, advances the system by one frame, then submits video and audio. `SystemClock` asks the CPU to execute one M-cycle. On the first bus access during the step, it advances the timer, serial port, APU, PPU, and OAM DMA through four T-cycles before dispatching the transaction. A step without a bus access advances all reported T-cycles after the CPU step. The timer sends the APU a clock event when divider bit 12 falls, including when a DIV write resets that bit from high to low; the APU uses this to clock its frame sequencer. A zero-time CPU step does not advance system time or peripherals.

DMA is a separate bus master. `AddressBus::read()`/`write()` advance to the CPU
bus phase and then consult the borrowed `ICpuBusArbiter`. The DMA device uses
`readForDma()` to dispatch source reads without re-entering CPU timing or CPU
arbitration. FF46 schedules a transfer; the shared clock moves one byte per
M-cycle after the startup delay. CPU OAM reads return `$FF` while DMA owns OAM;
HRAM and I/O remain accessible, including FF46 for restarting the transfer.
The basic DMG conflict model distinguishes VRAM from the shared cartridge/WRAM
bus: CPU reads on the occupied bus see the latest DMA byte, and CPU writes to
that bus are discarded. Write-induced corruption and PPU/DMA rendering
contention are not implemented. This is DMG behavior, not a CGB bus model.

The PPU applies its own mode-based VRAM/OAM access restrictions at the bus
boundary. In LotusGB's DMG model, a denied read returns `$FF` and a denied write
is discarded immediately; the CPU does not wait for the next accessible mode,
and the write is not replayed later. Games schedule updates around these windows
by polling LY/STAT or using their interrupts. A system-level CPU test exercises
that pattern: it verifies rejected Mode 3 writes do not stall or replay, then
confirms updates succeed after software polls to Mode 0. The exact
transition-dot behavior remains a separate PPU timing question.

`GameBoySystem` owns the hardware components. `AddressBus` routes CPU accesses to mapped regions and borrows those regions; the system keeps them alive. RetroArch callbacks and lifecycle handling stay in the libretro adapter, outside CPU and hardware components. The adapter catches exceptions during `retro_run`, logs the failure, and leaves emulation faulted until reset or content reload so a C++ exception cannot escape the libretro callback.

## Save-state boundary

`GameBoySystem` exports/imports versioned value-only snapshots between M-cycles.
`StateCodec` validates a separate candidate before committing, preserving live
references, callbacks and published cartridge storage addresses. Libretro owns
the file/slot interface and contains exceptions. See [save states](save-states.md)
for the schema, version policy, RTC behavior and validation scope.

## Main code boundaries

| Area | Location | Responsibility |
| --- | --- | --- |
| CPU and instruction execution | `src/lib/GBCPU.cpp`, `src/lib/decoder.cpp`, `src/lib/instSubFunc*.cpp` | Execute instructions in M-cycle steps and report memory accesses, flags, and CPU state. |
| System composition and clock | `src/lib/game_boy_system.cpp`, `src/lib/system_clock.cpp` | Own and connect hardware, initialize the default post-boot profile, and advance components from CPU time. |
| Memory and hardware | `src/lib/address_bus.cpp`, `src/lib/cartridge.cpp`, `src/lib/memory_region.cpp`, `src/lib/timer.cpp`, `src/lib/dmg_apu.cpp`, `src/lib/dmg_ppu.cpp`, `src/lib/dmg_oam_dma.cpp` | Implement mapped memory regions, ROM-only/MBC1/MBC2/MBC3/MBC5 cartridges, cartridge RAM, and DMG devices. |
| Frontend adapter | `src/libretro/libretro_core.cpp` | Load supported content, poll RetroArch input, run frames, contain runtime exceptions, and deliver video/audio callbacks. |
| Tests and test ROMs | `src/unit_test/`, `tests/roms/` | Check observable component, system, rendering, and frontend behavior. |

## Development workflow

1. Pick a bounded behavior from the [roadmap](roadmap.md) and state what an external observer should see: registers, flags, cycles, memory accesses, interrupts, pixels, audio samples, or input response.
2. Check the relevant hardware reference and existing test ROMs. Record model and boot assumptions when a ROM depends on them.
3. Add a focused regression at the narrowest useful boundary, then implement the behavior in its owning CPU or hardware component. Keep frontend-specific code in the libretro adapter.
4. Run the [standard testing workflow](testing.md). Use GBIT or a targeted ROM when it covers the changed behavior; investigate disagreements against hardware behavior.
5. Update the roadmap when milestone status, known limits, or implementation order changes. Update this document when ownership or runtime flow changes.

`make test` covers the in-repository unit and libretro adapter tests. GBIT integration is a separate check; use the [testing workflow](testing.md) and the CI workflow's pinned revision.

## Refactoring guideline

- State the externally visible behavior or invariant before changing structure. Make one small structural change at a time and keep it behavior-preserving; handle emulation behavior changes separately.
- Give a function or component a clearer owner only when that improves naming, state ownership, or a useful test boundary. File length or a design-pattern name alone is not a reason to add an abstraction.
- Keep the start, advance, and completion of a clocked operation under one owner. Return whether the operation occupied the current dot rather than deriving that from its state afterward: an OBJ fetch can become idle while its completion dot still stalls pixel output.
- Re-run checks that cover the affected behavior, including a focused ROM or image/audio comparison when relevant. Keep known model limits explicit instead of treating a passing test as proof of full hardware accuracy.

These project-specific notes are informed by [Martin Fowler's *Refactoring* (2nd edition)](https://martinfowler.com/books/refactoring.html); they summarize our workflow in original wording.

## Serial scope

`DmgSerial` owns `$FF01` (SB) and `$FF02` (SC) and advances from the system
clock, including while the CPU is in HALT. Its free-running 512-T-cycle bit
clock is independent of Timer/DIV and is not restarted by SC writes.
In internal-clock mode it shifts in one high bit per edge with no connected
peer; after eight bits it clears SC bit 7 and requests the serial interrupt.
External-clock mode remains pending because no peer supplies a clock.
Clearing SC bit 7 aborts without completing or requesting an interrupt.
The synthetic serial clock starts at phase zero; exact model-specific
post-boot phase, SC-write edge races, link cables, accessories and CGB fast
serial are not covered by this baseline.


## Readable LY versus PPU progress

The internal scanline and dot position determine rendering and PPU modes.
Readable LY can advance before a mode boundary and resets to zero early on
the last VBlank line, while Mode 1 continues. LYC comparison is a separate
clocked signal; it briefly compares 153 after readable LY is already zero,
then changes to zero. Preserve the LYC=0 signal through frame rollover so a
continuous match does not create another STAT rising edge. Do not derive
rendering/mode progression directly from the value returned by the LY register.
