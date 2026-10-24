# Big2Small local regression

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
