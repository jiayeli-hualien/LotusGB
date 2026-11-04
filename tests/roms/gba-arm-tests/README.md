# GBA ARM instruction test ROM

This is the ARM instruction test cartridge from [jsmolka/gba-tests](https://github.com/jsmolka/gba-tests), distributed under the accompanying MIT license.

The upstream binary contains the Nintendo logo in its cartridge header, but the upstream license does not separately license that asset. For redistribution, this copy has bytes 0x04 through 0x9F cleared, removing the logo while preserving the ARM entry branch and test program. LotusGBA begins at the cartridge entry point through its HLE startup, so it does not require the logo or a proprietary BIOS.

- Upstream file: arm/arm.gba
- Upstream binary SHA-256: 77ee88662552bdc885c1080c0172ff119d54db791bd73b21808cf1ff1fe5b40e
- Distributed binary SHA-256: 2da9a8c9f97e3bfa3422295c15e1f87245e520c0caf9621f8cd7d4628d16fe30
- Size: 8,824 bytes
- License: MIT; copyright 2019 Julian Smolka

Run it through the libretro core test with:

    make -C src/gba_libretro build_dir=../../build test

The test ROM runs its ARM condition, data-processing, multiplication, memory-transfer, swap and block-transfer tests. It writes “All tests passed” when its failure counter remains zero. The automated check verifies the first glyph in that success message after five frames.
