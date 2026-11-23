# GBA implementation status

LotusGBA is an experimental, separate libretro core. Automated repository checks cover ARM and Thumb instructions, licensed homebrew gameplay, selected video modes, audio output, cartridge save-memory families, and RTC behavior. These checks establish the listed test contracts; they do not establish broad commercial-game compatibility or hardware fidelity.

Build and run the repository checks with `make build` and `make test`. ROM provenance and test scope are documented beside the licensed assets in [`tests/roms/`](../tests/roms/).

## Implemented behavior and bounded checks

| Area | Implementation and evidence | Remaining limits |
| --- | --- | --- |
| CPU | Separate ARM7TDMI; licensed ARM/Thumb instruction suites through libretro | Untested instruction edge cases; prototype base cycles and incomplete pipeline timing |
| Startup | Optional 16-KiB BIOS; Automatic/Use BIOS/Skip BIOS option; HLE startup and selected SWIs | No BIOS bundled; incomplete HLE calls and BIOS timing |
| Video | Modes 0–5, text/affine backgrounds, regular/affine OBJ, windows and blending; original system/libretro pixel checks | Raster boundaries and rendering budgets are approximations, not hardware conformance |
| Audio | PSG, Direct Sound FIFO, SOUNDBIAS and 48-kHz stereo; licensed tone/routing checks | No broad listening or analog hardware comparison |
| Memory and timing | ROM/EWRAM/IWRAM/I/O/video maps, timers, interrupts, DMA and WAITCNT bus waits | Game Pak prefetch and complete CPU/DMA bus timing remain approximate |
| Battery saves | SRAM, 64/128-KiB Flash, EEPROM and GPIO RTC; original persistence and offline-progression checks | No GBA save-state serialization |
| Frontend | 240×160 output, local joypad, libretro battery memory and local two-ROM SIO subsystem | Rewind/run-ahead and general game/network compatibility are not established |

The licensed Pong fixture checks menu entry, ball movement, paddle input and
collision, and a CPU score. This is an automated gameplay checkpoint. See the
[ROM sources and terms](../tests/roms/) and `src/gba_libretro/` tests for the
reproducible scope. Unsupported CPU instructions stop with an explicit fault.

## SIO and RFU link

Local two-ROM multiplayer SIO and remote RFU use separate paths. The remote
Netpacket frontend has experimental Emerald Wireless Adapter (`BPEE`/`BPEJ`)
and English Emerald (`BPEE`) Link Cable profiles. Players select the device
in a core option and reload content to apply it. Starting the remote session
recreates the guest using its current battery memory and BIOS selection.
See the [link play guide](gba-link-play.md) for player-facing setup and
bounded compatibility, the [RFU protocol](gba-rfu-protocol.md) for radio
and NI tests, and the [cable protocol](gba-pokemon-cable.md) for cable
transport and validation.

Commercial ROMs, saves and game-derived captures stay private. Public
documents record bounded observations; reproducible repository checks use
licensed ROMs and original fixtures.

## HLE BIOS arithmetic

Without an external BIOS, SWI `0x08` implements unsigned integer square root:
`r0 = floor(sqrt(r0))`. Focused CPU tests cover ARM/Thumb calls, rounding around
perfect squares, the full unsigned input range boundaries, continued execution,
and external-BIOS vector dispatch. The HLE result follows
[GBATEK](https://mgba-emu.github.io/gbatek/#biosarithmeticfunctions); BIOS cycle
timing and scratch-register side effects are not reproduced. This does not
establish game-level capture compatibility.
