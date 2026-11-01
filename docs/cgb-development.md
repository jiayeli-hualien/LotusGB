# CGB implementation and playability

LotusGB now loads native CGB games through RetroArch, with `.gb` and `.gbc`
content advertised by both the libretro API and `lotusgb_libretro.info`.
The first bounded playability checkpoint passes with **Tobu Tobu Girl Deluxe**:
color gameplay, directional input, flap/dash and exact snapshot replay.
`cgb-acid2` v1.1 matches all **23,040 RGB555 pixels** of its official reference.
These results do not establish compatibility with every GBC game or complete
CGB hardware accuracy.

The initial hardware target is **CGB-E**. The frontend selects native CGB when
cartridge header `$0143` bit 7 is set, including dual-mode games distributed
as `.gb`. Other cartridges retain the existing DMG path. Filename extensions
control content discovery, not hardware behavior. DMG games running on CGB
hardware in compatibility mode are not exposed as a separate mode.

## Play in RetroArch

1. Run `make build` and install the matching core and `.info`. On macOS,
   `make install-retroarch-core-macos` updates both and creates RetroArch's
   `core_info.refresh` marker so the next startup rebuilds its info cache.
   Override `RETROARCH_CORE_DIR` / `RETROARCH_INFO_DIR` for other locations.
2. Restart RetroArch if an older core is loaded. Choose LotusGB and load a
   supported `.gb` or `.gbc` cartridge. Native CGB uses a synthetic fast
   post-boot profile; **no CGB BIOS is required or loaded by this adapter**.
3. Play with the same Game Boy controls. RetroArch owns battery saves,
   snapshot files, state slots and rewind. CGB snapshots use version 2/model 2;
   existing DMG version 1 snapshots retain their format.

On 2026-11-01, the local macOS installation was updated and RetroArch 1.22.2
(`69a4f0ea`) loaded the installed core, restored a Deluxe gameplay snapshot,
ran 300 callbacks with video/audio and initialized its 20 MiB rewind buffer.
The GPU screenshot was inspected. Its rebuilt info cache contains `gb|gbc`
and both Nintendo Game Boy databases. This is an automated macOS frontend
checkpoint, not a manual controller/handfeel, listening or Steam Deck result.

## Implemented contracts and evidence

| Area | Implemented behavior | Evidence / scope |
| --- | --- | --- |
| Shared CPU / bus / cartridge | Existing instruction engine, decoders, memory dispatcher and ROM-only/MBC1/2/3/5 implementations serve both models; MBC30 adds 4-MiB ROM / 64-KiB SRAM geometry. | CPU/DMG regressions and pinned GBIT remain required. Original MBC30 tests cover high ROM/SRAM banks, RTC/battery ownership and CGB state replay. |
| Startup | Explicit model selection; CGB A=`$11`, F=`$80`, BC=`$0000`, DE=`$FF56`, HL=`$000D`, SP=`$FFFE`, PC=`$0100`, IME off; white BG palettes and deterministic RAM/OBJ palette defaults. | Original probe checks handoff. This synthetic profile does not reproduce all BIOS VRAM/OAM contents or exact power-on phases. |
| WRAM | 32 KiB; fixed bank 0 at `$C000–CFFF`, bank 1–7 at `$D000–DFFF`; SVBK zero aliases bank 1 while preserving raw zero readback. Echo uses the existing bus alias. | Address-line, full-bank retention, echo-boundary and CPU tests in `cgb_memory_test`. |
| VRAM | 16 KiB in two banks; VBK selects CPU access, while PPU fetches explicitly select their bank. CPU access windows are applied by the PPU before storage dispatch. | Shared-storage rendering/access tests in `ppu_video_memory_test`. CGB-specific access-edge timing is not hardware-verified. |
| Color PPU | BG/window bank-1 attributes, tile bank selection, palette selection, X/Y flips, BG/OBJ priority and native OAM-order object priority; RGB555 live/completed frames. | Exact official `cgb-acid2` reference through the native system and both libretro pixel formats. Retains the existing approximate fetch timing. |
| Palette ports | Independent 64-byte BG/OBJ palette RAM, index masks, wraparound, Mode 3 read/write blocking and write auto-increment even when blocked. | Focused `cgb_hardware_test` and color-ROM tests. |
| Speed / audio | KEY1-armed STOP pauses CPU execution for 2050 M-cycles at the outgoing speed, resets DIV and switches rate on resume. CPU/timer/serial/OAM DMA use CPU-rate clocks; PPU/APU/VRAM DMA retain base-clock rates. APU divider tap changes with speed. | Clock-rate, CPU program, PCM-count and snapshot tests. Speed-switch PPU locking/rendering quirks and exact revision-specific transitions remain approximate. |
| VRAM DMA | FF51–55 source/destination masks, GDMA CPU stalls, 16-byte HBlank blocks, cancellation and remaining-count readback; 32 base T-cycles per block. | Focused copy/timing/cancel tests and system snapshots with active DMA. Conflicts with OAM DMA and invalid-source behavior are not full hardware models. |
| Other registers | Native serial fast-clock selection with an unconnected incoming line; KEY1 readback; unconnected IR register and FF72–75 storage/masks. | Fast serial and peripheral revisions have narrower coverage than the playable DMG path; no link/IR peer. KEY0/PGB switching is not exposed. Native OPRI stays locked to OAM order. |
| Snapshots / frontend | CGB banks, palettes, color buffers, fetch/OBJ metadata, DMA and speed-switch progress round-trip through an independent candidate before commit. SRAM/RTC pointers remain stable. | Native checkpoints plus repeated libretro rewind/replay check exact video, PCM and complete end state in XRGB8888 and 0RGB1555. |

The PPU fetches the same video storage owned by `GameBoySystem`; no shadow
VRAM needs synchronization. The DMG path retains its internal 8 KiB array
and original serialized field order. See the [architecture](architecture.md)
and [snapshot format](save-states.md).

References: [CGB registers](https://gbdev.io/pandocs/CGB_Registers.html),
[tile attributes and priority](https://gbdev.io/pandocs/Tile_Maps.html),
[color palette ports](https://gbdev.io/pandocs/Palettes.html),
[power-up sequence](https://gbdev.io/pandocs/Power_Up_Sequence.html), and
[VRAM/OAM access](https://gbdev.io/pandocs/Accessing_VRAM_and_OAM.html).

## Reproducible checks

The required gate is `make build`, `make test` and the pinned GBIT comparison
in [testing](testing.md). `make test` includes the original CGB memory/boot,
shared-VRAM, hardware-register/DMA and system/color tests, followed by both
libretro presentation policies. It requires no downloaded games. The MIT
[cgb-acid2 v1.1 assets](../tests/roms/cgb-acid2/README.md) are pinned in the
repository; the upstream test does not require WRAM banking or double speed,
so those have separate tests.

The original CGB exercise ROM initializes palettes, transfers VRAM with GDMA
and HBlank DMA, switches speed, emits a tone, and changes a red field to blue
on Right input. System checkpoints cover in-flight work and reproduce future
PCM/state exactly. Libretro tests additionally verify `.gb`/`.gbc` content,
header-based selection, both pixel formats, three rewind/replays and reset
with stable SRAM.

For the external Deluxe game check:

```sh
make test-tobudx TOBUDX_ROM=/path/to/tobudx.gb
```

The accepted 262,144-byte ROM is the author's free `tobudx.gb` download:
SHA-256 `0a0e8018dbbc8d7f8cd99f05e7cdc7b4cc9e358ecfe9377ebfb2291a84c6e310`.
The [publisher page](https://tangramgames.itch.io/tobu-tobu-girl-deluxe)
licenses code under MIT and assets under CC BY 4.0 and links the
[source](https://github.com/SimonLarsen/tobutobugirl-dx). The official download
is distributed from that page without a separate binary restriction in its
free download flow. Keep the ROM and game-derived artifacts outside the
repository/CI; this optional check does not download or redistribute it.
Game credits: Tangram Games / Simon Larsen; music by potato-tan.

The check presses Start at calls 900, 1200 and 1500, waits for the game-entry
animation, then tests checkpoints **1780–1791**. For each checkpoint it restores
identical state and compares neutral, one-poll and held input across eight
calls. Left/right/B/right+A/right+B all visibly respond on call **2**; A alone
is diagnostic. Compound actions differ from Right alone. Neutral replays
produce identical video, PCM and complete state, and post-restore input works.
This measures core/game response, not physical controller/display latency.

Local evidence is under `build/test-roms/cgb-playable/`: the full gate log,
`report.json`, `tobudx-acceptance/` (72 measurements and contact sheet), and
`retroarch/` (isolated configs, logs, restored state, screenshot and PCM).
Before-install core/info/cache backups remain in `installed-before/`.

## Remaining scope

Broader game compatibility, CGB hardware-specific Mode 3/OAM bus timing,
DMA collision behavior, speed-switch display quirks and physical revision
verification remain work. A full CGB boot animation/BIOS path, selectable DMG
compatibility mode on CGB, PGB, connected serial/IR and rumble are not part of
this checkpoint. The M8 playable checkpoint in the [roadmap](roadmap.md) is
bounded by the evidence above.

## Foundation history

The 2026-11-01 memory increment established raw SVBK/VBK, bank retention and
an original CPU probe (`11 F8 22 33 11 FF AA BB` in 154 M-cycles). Its log is
`build/test-roms/cgb-memory-foundation/validation.log`.
The split boot probe established `$0000–00FF` / `$0200–08FF` overlays with the
cartridge header hole, mapper writes and one-way FF50 unmapping. Its image
contract is address-indexed 0x900 bytes (0x800 mapped bytes plus a 0x100-byte
ignored hole); packed 0x800 images are rejected. `DmgBootRom` remains a source
alias for `BootRom`. This synthetic reset factory intentionally stays LCD-off
and rejects snapshots; the playable CGB constructor is a separate startup path.
The boot gate log is `build/test-roms/cgb-boot-composition/validation.log`.

The subsequent shared-VRAM increment compared two monochrome frames and
per-dot mode/LY/IRQ signals while VBK selected a different CPU bank. Its
before/after-core test produced byte-identical DMG version 1 snapshots at
calls 0, 1, 64 and 72. See `build/test-roms/ppu-shared-vram/report.json`.
