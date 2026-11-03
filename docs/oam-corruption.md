# DMG OAM corruption

LotusGB models DMG Mode-2 OAM corruption from CPU reads, writes and IDU
(increment/decrement unit) bus events. Native CGB is unaffected. Golden-data
component tests, CPU event traces and an original guest probe cover the
implemented contract. The test suite checks row selection at each M-cycle
boundary through the end of Mode 2. Exact sub-M-cycle bus phase,
hardware-revision differences and OAM corruption patterns caused by active
OAM DMA remain outside this model. The separate PPU behavior
that hides objects when OAM DMA overlaps Mode 2 is covered in the
[DMG validation matrix](dmg-validation.md).

The rules follow [Pan Docs OAM corruption](https://github.com/gbdev/pandocs/blob/master/src/OAM_Corruption_Bug.md).
During the OAM scan, the PPU selects an eight-byte row per M-cycle. Accesses
to the entire `FE00–FEFF` page, including its unusable part, can alter that row;
the CPU-requested offset and write value do not select the corrupted bytes.
The first row is protected. Read and write patterns differ, and a read sharing
an IDU update has an additional multi-row pattern except at the documented
first-four/last-row boundaries.

## CPU and bus contract

`IMemoryAccess` adds `readWithIdu`, `writeWithIdu` and `notifyIdu`. Flat-memory
and GBIT backends default to ordinary reads/writes or no extra IDU effect;
`AddressBus` synchronizes clocked devices then reports one combined event to
the PPU before dispatching data. Device-originated `readForDma` bypasses CPU
notifications. The live system installs the corruption observer only for DMG.

The CPU reports:

- INC/DEC BC, DE, HL and SP using the register's value before the operation.
- HL auto-increment/decrement loads and stores as combined access/IDU events.
- PUSH/CALL/RST and interrupt entry as an internal SP event, a combined stack
  write/decrement, then an ordinary write.
- POP/RET as a combined first read/increment then an ordinary second read.
- Opcode and immediate reads with PC increment, including crossings from FDFF.
  HALT-bug fetches omit the PC-increment event. Untaken conditional stack
  operations do not emit stack events.

Instruction bus-request flags are cleared before each M-cycle and consumed
within it. They are not pending hardware state at the snapshot boundary.
Corrupted OAM, PPU scan progress and CPU instruction progress already belong to
the snapshot; format v4 is unchanged. Host observers are reconstructed wiring.

## Automated evidence and limits

`make test` checks literal golden OAM words for read/write/IDU/combined patterns,
protected and excluded rows, FE-page address gating, LCD-off/Mode-3 behavior,
native CGB immunity, the Read+IDU multi-row thresholds at rows 3/4 and 18/19,
and the selected row at all 20 M-cycle boundaries plus the Mode-3 edge. CPU
tests check event order, pre-update addresses,
stack/interrupt sequences, PC crossings, HALT bug and untaken conditions.

An original program fills OAM with a known byte pattern, reaches visible-line
Mode 2 and executes `INC BC` with BC=`FEFF`. It later copies OAM to cartridge
SRAM through ordinary CPU instructions. DMG changes one scan row; CGB preserves
all bytes. A snapshot before the INC reproduces the CPU-observed bytes, PCM and
complete state exactly after replay.
The pinned GBIT comparison and the local standard 68-case Mooneye `all` group
also pass after the CPU bus-interface change. Mooneye's report is
`build/test-roms/oam-corruption-mooneye/report.md`; its external ROMs are not
part of the repository or CI.

These tests validate the documented patterns at the emulator's M-cycle bus
phase. The boundary sweep verifies the implementation's row mapping; it does
not establish the physical sub-M-cycle phase. They do not claim full Blargg
OAM-bug suite conformance, physical-model revision coverage, or rendering
accuracy during simultaneous DMA/OAM access. No commercial ROM or derived
artifact is used in the committed tests.
