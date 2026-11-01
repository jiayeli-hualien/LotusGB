# macOS RetroArch core installer

`make package-retroarch-macos` produces
`build/packages/LotusGB-RetroArch-macOS-universal.zip`: a LotusGB core installer
for an **existing RetroArch installation**. It includes the Universal
`arm64`/`x86_64` core, matching `.info`, a double-clickable installer,
checksums, build identity and license notices. It includes no RetroArch.app,
games or BIOS.

## Install

1. Install/open macOS RetroArch at least once, then quit it.
2. Extract the entire ZIP, retaining the `core` folder and other files together.
3. Double-click **Install LotusGB.command**. Alternatively, run
   `bash "Install LotusGB.command"` from the extracted folder in Terminal.
   Installing needs neither administrator privileges nor Python/Xcode.
4. Reopen RetroArch and choose LotusGB to load `.gb` or `.gbc` content.
   Native CGB uses fast startup without a BIOS.

The installer reads `libretro_directory` and `libretro_info_path` from
`~/Library/Application Support/RetroArch/config/retroarch.cfg`. With no saved
config it uses the `cores` / `info` folders under the existing RetroArch data
directory. Absolute paths and `~/` paths are supported; values are never
executed as shell code. Custom locations can be passed explicitly:

```sh
bash "Install LotusGB.command" --data-dir "/path/to/RetroArch"
bash "Install LotusGB.command" --config "/path/to/retroarch.cfg"
bash "Install LotusGB.command" --core-dir "/path/to/cores" --info-dir "/path/to/info"
```

Existing LotusGB core/info files are backed up under the RetroArch data
folder's `backups/LotusGB/<UTC timestamp>-<process id>/`; the printed backup
path contains a `paths.txt` destination record. Staged replacements are checked
against the payload. A failed replacement restores any file already replaced
and reports failure. Settings, saves and other cores are preserved.
`core_info.refresh` requests a cache rebuild at the next frontend startup,
so an older `.gb`-only cache does not hide GBC content support.

For manual rollback, quit RetroArch and copy the old `.dylib` / `.info` from
the chosen backup into the corresponding directories in `paths.txt`, then
create an empty `core_info.refresh` file in that info directory and reopen
RetroArch. A first installation has no old core to restore.

## Build and validation scope

The packaging target builds a separate core under
`build/macos-retroarch-universal/`, leaving the ordinary development core
and the installed user core untouched. Both slices target macOS 11.0. The
packager verifies their Mach-O targets, system-only dynamic dependencies and
`gb|gbc` API/info agreement. It rewrites the packaged copy's library ID to
`@rpath/lotusgb_libretro.dylib`, signs that copy ad hoc, verifies both slices,
and writes the ZIP plus a `.zip.sha256` checksum.

This development bundle is **not Developer ID signed or notarized**.
Checksums provide integrity checking, not Apple certification. The deployment
target is a build property; runtime checks below were on macOS 27.0.1, not
an older macOS 11 host.

The 2026-11-01 package checkpoint checked:

- Installer custom paths (including spaces, literal shell characters and `~/`),
  previous-file backups, repeat installation and preserved settings/saves.
- Missing-config default paths, rejection of a corrupted payload before changes,
  and rollback when the second file replacement fails.
- Universal libretro regressions on the `arm64` slice and the `x86_64` slice
  under Rosetta: DMG/CGB color, input, reset, audio, state and rewind checks.
- Loading the actual relocated/signed package core with each runner slice:
  `cgb-acid2` matched all 23,040 official reference pixels in both cases.

Local evidence lives under `build/test-roms/macos-package/`, including
`package-and-installer-tests.log`, architecture-specific core logs and
`report.json`. Emulator capability/limits remain in the [CGB guide](cgb-development.md).
Packaging-only changes do not alter the CPU or emulator implementation.

The original development artifact remains `build/lotusgb_libretro.dylib`.
That artifact uses the developer's current host architecture/deployment
settings; use the Universal ZIP when moving the core to another Mac.
