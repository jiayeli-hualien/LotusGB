# DMG compatibility on CGB hardware

LotusGB now supports boot-selected DMG compatibility on its CGB-E hardware
model. Choose actual DMG hardware for four-shade grayscale rendering, or CGB
compatibility to run a monochrome cartridge with Game Boy Color hardware
behavior. Compatibility rendering can use color palettes selected by a CGB
boot ROM; without one, LotusGB's synthetic startup uses a neutral grayscale
palette. Original fixtures verify mode locking, restricted registers, palette
remapping, object priority and exact state/video/audio replay. This is an
automated specification checkpoint; authentic BIOS revisions, title-specific
boot colorization and real-device timing are not validated by these fixtures.

## Select hardware

The libretro **Hardware model (reload content)** option (`lotusgb_model`) has
three values:

| Value | Behavior on the next content load |
| --- | --- |
| `auto` (default) | CGB hardware for header `$0143` bit 7; actual DMG hardware otherwise. |
| `dmg` | Actual DMG hardware, including for dual-mode cartridges. |
| `cgb` | CGB hardware; monochrome cartridges use compatibility startup. |

In RetroArch, open the loaded game's **Quick Menu → Core Options → Hardware
model (reload content)**. Choose `dmg` for actual DMG hardware and its four
grayscale shades, or `cgb` to run a monochrome `.gb` cartridge on CGB hardware
in DMG compatibility mode. Then unload and reload the content so the option
takes effect. With `auto`, a monochrome `.gb` selects DMG; a cartridge whose
header marks CGB support selects CGB hardware.

Reset keeps the hardware selected when the content was loaded, even if the
option has changed. Reload content to change hardware. Frontends without core
variables retain `auto`. Invalid option values fail explicitly.
The core API also accepts a monochrome cartridge with
`GameBoySystem(rom, HardwareModel::CgbE, ..., bootRom)`;
`isDmgCompatibilityMode()` reports execution mode independently of physical
`getHardwareModel()`.

On CGB hardware the loader uses optional `cgb_boot.bin` / `gbc_bios.bin`, even
for monochrome cartridges; see the [BIOS contract](cgb-development.md#optional-cgb-bios-through-libretro-2026-11-02).
The boot program can choose its own mode and palettes. Without a BIOS, synthetic
startup selects compatibility for a monochrome header and initializes neutral
grayscale BG/OBJ palettes, so choosing `cgb` alone does not colorize the game.
This fast path does not implement the official BIOS's title checksum tables or
button-selected palettes.

## Implemented behavior

[Pan Docs KEY0/OPRI](https://gbdev.io/pandocs/CGB_Registers.html#ff4c--key0sys-cgb-mode-only-cpu-mode-select)
and the [color boot sequence](https://gbdev.io/pandocs/Power_Up_Sequence.html#behavior)
are the specification references. Boot code can stage KEY0 bit 2 and OPRI bit 0
while the overlay is mapped. A nonzero FF50 write unmaps the BIOS, applies the
selected mode/priority and locks those selections until reset. Native palette
ports remain available before FF50, including after KEY0 is staged. A game
cannot unlock the selections by rewriting KEY0/OPRI or FF50.

Compatibility blocks native VBK/SVBK, speed-switch, VRAM-DMA, palette and PCM
ports, RP and FF74; reads return FF and writes have no effect. FF72/73/75 retain
their CGB storage/masks. CPU VRAM uses bank 0 and switchable WRAM uses bank 1.
SC's fast-clock selector is unavailable. Physical CGB APU behavior and DMA bus
wiring remain CGB behavior; the mode does not acquire DMG STAT-write interrupts
or DMG OAM corruption. It retains the DMG Window Y condition when LCDC.5 is
cleared; native CGB resets that condition and waits for another WY match.

Rendering uses monochrome tile/OAM interpretation and LCDC background/window
enable. BGP/OBP0/OBP1 remap digital indices into the boot's RGB555 BG palette 0
and OBJ palettes 0/1. OPRI selects X-coordinate versus OAM-index object conflicts.
Native tile attributes and object bank/palette bits do not select native color
features in compatibility mode. The frontend still converts CGB RGB555 colors
in both XRGB8888 and 0RGB1555 formats.

CGB snapshot v8 introduced staged KEY0/OPRI values, the CGB IR receiver's pending
settling time and its last sampled sensor level; current CGB v13 also stores MBC7
cartridge protocol and rumble control state. The boot overlay's existing mapped flag supplies lock state.
Mode, serial feature gating, memory-bank views and renderer wiring are rebound
after atomic restore. Host IR callbacks remain on the live system. Current DMG
snapshots use v10. Older CGB v2–v11 states migrate to their historical mode; see the
[format policy](save-states.md).

## Automated evidence and limits

`make test` includes `cgb_compatibility_test`. Its original cartridge/bootstrap
checks native access before FF50, attempted unlocks after FF50, retained palettes,
fixed compatibility banks, register masks and boot selection independent of the
cartridge header. Snapshots before and after locking replay complete state;
the locked checkpoint includes an ongoing serial transfer.
Renderer checks cover BGP/window mapping, both object palettes, object conflicts,
LCDC disable and physical CGB STAT/OAM behavior. Existing exact cgb-acid2 and
native-CGB regressions remain separate controls.

Libretro tests exercise hardware selection, unsupported-variable fallback,
invalid values, optional compatibility BIOS loading, reset retaining selection,
both pixel formats and exact rewind replay in both presentation builds. All
committed ROM code is original test code; no commercial artifacts or BIOS bytes
are needed.

Exact KEY0/OPRI transition pixels, PGB/NMI behavior, unusual boot-time speed/DMA
combinations, official BIOS palette selection and physical revision timing
remain unverified. The synthetic native-only `createCgbBootProbe` remains a
separate LCD-off, nonserializable mapping test factory.
