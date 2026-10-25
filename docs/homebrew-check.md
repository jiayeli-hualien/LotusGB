# Local homebrew regressions

These optional local profiles run freely available homebrew through the built
libretro core. Big2Small is the repeatable gameplay target and the manual
save-persistence acceptance game. Tobu Tobu Girl and Canyon Racer provide
additional MBC1 and raster-effect smoke coverage. The automated profiles check
that the selected ROM reaches a visible state and callbacks continue; they do
not prove full game compatibility or exact hardware output. ROMs and player
saves stay outside the repository.

Run from the repository root:

```sh
make test-homebrew
# Optionally restore a copy of an existing SRAM image into the core:
make test-homebrew HOMEBREW_SAVE=/absolute/path/to/big2small.srm
```

`HOMEBREW_ROM` defaults to `../test-roms/homebrew/big2small.gb`.
Obtain the free ROM from the [author's page](https://mdsteele.itch.io/big2small).
The accepted ROM SHA-256 is
`59c096333f93c12c8eb25a5fcffa2be15001abcd5d20b9a82c2bc2dae7e625b2`
(64 KiB, MBC5 + RAM + battery). A different release needs explicit review of
the input sequence before updating the pinned hash.
ROMs and player saves are not committed. This optional workflow is outside CI.

The script runs 3,600 frames with fixed A and directional inputs through the
actual libretro core. The optional save is read into memory; output SRAM goes
only to `build/test-roms/big2small/output.srm`. It checks the original save hash
again afterward. Do not treat the generated SRAM as a replacement for your
player save. Starting progress affects the scene reached by the fixed inputs.

Artifacts under `build/test-roms/big2small/`:

- `input.txt`: input intervals; zero-based start, exclusive end, button name.
- `runner.log`: video/audio callback counts and SRAM size, or runner errors.
- `frame.ppm`: final 160×144 RGB frame for manual inspection.
- `frame.rgb`, `output.srm`: raw framebuffer and final emulated SRAM.
- `report.json`: ROM/core hashes, command, exit status, original save hash and
  preservation check, final frame hash and callback summary.

Success means the core accepted the cartridge, executed the scripted run, and
published one valid video frame per call. Review the frame for the expected
scene. Callback counts do not prove audible sound quality, responsive gameplay,
puzzle completion, or save persistence. The user's separate RetroArch
save/reload check remains the evidence for actual disk persistence.

## General ROM runner interface

```sh
build/libretro_rom_runner CORE.so ROM.gb FRAMES OUTPUT.rgb OUTPUT.srm \
  [INPUT.txt [RESTORE.srm]]
```

Without optional arguments it preserves the neutral-input ROM-test behavior.
Input lines contain `START END BUTTON`; blank lines and `#` comments are allowed.
Buttons: `A B START SELECT UP DOWN LEFT RIGHT`. Overlapping intervals permit
combinations. Intervals must fit within the requested run. Restored SRAM must
match the cartridge's exposed SRAM size. An output cannot alias the restored
save, including through a symlink or hard link. To restore without input, supply
an empty input file.

## Tobu Tobu Girl gameplay smoke check

```sh
make test-homebrew HOMEBREW_GAME=tobutobugirl
```

The default ROM is `../test-roms/homebrew/tobutobugirl/tobu.gb`, obtained from
[the author's page](https://tangramgames.itch.io/tobutobugirl). The pinned
SHA-256 is `5d3871cae77db2287807e8914fd21f76203e73aa3fec20955489d45cb4571d8f`
(256 KiB, MBC1 + RAM + battery, 8 KiB SRAM). Source and assets have separate
licenses; keep the downloaded ROM external. See [the source project](https://github.com/SimonLarsen/tobutobugirl).

This profile starts without player SRAM, presses START during the opening
scenes, confirms a stage with A, then exercises rightward movement, A dash
and B upward boost. The 3,600-frame run writes the same artifact types under
`build/test-roms/tobutobugirl/`. Review `frame.ppm` for actual gameplay; the
exit status checks execution and callbacks, not scene recognition. Timing is
pinned to this ROM release and may need adjustment if restored progress is
supplied. Big2Small remains the manual battery-save acceptance game; this
profile does not require beating a Tobu Tobu Girl level or writing a high score.

On 2026-10-24, the scripted run reached a visible gameplay scene with the
character, enemies and HUD. The neutral-input run remained in the opening
scene. These observations demonstrate a limited startup/input path, not
complete level compatibility, precise rendering or sound quality.

The same profile was rerun on 2026-10-26. It completed 3,600 video callbacks
and 2,893,139 stereo audio frames, produced an 8 KiB SRAM block, and showed a
gameplay scene. The final frame SHA-256 was
`260f1d3926acb5a2db9b2ec757b6a493158dc4f9de3b98b7e939df484e35c8f4`; the core
SHA-256 was `80ec278b8906ac60ce3f865098cec05c50bddef291461aff5cb5a4963fa1b006`
and the input script SHA-256 was
`2c205fb0ed775c6d8138cef8cb5821ee29e864bb402b88a45a61afd7c44a724c`.
This remains a scripted execution check, not a level-completion or save-
persistence acceptance test.

## Automated output checks

For all pinned profiles, success requires a runner summary reporting 3,600
requested/video frames, nonzero delivered audio frames, 160×144 RGB output
of the matching size, and an 8,192-byte SRAM output matching the reported
size. The report includes `validation_error` (null on success). Old RGB,
preview and SRAM outputs are removed before execution so a failed attempt
cannot reuse the preceding frame as fresh evidence. A zero runner exit with
missing or inconsistent artifacts still fails and records the reason.

`make test` runs `tools/test_homebrew_check.py` without external ROMs. It
checks valid output and rejects missing/duplicated summaries, wrong counts,
missing audio, wrong dimensions and truncated/oversized artifacts. A mocked
runner also verifies that a zero exit without fresh artifacts cannot pass
using stale files. These checks establish output delivery, not audible audio
quality, correct scene contents or disk-save persistence.

## Canyon Racer raster-effect gameplay check

```sh
make test-homebrew HOMEBREW_GAME=canyon-racer
```

Official source/download: [Canyon Racer by bbbbbr](https://bbbbbr.itch.io/canyon-racer).
Use version 0.7.7's standard `canyon_racer_0.7.7_mbc5.gb`, downloaded to
`../test-roms/homebrew/canyon-racer/`. The pinned ROM SHA-256 is
`19f57c1fefe8ebdcef0ff9def783246a6618d4b822b5c44188a1e9c1e7448c51`
(32 KiB, type `$1B`, MBC5 + RAM + battery, 8 KiB SRAM). The official ZIP
SHA-256 is `f913eae23b43bfc87cb3fa44863ed7e97eb77d4356540d69d7656cc6d8ae5d5b`.
ROM, archive and mixed-license game assets remain outside version control.

The clean-SRAM profile lets the credits/title settle, presses START at frame
900, dismisses first-run help with A at frame 1500, then holds A for 30 frames
every 90 frames from 1800 onward. Outputs use `build/test-roms/canyon-racer/`.
This profile targets the selected release and clean startup; restored progress
can alter the initial help sequence. It does not require winning the game.

On 2026-10-24, intermediate captures at 1800/2400/3600 frames showed the ship,
obstacles, HUD and changing canyon scene. The 3,600-frame run completed with
3,600 video callbacks, 2,893,139 stereo audio frames and an 8 KiB SRAM output.
These are execution/visible-progress observations, not exact hardware rendering
or sound-quality acceptance. The game is difficult and the scripted run loses
lives; this is not a full-level gameplay assertion.

On 2026-10-26, two 3,600-frame runs with the same ROM, core and input script
produced byte-identical final frames. The framebuffer SHA-256 was
`8b2cba922ae41dcb3e9cfb107b37cbb0dc870e687acc1da0226c2735d73cd313`; ROM,
core and input-script SHA-256 values were
`19f57c1fefe8ebdcef0ff9def783246a6618d4b822b5c44188a1e9c1e7448c51`,
`80ec278b8906ac60ce3f865098cec05c50bddef291461aff5cb5a4963fa1b006`, and
`b0e199174963c1fae245255a3c1a9af3a289d1de09355d81eefb2cdab8cb5796`.
This shows repeatability for this profile and build, not hardware accuracy.
The 407-pixel difference recorded in the validation matrix was between
separate historical captures; those records do not retain enough detail to
attribute that difference to one cause.

A temporary build of `c13d0f0` (before separate SCY bitplane sampling) was run
with the same startup/input sequence. At frame 1800 it differed from the
current frame by 3,463 pixels, mainly canyon texture/row positions. Both
images were inspected. Artifacts are under `build/canyon-check/`, including
`scy-comparison.png` (old left/new right) and its JSON metadata. This demonstrates
the actual game's sensitivity to SCY sampling; it does not establish that
every current pixel is correct.

### Three-core raster diagnostic (2026-10-25)

LotusGB, SameBoy and mGBA completed the same 3,600-frame Canyon Racer input
profile and produced identical 8 KiB SRAM contents. SameBoy and mGBA each
produced byte-identical final frames on repeat runs. After mapping mGBA's four
output colors to the corresponding DMG shade levels, LotusGB differs from
SameBoy in 4,006 pixels and from mGBA in 5,426 pixels; SameBoy and mGBA differ
in 2,980 pixels. The broad gameplay scene is the same, but the raster output
differs across all three implementations.

The common SRAM SHA-256 is
`9df428893828811063e37b31114bbd24a4977d97cd4c72797a5bebb1a48bbc9e`. The ROM
SHA-256 is
`19f57c1fefe8ebdcef0ff9def783246a6618d4b822b5c44188a1e9c1e7448c51`; input
script SHA-256 is
`b0e199174963c1fae245255a3c1a9af3a289d1de09355d81eefb2cdab8cb5796`. The
SameBoy, mGBA and LotusGB core SHA-256 values are, respectively,
`5f3cbc610d1de2c04d41319b6678bece8c5bc78b1067436e1f6d9d440b89b56e`,
`c840f305825d07c9bec7a6ec18841073c4b539fd234f73623474f97c507f1cdc`, and
`33ea67640003f33ef447ecfe9607cda63206fd74a30af36451df0ade234afc44`.
Normalized SameBoy, mGBA and LotusGB frame SHA-256 values are
`d4978737e5cafbcb6dfc601f0f27958f687556929dc6a2c683d740ffb50a9fa6`,
`fb51bb29082af55853fb9bb071381c42e2f746a78938401666badbe82b9b2762`, and
`bc0b5fec05a88c49a82959933122dc1993a90e78678295583dfac4951dc39c22`.

Artifacts and a machine-readable report are under
`build/test-roms/canyon-racer-crosscore/`, including the side-by-side image and
per-core difference maps. mGBA requested RGB565, so this local comparison used
a temporary frontend runner that accepts 0RGB1555/RGB565 as well as XRGB8888;
its source is kept with the ignored local artifacts and was not added to the
project test tooling. mGBA's observed palette levels were
mapped as `(0,0,0) → (0,0,0)`, `(82,81,82) → (85,85,85)`,
`(173,170,173) → (170,170,170)`, and `(255,251,255) → (255,255,255)` before
counting pixels.

This is cross-implementation evidence that the runs reached comparable game
progress, not hardware ground truth. No expected screenshot from a physical
DMG is available, so keep these differences diagnostic rather than treating
them as a pass/fail accuracy score.
