# GB Pong Clone test ROM

The checked-in test copy clears the Nintendo logo bytes at `0x0104..0x0133` and recalculates the cartridge header checksums. It is intended for post-boot tests and will not pass the logo check in a physical console's boot ROM.

This ROM is from [AlexMGiroux/gb-pong-clone](https://github.com/AlexMGiroux/gb-pong-clone), a Game Boy homebrew Pong game built with GBDK.

- Redistribution: MIT; see the accompanying `LICENSE`.
- `gb-pong.gb`: ROM-only Game Boy Pong test copy, with Nintendo logo bytes cleared. Source ROM SHA-256: `9c5e337db0ee1416f2c7f1de02e7e0a78a6064cf1da49209b4bc1a36a7b35415`; checked-in test copy SHA-256: `e5a7115757020e05a3ecb253a181abe345b1286929260a5536f8985167c7aa5c`.
- Header cartridge type: `$00` (ROM-only), ROM size: 32 KiB.
- Controls: Up and Down move the player's paddle.

This is the candidate gameplay ROM for the first RetroArch acceptance check. It is stored here so local integration tests and CI can use the same input without a network download.
