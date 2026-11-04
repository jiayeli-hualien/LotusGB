# CGB implementation and playability

LotusGB now loads native CGB games through RetroArch, with `.gb` and `.gbc`
content advertised by both the libretro API and `lotusgb_libretro.info`.
The first bounded playability checkpoint passes with **Tobu Tobu Girl Deluxe**:
color gameplay, directional input, flap/dash and exact snapshot replay.
`cgb-acid2` v1.1 matches all **23,040 RGB555 pixels** of its official reference.
These results do not establish compatibility with every GBC game or complete
CGB hardware accuracy.

The automated libretro gate also exercises original CGB-compatible MBC1 and
MBC5 battery game fixtures: color output and directional response work,
gameplay advances cartridge SRAM, and a fresh content instance resumes from
restored SRAM. The optional local homebrew profile pairs Big2Small's MBC5 save
cycle with Deluxe's CGB/MBC1 gameplay and input-state replay.

## Pokemon Crystal post-update check (2026-11-04)

The user reports that Pokémon Crystal single-player playability was complete
before the latest core update. After installing the current Universal core into
the macOS Steam RetroArch distribution, the user confirmed that Crystal runs,
an in-game save can be made, and the save is still usable after reopening. The
user accepted this updated build for the bounded personal-use target. The
installed core SHA-256 is
`2dd7b70c1131383796da1c8e684e488ff6d5ec65eecf9abd9000d1f8292f87fa`. The ROM
region and earlier route/coverage were not recorded; this post-update check
confirms the tested play/save/resume path, not every story route or long-term
real-title RTC progression.

The initial hardware target is **CGB-E**. The frontend selects native CGB when
cartridge header `$0143` bit 7 is set, including dual-mode games distributed
as `.gb`. Other cartridges default to the existing DMG path. The `lotusgb_model` core
option can force DMG or CGB hardware; CGB hardware now supports boot-selected
DMG compatibility. See the [mode contract and evidence](cgb-compatibility.md).
Filename extensions control content discovery, not hardware behavior.

## Play in RetroArch

1. Run `make build` and install the matching core and `.info`. On macOS,
   `make install-retroarch-core-macos` updates both and creates RetroArch's
   `core_info.refresh` marker so the next startup rebuilds its info cache.
   Override `RETROARCH_CORE_DIR` / `RETROARCH_INFO_DIR` for other locations.
2. Restart RetroArch if an older core is loaded. Choose LotusGB and load a
   supported `.gb` or `.gbc` cartridge. Native CGB uses a synthetic fast
   post-boot profile unless `cgb_boot.bin` (or `gbc_bios.bin`) is present in the
   system directory. The optional image must use the 2304-byte address-indexed
   layout; see the [loader contract](#optional-cgb-bios-through-libretro-2026-11-02).
3. Play with the same Game Boy controls. RetroArch owns battery saves,
   snapshot files, state slots and rewind. Current snapshots use DMG v11/CGB v13 with a hardware-model tag;
   legacy DMG v1/v3/v4/v6/v7/v8/v9/v10 and CGB v2–v12 snapshots remain loadable. See the [format](save-states.md).

On 2026-11-01, the local macOS installation was updated and RetroArch 1.22.2
(`69a4f0ea`) loaded the installed core, restored a Deluxe gameplay snapshot,
ran 300 callbacks with video/audio and initialized its 20 MiB rewind buffer.
The GPU screenshot was inspected. Its rebuilt info cache contains `gb|gbc`
and both Nintendo Game Boy databases. This is an automated macOS frontend
checkpoint, not a manual controller/handfeel, listening or Steam Deck result.

## Implemented contracts and evidence

| Area | Implemented behavior | Evidence / scope |
| --- | --- | --- |
| Shared CPU / bus / cartridge | Existing instruction engine, decoders, memory dispatcher and ROM-only/MBC1/2/3/5 implementations serve both models; standard 1-MiB MBC1M multicarts use alternate bank wiring; MBC30 adds 4-MiB ROM / 64-KiB SRAM geometry; MBC5/MBC7 expose rumble state to the optional frontend interface; MBC7 adds banked ROM, sensor registers and 256-byte serial EEPROM storage. | CPU/DMG regressions and pinned GBIT remain required. Original CGB MBC3 tests cover guest RTC registers and fresh-content battery import; MBC30 tests cover high ROM/SRAM banks, RTC/battery ownership and CGB state replay. MBC5/MBC7 rumble tests cover register/RAM-bank separation, frontend callbacks and snapshot restore. MBC7 register/EEPROM tests and DMG/CGB partial-command snapshot replay are covered below. |
| Compatibility | Boot-selected KEY0/OPRI mode, native-register gates, BGP/OBP palette remapping on physical CGB, hardware-selection core option and model-tagged snapshots. | Original boot/CPU/render/rewind checks in both frontend pixel/presentation formats; see the [mode contract](cgb-compatibility.md). |
| Startup | Explicit model selection; CGB A=`$11`, F=`$80`, BC=`$0000`, DE=`$FF56`, HL=`$000D`, SP=`$FFFE`, PC=`$0100`, IME off; white BG palettes and deterministic RAM/OBJ palette defaults. | Original probe checks handoff. This synthetic profile does not reproduce all BIOS VRAM/OAM contents or exact power-on phases. |
| WRAM | 32 KiB; fixed bank 0 at `$C000–CFFF`, bank 1–7 at `$D000–DFFF`; SVBK zero aliases bank 1 while preserving raw zero readback. Echo uses the existing bus alias. | Address-line, full-bank retention, echo-boundary and CPU tests in `cgb_memory_test`. |
| VRAM | 16 KiB in two banks; VBK selects CPU access, while PPU fetches explicitly select their bank. CPU access windows are applied by the PPU before storage dispatch. | Shared-storage rendering/access tests in `ppu_video_memory_test`. CGB-specific access-edge timing is not hardware-verified. |
| Color PPU | BG/window bank-1 attributes, tile bank selection, palette selection, X/Y flips, BG/OBJ priority and native OAM-order object priority; RGB555 live/completed frames. Native CGB treats LCDC.0 as BG/OBJ priority control while CGB DMG compatibility uses it to enable/disable BG and Window display. Native CGB clears the Window Y condition when LCDC.5 is cleared. | Exact official `cgb-acid2` reference through the native system and both libretro pixel formats; `cgb_hardware_test` checks LCDC.0's model-specific output/priority behavior and distinguishes native CGB from DMG and CGB DMG-compatibility Window behavior. Retains the existing approximate fetch timing. |
| VBlank/STAT edge | CGB hardware asserts the line-144 Mode 2 STAT source one M-cycle before VBlank; DMG asserts its Mode 2 source at VBlank entry. | Mooneye `misc/ppu/vblank_stat_intr-C` passes with CGB-E synthetic startup. `dmg_ppu_test` checks the edge on native CGB and CGB compatibility, plus a DMG timing control. Physical revision timing remains unverified. |
| Palette ports | Independent 64-byte BG/OBJ palette RAM, index masks, wraparound, Mode 3 read/write blocking and write auto-increment even when blocked. | Focused `cgb_hardware_test` and color-ROM tests. |
| Speed / audio | KEY1-armed STOP pauses CPU execution for 2050 M-cycles at the outgoing speed, resets DIV and switches rate on resume. CPU/timer/serial/OAM DMA use CPU-rate clocks; PPU/APU/VRAM DMA retain base-clock rates. APU divider tap changes with speed. | Clock-rate, CPU program, PCM-count and snapshot tests; all four entry-mode PPU locks and both switch directions have automated checks. An original scanline fixture confirms OAM DMA completes early enough to expose the next line's object scan only at double speed. Exact boundary/revision effects remain unverified. |
| PCM12 / PCM34 | Native CGB maps read-only FF76/FF77 to packed CH1/2 and CH3/4 digital outputs before master mixing. Inactive channels read zero; DMG keeps these addresses unmapped. | Component tests check nibble order, pulse low/high levels, wave sample/volume shifts, noise steps, DAC/power-off behavior, ignored writes and independence from panning/volume. An original CPU probe covers both hardware models and active-generator snapshot replay. Existing generator timing remains approximate. |
| Output high-pass filter | Shared fixed-point filter uses model-specific charge factors: DMG `0.999958`, CGB `0.998943` per master clock, converted once to Q16 coefficients at 48 kHz. | Constant-wave PCM step tests check decay against the published formula at several times, within output quantization; both models mute when all DACs are off. This is an analog approximation, not measured-device or listening acceptance. |
| APU model differences | Active CGB wave RAM accesses alias the channel-selected byte without DMG's fetch-cycle gate. CH3 retrigger leaves wave RAM intact. Power-off clears CGB length counters and off-state length writes are ignored. | Component tests distinguish both models, check both DMG corruption windows and full-length expiration on all four channels. An original CGB CPU probe verifies active/inactive addressing and exact snapshot replay. Broader CGB audio conformance and analog output remain unverified. |
| OAM DMA bus ownership | Native CGB separates cartridge ROM/SRAM from WRAM (including echo); the CPU can use the other bus during a transfer. DMA owns OAM and runs at one byte per CPU M-cycle: a complete transfer takes 640 base-clock dots at normal speed and 320 at double speed. Active Mode-2 OAM scan hides objects on the affected line. | `dmg_oam_dma_test` checks both bus directions and drives the CGB DMA through `SystemClock` at normal and double speed, checking first/last byte and its 160-M-cycle completion boundary. `cgb_hardware_test` checks a ROM-window source page, selected VRAM bank 1 and WRAM bank 3; `cgb_system_test` reads 160 bytes from MBC1 ROM bank 1 into OAM while executing the routine from WRAM, copies OAM into SRAM, and checks every byte. Guest fixtures cover WRAM- and ROM-source same-bus instruction fetches plus cross-bus accesses: DMA data executes as opcodes, and ROM-source DMA blocks SRAM writes until the cartridge bus is released. Other fixtures execute ROM and write cartridge SRAM during WRAM DMA, restore an in-flight transfer, and run a WRAM-source byte as CPU instructions while writing through the cartridge bus; the DMG control blocks that write. A guest sprite fixture distinguishes normal from double-speed completion at the following line's Mode 2. Other conflict phases, revision boundaries, write-induced corruption and Mode-3 word contamination remain approximate. |
| VRAM DMA | FF51–55 source/destination masks, GDMA CPU stalls, 16-byte HBlank blocks on visible lines LY 0–143, no HBlank transfer during VBlank, HALT and LCD-off pause/resume, cancellation and remaining-count readback; 32 base T-cycles per block at either CPU speed. GDMA consumes its transfer time during Mode 3, but PPU-owned VRAM ignores destination writes. A destination that reaches `$A000` stops early rather than wrapping to `$8000`. | Focused copy/timing/cancel/overflow tests and system snapshots with active DMA. Original CPU probes verify HALT suppression and LCD-off single-block behavior, retained counts, next-HBlank continuation and exact snapshot replay. `cgb_hardware_test` checks banked WRAM sources, block rates, Mode-3 write loss, VBlank pause/resume, HBlank writes after Mode 3 and stopping at the VRAM destination boundary; `cgb_system_test` verifies guest-visible 8/16-M-cycle stalls at normal/double speed and reads back the copied byte. OAM-DMA collisions and invalid-source data remain approximate. The overflowed `FF55` status value remains unspecified. |
| Other registers | Native serial fast-clock selection and host bit-exchange interface; CGB RP digital IR input/output with receive settling; KEY1 and FF72–75 behavior. | Original serial and CGB IR two-system CPU tests cover link-level contracts ([serial](serial-link.md), [IR](cgb-infrared.md)). RetroArch link transport, analog IR sensor response, KEY0/PGB switching and physical peripheral revisions remain outside the model. |
| Snapshots / frontend | CGB banks, palettes, color buffers, fetch/OBJ metadata, DMA, speed-switch progress and IR receiver state round-trip through an independent candidate before commit. SRAM/RTC pointers remain stable. | Native checkpoints plus repeated libretro rewind/replay check exact video, PCM and complete end state in XRGB8888 and 0RGB1555. |

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
with stable SRAM. Original CGB MBC1 and MBC5 fixtures change a color field on
Right, increment battery-RAM progress during play, then resume and continue
that progress after a frontend-style unload, reload and SRAM import. `make test`
runs these regressions without external ROMs. An original CGB-compatible
MBC3+RTC fixture reads and writes the guest-visible seconds register, then
verifies SRAM and RTC state continue after frontend-style reload and import in
both libretro presentation builds.
A second original fixture imports a running clock timestamped 125 seconds
in the past, latches and halts it through guest instructions, and copies all
five RTC registers into SRAM. Both presentation builds check elapsed wall time,
seconds/minutes/day rollover and the 512-day carry, then reload both battery
blocks and require fresh guest reads of the retained clock. It uses host-time
bounds without sleeps; it does not establish RTC behavior in a commercial game.

The original 8-MiB MBC5 CGB fixture executes a subroutine in ROM banks
`$100` and `$1FF` every game frame and advances imported progress in SRAM bank
15. Both pixel formats and presentation policies check color/input response,
all sixteen SRAM banks' independent battery bytes, fresh-content resume,
stable SRAM pointers and exact per-callback video/PCM plus complete-state
replay. This covers the ninth ROM-bank bit through the guest CPU and frontend;
it does not claim compatibility with all large MBC5 titles. The cartridge
motor's libretro output is covered by a synthetic fixture, not by a manual game.

For local gameplay checks of CGB-compatible MBC5 and MBC1 homebrew releases:

```sh
make test-cgb-homebrew
```

This optional local target runs the Big2Small gameplay/save-resume profile and
the Deluxe input/state-replay profile. Both check the pinned release's
CGB-compatible header and mapper geometry. The ROMs and generated artifacts
remain outside the repository and CI; see the [homebrew guide](homebrew-check.md#big2small-save-and-resume-check)
and the publisher's [Deluxe page](https://tangramgames.itch.io/tobu-tobu-girl-deluxe)
for source and licensing details.

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

## CGB OAM DMA bus separation (2026-11-02)

The shared DMA component previously applied DMG cartridge/WRAM arbitration to
native CGB too. A focused CGB bus regression failed before the correction:
WRAM-sourced DMA incorrectly owned cartridge ROM and SRAM. Model-aware
arbitration now keeps the buses separate, as specified by
[Pan Docs OAM DMA bus conflicts](https://github.com/gbdev/pandocs/blob/master/src/OAM_DMA_Transfer.md).
The DMG arbitration path remains separately tested. The model is immutable
configuration selected when constructing the system; snapshot formats and
serialized DMA progress fields do not change.

The original CPU probe writes cartridge SRAM from ROM code before the
160-M-cycle WRAM transfer ends, then reads the copied OAM byte. An in-flight
snapshot resumes the same program and produces identical PCM and full state.
This is automated specification coverage, not hardware verification or a
commercial-game acceptance result. Invalid source pages and bus-value/corruption
quirks remain outside this bounded correction.

## CGB OAM DMA fetch conflicts (2026-11-03)

The original system fixture now exercises the same-bus conflict in the CPU
path. It starts OAM DMA from WRAM while the CPU fetches from another WRAM
address; the byte supplied by DMA decodes as `LD A,$5A` followed by a write to
cartridge SRAM. CGB must execute the transferred byte and allow the SRAM write
on the free cartridge bus. The matching DMG control executes the conflict stream
but cannot write SRAM while its single bus is occupied. This follows the
documented CGB bus split in [Pan Docs](https://eldred.fr/pandocs/OAM_DMA_Transfer.html#oam-dma-bus-conflicts)
and the fetch/write cases in the [BullyGB DMA bus-conflict test](https://github-wiki-see.page/m/Ashiepaws/BullyGB/wiki/DMA-Bus-Conflicts).

`cgb_system_test` checks the guest-visible SRAM marker on both models. A
companion probe now starts DMA from MBC1 ROM bank 1 while the CPU fetches from
the occupied cartridge bus. ROM bytes replace a HALT instruction with a jump
to a WRAM routine; the routine's SRAM writes remain blocked until the DMA
releases the cartridge bus. These fixtures cover WRAM- and ROM-source opcode
fetches. A further WRAM-executed probe performs `PUSH BC` and `POP DE` using a
WRAM stack during ROM-source DMA, verifies the recovered word, and leaves a
success marker in SRAM once the cartridge bus is released. This exercises the
free bus through real CPU stack accesses, not only direct bus reads and writes.
These fixtures do not cover all data-read phases, silicon revisions, or PPU
collisions. No snapshot fields or format versions change.

## CGB OAM DMA banked sources (2026-11-03)

The CGB uses VBK to select the source bank for OAM DMA from `$8000–$9FFF`,
and SVBK for sources from `$D000–$DFFF`, as documented in section 2.4.10 of
the [Game Boy Programming Manual](https://files.nekoblog.org/uploads/pdf/39999184-GameBoy-Programming-Manual.pdf).
`cgb_hardware_test` now starts DMA from both ranges with the alternate bank
filled with distinct decoy bytes, then checks all 160 OAM bytes came from the
selected bank. A ROM-bus fixture also copies all 160 bytes from `$4000–$409F`,
covering the CGB-specific cartridge-ROM source range. An original MBC1 CPU
probe runs the transfer routine from WRAM, copies all of OAM back to cartridge
SRAM and checks every byte against the ROM bank pattern. This validates the
mapped bus behavior; bank changes during an active transfer and silicon
revisions are not covered.

## CGB VRAM DMA banked sources (2026-11-03)

The [Game Boy Programming Manual](https://files.nekoblog.org/uploads/pdf/39999184-GameBoy-Programming-Manual.pdf)
specifies that VRAM DMA reads `$D000–$DFFF` from the SVBK-selected WRAM bank
and writes `$8000–$9FFF` to the VBK-selected VRAM bank. `cgb_hardware_test`
fills WRAM bank 1 with decoy bytes, selects bank 3, and checks both a GDMA
block and an LCD-off HBlank-DMA block copy bank 3 into VRAM. This covers the
selected-bank data path for each transfer mode; changing SVBK during an active
transfer is not covered.

## CGB VRAM DMA rate at double speed (2026-11-03)

The [Game Boy Programming Manual](https://files.nekoblog.org/uploads/pdf/39999184-GameBoy-Programming-Manual.pdf)
specifies that CGB VRAM DMA transfers at a fixed rate in normal and double CPU
speed. A 16-byte block therefore consumes 32 base-clock T-cycles at either
speed, while CPU blocking spans eight M-cycles at normal speed or sixteen at
double speed. `cgb_hardware_test` advances the actual GDMA and LCD-off one-block
HBlank-DMA components through `SystemClock`, checks that both remain active at
28 base-clock T-cycles, and checks that each completes at 32. An original
`cgb_system_test` ROM then starts both modes through FF55 at each speed, counts
the production system's CPU-stall cycles and reads the copied byte back from
VRAM. Active-LCD HBlank scheduling is covered separately.

## CGB OAM DMA rate at double speed (2026-11-03)

OAM DMA advances at the CPU M-cycle rate. A complete transfer therefore spans
640 base-clock dots at normal speed or 320 at CGB double speed, as documented
in [Pan Docs](https://eldred.fr/pandocs/OAM_DMA_Transfer.html#ff46-dma-oam-dma-source-address--start).
The focused component regression drives the production DMA through `SystemClock`
at both speeds: it checks the first byte at M=2, the 160th copied byte, and
release exactly 160 CPU M-cycles after the first-byte boundary (640 or 320
base-clock dots). This is model timing coverage, not a physical-CGB measurement.
The original `cgb_system_test` fixture starts WRAM-sourced DMA during line 1
Mode 3 and places a visible object on line 2. At normal speed, line 2's Mode 2
scan still overlaps active DMA and hides the object; at double speed, the
transfer has completed before that scan and the object is visible. This is a
separate guest-visible timing comparison; exact sub-M-cycle edges and revision
differences remain unverified.

## OAM DMA during Mode-2 scan (2026-11-02)

When OAM DMA is active during the Mode-2 scan, the PPU treats the objects on
that line as off-screen, matching the common behavior documented by
[Pan Docs OAM DMA](https://eldred.fr/pandocs/OAM_DMA_Transfer.html#oam-dma-bus-conflicts).
`DmgOamDma` exposes its live activity to the PPU through host wiring; the PPU
uses it at the Mode-2-to-Mode-3 boundary and skips object selection for that
line. The callback is not serialized, and no snapshot fields changed.

The `cgb_hardware_test` control renders an object with no transfer. Its paired
case starts a real DMA from a byte-for-byte copy of OAM during Mode 2, so the
source data cannot explain the hidden object. Both DMG and CGB hide it. This
checks the Mode-2 case only; Mode-3 behavior is covered below.

## OAM DMA during Mode-3 object fetch (2026-11-02)

When DMA overlaps a Mode-3 object fetch, tile and attribute reads use the
aligned OAM word at the DMA destination. The selected object's Y/X and priority
remain from Mode 2. `DmgPpu` resolves tile data at the object-fetch low-plane
read point, using the DMA destination word through host wiring. The callback is
not serialized; the DMA offset and pending object-fetch state are.

An original pixel fixture selects an object before Mode 3, starts a real DMA
from WRAM during Mode 3, and gives the DMA source tile/attribute bytes that
produce color 2 in place of the selected object's color 1. The control and
DMA cases pass on DMG and CGB. A CPU-driven DMG snapshot test saves while DMA
is active and the far-right object fetch is pending, then checks that restored
execution produces the same pixels and complete state. The PPU-before-DMA
component order uses the last completed destination word at the shared dot;
the exact first/last transfer edges and PPU revision differences remain
unverified. This is automated model coverage, not physical hardware
verification. See [Pan Docs OAM DMA](https://eldred.fr/pandocs/OAM_DMA_Transfer.html#oam-dma-bus-conflicts)
and [SameBoy's OAM read mapping](https://github.com/LIJI32/SameBoy/blob/master/Core/display.c#L3152-L3177).

## HBlank DMA and HALT (2026-11-02)

HBlank DMA now pauses while the CPU is in HALT, retaining its remaining
blocks until CPU execution resumes. The PPU and interrupt clocks keep running;
this is separate from STOP and the KEY1 speed-switch pause. The HBlank callback
checks CPU HALT before starting a block; an in-flight block already stalls CPU
execution, so the CPU cannot execute HALT midway through that block.

The original `cgb_system_test` probe arms three blocks on visible line 1,
halts with VBlank enabled and IME clear, and reads the remaining count and
untouched VRAM on wake. It then waits for the pending transfer and reads its
copied byte. The uncorrected core fails the retained-count/data assertion.
Saving while halted and restoring into a fresh system reproduces video, PCM
and full final state. See [Pan Docs HBlank DMA](https://github.com/gbdev/pandocs/blob/master/src/CGB_Registers.md#bit-7--1--hblank-dma)
and the linked [MagenTests description](https://github.com/alloncm/MagenTests#vram-dma-hblank-mode).
These tests establish the documented pause/resume contract; they do not settle
sub-M-cycle interrupt/HBlank races or prove physical hardware accuracy.

## HBlank DMA and LCD-off transitions (2026-11-03)

HBlank DMA retains its transfer mode when the LCD is disabled. Starting it with
the LCD off transfers one 16-byte block, releases the CPU, then waits until the
LCD resumes and a visible HBlank begins. Disabling the LCD during an active
HBlank transfer likewise schedules one block; it does not convert the
remaining transfer into unrestricted GDMA or leave it permanently stalled.

`cgb_hardware_test` checks block duration, FF55 remaining-count readback,
single-block progress while off, and continuation across successive HBlanks.
An original CGB CPU probe disables the LCD during a running transfer, checks
the copied source offsets, and saves/restores while blocks remain; replay
produces identical video, PCM and complete state. The existing HDMA state
fields represent the waiting transfer, so this fix changes no snapshot version.

This follows the LCD-off cases described in AntonioND's
[Cycle-Accurate Game Boy Docs, VRAM DMA section](https://github.com/AntonioND/giibiiadvance/blob/master/docs/TCAGBD.pdf)
and the per-HBlank transfer contract in [Pan Docs](https://gbdev.io/pandocs/CGB_Registers.html#ff55-hdma5-cgb-mode-only-vram-dma-lengthmodestart).
The external docs do not settle sub-M-cycle LCD/HDMA races or physical CGB
revision differences.

## HBlank DMA across VBlank (2026-11-04)

An active HBlank DMA transfers on visible HBlanks only (LY 0–143). It retains
its pending block throughout VBlank (LY 144–153), then continues at line 0 of
the next frame. `cgb_hardware_test` arms two blocks before line 143's HBlank,
checks the first block completes there, verifies the second remains untouched
through all ten VBlank lines, and confirms it completes at line 0's HBlank.
This covers the documented PPU-to-DMA scheduling path; it does not establish
sub-M-cycle arbitration or physical revision timing.

This behavior follows [Pan Docs' HBlank DMA contract](https://github.com/gbdev/pandocs/blob/master/src/CGB_Registers.md#ff55-hdma5-cgb-mode-only-vram-dma-lengthmodestart).

## Native CGB LCDC.0 BG/OBJ priority (2026-11-04)

LCDC.0 has different meanings in native CGB and DMG compatibility. Native CGB
continues to render BG/Window when the bit is clear, but ignores BG-map and OBJ
"behind BG" priority so visible objects win. In DMG compatibility, clearing the
bit disables BG/Window output and an OBJ marked behind BG can still appear over
the blank background.

`cgb_hardware_test` renders the same red BG/green OBJ fixture with LCDC.0 on
and off in both modes. It checks both the overlapped pixel and the adjacent
background-only pixel, so it distinguishes BG visibility from priority. This
is a renderer regression, not hardware-measured fetch timing.

This follows [Pan Docs LCDC.0](https://github.com/gbdev/pandocs/blob/master/src/LCDC.md#lcdc0--bg-and-window-enablepriority).

## GDMA writes while the PPU owns VRAM (2026-11-03)

GDMA still consumes the full block and advances its source/destination during
Mode 3, but destination writes are dropped while the LCD controller owns VRAM.
The previous implementation wrote directly to banked VRAM and incorrectly
preserved all bytes during a transfer that overlapped pixel transfer. The PPU
now exposes its VRAM write-block state to the CGB DMA component, and the
regression checks both lost Mode-3 writes and successful writes in the
following HBlank. HBlank DMA continues to transfer within its scheduled blank.

This follows the [Pan Docs GDMA contract](https://gbdev.io/pandocs/CGB_Registers.html#bit-7--0--general-purpose-dma),
which says GDMA attempts transfers while the LCD is using VRAM and those writes
fail. The check is component-level and does not claim sub-dot conflict timing
or physical-CGB verification; VRAM as a DMA source and simultaneous OAM-DMA
conflicts remain separate gaps.

## CGB APU register behavior (2026-11-02)

Native CGB now selects model-specific behavior in the shared APU. Active
wave RAM reads and writes use CH3's selected byte even outside the narrow
DMG fetch window; stopping CH3 restores ordinary CPU addressing, including
when its DAC is enabled. CH3 retrigger corruption is restricted to DMG.
CGB power-off clears all four length counters and ignores writes to length
registers while off; DMG retains its existing writable-length exception.
These contracts follow [Pan Docs audio registers](https://github.com/gbdev/pandocs/blob/master/src/Audio_Registers.md).

The new active-wave access test fails against the previous core. Component
tests check aliased reads/writes, inactive DAC-on access, both DMG corruption
windows without CGB corruption, and full 64/256-clock CGB lengths after a
power cycle or ignored off-state writes. Existing DMG fetch/corruption tests
remain in the required gate. An original CGB CPU probe reads and writes active
wave RAM, disables CH3 and reads both physical locations; restoring an active
channel reproduces its SRAM observations, PCM and full state exactly.

The immutable model flag comes from the system's existing hardware model;
no APU progress fields or snapshot versions change. This covers register
behavior, not all CGB APU revisions, physical waveform accuracy or listening
acceptance. In particular, the existing startup and mixer remain approximate.

## CGB PCM observation registers (2026-11-02)

PCM12 (`FF76`) and PCM34 (`FF77`) now expose packed four-bit generator
outputs in native CGB mode. CH1/CH3 occupy the low nibble and CH2/CH4 the high
nibble. These are live digital levels before master panning, volume and output
filtering; writes have no effect. DMG returns `FF` at both addresses. See
[Pan Docs PCM registers](https://github.com/gbdev/pandocs/blob/master/src/Audio_details.md#pcm-registers).

The component regression fails against the earlier missing-register behavior.
It checks each channel's level, CH3's digital volume shift, disabled channels,
power-off, ignored writes and mixer independence. The original CPU probe
plays a constant CH3 pattern with no master routing, reads both registers,
attempts a write and reads again. It runs under both DMG and CGB and repeats
from an active-channel snapshot with identical SRAM observations, PCM and full
state. Values are derived from existing generator progress, so no new state
fields or snapshot versions are required.

This adds an observation interface for future APU checks; it does not establish
hardware-exact pulse startup, sample-edge timing or analog sound quality.

## Pulse first duty and retrigger phase (2026-11-02)

CH1 and CH2 now suppress their first duty output after APU power-on until
the first timer-driven duty advance. Suppression is independent for each
channel, does not recur on waveform wrap or retrigger, and resets on an APU
power cycle. Retrigger reloads the countdown's upper bits while retaining its
lowest two bits. These contracts follow [Pan Docs pulse and obscure behavior](https://github.com/gbdev/pandocs/blob/master/src/Audio_details.md#obscure-behavior).

A PCM regression reproduces the old initial-high failure and checks all four
duties, both channels, waveform wrap, power cycling and a retrigger two clocks
into the period. Both suppression flags now round-trip in version 3 snapshots;
legacy DMG v1/CGB v2 migration preserves their unsuppressed semantics. See the
[format and compatibility policy](save-states.md). These checks do not establish
revision-specific startup delays, clocking after DAC shutdown or analog sound
accuracy.

## Length-enable and trigger clocks (2026-11-02)

All four channels now apply the documented extra length clock when a write
enables length counting before a DIV-APU step that does not normally clock
length. If a trigger reloads a zero counter with length enabled in that phase,
it loads 63 (pulse/noise) or 255 (wave), rather than 64/256. A simultaneous
enable/trigger that expires the old count reloads it once; repeated enabled
writes do not apply another edge. DMG and the selected CGB-E revision share
this behavior; the early CGB-02 disabled-to-disabled quirk is not selected.
See [Pan Docs obscure behavior](https://github.com/gbdev/pandocs/blob/master/src/Audio_details.md#obscure-behavior).

The previous core fails the new immediate-enable test. Component tests cover
all eight sequencer phases, both models, all four channels, exact full-length
expiry, repeated writes, zero reloads and simultaneous expiration/trigger.
An original CPU program forces DIV falling edges and reads NR52 after odd-phase
enable, even-phase enable and regular length expiration. Both models reproduce
those observations, PCM and full state from a pre-enable snapshot. Existing
sequencer/counter fields carry all required progress; snapshot v3 is unchanged.
Sub-M-cycle races and other CGB revisions remain unverified.

## Envelope trigger reload phase (2026-11-02)

CH1, CH2 and CH4 trigger reloads now add one envelope tick when the next
DIV-APU step is the envelope step (7). The additional delay applies only to
the trigger reload; subsequent changes retain the configured period. A zero
period still disables automatic envelope changes. See [Pan Docs obscure
behavior](https://github.com/gbdev/pandocs/blob/master/src/Audio_details.md#obscure-behavior).

A new PCM test fails against the previous reload behavior. It observes digital
pulse/noise peaks across all eight trigger phases, periods 0–7 and both volume
directions, checking the first change and two later periods. An original CGB
CPU program triggers at phase 7, forces DIV edges and records volumes 7, 6,
then 5 through PCM12; a snapshot with the extra tick pending reproduces PCM,
SRAM observations and complete state. Existing serialized envelope counters
carry this progress, so the snapshot format remains v3. Other
revision-specific mid-playback NRx2 writes and sub-M-cycle trigger races are
outside this probe; see the separately tested common write case below.

## CH1 sweep countdown and overflow (2026-11-02)

CH1 sweep now updates the period reload registers without replacing the
currently running duty countdown. The original pending edge completes on its
old timing; subsequent steps use the swept period. Trigger-time overflow now
also disables CH1, including with sweep pace zero: the previous code computed
that result but discarded it. See [Pan Docs period divider](https://github.com/gbdev/pandocs/blob/master/src/Audio_Registers.md#ff13--nr13-channel-1-period-low-write-only)
and [sweep overflow](https://github.com/gbdev/pandocs/blob/master/src/Audio_Registers.md#ff10--nr10-channel-1-sweep).

Both defects were reproduced by focused tests. PCM traces compare positive and
negative sweep against a nontrigger CPU-register update at five offsets,
including one clock before the pending edge. They check the retained edge and
later swept periods. NR52 tests cover immediate overflow at pace zero, the
second overflow calculation after a valid update, and disabling CH1 after
clearing a used negate bit on both models. These establish the shared counter
contract, not exact revision-specific sweep calculation delays. Existing
snapshot fields remain sufficient; the format stays v3.

## Optional CGB BIOS through libretro (2026-11-02)

Native CGB now loads optional `cgb_boot.bin`, then `gbc_bios.bin`, from the
RetroArch system directory. DMG retains its own filenames and 256-byte size.
An absent CGB image uses fast startup; an existing unreadable or wrong-size
preferred image fails explicitly rather than silently choosing an alias or
fast startup. Reset reuses the selected image already loaded into memory.
The `.info` advertises both optional firmware files.

CGB images must be **2304 bytes (`0x900`), indexed by CPU address**. Bytes
`0000–00FF` and `0200–08FF` are mapped; the `0100–01FF` hole remains cartridge
memory. This adapter does not accept packed 2048-byte images. The mapping
follows [Pan Docs CGB boot layout](https://github.com/gbdev/pandocs/blob/master/src/Power_Up_Sequence.md#size).
No BIOS is included or downloaded.

An original boot program jumps to the second window, reads the cartridge
header hole, renders four blue frames and unmaps from `00FE`, passing A=`11`
to the game at `0100`. The game then renders red gameplay. Both libretro
presentation builds test rendering/handoff, mapped-BIOS snapshot replay,
stable SRAM on reset, cached-image reuse after file removal, alias loading and
malformed-image diagnostics. Snapshot identity follows BIOS bytes rather than
filename; restoring that state under fast startup is rejected without changing
the running game. The existing synthetic boot probe remains a separate
LCD-off unit-test entry point.

This establishes the optional frontend loader and an original boot/render/state
path. It does **not** establish that Nintendo's boot animation or every real
BIOS revision completes. KEY0/OPRI-based DMG compatibility now has separate
[implementation evidence](cgb-compatibility.md); real BIOS revision validation
and PGB behavior remain separate work.

## STAT write model distinction (2026-11-02)

DMG now temporarily enables all STAT interrupt sources for one M-cycle when
writing STAT with the LCD running. Native CGB has no such write quirk. The
source mask returns to the requested value after four T-cycles, preserving
combined-line edge blocking. Model/mode/LYC and LCD-off regressions cover the
behavior; a CPU-driven write and transient snapshot replay are covered by
`save_state_test`. The countdown is part of snapshot v4; v1/v2/v3 loads begin
without this newly modeled transient. See [DMG evidence](dmg-validation.md#stat-write-interrupt-quirk-2026-11-02)
and [Pan Docs STAT](https://github.com/gbdev/pandocs/blob/master/src/STAT.md#spurious-stat-interrupts).

## MBC1M multicart wiring (2026-11-02)

The shared cartridge now detects standard 1-MiB MBC1M images by the standard
header logo at both the primary header and bank `$10`'s subgame header. Its
upper register selects ROM bank bits 4–5 instead of 5–6. The low register's
bit 4 is ignored for banking **after** full five-bit zero-to-one translation:
writing `$10` therefore selects a subgame's bank zero, while writing `$00`
selects bank one. Advanced mode remaps the lower window to `$00/$10/$20/$30`.
See [Pan Docs MBC1M](https://github.com/gbdev/pandocs/blob/master/src/MBC1.md#mbc1m-1-mib-multi-game-compilation-carts).

Original marker-ROM tests cover both modes, all upper bits and every byte
written to the low register. Corrupted/missing logo and smaller-image controls
retain ordinary MBC1 wiring. Original CPU code is present in each subgame's
lower window, switches to subgame 3 and observes `$30/$31/$3F` in the upper
window. DMG and CGB system tests cover repeated M-cycle snapshots, stable SRAM
pointers, exact PCM/full-state continuation, and battery progress advancing in
a fresh content instance.

Detection is a header heuristic, not a physical-cartridge identity claim; no
title or ROM hash special cases are used. Wiring is immutable ROM-derived
configuration, so existing mapper progress fields and snapshot v4 suffice.
Other image geometries and nonstandard multicart headers are not detected;
commercial multicart playability and physical hardware remain unverified.
The local Mooneye `mapper` profile now includes `multicart_rom_8Mb`; all 28
MBC1/MBC1M/MBC2/MBC5 cases pass. The report is
`build/test-roms/mbc1m-mapper/report.md`; the external ROMs remain outside the
repository and CI.

## Model-specific high-pass decay (2026-11-02)

CGB output now uses its faster high-pass charge factor instead of inheriting
DMG's filter. At 48 kHz the Q16 coefficients are 65296 (DMG, retained) and
59751 (CGB), obtained by rounding `65536 * charge^(4194304/48000)` with the
[Pan Docs master-clock charge factors](https://github.com/gbdev/pandocs/blob/master/src/Audio_details.md#obscure-behavior).
The filter remains in the base-time audio domain at either CPU speed.

A constant-wave test reproduces the former CGB decay mismatch. It checks
several sampled times against the published continuous decay, allowing one
PCM quantization step, verifies unrouted-channel silence and confirms both
models disconnect when all DACs turn off. Existing CGB snapshot/rewind checks
cover filter charge continuation. Filter history was already serialized, and
model selection is immutable, so this filter correction adds no serialized state. This does
not establish physical analog response, speaker/headphone behavior or frontend
listening acceptance; the mixer remains approximate.

## DMG OAM corruption and native CGB immunity (2026-11-02)

The shared CPU now reports IDU/address events for the documented OAM bug
operations. DMG wiring applies read/write/combined corruption during Mode 2;
native CGB leaves OAM intact. Golden-data patterns, CPU event traces and a
pre-INC snapshot replay verify the model distinction. See the
[canonical OAM corruption contract and limits](oam-corruption.md).

## Speed-switch PPU locking (2026-11-02)

KEY1/STOP now retains PPU access permissions for the existing 2050-M-cycle
CPU pause. Following [Pan Docs](https://gbdev.io/pandocs/CGB_Registers.html#ff4d--key1spd-cgb-mode-only-prepare-speed-switch),
entry in mode 0/1 blocks PPU video-memory reads and emits black pixels; mode 2
allows background VRAM reads but suppresses objects; mode 3 permits normal
rendering. Completion releases the lock. CPU/DIV remain paused while base-clock
PPU progress continues.

`cgb_hardware_test` checks all four entry modes, actual VRAM read gating,
RGB555 background/object output and recovery. `cgb_system_test` drives real
KEY1/STOP instructions in both directions for every mode and checks the complete
2050-cycle pause, persistent mode latch, guest KEY1 result and exact snapshot
replay. CGB snapshot v5 records the latch; older CGB states retain their legacy
unrestricted PPU behavior. DMG stays on v4; see the [format policy](save-states.md).
Validation: `make build`, `make test`, GBIT (244/244 base and 256/256 CB)
and the available Mooneye `all` group (91/91) pass. Mooneye is a DMG regression
gate here, not direct evidence for these CGB display effects.
These are specification-based automated checks, not physical revision evidence.
Exact boundary pixels, interrupted object-fetch recovery, DMA conflicts and
interrupt arrival during STOP remain unverified.

## Digital generation and DAC conversion (2026-11-02)

All four sound channels now separate their 0–15 digital generator output from
DAC conversion. Per [Pan Docs audio details](https://gbdev.io/pandocs/Audio_details.html#dacs),
an enabled DAC maps digital zero to positive analog bias and digital fifteen
to negative bias; a stopped generator still emits digital zero. Only disabling
the DAC removes that channel's contribution. NR51 panning and NR50 gain act on
the analog result before the existing model-specific HPF. PCM12/34 continue to
expose digital levels, independently of DAC polarity and mixer settings.

This corrects the previous pulse/noise volume-centered approximation and CH3
polarity. `dmg_apu_test` checks all sixteen generator levels for every channel
and both hardware models, minimum/maximum master gain, digital PCM observation,
inactive generators, disconnected DACs and stereo isolation. A mixed-channel
check verifies cancellation between an inactive pulse DAC and a constant maximum
wave DAC, then the stereo transient when the pulse is unrouted. Existing decay
tests now check the documented polarity; the synthetic post-boot profile starts
with settled filter bias so it retains silent handoff. Full-state/PCM replay
uses the existing serialized filter history; this correction adds no state
fields or format version. Old snapshots retain recorded filter charge but
continue with the corrected mixer, so old-core PCM streams are not promised.
Validation: `make build`, `make test` (including both libretro presentation
builds and exact PCM/state replay) and GBIT (244/244 base, 256/256 CB) pass.
Analog DAC fade timing and real-device listening acceptance remain unverified.

## Envelope divider and automatic stop (2026-11-02)

CH1/CH2/CH4 envelopes now stop automatic updates only after an attempted
out-of-range step, and resume when retriggered. Merely reaching volume 0 or 15
does not yet stop updates; it also does not disable the generator or DAC.
Period zero suppresses volume changes while its divider continues counting
with an eight-tick reload. See the researcher's
[volume envelope description](https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware#Volume_Envelope)
and [Pan Docs obscure behavior](https://gbdev.io/pandocs/Audio_details.html#obscure-behavior).

Component checks cover all nonzero paces, both directions, final valid versus
first invalid steps, direction writes after stopping, retrigger recovery and
period-zero divider phase across multiple reloads. The supported common
zero-pace zombie case is tested separately below. A real CPU/DIV probe stops
all three envelopes on DMG and CGB, captures their flags and replays exact
PCM/full state. Snapshot v6 preserves automatic-update flags separately from
divider countdowns; old versions migrate as described in the
[format policy](save-states.md). `make build`, `make test` (including both
libretro presentation builds) and GBIT (244/244 base, 256/256 CB) pass.

## Common period-zero NRx2 write step (2026-11-02)

During playback, rewriting an active increase envelope from a period-zero
`x8` value to `$08` increments its current volume by one, wrapping from 15 to
0. This common case is implemented for CH1, CH2 and CH4. Component tests read
CGB PCM12/PCM34 to verify each step and wrap; a DMG PCM comparison confirms
the write changes the mixed output there too. This matches the documented
cross-model case in [Pan Docs](https://github.com/gbdev/pandocs/blob/master/src/Audio_details.md#obscure-behavior).
Other zombie-write combinations vary by hardware revision and remain
unimplemented; sub-clock write races remain unverified.

## Pulse duty clocks while output is disabled (2026-11-02)

The shared DMG/CGB pulse dividers now keep clocking after length expiration,
DAC shutdown or CH1 sweep disables output. The first trigger starts the divider;
a trigger with the DAC off still performs its internal actions, and APU power
cycling resets the divider/duty history. This follows the researcher's
[channel and trigger description](https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware#Channels)
and [Pan Docs duty-clock behavior](https://gbdev.io/pandocs/Audio_details.html#pulse-channels-ch1-ch2).
The previous implementation froze duty position with the output-enable flag,
so later retriggers could resume at the wrong waveform phase.

`dmg_apu_test` observes CGB digital PCM through the shared pulse implementation:
both channels, all four duties, periods from 4 to 8192 T-cycles, varied silent
intervals, all applicable disable sources, first-trigger gating, DAC-off triggers
and power-cycle reset. Golden phase checks cover a complete waveform after
retrigger, including retained countdown low bits. Rebuilding the old output-gated
divider reproduces the new regression failure.

An original guest runs five silence lengths and compares 128 PCM observations
after retrigger with an otherwise identical continuously active reference.
A snapshot inside the muted interval replays exact PCM/full state. Existing
serialized duty/countdown fields suffice; the format stays v6. Old snapshots
retain their recorded phase, then continue with corrected clocking. These are
specification-based component/CPU checks, not physical revision measurements;
noise-divider internals and sub-clock register races remain separate work.
Validation: `make build`, `make test` (including both libretro presentation
builds) and GBIT (244/244 base, 256/256 CB) pass.

## MBC5/MBC7 rumble through libretro (2026-11-02)

The [GBDev hardware definitions](https://github.com/gbdev/hardware.inc/blob/master/hardware.inc)
assign register bit 3 as the rumble control for MBC5 and MBC7. MBC5 keeps
that bit separate from RAM-bank bits 0–2; non-rumble MBC5 retains all four
RAM-bank bits. MBC7 uses bit 6 of the same control byte to open its sensor/EEPROM
register gate, so motor state and register access are independent.

The core maps motor transitions to libretro's optional port-0 strong rumble
interface at full strength (`$FFFF`) or off (`$0000`). It sends updates when a
guest changes the control bit, resynchronizes after state restore, and turns the
motor off on reset or content unload. A frontend without the interface keeps
gameplay running and receives one informational diagnostic. DMG v11/CGB v13
snapshots preserve the full control byte; earlier v8/v10 boolean gate values
migrate with rumble off, without changing payload length.

`make test` covers MBC5 RAM banking with rumble on, MBC7 gate/motor independence,
frontend on/off callbacks, snapshot restore, reset/unload cleanup and missing
frontend support. This validates the digital host contract, not the strength,
spin-up/coast-down or feel of a physical motor.

## MBC7 EEPROM and accelerometer path (2026-11-02)

MBC7 cartridge type `$22` now has 7-bit ROM banking, its two register gates,
the `$A000–$AFFF` register window, X/Y sensor latching, and a 128-word × 16-bit
serial EEPROM. The EEPROM accepts EWEN/EWDS, READ, WRITE, ERASE, ERAL and WRAL;
guest-programmed bytes occupy the 256-byte battery block. READ supports the
93LC56 sequential-address behavior. Programming starts on CS falling, presents
DO busy/ready status after the minimum CS-low poll interval, and commits pending
data only when its self-timed cycle ends. DMG v7/CGB v9 preserve serial command
state; DMG v8/CGB v10 also preserve pending program and status-poll progress;
DMG v9/CGB v11 preserve the full MBC7 gate/rumble control register. This follows
the register and command description in [Pan Docs MBC7](https://eldred.fr/pandocs/MBC7.html)
and the [93LC56 data sheet](https://ww1.microchip.com/downloads/en/devicedoc/21794f.pdf).

Libretro requests its optional primary-port accelerometer for MBC7 content,
samples both axes each frame and converts normalized input to the cartridge's
centered sensor values. Missing frontends use neutral values and log that
fallback. `address_bus_test` covers both gates, register decode, latch behavior,
EEPROM commands, busy/ready boundaries, sequential reads and battery storage.
CPU-driven DMG/CGB save-state tests resume a partial command and an active busy
cycle; libretro tests cover sensor enable/sample/reset/unload, input conversion
and the unavailable-sensor path.

This is automated interface coverage, not physical-cartridge acceptance. The
axis center and roughly `$70` per g conversion are approximate; sensor settling
and noise are not modeled. EEPROM delay uses the 93LC56 data sheet's maximum
cycle times for the documented voltage conditions, not per-device timing.
Bank-zero behavior remains unverified, and no MBC7 game or physical rumble
motor has been checked manually.

## Native CGB Window Y-condition reset (2026-11-02)

Native CGB resets the Window Y condition when software clears LCDC.5. If the
Window is re-enabled while WY is already behind LY, it stays hidden until a
later scanline matches WY. DMG and CGB DMG-compatibility mode retain the Y
condition instead. The rule follows the [Pan Docs Window description](https://eldred.fr/pandocs/Window.html#window-rendering-criteria)
and the documented CGB-versus-DMG-compatibility distinction.

`cgb_hardware_test` renders an original tile fixture through all three modes,
checks the Window disappear/reappear boundary, and confirms that a future WY
match re-arms it on native CGB. This changes no snapshot fields; the existing
serialized Y-condition latch already captures the state.

## CGB VBlank/STAT interrupt edge (2026-11-03)

The CGB Mode 2 STAT source for line 144 rises one M-cycle before the VBlank
interrupt. The shared PPU previously requested this source only at VBlank
entry, which made the CGB timing match the DMG edge. It now asserts the CGB
source during the final four dots of line 143 after readable LY reaches 144;
the DMG entry-edge behavior remains separate.

The local MIT-licensed Mooneye `misc/ppu/vblank_stat_intr-C` ROM now passes
with CGB-E synthetic startup. `dmg_ppu_test` checks the exact boundary for
native CGB, CGB DMG-compatibility, and DMG control. This verifies the selected
model contract and Mooneye ROM result, not physical CGB revision timing. The
Mooneye runner separates this PPU check from boot-state ROMs: `cgb-boot`
requires an external address-indexed CGB BIOS because synthetic startup cannot
prove the BIOS-produced register and divider values.

## Remaining scope

Serial external-clock bit exchange now has a portable core interface and
original two-system CPU/clock/snapshot tests; see the [serial guide](serial-link.md).
A broader remaining-specification audit is next; frontend link transport remains separate.
Broader revision-specific envelope/length quirks and analog behavior beyond
the modeled charge factors remain separate from the differences already covered.

Broader game compatibility, CGB hardware-specific Mode 3/OAM bus timing,
DMA collision behavior, speed-switch boundary/revision effects and physical revision
verification remain work. CGB DMG compatibility now has a separate
[automated checkpoint](cgb-compatibility.md). Real CGB BIOS animation/revision
validation, PGB and frontend link transport are not part of this
checkpoint. Connected digital IR now has a separate [core contract](cgb-infrared.md).
The M8 playable checkpoint in the [roadmap](roadmap.md) is
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
