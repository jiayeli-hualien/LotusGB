# Development roadmap

This roadmap keeps the next work in dependency order. Prefer small changes with a focused unit test or integration check. Revisit the order when implementation reveals a real dependency; do not rewrite the CPU just to make the design look cleaner.

## Development priorities

- Make emulation correctness the first priority. Define hardware behavior from reliable specifications and observable behavior, and cover it with focused unit or integration tests.
- Make end-to-end compatibility with a small set of commonly played DMG games the active product goal. Do not wait for every documented hardware edge case before trying games; use real games to reveal integration gaps, then add focused regressions for the behavior that caused them.
- Once behavior is tested and stable, measure performance before optimizing. Implementation and storage choices must not define emulation behavior or rely on operating-system-specific semantics in the core.
- Keep platform-specific file access behind a narrow boundary so the emulator core can use portable ROM storage and support alternate backends when profiling justifies them.

## Current route (2026-10-25)

### Active: DMG PPU visual and raster-effect accuracy

The current renderer passes the expanded 14-case Mooneye PPU timing group and
matches the Mooneye `manual-only/sprite_priority` DMG reference image exactly.
The selected Mealybug `DMG-blob` images still differ by 3,642 pixels for BGP,
540 for SCX-low, 84 for SCX-high, 8,334 for SCY, 1,082 for BG-map changes and
2,210 for BG-enable changes. These counts are diagnostics against emulator
reference images, not a hardware score. Canyon Racer cross-core output also
differs between LotusGB, SameBoy and mGBA; no physical-DMG reference image is
available for that run (see the
[three-core diagnostic](homebrew-check.md#three-core-raster-diagnostic-2026-10-25)).

Mode 3 has separate 16-pixel BG and OBJ FIFOs and explicit BG address/data,
push and tile-advance phases. OBJ event timing and shared BG/OBJ bus
arbitration remain coarse. The BG catch-up rule follows the Pan Docs wording;
SameBoy uses a different fetcher-state boundary. Keep the public regression as
a guard for that documented hypothesis, not as hardware verification. Before
changing the boundary or event durations, seek an independent hardware-backed
measurement or a ROM with a hardware-backed expected result. Keep the remaining
Mealybug differences visible and change one observable behavior at a time.
The detailed implementation record, captures and bounded experiments are in
the [PPU guide](ppu-scrolling.md).

### Product follow-ups

- Continue validating common DMG games and PPU effects with the available
  homebrew profiles; keep commercial ROMs and saves outside the repository.
- Rerun Blargg CH3 tests 10 and 12 using the current M-cycle-end bus phase when
  their external ROMs are available. Verify MBC3 RTC disk persistence in
  RetroArch with a legally available timer cartridge.

### Deferred goals

- Boot-ROM execution and CGB support remain outside the current DMG target.
- Save-state, rewind and replay remain deferred; discuss the desired player
  workflow before designing the state snapshot mechanism.
- Raspberry Pi with Recalbox is the next platform target after Ubuntu
  RetroArch; Steam Deck and Windows follow without an immediate deadline.

## Delivery targets

1. **Ubuntu RetroArch core — complete.** RetroArch loads the core, runs a
   supported ROM, displays frames and responds to input. GB Pong Clone is the
   redistributable input-acceptance ROM; see the validation matrix.
2. **Common DMG game compatibility — active.** Verify title-to-gameplay
   progress, visible input response, stable video/audio, and battery-save
   persistence where applicable. Use targeted ROMs and hardware evidence for
   specific behavior; SameBoy and mGBA are comparison points, not authorities.
3. **Raspberry Pi with Recalbox.** Verify the libretro core, controls, video
   and audio on the supported Pi/Recalbox combination.
4. **Steam Deck.** Verify RetroArch loading, controls, display scaling and
   audio on SteamOS.
5. **Windows.** Add a Windows build and CI job, then verify Windows RetroArch.

## Implemented foundation

- **CPU:** instruction execution uses M-cycle steps; focused tests cover flags,
  bus accesses, EI delay, interrupts, and STOP/HALT behavior. GBIT is a separate
  integration check. See the [architecture guide](architecture.md) and
  [interrupt guide](interrupts.md).
- **DMG system:** the headless system composes the CPU, address bus, RAM,
  cartridge, timer, interrupts, joypad, serial, PPU, OAM DMA and APU. The
  default profile skips boot-ROM execution. See the
  [DMG validation matrix](dmg-validation.md).
- **Cartridges:** ROM-only, MBC1, MBC2, MBC3 and MBC5 banking work within the
  limits recorded in the validation matrix. Battery RAM and MBC3 RTC state are
  exposed through libretro memory interfaces.
- **Frontend:** `make build` produces the libretro shared core, which has run
  in Ubuntu RetroArch and delivers video, audio, input and save interfaces.
  The first playable milestone is complete; ongoing game compatibility is
  separate work.
- **Build and CI:** GitHub Actions builds LotusGB, runs in-repository tests,
  and checks instructions against a pinned GBIT revision.
- **Documentation:** the README is the project entry point, implementation
  status lives in topic guides, and sources are grouped in the
  [reference catalog](references.md). The [documentation guide](documentation-guide.md)
  sets the answer-first writing standard.

## Code cleanup

- **Synthetic test startup separation — implemented.** Normal system construction always uses the DMG post-boot profile. Tests request the synthetic CPU-reset profile through `GameBoySystem::createForTest()`, so `TestReset` is no longer a normal startup-mode option.

## Open design questions

These are discussion prompts, not approved implementation tasks. Discuss them one at a time; promote an item into the roadmap only if it is worth addressing.

1. **Joypad and STOP wake-up.** Should button input wake STOP in the current system, and what exact joypad signal should trigger it? The current input API updates JOYP and requests its interrupt but does not wake the CPU from STOP. Gather a hardware-backed case before changing behavior.
2. **GBIT adapter boundary.** GBIT-only sources remain in `src/lib`, but the libretro core excludes them from its build. Consider moving them into test support during later source cleanup; the main shared library still includes every `.cpp` in `src/lib`.

## Resolved design questions

- Keep virtual interfaces where they let unit tests substitute fake dependencies; do not add them to every class by default.
- Let the PPU own VRAM/OAM and route CPU access through the PPU so it can enforce mode-based access restrictions.
- For the first RetroArch core, skip boot-ROM execution and start supported DMG cartridges from an explicit post-boot profile; keep CPU unit-test reset independent. Revisit boot-ROM support only if a later compatibility target needs its animation or exact handoff timing.
- Defer a broad `src` directory reorganization. Isolate GBIT adapter code before libretro, then group hardware modules when the PPU makes the current flat library directory harder to navigate.
- Use Pan Docs as a hardware reference; verify behavior with focused tests rather than treating any one reference emulator as the specification. The [reference catalog](references.md) groups sources by topic.

## Validation rule

For each step, state what externally observable behavior changes, add a focused regression test at the narrowest useful boundary, and run the relevant unit or integration checks. Use the [DMG implementation and validation matrix](dmg-validation.md) to distinguish implemented behavior from independently validated behavior. Use GBIT as one CPU comparison source; investigate disagreements instead of copying behavior without hardware evidence.

## Validation lessons

- GBIT's isolated CPU mock returns `$AA` for every read outside the injected instruction bytes and records writes, not read addresses. It can verify instruction results while missing an incorrect read address: with `C = $30`, `LDH A,(C)` reading `$0030` instead of `$FF30` sees the same mock value. The Blargg CH3 test exposed this bug. Whole-game testing also exposed interrupt entry between the two bytes of `CB 33`; targeted tests should cover bus addresses and instruction boundaries that an isolated opcode test cannot observe.
- **PPU object fetch stalls:** Pan Docs describes a 6–11-dot Mode 3 penalty for each selected object drawn on a scanline, with the first object in a background/window tile paying a position-dependent wait and later objects in that tile paying the flat six dots. The PPU now stalls pixel output at object positions and unit tests check those delays and Mode 3 length. Mid-mode-3 tile-fetch/register timing still needs comparison against a DMG PPU ROM suite.
- **PPU Mode 2/STAT timing diagnostic (2026-10-23):** The local `make test-mooneye` workflow runs a curated set of external timing ROMs through the DMG post-boot system and checks Mooneye's register-result protocol. At the previous bus phase (after 3 T-cycles), `intr_1_2_timing-GS`, `intr_2_0_timing`, `intr_timing`, and `halt_ime1_timing` passed; `intr_2_mode3_timing`, `intr_2_mode0_timing`, and `intr_2_oam_ok_timing` failed on the first measurement by one count. Changing the Mode 2 threshold to 79 dots made two tests pass but violated the existing 80-dot boundary test; delaying STAT evaluation at visible-line start broke `intr_1_2_timing-GS`. Both were discarded. Moving CPU bus transactions to the end of the M-cycle makes all seven selected Mooneye ROMs pass while preserving the 80-dot Mode 2 test. The current local Blargg run also passes test 09; tests 10 and 12 need rerunning with this phase when available. ROM binaries remain outside the repository; reports and logs are under `build/test-roms/mooneye/`.

## Test ROMs and acceptance

- **Real-game acceptance:** use a short, representative set of commonly played DMG games for end-to-end checks. A game counts as playable only after it reaches gameplay, accepts controls that change the game state, and runs with stable video and audio; verify save/load for games with battery-backed saves. Keep commercial ROMs outside the repository. Use targeted homebrew and hardware tests to isolate any behavior exposed by these checks.
- **Local Pokémon Red check (2026-10-23):** a user-provided commercial ROM remained outside the repository. SameBoy comparison exposed an interrupt accepted between a `CB` prefix and its opcode; the CPU fix now executes the prefixed instruction before interrupt entry. The revised libretro input run reached the player's room, and a Right-button hold changed the framebuffer; LotusGB and SameBoy matched on the sampled frames from frame 1,500 onward. The run produced 5,400 video frames without unsupported-opcode diagnostics. A scripted in-game save completed; restoring its 32 KiB SRAM block into a fresh libretro core showed the Continue option, resumed in the saved room, and accepted directional input. This used a temporary runner and kept the commercial ROM outside the repository. RetroArch's actual `.srm` disk save/reload and broader gameplay remain to be verified.
- **Local Pokémon Yellow check (2026-10-23):** a user-provided commercial ROM remained outside the repository. Its header declares a 1 MiB MBC3 + RAM + battery cartridge (`$13`) with 32 KiB RAM. LotusGB reached the title screen, started a new game, reached the player's room, and moved the player in response to Right input. Sampled title and gameplay frames matched SameBoy pixel-for-pixel. This verifies startup and basic control response for another MBC3 game; outdoor progression and battery-save reload still need verification.
- **Local audio comparison (2026-10-23):** the captured PCM contained the town music that sounded noisy during live RetroArch playback in WSL. Playing that same capture directly in Windows produced no audible noise. This points to the WSL/RetroArch playback path rather than the APU sample stream; it has not been compared on a native Linux or Windows RetroArch installation.
- Use focused tests from the [Mooneye Test Suite](https://github.com/Gekkio/mooneye-test-suite) and [Blargg test ROM collection](https://github.com/c-sp/game-boy-test-roms) for CPU, timer, interrupt, memory, and other hardware behavior as those features become available. Record each selected ROM's model, boot ROM, hardware dependencies, and pass/fail signal. Do not make passing an entire suite an early milestone.
- **APU ROM targets:** Blargg's [DMG sound suite](https://github.com/retrio/gb-test-roms/tree/master/dmg_sound) covers CPU-visible APU behavior; `09-wave read while on`, `10-wave trigger while on`, and `12-wave write while on` target CH3 wave RAM behavior. The MBC1 external RAM result channel now lets the local runner check their `Passed`/`Failed` status automatically. The [Game Boy Noise Test](https://github.com/not-chciken/gb-noise-test) is an interactive CH4 listening tool with DMG-01 reference recordings, not an automated pass/fail test. Do not check in Blargg binaries until their redistribution terms are clear. The Noise Test project is Apache-2.0 licensed; if we vendor its ROM, include the required license notices and pin its checksum.
- **Local Blargg run procedure:** `make test-roms` looks in `../test-roms/dmg_sound` by default; override with `TEST_ROM_DIR=/path/to/dmg_sound`. The script accepts the original test 09, 10, and 12 MBC1+RAM+BATTERY ROMs and checks their SHA-256 values: test 09 `378b86f6a25daa16855260d7ef0c24e48146ba05df434a0b5413983a1447e875`, test 10 `fa63c8ed7473411e54285d318e33bf23ff6d637ed2caa7555ddeaf80578e3279`, and test 12 `2efbecd2c6d40928d44f45da4f634626bcd2790165b3949aedfe377c73913774`. It runs 1,200 frames by default (`TEST_ROM_FRAMES` can override), saves screenshots, SRAM dumps and logs, and records the automatic result in a report under `build/test-roms/`. Missing ROMs are marked skipped. Keep this optional workflow out of required CI for now. On 2026-10-22, tests 09, 10, and 12 all passed after 1,200 frames; the 10 and 12 output frames matched SameBoy's libretro core pixel-for-pixel.
- **DMG PPU visual acceptance passed (2026-10-21):** [dmg-acid2 v1.0](https://github.com/mattcurrie/dmg-acid2/releases/tag/v1.0), 32 KiB ROM-only, DMG mode, no actual boot ROM. A temporary runner compared a 60-frame output with the published `reference-dmg.png`: all 23,040 pixels matched. The checked-in regression now starts from the DMG post-boot profile, runs the ROM for 10 frames from `$0100`, and compares the 160×144 shade-index framebuffer exactly. ROM SHA-256: `464e14b7d42e7feea0b7ede42be7071dc88913f75b9ffa444299424b63d1dff1`. ROM, reference image, and MIT attribution are in `tests/roms/dmg-acid2/`. The test uses LY=LYC interrupts to change settings between scanlines and does not test mode 3 penalties or mid-mode-3 writes.
- **PPU timing reference:** the [Mealybug Tearoom test ROMs](https://github.com/mattcurrie/mealybug-tearoom-tests) focus on PPU register changes during mode 3 and compare rendered screenshots against DMG references. Use them to validate finer fetch timing after the tile-fetcher model is improved; the suite is MIT licensed.
- **Gameplay input regression:** [GB Pong Clone](https://github.com/AlexMGiroux/gb-pong-clone), 32 KiB, cartridge type `$00` (ROM-only), distributed under MIT. The libretro test runs 60 frames with neutral input and 60 with Up held; the resulting framebuffers differ. ROM SHA-256: `9c5e337db0ee1416f2c7f1de02e7e0a78a6064cf1da49209b4bc1a36a7b35415`. ROM and license attribution are in `tests/roms/gb-pong/`.
- **Open-licensed MBC1 gameplay and save target:** [Tobu Tobu Girl](https://tangramgames.itch.io/tobutobugirl), the original DMG release. The author offers the ROM as a free download; the source code is MIT and the assets are CC BY 4.0. Its build uses cartridge type `$03` (MBC1 + RAM + battery), 16 ROM banks, and one RAM bank; the game saves progress and high scores. Keep it as an MBC1 gameplay target. Its high-score write requires completing a level, so do not require this game for the manual save-persistence check. Keep the ROM outside the repository unless we later add the required attribution, license notices, and pinned checksum.
- **First playable RetroArch acceptance:** GB Pong Clone is the fixed homebrew target: its 32 KiB ROM-only header and MIT redistribution terms are confirmed, and its ROM plus attribution are in `tests/roms/gb-pong/`. The in-process libretro regression verifies that Up changes the framebuffer; an interactive RetroArch run with visible input response remains the final manual check.
- Keep downloaded ROM binaries out of the repository unless their redistribution terms are clear. Pin the selected source or release and record a checksum for repeatable local and CI runs.

## Platform support

Use Linux as the primary development and CI platform for now. Raspberry Pi running Recalbox is the preferred next target, followed by Steam Deck and Windows. Build the portable emulator core and libretro adapter first, then add platform-specific build and integration checks. Keep code and build configuration portable where practical; defer dedicated checks for each target until the core and libretro adapter are established.

- **Big2Small battery-save acceptance (2026-10-23):** the user played the locally supplied, freely available [Big2Small ROM](https://mdsteele.itch.io/big2small), kept outside the repository at `../test-roms/homebrew/big2small.gb`. Its header declares 64 KiB ROM, MBC5 + RAM + battery (`$1B`), and 8 KiB RAM. RetroArch created an 8 KiB `big2small.srm`; the user confirmed that game progress remained after reopening the game. This completes the actual RetroArch disk-save/reload check for this MBC5 game. Existing `testLibretroPersistsMbc5BatteryRamAcrossFrontendReload` covers SRAM writes and frontend restoration with a synthetic ROM; retain both forms of evidence. MBC3 RTC disk persistence and broader game compatibility remain separate follow-up work.

- **Big2Small scripted soak (2026-10-23):** a temporary in-process libretro runner restored a copy of the user's 8 KiB SRAM, selected a save slot, dismissed tutorial dialogue, and issued periodic animal-selection and directional inputs in a puzzle scene. The core completed 36,000 frames (about ten minutes of emulated time), publishing 36,000 video callbacks and 28,931,396 stereo audio frames without runner errors. Periodic framebuffer hashes changed and the inspected early/final gameplay frames showed no obvious rendering corruption. Local artifacts are under `build/big2small-check/`; the original RetroArch save was not modified. This checks continued execution and callback delivery, not puzzle completion, broader level coverage, audio quality, or a pixel-accurate comparison with SameBoy.

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

- **Second homebrew gameplay smoke target (2026-10-24):** add `make test-homebrew HOMEBREW_GAME=tobutobugirl`, preserving Big2Small as the default and battery-save acceptance target. Pin the external original Tobu Tobu Girl ROM hash and use a separate input profile/output directory. A 3,600-frame run reaches a visually inspected action scene with the character, enemies and HUD; neutral input stays in the opening scene. This expands MBC1/action-game execution evidence without changing runtime code. It does not prove level completion, exact graphics/audio or high-score persistence. See [the local procedure](homebrew-check.md#tobu-tobu-girl-gameplay-smoke-check).

- **Homebrew output checks (2026-10-24):** require matching video counts, nonzero audio, correct RGB dimensions and matching 8 KiB SRAM artifacts for both pinned profiles. Remove stale outputs before execution and retain validation errors in the report. Four ROM-free test methods, including zero-exit/stale-artifact rejection, join `make test` and CI. Both real 3,600-frame profiles pass with unchanged final frame hashes; Big2Small's original player save remains unchanged. This strengthens the local harness without changing emulator behavior or claiming scene recognition.

- **Reported rapid-pause freeze triage (2026-10-24):** prioritize the user's Tetris/WSL report over unrelated feature work. Four headless START burst rates and a baseline complete; longer system probes confirm complete frames and resumed visual progress after one extra START. Some burst sequences end on the game's normal pause screen. The original report remains unreproduced because the exact key and frontend responsiveness were not retained. No runtime workaround or host-setting change was made. See [the diagnostic procedure and remaining evidence](runtime-troubleshooting.md#unreproduced-rapid-pause-report-2026-10-24).

- **Libretro diagnostics (2026-10-24):** connect the frontend log interface, retain stderr fallback, and report rejected content/mapper sizes plus cartridge initialization exceptions. Observe incomplete frame execution with PC/elapsed cycles and suppress repeated warnings until progress, reset or reload. Focused frontend tests cover rejection reasons, normal execution, STOP warning suppression and reset. This improves real-game issue diagnosis; it is not a STOP implementation or a fix for the unreproduced pause report.

- **Mode 3 scrolling baseline (2026-10-24):** pin a Mealybug source checkout and capture selected visual ROMs at their documented LD B,B marker. Measure exact DMG-blob PNG differences and retain mismatch maps/hashes. Initial BGP/SCX-low/SCX-high/SCY results remain failures (3,642 / 540 / 6,426 / 9,873 differing pixels). ROM-free marker/CB/immediate/STOP probes join `make test`; external image comparisons remain optional. No renderer change is claimed at this checkpoint. The next implementation boundary is background tile-fetch/cache sampling; see [the PPU scrolling plan and baseline](ppu-scrolling.md).

- **SCX coarse tile-fetch checkpoint (2026-10-24):** replace the background line snapshot with per-tile cache fills on the existing simplified fetch clock. Preserve line-start fine discard and move object-behind-BG priority to pixel output. New public-interface tests reproduce the old failure. The selected high-SCX image improves from 6,426 to 84 differing pixels, still FAIL; other selected image counts are unchanged. SCY/bitplane stages, full FIFO occupancy and precise sprite arbitration remain separate work. See [the renderer scope](ppu-scrolling.md#scx-coarse-sampling-implementation).

- **SCY bitplane sampling checkpoint (2026-10-24):** split background tile-number and data-plane reads into separate clock stages. New public PPU tests protect mixed-row bitplanes, already-read data and retained tile numbers. The SCY image improves from 9,873 to 8,330 differing pixels, still FAIL; SCX-high remains at 84. A bounded phase experiment did not establish a correct alternative and was discarded. `make test`, 67/67 Mooneye, GBIT and both homebrew profiles pass with unchanged final frames/player SRAM. Precise FIFO/sprite arbitration remains open; see [the PPU sampling notes](ppu-scrolling.md#separate-scy-bitplane-reads).

- **Canyon Racer gameplay baseline (2026-10-24):** pin the author's 0.7.7 standard MBC5 ROM outside the repository and add `HOMEBREW_GAME=canyon-racer`. START at 900, A at 1500 and later jump holds reach gameplay; captures at 1800/2400/3600 show the ship, obstacles, HUD and changing canyon. The full run completes 3,600 video callbacks and 2,893,139 stereo audio frames. A same-input temporary `c13d0f0` build differs by 3,463 pixels at frame 1800, demonstrating a real-game effect of the SCY stages without claiming hardware-exact output. Keep remaining Mealybug differences as diagnostics. See [the profile and limits](homebrew-check.md#canyon-racer-raster-effect-gameplay-check).

- **Mid-scanline background-map selection (2026-10-24):** sample LCDC bit 3 when each background tile-map entry is fetched instead of once at Mode 3 entry. A focused PPU-interface test confirms an already-fetched tile stays unchanged and the next tile uses the newly selected map. Add Mealybug `m3_lcdc_bg_map_change` to the optional visual set; the reference difference improves from 1,234 to 1,082 pixels, still FAIL, due to unresolved sprite/fetch timing. `make test` and all three 3,600-frame homebrew profiles pass; Big2Small's frame matches a run from the unmodified parent revision. See [the PPU investigation](ppu-scrolling.md#mid-scanline-background-tile-map-select).

- **Mid-scanline DMG background enable (2026-10-25):** read LCDC.0 at pixel output so DMG BG/window pixels become color 0 while disabled, and behind-BG objects become visible. Public PPU tests cover the output transition and object priority. Add Mealybug `m3_lcdc_bg_en_change` to the optional image set; its reference mismatch decreases from 2,189 to 1,750 pixels but still fails. Remaining fetcher/FIFO interaction is tracked separately; see [the PPU notes](ppu-scrolling.md#mid-scanline-lcdc0-background-enable).

- **Re-enable DMG BG during Mode 3 (2026-10-25):** keep the background fetch clock running while LCDC.0 blanks output, so enabling it mid-line can reveal data fetched before the write. A focused public PPU test first reproduces the previous failure when the line starts with BG disabled. The Mealybug BG-enable image remains at 1,750 differing pixels because its sprite-dependent timing mismatch is separate; see [the PPU investigation](ppu-scrolling.md#re-enabling-bg-after-it-starts-disabled).

- **Mid-scanline DMG object enable (2026-10-25):** collect object data for each scanline even while LCDC.1 hides output, and gate emitted object pixels with the live bit. Avoid adding modeled object-fetch stalls when OBJ output is disabled at scanline preparation. Public PPU tests cover output toggles, late enable after scanline preparation, and unchanged Mode 3 duration while disabled. The selected Mealybug object-enable screenshot remains 146 pixels different, but its source uses the boot-ROM Nintendo logo tile `$19`, absent from LotusGB's skipped-boot VRAM profile; the image is not a valid acceptance signal for this change. Exact toggle-during-fetch timing remains open; see [the PPU notes](ppu-scrolling.md#mid-scanline-lcdc1-object-enable).

- **DMG ten-object scanline limit (2026-10-25):** add a public PPU regression proving that ten earlier Y-matching objects with X=$A8 (off-screen) still prevent the eleventh OAM entry from appearing, while an earlier object with off-screen Y does not use a slot. This protects the OAM scan selection rule without changing rendering or Mode 3 timing. The rule is specified in [Pan Docs OAM selection priority](https://gbdev.io/pandocs/OAM.html#selection-priority); it does not validate exact per-dot OAM scan behavior.

- **Sprite Mode 0 timing (2026-10-25):** calibrate the modeled first OBJ-fetch overlap against the full Mooneye `intr_2_mode0_timing_sprites` matrix. The ROM now passes after accounting for one dot of overlap between the scanline's first OBJ fetch and initial BG-fetch phase; repeated X=0 objects at the same fetch position pay the flat six-dot cost. Add PPU-interface boundary regressions for X=0 and ten X=10 objects. `make test` passes, and the expanded 14-case PPU run improves from 12/14 to 13/14; only `hblank_ly_scx_timing-GS` remains. The overlap explains the external timing result but is not independently verified on hardware, and Pan Docs describes X=0 as an 11-dot penalty. Keep the discrepancy and incomplete FIFO/sprite arbitration visible in [the validation matrix](dmg-validation.md#sprite-to-mode-0-timing-2026-10-25).

- **SCX/HBlank LY timing resolution (2026-10-25):** a same-clock trace placed the SCX=0 LY read at dot 452 while normal-line LY still changed at dot 454. Move only the PPU's normal-line readable LY edge to dot 452; preserve the dot-454 LCD-start edge and line-153 comparison sequence. The extra HALT-cycle candidate remains rejected because it regressed six passing ROMs. Focused PPU tests, `make test`, and the expanded Mooneye PPU set pass; 14/14 external cases pass. The timing-change workflow and evidence are in the [validation matrix](dmg-validation.md#timing-change-workflow) and [LY edge follow-up](dmg-validation.md#normal-line-ly-edge-follow-up-2026-10-25). Commit `5d42e95` is pushed, and its GitHub CI run passed.

- **Stack-push IDU phase (2026-10-25):** model the first implied SP decrement in the internal M-cycle for PUSH, CALL, and RST; perform the second decrement alongside the high-byte write, then write the low byte without another decrement. CPU-interface regressions check per-M-cycle SP and stack bus behavior for PUSH, CALL, taken/untaken CALL cc, and RST. This aligns the modeled IDU phases with the documented DMG sequence and supports future OAM-corruption modeling; OAM corruption itself remains unimplemented. Other instruction-internal timing remains under audit.

- **Stack-pop and conditional-return audit (2026-10-25):** GB-CTR's detailed schedule places each POP SP increment in the same M-cycle as its corresponding stack read; the existing CPU tests already check those boundaries. Add taken and untaken `RET cc` regressions for the 5/2 M-cycle paths, stack-read order, SP progression and return-PC commit. No instruction sequence or CSV change was needed; the current subfunctions match the documented M-cycle schedule. This remains reference-based validation, not a hardware measurement.

- **Interrupt-entry stack phase (2026-10-25):** the independent interrupt path previously decremented SP alongside each stack write, one M-cycle later than the SM83 reverse-engineered sequence. Move the first implied decrement to the preceding internal cycle, pair the second decrement with the high-byte write, keep SP stable during the low-byte write, and set the vector PC in that low-byte cycle. Per-cycle CPU tests preserve the five-cycle entry, stack write addresses, and vector fetch. This follows reverse-engineered sequencing and Pan Docs' implied-SP-decrement behavior; exact DMG bus address traces remain model-specific and OAM corruption is not implemented.

- **16-bit arithmetic phases (2026-10-25):** align `INC rr` and `DEC rr` with their M2 IDU updates; split `ADD HL,rr` into low-byte and high-byte ALU phases; split `ADD SP,e8` and `LD HL,SP+e8` into low-byte flags, high-byte calculation, and the documented result commit. The CSV now lists these phases explicitly and the generated decoder table is regenerated. CPU tests assert each externally visible phase, including all 65,536 SP-low-byte/offset combinations for `ADD SP,e8`; `make test`, `make build`, and the pinned-GBIT GitHub CI pass. This follows [GB-CTR's detailed instruction schedules](https://gekkio.fi/files/gb-docs/gbctr.pdf) and is reference-based, not physical-hardware verification.

- **LD SP,HL timing check (2026-10-25):** GB-CTR places the SP transfer in the instruction's M2 internal cycle, followed by the next opcode fetch at M3/M1. The existing subfunction already follows this sequence; add a CPU-interface regression checking the transfer phase, unchanged flags, no data-bus access and the next fetch. No CSV or production-code change is needed. This is reference-based, not a physical-hardware measurement.

- **JP HL fetch overlap check (2026-10-25):** GB-CTR documents `JP HL` as a one-M-cycle instruction whose redirected opcode fetch happens at the HL target in the same M-cycle. Add a CPU-interface regression checking the target read address, post-fetch PC, unchanged HL/flags and cycle count; the existing implementation already has the required ordering. No CSV change is needed. This is reference-based, not a physical-hardware measurement.

- **16-bit immediate load phases (2026-10-25):** verify that `LD rr,d16` reads both immediate bytes before committing the selected pair and fetches the next opcode after commit; verify that `LD (a16),SP` writes SPL then SPH to consecutive addresses in separate M-cycles before the next fetch. The existing CSV and subfunctions match GB-CTR, so no implementation change is needed. Focused CPU tests cover all register-pair selectors, bus addresses/order, flags and final PC. This is reference-based, not a physical-hardware measurement.
