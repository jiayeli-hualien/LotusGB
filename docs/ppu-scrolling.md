# PPU mid-scanline scrolling

## Scope

The active route is one observable SCX/SCY fetch behavior at a time, with
selected independent visual ROMs and existing regressions. The first step
establishes a baseline; it does not yet change the PPU renderer. Complete
Mealybug conformance and a full FIFO rewrite are not acceptance prerequisites.

The current renderer prepares background and object color indices at Mode 3
entry. It emits pixels and samples palettes later, but does not re-read SCX
and SCY at tile-fetch time. Changing this requires keeping object-behind-BG
priority consistent with the background actually fetched, and retaining
window behavior and existing Mode 3 stalls.

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
