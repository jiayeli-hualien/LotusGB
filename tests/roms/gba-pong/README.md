# Pong-Homebrew-GBA test ROM

This is the Pong homebrew game by ZeroDayArcade, copied from the project's
MIT-licensed repository at
<https://github.com/ZeroDayArcade/Pong-Homebrew-GBA>, revision
`c784b6036a4f188c50932b411e98126bfcbd07d6`. The upstream project describes
this Mode 3 game as an example for input, graphics, collisions, scoring and
emulator testing. Its source, generated graphics and distributed ROM are
covered by the included MIT license.

The upstream cartridge header contains Nintendo's logo, which is not covered
by the project's license. This repository copy clears bytes `0x04..0x9F` and
preserves the entry branch and game code. LotusGBA starts through HLE, so the
header logo is not needed by this test.

- Upstream file: `Pong-Homebrew-GBA.gba`
- Upstream Git blob: `ae5102831471bf61baef7ae51d597877879612b1`
- Upstream SHA-256: `3c850319ddef6d237756dd64b85ddf3b75bf7c096feb2408ed1b2dd0a05a0158`
- Distributed SHA-256: `05bde674b6c16ebae85a1657ce9f9f7e80b28e737dc601a635150b9b7d25020c`
- Size: 8,884 bytes
- License: MIT; copyright 2023 zerodayarcade.com

`make test` runs the game through LotusGBA's libretro callbacks. It checks the
menu, starts a match, confirms ball movement, checks Up and Down paddle input,
verifies that the paddle returns the ball, and checks the first score.
