# dmg-acid2 v1.0 test assets

The checked-in test copy clears the Nintendo logo bytes at `0x0104..0x0133` and recalculates the cartridge header checksums. It is intended for post-boot tests and will not pass the logo check in a physical console's boot ROM.

These assets come from Matt Currie's [dmg-acid2 v1.0 release](https://github.com/mattcurrie/dmg-acid2/releases/tag/v1.0). The ROM and reference image are distributed under the accompanying MIT license.

- `dmg-acid2.gb`: ROM-only DMG PPU visual test copy, with Nintendo logo bytes cleared. Source ROM SHA-256: `464e14b7d42e7feea0b7ede42be7071dc88913f75b9ffa444299424b63d1dff1`; checked-in test copy SHA-256: `2d12f3ecc683ad78e303f76bc9af07b7b0d2c7ea347b6baacb522e8266c112ad`.
- `reference-dmg-v1.0.png`: upstream 160×144 DMG reference image. SHA-256: `ca966d50895c7efef05838590d148c2cbfd7fba57dab986f25b35b4da71abb57`.
- `reference-shades.bin`: expected framebuffer as one DMG shade index per pixel, generated from the reference image by `generate_reference.py`. SHA-256: `f844ea760a6f1fe137f7f992c7ab1c72d34c7fcd3a807b4174a78eb04a32a458`.

Regenerate the expected framebuffer with:

```sh
python3 tests/roms/dmg-acid2/generate_reference.py
```

Run the automated comparison with `make test`. The test patches the ROM's first three bytes to jump from the current synthetic reset PC (`$0000`) to `$0100`, runs 10 frames, then compares the 160×144 shade-index framebuffer. This redirect is temporary until the system has a real cartridge startup profile.

The upstream test documentation says a scanline renderer is sufficient: it changes settings using LY=LYC interrupts between scanlines and does not test mode 3 timing penalties or writes during mode 3. See the [upstream README](https://github.com/mattcurrie/dmg-acid2/blob/master/README.md) for the behaviors covered.
