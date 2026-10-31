# DMG save states

LotusGB implements versioned DMG snapshots through `GameBoySystem::saveState()` /
`loadState()` and libretro's serialization callbacks. A snapshot restores the
machine's execution state, including cartridge RAM. It is separate from the
game's own battery-save mechanism. Current validation is recorded in the
[DMG validation matrix](dmg-validation.md).

## Using a snapshot in RetroArch

1. Build with `make build` and load the resulting LotusGB core. Restart
   RetroArch if it had an older build loaded. On the tested Steam Deck setup,
   reinstall the updated `.so` through the GUI core-install action first;
   changing the original installation source file alone did not update the
   installed core. See the [Deck update procedure](steam-deck-check.md).
2. Load the same ROM and optional DMG boot ROM used for the snapshot.
3. In Quick Menu, select a state slot and use **Save State**.
4. Continue playing, then use **Load State** to return to the saved moment.

On the macOS build with a native menu bar, the equivalent commands are
**Command → Save State Options → Save State / Load State**.

For a frontend checkpoint, use a homebrew game and record:

1. Load `build/lotusgb_libretro.dylib` from this checkout and start the game.
2. Save during gameplay and note an observable position or score.
3. Advance until that position or score changes, then load the state.
4. Confirm the saved scene returns and directional input still works.
5. Close and reopen the content, then load the same state again to check the
   frontend file round trip. Record the frontend version and core hash alongside
   the result. Steam Deck remains a separate device check.

RetroArch owns the files, slots and hotkeys. LotusGB returns snapshot bytes and
accepts them back; it does not choose a filename or write state files itself.
Loading a state also restores cartridge SRAM/RTC memory. RetroArch may later
persist that restored memory to the game's battery save. Keep a separate backup
of valuable battery saves before returning to an earlier state.

A corrupt snapshot, another ROM, a different boot-ROM configuration, or an
unsupported format/model is rejected with a log message. A failed load leaves
the current machine intact. A successful load may recover a machine that
faulted after the snapshot was made.

## macOS RetroArch acceptance (2026-10-30)

The bounded M5 macOS frontend checkpoint passed with the licensed GB Pong
test copy `build/test-roms/save-states/lotus-state-smoke.gb`. Saving, advancing,
restoring and further input worked, including restoration after a full
RetroArch relaunch. The separate [Steam Deck checkpoint](steam-deck-check.md#m5-save-state-checkpoint--user-reported-pass-2026-10-31)
also passed by user report on 2026-10-31; installed-core hash and frontend/OS
versions were not supplied.

| Session identity | Value |
| --- | --- |
| Platform | arm64 macOS 27.0.1 (26A434) |
| Frontend | `/Applications/RetroArch.app`, RetroArch 1.22.2 |
| Loaded core | `build/lotusgb_libretro.dylib` |
| Core SHA-256 | `93d9e02e5bce9ae0ea2bbec87b4f8dc9ba11bbb948d6ec7dcfe8cefe53f8f842` |
| ROM SHA-256 | `9c5e337db0ee1416f2c7f1de02e7e0a78a6064cf1da49209b4bc1a36a7b35415` |
| Startup | Fast startup; configured system directory contained no BIOS |
| State | Slot 0, `~/Documents/RetroArch/states/LotusGB/lotus-state-smoke.state`, 2,160 bytes (frontend container/compression, not the raw 512 KiB core buffer) |
| State-file SHA-256 | `257f1906ddfa362a0217d297159215c3407dd5131e5db97e3ea9475585c5ed96` |

The session used the native **Command → Save State Options** menu:

1. Paused gameplay with the left paddle at screenshot Y=646–965 and the ball
   near the lower right, then saved slot 0. RetroArch displayed successful save.
2. Resumed and advanced to a visibly different ball/score scene, then loaded
   slot 0. A frame advance refreshed the paused display to the saved scene.
3. Resumed and held Up for one second. The left paddle moved to Y=86–405.
4. Loaded the same slot again. **File → Close** ended the frontend process;
   Computer Use reported it no longer running. Relaunched the same application,
   explicitly loaded the same core and ROM, paused, and loaded slot 0.
5. The saved scene returned after a frame advance. Resumed and held Down for
   one second; the left paddle moved to Y=886–1205. Left the game paused.

Local screenshots and `image-checks.json` are under the ignored directory
`build/test-roms/save-states/retroarch-macos-2026-10-30/`. Both restored
screenshots match the pre-save playfield crop `(0, 220, 1600, 1320)` exactly;
the crop excludes the title bar, pause icon and notification area. This is a
scene check, not a whole-frame timing comparison. The state-file hash remained
unchanged across the relaunch and load. Short synthetic key taps initially
produced no visible paddle movement; holding the keys supplied the observable
input evidence. A paused load required a frame advance to update the display.

This session verifies one ROM, core build, startup configuration and frontend.
It does not establish Steam Deck support, interactive RTC behavior, audio
quality or broad game compatibility. Earlier automated test results below
were retained; this documentation-only acceptance session did not rerun them.

## State ownership and restoration

`GameBoySystem` owns the snapshot operation. `StateCodec` in
`src/lib/save_state.cpp` is the explicit value-only schema and is a friend of
the hardware components. It serializes:

- CPU registers, the prefetched base/CB opcode identity, instruction temporary
  values and M-cycle position, delayed IME, HALT/STOP and interrupt entry.
- WRAM/HRAM, interrupt registers, timer reload phases, serial shift position,
  selected/pressed joypad lines, and DMA progress.
- PPU VRAM/OAM, current and completed frames, scanline/dot position, BG/OBJ
  FIFOs and fetchers, and window/STAT latches.
- APU registers, channel counters, wave RAM, envelope/sweep/frame sequencer,
  sample phase, high-pass filter history, and pending PCM.
- Mapper bank/enable registers, all cartridge RAM, MBC3 live/latched RTC
  registers and backing persistence block, boot mapping latch, and elapsed
  system time.

Snapshots are taken between public M-cycle steps, including when the CPU is
partway through an instruction. ROM and BIOS bytes, decoder objects, pointers,
callbacks, bus mappings, frontend pixel format and diagnostic logs are not
serialized. Immutable ROM/BIOS content is identified by SHA-256.

Loading parses into a separately constructed machine, checks lengths, types,
component IDs and safety bounds, and resolves the CPU instruction through the
new machine's decoder. Only after validation does a non-allocating commit copy
state into the live components. SRAM and RTC contents are copied in place so
pointers already published to RetroArch remain valid. Live bus references and
callbacks remain attached to their original owners.

### RTC policy

The existing MBC3 wall-clock policy is retained: a state saves the clock's
absolute timestamp, live/latched registers and HALT flag. A running RTC catches
up with wall time on the next RTC access, including time elapsed since the
snapshot. A halted RTC remains halted. Lazy, not-yet-loaded RTC persistence
blocks are preserved as well. Exact replay across different wall times is
therefore only claimed for non-RTC or halted-RTC scenarios, not a running RTC.

## Format and compatibility

Version 1 uses a 116-byte header: eight-byte magic `LOTUSST\0`, 32-bit version,
32-bit hardware model (`1` = DMG), 32-bit used length, and three SHA-256 digests
for ROM, boot ROM (empty for fast startup) and payload. Integers use little
endian encoding, booleans one byte, enums 32 bits, and signed integers two's
complement. Padding and C++ object layout are never serialized.

The payload starts with the CPU instruction-table selector and then ordered,
numbered component sections. Section order and field order are part of the
versioned schema. Vector lengths are checked against the selected cartridge
or fixed memory geometry before loading. Extra frontend buffer bytes must be
zero. Digests detect corruption; they do not replace semantic validation.

Libretro advertises a fixed 512 KiB buffer while content is loaded (zero with
no content). Core snapshots use only the header's used length; the adapter
zero-fills the remainder. Pending PCM is bounded to 96,000 interleaved samples
(one second); standalone callers must drain audio regularly. Exceeding that
limit fails explicitly. `retro_run()` already drains PCM every frame.

Future CGB support can use a new model identifier and a revised schema.
Changes to field meaning/order require a version change or explicit migration.
There is no promise of compatibility with arbitrary future core versions.
[Rewind](rewind.md) passed its bounded macOS checkpoint and the Deck
rewind/release/control checkpoint by user report with Ping Pong. Long-session
stability and measured Deck performance remain unverified. Run-ahead and
replay performance remain separate validation targets.

## Automated checks

The 2026-10-31 M5 closeout reran `make build`, full `make test`, and the
LotusGB GBIT comparison against the CI-pinned GBIT revision
`40c5247b818a7c524e3c3e75fe0fde900a4ee8e9`; all passed. The comparison
reported 244/244 ordinary and 256/256 CB opcodes. The macOS and delivered
Linux core hashes still matched the frontend acceptance records. Local logs
are `build/m5-final-build.log`, `build/m5-final-test.log` and
`build/m5-final-gbit.log`. This was a local macOS gate, not a new Ubuntu CI run
or another device session.

`make test` includes `save_state_test` and libretro snapshot regressions:

- Machine-cycle checkpoints and future execution comparisons, including CB
  instructions, delayed IME, HALT bug, interrupt entry, STOP wake-up, active
  LCD/audio/DMA/timer/serial work, and the boot overlay.
- MBC1/2/3/5 RAM restoration, maximum MBC5 RAM, lazy and halted RTC state, and
  stable published SRAM/RTC addresses.
- Invalid header/version/model/content/checksum, truncated data, invalid typed
  fields with a recomputed checksum, and atomic rejection.
- GB Pong runs through the actual libretro callbacks. A snapshot is restored
  after unloading/reloading content; all 30 subsequent video frames and PCM
  batches match the original input sequence in both negotiated pixel formats.
- SHA-256 known-answer tests, including padding across a block boundary and a
  million-byte input.

These checks use synthetic programs and the existing licensed GB Pong asset.
They establish bounded save/restore behavior, not whole-library compatibility,
physical-hardware accuracy or Steam Deck frontend validation.
