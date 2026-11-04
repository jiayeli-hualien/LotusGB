# GBA Thumb instruction test ROM

This is the Thumb instruction test cartridge from [jsmolka/gba-tests](https://github.com/jsmolka/gba-tests), distributed under the accompanying MIT license.

The upstream binary contains the Nintendo logo in its cartridge header, but the upstream license does not separately license that asset. For redistribution, this copy has bytes 0x04 through 0x9F cleared, removing the logo while preserving the ARM entry branch and test program. LotusGBA starts at the cartridge entry point through its HLE startup, so it does not require the logo or a proprietary BIOS.

- Upstream file: thumb/thumb.gba
- Upstream binary SHA-256: b5cb2291df4ab314b31c598acd9bff2ccfa0b38efff29daadfe97422ce369b67
- Distributed binary SHA-256: fad5a8b4070a302a56254b960dbe6d8a6dcf469d8e627ecc7eff50317b52bf2e
- Size: 3,680 bytes
- License: MIT; copyright 2019 Julian Smolka

Run it through the libretro core test with:

    make -C src/gba_libretro build_dir=../../build test

The suite covers Thumb logical operations, shifts, arithmetic, branches and memory transfers. It displays “All tests passed” when its failure counter remains zero. The automated check verifies the first glyph in that success message after five frames.
