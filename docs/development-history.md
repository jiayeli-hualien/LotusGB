# Development history through 2026-10-28

These dated records were moved out of the roadmap during the 2026-10-28
replan. They preserve observations and decisions at the time, including
superseded failures, test counts, and proposed next steps. They are not the
current task queue or a claim that historical checks were rerun today.

Use the [roadmap](roadmap.md) for current priorities, the
[DMG validation matrix](dmg-validation.md) for current implementation evidence,
and the [homebrew guide](homebrew-check.md) for current game profiles.

## 2026-10-21

- **DMG PPU visual acceptance passed (2026-10-21):** [dmg-acid2 v1.0](https://github.com/mattcurrie/dmg-acid2/releases/tag/v1.0), 32 KiB ROM-only, DMG mode, no actual boot ROM. A temporary runner compared a 60-frame output with the published `reference-dmg.png`: all 23,040 pixels matched. The checked-in regression now starts from the DMG post-boot profile, runs the ROM for 10 frames from `$0100`, and compares the 160×144 shade-index framebuffer exactly. ROM SHA-256: `464e14b7d42e7feea0b7ede42be7071dc88913f75b9ffa444299424b63d1dff1`. ROM, reference image, and MIT attribution are in `tests/roms/dmg-acid2/`. The test uses LY=LYC interrupts to change settings between scanlines and does not test mode 3 penalties or mid-mode-3 writes.

## 2026-10-23

- **PPU Mode 2/STAT timing diagnostic (2026-10-23):** The local `make test-mooneye` workflow runs a curated set of external timing ROMs through the DMG post-boot system and checks Mooneye's register-result protocol. At the previous bus phase (after 3 T-cycles), `intr_1_2_timing-GS`, `intr_2_0_timing`, `intr_timing`, and `halt_ime1_timing` passed; `intr_2_mode3_timing`, `intr_2_mode0_timing`, and `intr_2_oam_ok_timing` failed on the first measurement by one count. Changing the Mode 2 threshold to 79 dots made two tests pass but violated the existing 80-dot boundary test; delaying STAT evaluation at visible-line start broke `intr_1_2_timing-GS`. Both were discarded. Moving CPU bus transactions to the end of the M-cycle makes all seven selected Mooneye ROMs pass while preserving the 80-dot Mode 2 test. Blargg CH3 tests 09, 10, and 12 were rerun at this phase on 2026-10-26; all three passed. ROM binaries remain outside the repository; reports and logs are under `build/test-roms/mooneye/`.



- **Local audio comparison (2026-10-23):** the captured PCM contained the town music that sounded noisy during live RetroArch playback in WSL. Playing that same capture directly in Windows produced no audible noise. This points to the WSL/RetroArch playback path rather than the APU sample stream; it has not been compared on a native Linux or Windows RetroArch installation.

- **Big2Small battery-save acceptance (2026-10-23):** the user played the locally supplied, freely available [Big2Small ROM](https://mdsteele.itch.io/big2small), kept outside the repository at `../test-roms/homebrew/big2small.gb`. Its header declares 64 KiB ROM, MBC5 + RAM + battery (`$1B`), and 8 KiB RAM. RetroArch created an 8 KiB `big2small.srm`; the user confirmed that game progress remained after reopening the game. This completes the actual RetroArch disk-save/reload check for this MBC5 game. Existing `testLibretroPersistsMbc5BatteryRamAcrossFrontendReload` covers SRAM writes and frontend restoration with a synthetic ROM; retain both forms of evidence. MBC3 RTC disk persistence and broader game compatibility remain separate follow-up work.

- **Big2Small scripted soak (2026-10-23):** a temporary in-process libretro runner restored a copy of the user's 8 KiB SRAM, selected a save slot, dismissed tutorial dialogue, and issued periodic animal-selection and directional inputs in a puzzle scene. The core completed 36,000 frames (about ten minutes of emulated time), publishing 36,000 video callbacks and 28,931,396 stereo audio frames without runner errors. Periodic framebuffer hashes changed and the inspected early/final gameplay frames showed no obvious rendering corruption. Local artifacts are under `build/big2small-check/`; the original RetroArch save was not modified. This checks continued execution and callback delivery, not puzzle completion, broader level coverage, audio quality, or a pixel-accurate comparison with SameBoy.

## 2026-10-24

- **LCD-off LYC correction (2026-10-24):** retain the coincidence flag and comparison interrupt edge history while the LCD comparator is stopped. PPU interface tests cover all four match transitions. `make test` and the seven default Mooneye checks pass. The external `stat_lyc_onoff` test advances to round 1 step 3 but still fails because LCD startup-line mode/timing remains incomplete; follow up with the dedicated LCD-enable timing ROMs before declaring this edge fully supported.

- **LCD startup mode order (2026-10-24):** LCD enable now starts its first scanline in Mode 0 before Mode 3; following lines resume normal Mode 2. Combined with the retained LCD-off coincidence flag, the original Mooneye `stat_lyc_onoff` passes and joins the default eight-case regression set. Startup memory access and mode/interrupt behavior have focused PPU tests. Five expanded timing cases still fail, including both dedicated LCD-enable timing tests; precise startup dot timing remains the next investigation.

- **LCD-enable timing acceptance (2026-10-24):** the original Mooneye `lcdon_timing-GS` and `lcdon_write_timing-GS` now pass after separating startup timing, readable LY, coincidence, and OAM/VRAM read/write boundaries. Focused PPU tests preserve the observed dot edges, while the ordinary Mode 3 baseline remains unchanged. Both ROMs join the default ten-case regression set. The expanded 14-case set now has 11 passes and three failures (sprite/SCX timing and VBlank/STAT interaction); those remain the next compatibility investigations.

- **VBlank/STAT acceptance (2026-10-24):** model the DMG Mode 2 STAT source event at VBlank entry, while keeping the reported mode at 1 and honoring the combined STAT line. Focused tests cover blocking and one-entry behavior. The original `vblank_stat_intr-GS` now passes and joins the default eleven-case set. The expanded set has twelve passes and two remaining failures: sprite Mode 0 timing and SCX/HBlank timing.

- **SCX investigation / post-boot IME (2026-10-24):** the SCX ROM revealed that normal skipped-boot startup incorrectly enabled IME. Set the post-boot profile to IME=0 and add a system-interface regression covering IE/IF before EI and handler return afterward. Full tests pass. SCX remains unresolved at its initial SCX=0 boundary; an extra HALT wake M-cycle makes it pass but breaks five existing timing cases, so that experiment was discarded. Continue investigating interrupt and bus timing without a test-specific workaround.

- **Interrupt behavior audit (2026-10-24):** [interrupts.md](interrupts.md) now collects the request/wake/service model, source priority and vectors, current CPU entry implementation, peripheral ownership, test evidence, and explicit coverage gaps. Keep the SCX timing conflict first; investigate EI combinations and entry races with targeted evidence rather than treating this audit as a requirement to implement every interrupt corner case immediately.

- **SCX investigation checkpoint (2026-10-24):** a four-configuration bus-phase/HALT-wake experiment gave 12/14 passes for the current model, versus 9/14, 8/14 and 11/14 for alternatives. No alternative consistently improves coverage. See [the interrupt audit](interrupts.md#bounded-scx-timing-experiment-2026-10-24-historical-checkpoint). Defer further SCX tuning until new evidence or a real-game issue appears; return to the common-game compatibility milestone and regression-check Big2Small after the recent startup/PPU fixes.

- **Big2Small post-fix regression (2026-10-24):** the current libretro core restored a copy of the user's 8 KiB SRAM, accepted scripted A/directional inputs, and reached a puzzle scene. It completed 3,600 frames with 3,600 video callbacks and 2,893,139 stereo audio frames. The final image was inspected without obvious corruption, and the original RetroArch save SHA-256 was unchanged. Artifacts are under `build/homebrew-regression-20261024/`. This checks startup and continued callback delivery after the recent PPU/IME fixes; it does not establish audio quality, level completion, or broad game compatibility. No new gameplay blocker was identified.

- **Repeatable homebrew regression (2026-10-24):** `make test-homebrew` now builds the libretro runner and executes the external Big2Small ROM for 3,600 frames with a recorded input script. Optional `HOMEBREW_SAVE` restores SRAM read-only, checks its hash afterward, and writes separate output SRAM. Reports pin ROM/core hashes and retain a final viewable frame. See [the procedure and limits](homebrew-check.md). The restored-save run passed with 3,600 video callbacks, 2,893,139 stereo audio frames, and unchanged original SRAM; the final puzzle frame was inspected. `make test` also passed. This replaces the temporary-runner workflow; callback success alone is not a gameplay or audio-quality assertion.

- **Timer reload write acceptance (2026-10-24):** the original Mooneye timer set exposed two failures out of thirteen. Separate the pending overflow delay from the four-T-cycle reload-write window, so TIMA writes cancel only before reload, are ignored during reload, and TMA writes update TIMA during that window. Focused Timer-interface tests cover offsets before, during and after reload. All thirteen timer ROMs now pass, along with the eleven default PPU/interrupt ROMs (24/24 combined). `make test` and the 3,600-frame Big2Small restored-save smoke check also pass; the final frame hash and original save hash remain unchanged. `MOONEYE_GROUP=timer` or `all` adds this coverage to the optional local workflow; see [the validation matrix](dmg-validation.md#timer-reload-write-window-2026-10-24).

- **DMG DMA source decoding (2026-10-24):** the first three-case Mooneye DMA check found a source-map failure. DMA now maps pages `$E0`–`$FF` to `$C0`–`$DF` without changing CPU OAM/I/O mappings or FF46 readback. The original `sources-GS` ROM passes, with focused tests covering all high source pages. The local workflow adds `MOONEYE_GROUP=dma`; `all` now includes 27 PPU/interrupt, timer and DMA cases. `make test`, all 27 selected Mooneye cases, and the 3,600-frame Big2Small restored-save check pass; the final image hash matches the prior run and the original save is unchanged. DMA transfer duration, CPU bus restrictions and PPU contention remain unimplemented; do not infer timing accuracy from these three ROMs.

- **Clocked DMA baseline (2026-10-24):** replace immediate copying with a shared-clock DMA device and a separate source-read path that never re-enters CPU timing. Implement startup, 160 byte transfers, delayed restart, CPU OAM blocking, and a basic DMG source-bus conflict model while retaining HRAM and FF46 access. All six original Mooneye DMA ROMs now pass, including the three formerly failing timing cases. Existing completed-copy tests now wait for transfer completion; new component/bus/system tests cover partial progress, ownership boundaries, restart continuity, callback bypass and CPU execution of an HRAM wait routine. `make test`, the expanded default 30-case Mooneye set, and Big2Small's 3,600-frame restored-save regression pass; its final frame hash matches the preceding run and the player save is unchanged. This remains an M-cycle baseline; defer exact DMA write corruption and PPU rendering contention until targeted evidence requires them.

- **GBIT false-green CI audit (2026-10-24):** the first DMA milestone CI run was green despite a logged STOP mismatch (243/244 ordinary opcodes). GBIT's keep-going path printed failures but returned success. The sibling local checkout was older still and also disagreed on EI. Correct normal STOP's padding consumption and keep-going failure propagation in the user's GBIT fork, add reference/tester regressions including a deliberately wrong CPU, and pin CI to `40c5247b818a7c524e3c3e75fe0fde900a4ee8e9` on `fix-stop-and-test-status`. The corrected local integration reports 244/244 ordinary and 256/256 CB opcodes. LotusGB's runner now fails immediately on a mismatch and links GBIT by name so an explicitly supplied library path is effective. `GBIT_DIR` supports an isolated checkout without replacing the user's sibling repository. A green status alone was insufficient evidence; inspect detailed counts when validating a comparison harness.

- **CPU interrupt acceptance checkpoint (2026-10-24):** ten additional Mooneye EI/DI, RETI, HALT and IF/IE ROMs all pass without modifying CPU behavior. Add `MOONEYE_GROUP=interrupt`; the combined default group now has 40 checks, all passing alongside `make test`. New CPU-interface regressions protect EI; EI with an already-pending request and its return PC, DI cancellation across later instructions, and immediate pending service after RETI before the resumed opcode executes. See [the interrupt guide](interrupts.md#cpu-interrupt-acceptance-checkpoint-2026-10-24). Already-pending EI/HALT combinations and IF/IE entry races remain separate gaps; do not treat this checkpoint as full interrupt accuracy.

- **Unconnected DMG serial baseline (2026-10-24):** add SB/SC and an independent free-running serial clock to the system. With internal clock and no peer, eight incoming high bits complete a byte, clear SC bit 7 and request serial IF. External-clock requests stay pending with no peer; clearing enable aborts. Public-interface tests check bit boundaries, partial data, completion and SC-clock phase independence, while CPU-driven tests check HALT wake/serial handler return and continued completion through repeated DIV resets. `make build`, `make test`, the existing 40-case Mooneye set, corrected GBIT (244/244 + 256/256), and Big2Small 3,600-frame regression pass. Actual link peers, accessories, CGB fast serial, exact post-boot clock phase and SC-write races remain future work; the dedicated serial boot-alignment ROM still fails and is tracked separately.

- **Mooneye result-marker correction (2026-10-24):** the serial boot-alignment ROM temporarily sets all six result registers to `$42` during ordinary counting, exposing a false failure in our runner. Require the protocol's `LD B,B` marker alongside the register signature. Seven generated-ROM probes now run in `make test` and cover temporary signatures and marker lookalikes. The corrected runner preserves the default 40 passes and the expanded PPU set's two known failures; the serial boot-alignment ROM still reaches a genuine failure and remains a separate timing gap. Test-register values alone are not completion events.

- **Common mapper external acceptance (2026-10-24):** standard MBC1, MBC2 and MBC5 pass 27 independent Mooneye cases without runtime changes, including the advertised maximum ROM sizes (2 MiB, 256 KiB and 8 MiB respectively). Add `MOONEYE_GROUP=mapper`; `all` now contains 67 passing checks, and reports record each ROM's SHA-256. MBC1M multicarts remain explicitly excluded; the archive supplies no equivalent MBC3 group. Keep RTC persistence, rumble output and actual gameplay acceptance separate from bank-selection coverage. See [the mapper checkpoint](dmg-validation.md#common-mapper-acceptance-2026-10-24).

- **Last VBlank line LY correction (2026-10-24):** reset readable LY to zero during line 153 while retaining the full Mode 1 scanline. Model the separate delayed 153/blank/zero LYC comparison and hold the zero match through frame rollover. Focused PPU tests first reproduced the old late-reset bug, then passed after the correction; a CPU-handler regression observes LYC=0 with LY=0 while still in VBlank. The timing tables and reference comparison are recorded in [the validation matrix](dmg-validation.md#final-vblank-line-ly-and-comparison-2026-10-24). `make build`, `make test`, the default combined 67-case Mooneye set, corrected GBIT (244/244 + 256/256) and Big2Small 3,600-frame regression pass. The final Big2Small frame hash and original player save are unchanged; the expanded PPU diagnostics retain their same two known failures. This does not claim complete STAT race timing or CGB support.

- **Window Y condition (2026-10-24):** replace the per-line `LY >= WY` shortcut with a retained line-start equality condition, cleared during VBlank. Protect WY changes, hidden window rows, frame/LCD reset and DMG window-enable independence through public PPU tests. Keep rendering and object penalties consistent. Exact mid-scanline window glitches remain deferred; see [validation notes](dmg-validation.md#window-y-condition-and-hidden-rows-2026-10-24).

- **Homebrew output checks (2026-10-24):** require matching video counts, nonzero audio, correct RGB dimensions and matching 8 KiB SRAM artifacts for both pinned profiles. Remove stale outputs before execution and retain validation errors in the report. Four ROM-free test methods, including zero-exit/stale-artifact rejection, join `make test` and CI. Both real 3,600-frame profiles pass with unchanged final frame hashes; Big2Small's original player save remains unchanged. This strengthens the local harness without changing emulator behavior or claiming scene recognition.

- **Reported rapid-pause freeze triage (2026-10-24):** prioritize the user's Tetris/WSL report over unrelated feature work. Four headless START burst rates and a baseline complete; longer system probes confirm complete frames and resumed visual progress after one extra START. Some burst sequences end on the game's normal pause screen. The original report remains unreproduced because the exact key and frontend responsiveness were not retained. No runtime workaround or host-setting change was made. See [the diagnostic procedure and remaining evidence](runtime-troubleshooting.md#unreproduced-rapid-pause-report-2026-10-24).

- **Libretro diagnostics (2026-10-24):** connect the frontend log interface, retain stderr fallback, and report rejected content/mapper sizes plus cartridge initialization exceptions. Observe incomplete frame execution with PC/elapsed cycles and suppress repeated warnings until progress, reset or reload. Focused frontend tests cover rejection reasons, normal execution, STOP warning suppression and reset. This improves real-game issue diagnosis; it is not a STOP implementation or a fix for the unreproduced pause report.

- **Mode 3 scrolling baseline (2026-10-24):** capture selected mid-scanline ROM images at their documented marker and retain hashes as an exploratory baseline. These emulator-image comparisons were not hardware acceptance tests. ROM-free marker/CB/immediate/STOP probes join `make test`; no renderer change is claimed at this checkpoint. The next implementation boundary is background tile-fetch/cache sampling; see [the PPU scrolling investigation](ppu-scrolling.md).

- **SCX coarse tile-fetch checkpoint (2026-10-24):** replace the background line snapshot with per-tile cache fills on the existing simplified fetch clock. Preserve line-start fine discard and move object-behind-BG priority to pixel output. New public-interface tests reproduce the old failure. An exploratory emulator-image difference decreased, but it did not establish hardware accuracy and was not used as an acceptance criterion. SCY/bitplane stages, full FIFO occupancy and precise sprite arbitration remain separate work. See [the renderer scope](ppu-scrolling.md#scx-coarse-sampling-implementation).

- **SCY bitplane sampling checkpoint (2026-10-24):** split background tile-number and data-plane reads into separate clock stages. New public PPU tests protect mixed-row bitplanes, already-read data and retained tile numbers. Exploratory emulator-image differences decreased, but did not establish hardware accuracy. A bounded phase experiment did not establish a correct alternative and was discarded. `make test`, 67/67 Mooneye, GBIT and both homebrew profiles pass with unchanged final frames/player SRAM. Precise FIFO/sprite arbitration remains open; see [the PPU sampling notes](ppu-scrolling.md#separate-scy-bitplane-reads).

- **Canyon Racer gameplay baseline (2026-10-24):** pin the author's 0.7.7 standard MBC5 ROM outside the repository and add `HOMEBREW_GAME=canyon-racer`. START at 900, A at 1500 and later jump holds reach gameplay; captures at 1800/2400/3600 show the ship, obstacles, HUD and changing canyon. The full run completes 3,600 video callbacks and 2,893,139 stereo audio frames. A same-input temporary `c13d0f0` build differs by 3,463 pixels at frame 1800, demonstrating a real-game effect of the SCY stages without claiming hardware-exact output. Archived emulator-image comparisons do not validate this game result against hardware. See [the profile and limits](homebrew-check.md#canyon-racer-raster-effect-gameplay-check).

- **Mid-scanline background-map selection (2026-10-24):** sample LCDC bit 3 when each background tile-map entry is fetched instead of once at Mode 3 entry. A focused PPU-interface test confirms an already-fetched tile stays unchanged and the next tile uses the newly selected map. An earlier emulator-image comparison was not hardware-backed and was not used as a pass/fail signal. `make test` and all three 3,600-frame homebrew profiles pass; Big2Small's frame matches a run from the unmodified parent revision. See [the PPU investigation](ppu-scrolling.md#mid-scanline-background-tile-map-select).

## 2026-10-25

- **Mid-scanline DMG background enable (2026-10-25):** read LCDC.0 at pixel output so DMG BG/window pixels become color 0 while disabled, and behind-BG objects become visible. Public PPU tests cover the output transition and object priority. An earlier emulator-image comparison was not hardware-backed and was not used as a pass/fail signal. Remaining fetcher/FIFO interaction is tracked separately; see [the PPU notes](ppu-scrolling.md#mid-scanline-lcdc0-background-enable).

- **Re-enable DMG BG during Mode 3 (2026-10-25):** keep the background fetch clock running while LCDC.0 blanks output, so enabling it mid-line can reveal data fetched before the write. A focused public PPU test first reproduces the previous failure when the line starts with BG disabled. The earlier external image comparison was inconclusive for this change; see [the PPU investigation](ppu-scrolling.md#re-enabling-bg-after-it-starts-disabled).

- **Mid-scanline DMG object enable (2026-10-25):** collect object data for each scanline even while LCDC.1 hides output, and gate emitted object pixels with the live bit. Avoid adding modeled object-fetch stalls when OBJ output is disabled at scanline preparation. Public PPU tests cover output toggles, late enable after scanline preparation, and unchanged Mode 3 duration while disabled. An earlier image comparison used the boot-ROM Nintendo logo tile `$19`, absent from LotusGB's skipped-boot VRAM profile, so it was not a valid acceptance signal for this change. Exact toggle-during-fetch timing remains open; see [the PPU notes](ppu-scrolling.md#mid-scanline-lcdc1-object-enable).

- **DMG ten-object scanline limit (2026-10-25):** add a public PPU regression proving that ten earlier Y-matching objects with X=$A8 (off-screen) still prevent the eleventh OAM entry from appearing, while an earlier object with off-screen Y does not use a slot. This protects the OAM scan selection rule without changing rendering or Mode 3 timing. The rule is specified in [Pan Docs OAM selection priority](https://gbdev.io/pandocs/OAM.html#selection-priority); it does not validate exact per-dot OAM scan behavior.

- **Sprite Mode 0 timing (2026-10-25):** calibrate the modeled first OBJ-fetch overlap against the full Mooneye `intr_2_mode0_timing_sprites` matrix. The ROM now passes after accounting for one dot of overlap between the scanline's first OBJ fetch and initial BG-fetch phase; repeated X=0 objects at the same fetch position pay the flat six-dot cost. Add PPU-interface boundary regressions for X=0 and ten X=10 objects. `make test` passes, and the expanded 14-case PPU run improves from 12/14 to 13/14; only `hblank_ly_scx_timing-GS` remains. The overlap explains the external timing result but is not independently verified on hardware, and Pan Docs describes X=0 as an 11-dot penalty. Keep the discrepancy and incomplete FIFO/sprite arbitration visible in [the validation matrix](dmg-validation.md#sprite-to-mode-0-timing-2026-10-25).

- **SCX/HBlank LY timing resolution (2026-10-25):** a same-clock trace placed the SCX=0 LY read at dot 452 while normal-line LY still changed at dot 454. Move only the PPU's normal-line readable LY edge to dot 452; preserve the dot-454 LCD-start edge and line-153 comparison sequence. The extra HALT-cycle candidate remains rejected because it regressed six passing ROMs. Focused PPU tests, `make test`, and the expanded Mooneye PPU set pass; 14/14 external cases pass. The timing-change workflow and evidence are in the [validation matrix](dmg-validation.md#timing-change-workflow) and [LY edge follow-up](dmg-validation.md#normal-line-ly-edge-follow-up-2026-10-25). Commit `5d42e95` is pushed, and its GitHub CI run passed.

- **Stack-push IDU phase (2026-10-25):** model the first implied SP decrement in the internal M-cycle for PUSH, CALL, and RST; perform the second decrement alongside the high-byte write, then write the low byte without another decrement. CPU-interface regressions check per-M-cycle SP and stack bus behavior for PUSH, CALL, taken/untaken CALL cc, and RST. This aligns the modeled IDU phases with the documented DMG sequence and supports future OAM-corruption modeling; OAM corruption itself remains unimplemented. Other instruction-internal timing remains under audit.

- **Stack-pop and conditional-return audit (2026-10-25):** GB-CTR's detailed schedule places each POP SP increment in the same M-cycle as its corresponding stack read; the existing CPU tests already check those boundaries. Add taken and untaken `RET cc` regressions for the 5/2 M-cycle paths, stack-read order, SP progression and return-PC commit. No instruction sequence or CSV change was needed; the current subfunctions match the documented M-cycle schedule. This remains reference-based validation, not a hardware measurement.

- **POP AF flag-byte boundary (2026-10-25):** add a CPU-interface regression that supplies a stack flag byte with its low nibble set, confirms AF remains unchanged during both stack reads, then checks the AF commit, flag values, SP and next opcode fetch. The existing subfunction already masks F's unused low bits after loading the pair; no production-code or CSV change was needed. The three-M-cycle read/commit sequence follows [GB-CTR](https://gekkio.fi/files/gb-docs/gbctr.pdf), and flag-byte semantics follow the [RGBDS opcode reference](https://rgbds.gbdev.io/docs/master/gbz80.7). This is reference-based validation, not a physical-hardware measurement.

- **PUSH AF stack writes (2026-10-25):** add a CPU-interface regression confirming A is written first, F's packed flag byte second, SP changes in the expected phases, and the next opcode fetch follows both writes. This covers the AF selector alongside the existing PUSH BC trace without changing instruction code or CSV metadata. The write order and four-M-cycle schedule follow [GB-CTR](https://gekkio.fi/files/gb-docs/gbctr.pdf); this is reference-based validation, not a physical-hardware measurement.

- **Conditional control-flow timing coverage (2026-10-25):** exercise all four `JP cc` predicates on taken and untaken paths, including immediate reads, M-cycle counts, target fetches and preserved flags; add a taken `JR cc` path and a negative relative offset; extend taken `CALL cc` through both stack writes and the target fetch. Existing instruction functions match [GB-CTR's schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf), so no production-code or CSV change is needed. This is reference-based validation, not a physical-hardware measurement.

- **Interrupt-entry stack phase (2026-10-25):** the independent interrupt path previously decremented SP alongside each stack write, one M-cycle later than the SM83 reverse-engineered sequence. Move the first implied decrement to the preceding internal cycle, pair the second decrement with the high-byte write, keep SP stable during the low-byte write, and set the vector PC in that low-byte cycle. Per-cycle CPU tests preserve the five-cycle entry, stack write addresses, and vector fetch. This follows reverse-engineered sequencing and Pan Docs' implied-SP-decrement behavior; exact DMG bus address traces remain model-specific and OAM corruption is not implemented.

- **16-bit arithmetic phases (2026-10-25):** align `INC rr` and `DEC rr` with their M2 IDU updates; split `ADD HL,rr` into low-byte and high-byte ALU phases; split `ADD SP,e8` and `LD HL,SP+e8` into low-byte flags, high-byte calculation, and the documented result commit. The CSV now lists these phases explicitly and the generated decoder table is regenerated. CPU tests assert each externally visible phase, including all 65,536 SP-low-byte/offset combinations for `ADD SP,e8`; `make test`, `make build`, and the pinned-GBIT GitHub CI pass. This follows [GB-CTR's detailed instruction schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf) and is reference-based, not physical-hardware verification.

- **LD SP,HL timing check (2026-10-25):** GB-CTR places the SP transfer in the instruction's M2 internal cycle, followed by the next opcode fetch at M3/M1. The existing subfunction already follows this sequence; add a CPU-interface regression checking the transfer phase, unchanged flags, no data-bus access and the next fetch. No CSV or production-code change is needed. This is reference-based, not a physical-hardware measurement.

- **JP HL fetch overlap check (2026-10-25):** GB-CTR documents `JP HL` as a one-M-cycle instruction whose redirected opcode fetch happens at the HL target in the same M-cycle. Add a CPU-interface regression checking the target read address, post-fetch PC, unchanged HL/flags and cycle count; the existing implementation already has the required ordering. No CSV change is needed. This is reference-based, not a physical-hardware measurement.

- **16-bit immediate load phases (2026-10-25):** verify that `LD rr,d16` reads both immediate bytes before committing the selected pair and fetches the next opcode after commit; verify that `LD (a16),SP` writes SPL then SPH to consecutive addresses in separate M-cycles before the next fetch. The existing CSV and subfunctions match GB-CTR, so no implementation change is needed. Focused CPU tests cover all register-pair selectors, bus addresses/order, flags and final PC. This is reference-based, not a physical-hardware measurement.

- **CB `BIT b,(HL)` timing (2026-10-25):** GB-CTR gives this instruction three M-cycles: fetch the CB opcode, read `(HL)`, then update flags while fetching the next opcode. The CSV had an extra NOP cycle after the flag operation, so `BIT_MEMVAL` held the CPU for one unnecessary cycle. Remove that bubble and let the flag phase share the next-opcode fetch. A CPU-interface regression covers all bit positions with both set and clear data, checks flag timing, no memory write, and the following fetch. `make test` passes. This follows [GB-CTR's `BIT b,(HL)` schedule](https://gekkio.fi/files/gb-docs/gbctr.pdf) and is reference-based, not a physical-hardware measurement.

- **Memory read/modify/write phase checks (2026-10-25):** GB-CTR schedules `INC/DEC (HL)` as a read followed by an ALU/write cycle, and CB rotate/shift/`RES`/`SET (HL)` as an extended-opcode fetch, data read, then ALU/write. The existing CSV and subfunctions match these phases. CPU-interface regressions now check read/write ordering, flags, and next-opcode fetches at 8-bit and half-carry boundaries. The full local `make test` suite passes. This is reference-based, not physical-hardware validation.

- **8-bit ALU `(HL)` phases (2026-10-25):** GB-CTR schedules these instructions as a memory read followed by the ALU result and next-opcode fetch in the second M-cycle. The existing CSV and subfunctions match. A CPU-interface regression covers `ADD`, `ADC`, `SUB`, `SBC`, `AND`, `XOR`, `OR`, and `CP`, checking that A and flags remain unchanged during the read phase and commit with the next fetch. Boundary inputs exercise carry, borrow, and zero results. This is reference-based, not physical-hardware validation.

- **Indirect A-load phases (2026-10-25):** GB-CTR schedules `LD A,(BC/DE)` and `LD A,(HL+/-)` as a data read in M2 followed by A commit with the next opcode fetch in M3/M1. For HL auto-update, the memory access uses the old HL and the increment/decrement occurs in the same M-cycle. Existing CSV and subfunctions match. CPU-interface tests check BC/DE retention, original bus address, HL wraparound at `0000`/`FFFF`, unchanged flags, and the later A update. This is reference-based, not physical-hardware validation.

- **Indirect A-store phases (2026-10-25):** GB-CTR schedules `LD (BC/DE),A` and `LD (HL+/-),A` as a write in M2, followed by the next opcode fetch in M3/M1. For HL auto-update, the write uses the old HL and the increment/decrement occurs in that write M-cycle. Existing CSV and subfunctions match. CPU-interface tests check the original write address and A value, HL wraparound at `0000`/`FFFF`, unchanged flags, and the next fetch. This is reference-based, not physical-hardware validation; see [GB-CTR's instruction schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **Indirect HL register and immediate transfers (2026-10-25):** GB-CTR schedules `LD r,(HL)` as a data read in M2 and destination-register commit alongside the next fetch in M3/M1; `LD (HL),r` writes in M2; and `LD (HL),n` reads the immediate in M2, writes in M3, then fetches the next opcode in M4/M1. The existing CSV and subfunctions match. CPU-interface tests cover all seven byte-register load destinations and store sources, immediate store phase order, bus addresses, register/flag commit timing, and cycle lengths. This is reference-based, not physical-hardware validation; see [GB-CTR's indirect-HL instruction schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **8-bit immediate register loads (2026-10-25):** GB-CTR schedules `LD r,n` as an immediate read in M2 followed by target-register commit with the next opcode fetch in M3/M1. The existing CSV and subfunctions match. A CPU-interface regression covers all seven byte-register destinations, confirms the old value and flags persist during the read cycle, and checks the commit, PC and next-fetch address. This is reference-based, not physical-hardware validation; see [GB-CTR's immediate-load schedule](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **Absolute and high-page A loads/stores (2026-10-25):** GB-CTR schedules `LD A,(a16)` / `LD (a16),A` as low and high address reads followed by the data read/write in M4; `LDH A,(n)` / `LDH (n),A` read the offset in M2 and access `FF00+n` in M3; and `LDH A,(C)` / `LDH (C),A` access `FF00+C` in M2. CPU-interface tests check each bus phase, effective address, A commit, unchanged flags and following opcode fetch. Existing CSV and subfunctions match, so runtime behavior is unchanged. This is reference-based, not physical-hardware validation; see [GB-CTR's instruction schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **8-bit immediate ALU phases (2026-10-25):** GB-CTR schedules `ADD`, `ADC`, `SUB`, `SBC`, `AND`, `XOR`, `OR`, and `CP A,n` as an immediate read in M2, then ALU/flag commit with the following opcode fetch in M3/M1. The existing CSV and subfunctions match. CPU-interface tests cover all eight operations, carry-in behavior, carry/half-carry, borrow, zero and CP's unchanged A; they check A/flags remain unchanged during M2 and verify the next-fetch boundary. This is reference-based, not physical-hardware validation; see [GB-CTR's immediate ALU schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **8-bit register ALU phases (2026-10-25):** GB-CTR schedules register-source `ADD`, `ADC`, `SUB`, `SBC`, `AND`, `XOR`, `OR`, and `CP` as one M-cycle. The existing CSV and subfunctions match. CPU-interface tests cover one B-source case for each operation and verify result/flag commit alongside the next opcode fetch, with no operand memory access. This is reference-based, not physical-hardware validation; see [GB-CTR's register ALU schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **8-bit register INC/DEC phases (2026-10-25):** GB-CTR schedules `INC r` and `DEC r` in one M-cycle. The existing CSV and subfunctions match. CPU-interface tests cover all seven byte registers, zero and nibble-boundary cases, carry preservation, and result/flag commit with the next opcode fetch. The separate `(HL)` read/modify/write path remains covered by its three-cycle test. This is reference-based, not physical-hardware validation; see [GB-CTR's INC/DEC schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **Single-cycle flag and accumulator operations (2026-10-25):** GB-CTR schedules `CCF`, `SCF`, `CPL`, and `RLCA`/`RLA`/`RRCA`/`RRA` as one M-cycle. The existing subfunctions match the documented Z/N/H/C behavior, including accumulator rotates forcing Z clear. CPU-interface tests check carry input/output, preserved flags, complemented/rotated A, and the following opcode fetch. This is reference-based, not physical-hardware validation; see [GB-CTR's flag and accumulator rotate schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **CB register operation phases (2026-10-25):** GB-CTR schedules register-form CB rotate/shift, `BIT`, `RES`, and `SET` instructions in two M-cycles. Existing decode tables and subfunctions match. CPU-interface tests check that fetching the second opcode does not update B or flags, then verify representative operation results, flag semantics, and the next opcode fetch in the following cycle. The separate `(HL)` tests cover memory read/write timing. This is reference-based, not physical-hardware validation; see [GB-CTR's CB register schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **Register-to-register load timing (2026-10-25):** GB-CTR schedules `LD r,r` as one M-cycle with no flag changes. CPU-interface tests cover all 49 source/destination combinations among B, C, D, E, H, L, and A; they verify that only the destination changes and the following opcode is fetched in the same cycle. The `(HL)` transfer forms remain covered separately. This is reference-based, not physical-hardware validation; see [GB-CTR's `LD r,r` schedule](https://gekkio.fi/files/gb-docs/gbctr.pdf).

- **Fresh SCX/OBJ dot trace (2026-10-25):** use compile-time tracing in a separate build to capture the current `m3_scx_high_5_bits` run. On LY 8, the model computes BG tile-column 2's map address at dot 112 and reads it at dot 113; the `SCX=8` CPU write reaches the PPU at dot 114, after the fetcher has moved into its low-plane data phase and the prior OBJ stall has ended. The captured shade buffer is identical to the ordinary runner. This confirms current CPU-to-PPU ordering at the modeled M-cycle boundary, not the real CPU's sub-M-cycle bus phase. No runtime timing changed; see the [PPU trace notes](ppu-scrolling.md#localizing-the-remaining-scx-mismatch-2026-10-25).

- **OBJ fetch lifecycle ownership (2026-10-25):** centralize pending-event selection, fetch startup and per-dot advancement in `advanceObjectFetchPipeline()`. Both newly started and active fetches use the same advancement path; the completion dot still stalls output, and another same-position fetch starts on the following dot. `make test`, `make build` and the expanded 14-case Mooneye PPU group pass. Archived image captures are byte-identical to the preceding baseline; this structural change claims no accuracy gain. Local reports remain under the ignored build directory. Record the current-dot outcome rule in the [refactoring guideline](architecture.md#refactoring-guideline); coarse OBJ event timing remains open.

## 2026-10-26

- **Big2Small input-response baseline (2026-10-26):** two neutral 3,600-frame
  runs match exactly; the pinned scripted profile changes 18,546 pixels and
  122 SRAM bytes. This confirms the profile changes visible and saved game
  state, not individual button timing or level completion. See the
  [paired control](homebrew-check.md#big2small-scripted-input-control-2026-10-26).

- **Libretro input phase diagnostic (2026-10-26):** controlled probes found a
  `LY=0`/`LY=144` input-phase difference and a one-callback response shift for
  VBlank-only handling. This is not a hardware accuracy verdict or an F-1 Race
  result; leave frame stepping unchanged pending target-game or hardware-backed
  evidence. See the
  [canonical report](dmg-validation.md#libretro-input-phase-and-vblank-response-2026-10-26).

- **Compiler-warning pass (2026-10-26):** enabled `-Wextra` for the core
  library. Left unused callback parameter names out of implementation
  signatures, narrowed arithmetic helper parameters, and documented the fixed
  storage behavior of the GBIT memory adapter. Both `make -B build` and
  `make test` completed without C++ warning diagnostics. Leak checking, repeated
  frontend lifecycle, and target-architecture checks remain.

- **PPU access-window system regression (2026-10-26):** run CPU code that polls
  STAT, attempts VRAM and OAM writes during Mode 3, then polls to Mode 0. The CPU
  continues through the denied writes, does not wait for access to reopen, and
  sees the original values rather than delayed writes. Subsequent Mode 0 writes
  succeed. This protects the software-visible bus contract; it does not claim
  exact transition-dot accuracy. Existing direct PPU tests still cover the
  blocked read/write windows.

- **Sanitizer and lifecycle baseline (2026-10-26):** the full `make test`
  workflow passed with AddressSanitizer, UndefinedBehaviorSanitizer, and
  LeakSanitizer enabled outside the `ptrace`-restricted sandbox. The dynamic
  libretro runner also passed 32 repetitions of opening the core, loading the
  included GB Pong ROM, running two frames, and closing the core, with no
  sanitizer findings. The runner now cleans up after load errors. These checks
  cover the exercised paths, not every cartridge or frontend state. Inside the
  sandbox LeakSanitizer still exits before analysis because of `ptrace`.

- **Initial Linux API scan (2026-10-26):** the emulator and libretro adapter
  have no direct POSIX/Linux API calls. The standalone ROM runner uses
  `dlopen`/`dlsym`, isolated from the core. Ubuntu `make -B build` passes.

- **Recalbox ARM64 headless check (2026-10-26):** the core cross-builds for
  AArch64 and runs on Recalbox 10.1.1. The included GB Pong ROM completed
  repeated core load/run/unload cycles; neutral and scripted Up final frames
  match Ubuntu x86-64 hashes. Actual RetroArch video, audio, controls, and
  saves on this device remain to be checked. LotusGB is now installed in the
  writable RetroArch user-core directory with an `.info` file, and RetroArch's
  user core/info paths point there. This supports manual core loading; it does
  not register LotusGB in EmulationStation, whose generator uses the fixed
  `/usr/lib/libretro/` core path. A parallel SSH-launched RetroArch fails at
  KMS while EmulationStation owns the display. See the
  [Recalbox procedure and evidence](recalbox.md). Recalbox 10's documented
  custom-system XML can add an ES system entry but reuses available emulators;
  first-class LotusGB selection still needs Recalbox packaging/build
  integration or another supported launcher mechanism.

- **Steam Deck RetroArch gameplay check (2026-10-26):** the user reports that
  the x86_64 LotusGB core loaded through Steam RetroArch and played F-1 Race
  and another DMG title normally. This confirms basic core loading and gameplay
  on that Steam Deck. It does not independently establish sound quality, save
  persistence, hardware accuracy, or broad compatibility. Commercial ROMs
  and game-derived files remain private and are not included in the repository.

- **RetroArch RTC disk-persistence check (2026-10-26):** a temporary MBC3 timer-only probe (type `$0F`) increments the RTC seconds register once at startup. RetroArch 1.22.2 wrote its 24-byte `.rtc` file on clean exit; a second independent process restored the saved timestamp and live clock, then saved the updated state. The test ROM and saves stayed outside the repository. This validates the RetroArch/core persistence path, not sub-second timing or physical RTC accuracy; see the local run record under `build/test-roms/mbc3-rtc-persistence/`.

- **GBMicrotest PPU latch diagnostics (2026-10-26):** the optional `make test-gbmicrotest-ppu-latch` workflow captures the four 800-series SCX, SCY, LCDC tile-select and BG-enable probes from [GBMicrotest](https://github.com/aappleby/gbmicrotest). Each is pinned by its upstream ROM checksum and run for four frames through the libretro core. All produced valid 4/4 video callbacks and patterned output; the images and report are under `build/test-roms/gbmicrotest-ppu-latch/`. The author reports checking the suite on DMG-CPU-08 hardware, revision uncertain. These are visual diagnostics interpreted with source comments, not pixel-accurate reference images or automatic hardware-accuracy gates; the specific LCDC race output remains model-scoped.

- **SCX problem decomposition (2026-10-26):** add an optional controlled-ROM diagnostic and split the investigation into Mode 2 entry/selection, CPU writes, Mode 3 BG-only/OBJ interaction, Mode 0 exit and final pixel composition. SameBoy matches LotusGB with OBJ disabled; single objects at X=1 and X=8 each reproduce the same six differing pixels, versus 84 for the original ROM. The comparison output is stable at 120/180 frames and its original image matches the upstream PNG. No emulator timing changed and no hardware correctness is claimed. See the [diagnostic procedure and next boundary](ppu-scrolling.md#investigation-by-boundary-2026-10-26).

- **Normalized SCX event traces (2026-10-26):** build and calibrate the actual installed SameBoy revision (1.0.4 `c87d256`; its `.info` file was stale). Both cores take 20 T-cycles from interrupt acceptance to vector fetch, but acceptance, SCX write visibility and BG sampling have distinct relative phases. A Mode 2 source-edge candidate leaves 84/0/6/6 differences unchanged; a T2 SCX-write candidate produces 77/0/6/0 but introduces 12 new differences. Both pass 14/14 Mooneye checks and neither is retained. Keep production timing unchanged and use the [normalized event table](ppu-scrolling.md#normalized-cpuppu-event-comparison-2026-10-26) to guide the next investigation. The diagnostic now obtains core versions from the libretro API.

## 2026-10-27

- **Adjustris gameplay smoke (2026-10-27):** build the CC0 game from pinned
  upstream source `d899e2f539c07ac49fd42c055605bf778a2df09d` with RGBDS
  v0.3.10, then run its title-to-board and movement/rotation sequence through
  the libretro core. The final frame differs from the start-only baseline in
  1,320 pixels (minimum 200); both runs deliver 3,600 video callbacks,
  2,893,139 audio frames, and identical 8 KiB SRAM. This verifies visible
  input response in a block game, not line clears, full playability or save
  persistence. See the [local profile and evidence](development-history.md#2026-10-27).

- **reproducible GB Pong gameplay regression (2026-10-27):**
  The `make test` libretro regression uses the included MIT-licensed GB Pong ROM
  with fixed 60-frame neutral and Up-held runs. It checks one input poll and one
  video frame per callback, then verifies the visible game result: the player's
  paddle sits in the middle with neutral input and moves into the top region
  while Up is held. This defines a repeatable startup, continued-execution, and
  control acceptance condition without relying on callback success alone. It
  does not claim full game compatibility or hardware accuracy; an interactive
  RetroArch check remains a separate player-facing validation. Details are in
  the [homebrew check guide](homebrew-check.md#gb-pong-in-process-gameplay-regression).

- **DMG joypad wake from STOP (2026-10-27):**
  The system now connects a selected JOYP P10–P13 input-line falling edge to the
  CPU's STOP wake operation, independently of IME and IE. The joypad interrupt
  request remains a separate IF effect of that selected line edge; HALT wake and
  interrupt service logic are unchanged. A system-level regression confirms an
  unselected direction press leaves STOP active, a selected A press wakes it,
  the joypad IF bit is set, and post-STOP instructions execute with the normal
  post-boot IME=0 / IE=0 state without vectoring. Pan Docs documents the selected
  P10–P13 low-line wake condition and recommends selecting input groups before
  STOP. This implementation is documentation-backed; no physical DMG run was
  available to validate edge phase, already-low-at-entry cases, or unusual STOP
  entry races. See the [joypad status](dmg-validation.md#target-model-and-ppu-acceptance)
  and [interrupt guide](interrupts.md#stop-wake-signal).

  A controlled synthetic input probe now reproduces a one-callback
  input-to-visible-output difference: with the same DMG BIOS and one-callback A
  pulse, SameBoy updates its fixed VBlank marker one output callback before
  LotusGB; moving the pulse by one callback moves both responses by one. Neither
  core misses the press. This is not a hardware-accuracy verdict or evidence of
  a general fixed offset. Defer further timing changes unless a concrete
  playability problem or hardware-backed evidence warrants them; keep the
  completed-frame snapshot and do not bypass JOYP to predict or advance player
  input. Detailed standalone Mode 3 investigations remain paused under the same
  evidence policy. See the [probe record](homebrew-check.md#controlled-input-to-display-probe-2026-10-27).

## 2026-10-28

- **Latest checkpoint (2026-10-28):** Big2Small, Adjustris, and Tobu Tobu Girl
  each completed 3,600 frames through the same core; their final images were
  inspected and showed the expected puzzle scene, changed board pieces, and
  gameplay scene. No new game blocker was found. The hashes and full results
  are in the [homebrew guide](homebrew-check.md#consolidated-local-run-2026-10-28).

- **Big2Small save/resume checkpoint (2026-10-28):** the default local profile
  completes Peanut Pathway, compares a restored 02% slot against a separate
  restored 00% slot, and starts another runner process from the completed SRAM.
  The saved SRAM differs by four bytes; the title frames differ by 29 pixels,
  and Right input after restoration changes 519 pixels. Five runner invocations
  pass the callback contract with no validation errors. The completed save and
  all captures stay under `build/test-roms/big2small/`; this verifies the
  runner/core round trip, not RetroArch disk persistence or hardware behavior.
  See the [homebrew guide](homebrew-check.md#big2small-save-and-resume-check).
