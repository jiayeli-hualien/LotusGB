# cgb-acid2 v1.1 assets

Matt Currie's [v1.1 release](https://github.com/mattcurrie/cgb-acid2/releases/tag/v1.1)
provides the source ROM and reference under the upstream MIT license. The
checked-in test copy clears the Nintendo logo bytes at `0x0104..0x0133` and
recalculates the cartridge header checksums. It is intended for the post-boot
test setup below; it will not pass the logo check in a physical console's boot
ROM. The accompanying `LICENSE` preserves the upstream notice.

| File | SHA-256 |
| --- | --- |
| `cgb-acid2.gbc` (32 KiB test copy with logo bytes cleared) | `479d79e320099dd7819a53c24d44fe3cfe56c8b3e41f189ceed26f2717204298` |
| `reference-cgb-v1.1.png` (upstream `img/reference.png` at v1.1) | `9ea9c262c5383353e77d715d021a0f7c5ccbe438f88082cb225756e50c4fdf01` |
| `reference-rgb555.bin` (little-endian RGB555, 160×144 pixels) | `c587a0e67f4a9e7ceccfc3b1c1991510a6476bd6b4a8b2f109f83e94f97116cb` |

Run `python3 tests/roms/cgb-acid2/generate_reference.py` to regenerate the
reference with Python's standard library. It validates the PNG shape, filters
and chunk CRCs, then converts the official indexed colors to RGB555.

The source release ROM SHA-256 is
`197fb0bcec544f0400527fc707e0a94f55435974986e6986b424ace5de81720e`.

`make test` compares all 23,040 pixels through the CGB system and both libretro
output formats. See [the upstream README](https://github.com/mattcurrie/cgb-acid2/tree/v1.1)
for scope: this is a graphics test; it does not require WRAM banking, double
speed or accurate Mode 3 timing. Separate original fixtures cover those
implemented memory/speed/DMA contracts.
