# PPU mid-scanline scrolling

## Scope

The active route is one observable SCX/SCY fetch behavior at a time, with
selected independent visual ROMs and existing regressions. The first checkpoint established a baseline; subsequent checkpoints add
SCX coarse sampling and separate SCY bitplane reads. Complete
Mealybug conformance and a full FIFO rewrite are not acceptance prerequisites.

At the initial baseline the renderer prepared background/object color indices
at Mode 3 entry. The current background path has distinct tile-map and
bitplane sampling stages. Object-behind-BG priority is evaluated at output;
window preparation and simplified Mode 3 stalls remain separate paths.

[Pan Docs scrolling](https://gbdev.io/pandocs/Scrolling.html) describes repeated
scroll-register sampling during tile fetch, with SCX's low three bits sampled
at scanline start. DMG also samples SCY separately for the tile bitplanes.
Use focused tests for each observable transition rather than updating scroll
coordinates independently on every emitted pixel, which would bypass fetch
latency and cached tile data.

## External visual baseline

Source: [Mealybug Tearoom Tests](https://github.com/mattcurrie/mealybug-tearoom-tests),
checkout `70e88fb90b59d19dfbb9c3ac36c64105202bb1f4`.
The source repository includes a prebuilt ROM archive and expected screenshots.
The `DMG-blob` screenshots are emulator reference renders; the project also
provides hardware photographs. Do not describe the reference PNGs as exact
raw hardware captures. Sources and images remain in the external checkout.

```sh
make -C src/lib ppu-frame-runner build_dir="$PWD/build"
# Pillow is required only for this optional image-comparison script.
python3 tools/run_mealybug_tests.py --suite-dir /path/to/mealybug-tearoom-tests
```

The runner captures 160×144 shade indices at the fixed-ROM0 `LD B,B` reporting
marker. Its PC convention matches the CPU's next-opcode prefetch. This is
scoped to the selected ROMs; bank-switched/RAM reporting code is unsupported.
A capture marker means an image is ready, not that the test passed. Timeout,
CPU failure, STOP and output errors fail capture. The Python comparison maps
shade indices to the documented `$FF/$AA/$55/$00` grayscale values and returns
failure for any differing pixel.

`--test m3_scx_high_5_bits` selects one case. Outputs default to
`build/test-roms/mealybug/`: ROM copies, shade buffers, PNGs, red mismatch maps,
logs and `report.json`. The report hashes the archive, each selected ROM and
reference image, and runner executable. The output directory must remain
outside the reference checkout. This optional external suite is not in CI;
`make test` includes ROM-free capture-protocol probes instead.

### Initial result (2026-10-24)

| Test | Different pixels / 23,040 | Result |
| --- | ---: | --- |
| `m3_bgp_change` | 3,642 | FAIL |
| `m3_scx_low_3_bits` | 540 | FAIL |
| `m3_scx_high_5_bits` | 6,426 | FAIL |
| `m3_scy_change` | 9,873 | FAIL |

All four reached their reporting marker. The high-SCX actual/reference images
were inspected; the current output lacks the reference's changed tile columns.
BGP is retained as an output-timing diagnostic, not a passing control.
No PPU behavior was modified to obtain these results. They are newly measured
coverage gaps, not evidence that this tooling change broke emulation.

## Next implementation checkpoint

1. Introduce an explicit background tile-fetch/cache boundary for SCX high-bit
   sampling while preserving scanline-start fine-scroll discard.
2. Add public PPU-interface tests that distinguish already-fetched pixels
   from later tiles and protect unchanged low-bit behavior.
3. Re-run the selected external images. Report exact remaining differences;
   do not tune a global delay solely to hide a single image mismatch.
4. Retain dmg-acid2, unit tests, the selected Mooneye regressions and both
   homebrew profiles before accepting the renderer change. Extend SCY and
   sprite/window fetch interactions as separate steps when evidence requires.

## SCX coarse sampling implementation

The first renderer step introduces a background tile-cache update on the
existing simplified fetch clock. It samples SCX bits 3–7 for each subsequent
8-pixel tile and retains the scanline-start SCX bits 0–2 for initial discard.
Already-emitted pixels and already-fetched tiles are preserved. SCY remains
latched for the line at this step; individual bitplane reads, exact FIFO
occupancy and sprite/fetcher arbitration remain unresolved timing work.

Object selection still happens at Mode 3 entry, but behind-BG priority is now
applied when each pixel is emitted, against the actual fetched background
color. Window pixels retain their existing path. This avoids deciding object
visibility from a stale background snapshot after SCX changes.

Focused public-interface tests cover SCX writes after a tile fetch, retained
fine discard for initial offsets 0 and 3, and behind-BG object visibility over
newly fetched color-zero background. The old implementation fails the new
coarse-scroll test. Exact external visual acceptance is tracked separately;
this cache clock is not a claim of a full hardware fetcher/FIFO implementation.

### Gameplay target

[Canyon Racer](https://bbbbbr.itch.io/canyon-racer) is a candidate for later
interactive validation. Its [source description](https://github.com/bbbbbr/canyon-racer#about-the-effects)
explicitly uses mid-scanline SCY writes for vertical parallax, alongside X
wave scrolling. Use the official Game Boy MBC5 build, keep its mixed-license
ROM/assets external, and record the selected release/hash when supplied.
The first gameplay check is recorded in [the homebrew procedure](homebrew-check.md#canyon-racer-raster-effect-gameplay-check); exact SCY effect accuracy remains unverified.

### Measured result after coarse SCX sampling

`m3_scx_high_5_bits` decreases from 6,426 to **84 differing pixels**, still
FAIL. Remaining differences are confined to x=16–23 on selected rows, so
fetch/sprite interactions need further investigation rather than assuming
complete timing accuracy. BGP (3,642), SCX-low (540) and SCY (9,873) remain
unchanged. The source of those differences has not been established by this
checkpoint.

Validation at this checkpoint: `make test` passes, including dmg-acid2 and
both protocol test tools. The default combined Mooneye set remains 67/67;
GBIT reports 244/244 ordinary and 256/256 CB opcodes. Big2Small and Tobu Tobu
Girl both complete 3,600 frames with their preceding final frame hashes;
the supplied Big2Small player SRAM remains unchanged. The rebuilt libretro
`.so` includes the change. These regressions supplement, rather than replace,
the still-failing Mealybug timing measurements above.

## Separate SCY bitplane reads

Background fetches now latch a tile number, read the low bitplane two dots
later and the high bitplane after another two dots, then commit its eight
pixels to the cache. SCY is sampled for the tile-map row and independently
for each bitplane's row offset. The tile number remains latched if SCY crosses
a tile-map row between stages. SCX coarse sampling keeps its prior phase.
This follows the DMG register sampling behavior described in
[Pan Docs scrolling](https://gbdev.io/pandocs/Scrolling.html) and the separate
[fetch stages](https://gbdev.io/pandocs/pixel_fifo.html).

New public PPU tests write SCY before, between and after the bitplane reads,
checking new-row data, mixed bitplanes and already-read data respectively.
Another test crosses a tile-map row after the map read and verifies that only
the next tile number changes. The preceding implementation fails the new
SCY regression. These are sampling tests on the current clock baseline, not
independent proof of every T-cycle in sprite/FIFO arbitration.

### Bounded investigation of the remaining SCX pixels

Temporary traces show, for example, line 8's third tile sampled at dot 112
and SCX written at dot 114. Testing coarse-sampling phases 6, 7, 8, 9 and 10
yielded 84, 84, 77, 80 and 45 SCX-high differing pixels. None reached an exact
match, and the lower mismatch count alone does not establish hardware timing.
Those variants were discarded; the existing phase remains. The surviving
SCX differences need explicit fetch/sprite interaction evidence.

### Measured SCY result

The SCY diagnostic decreases from 9,873 to **8,330 differing pixels**, still
FAIL. SCX-high remains 84; BGP and SCX-low stay at 3,642 and 540. `make test`,
67/67 default Mooneye cases and GBIT (244/244 ordinary + 256/256 CB) pass.
Both 3,600-frame homebrew profiles preserve their prior final frame hashes
and the original supplied SRAM. The rebuilt core includes these stages, but
full raster-effect compatibility has not been established.

## Gameplay acceptance boundary

The current work targets advanced visible raster effects beyond the selected
games already checked. Common-game smoke success does not establish broad
compatibility, and remaining synthetic pixel differences are diagnostic
evidence rather than a requirement to implement every obscure PPU behavior.
Use Canyon Racer's visible SCY parallax as the next gameplay check, retaining
Mealybug images to explain deviations. The official ROM is distributed on
the author's itch.io page; the GitHub release currently has no ROM asset.
The official 0.7.7 standard MBC5 ROM is now supplied and has completed a
scripted gameplay check. See the homebrew procedure for its hash, input
sequence and limits; no complete compatibility result is claimed.

## First Canyon Racer gameplay observation

The standard 0.7.7 MBC5 ROM reaches gameplay with visible ship, obstacles and
HUD during the 3,600-frame scripted run. Compared with `c13d0f0` at frame 1800,
the SCY-stage version changes 3,463 RGB pixels, with the same input and
startup profile. The canyon textures/rows visibly change between versions.
This provides a real-game target for the sampling work alongside the still
failing Mealybug checks; further accuracy claims require a controlled hardware
or reference comparison. [Procedure and artifacts](homebrew-check.md#canyon-racer-raster-effect-gameplay-check).
