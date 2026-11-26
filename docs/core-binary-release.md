# Core binary builds

`Core Binary Release` builds the LotusGB Game Boy / Game Boy Color libretro
core for macOS arm64, Linux x86-64 and arm64, Windows x86-64, and Steam Deck
(Linux x86-64). It uploads build artifacts to the GitHub Actions run. A
`core-v*` tag or a manual `workflow_dispatch` starts the workflow. The workflow
does not create a GitHub Release or publish release assets.

The regular C++ core build remains `src/libretro/Makefile`. Linux builds use
native Ubuntu runners, macOS uses an Apple Silicon runner, Windows uses a
native Windows MSYS2/MinGW runner for DLL load validation, and the Steam Deck
package uses the repository's existing Zig 0.17.0 package target.

## Existing build system inventory

- The root `Makefile` selects native development hosts for macOS arm64 and
  Linux x86-64. Its normal build delegates to `src/` and builds both LotusGB
  and LotusGBA.
- `src/libretro/Makefile` builds the LotusGB libretro shared core and matching
  `.info`; it emits `.dylib` on macOS by copying the native shared-library
  output. The release workflow reuses this target.
- `make package-retroarch-macos` builds and validates the existing Universal
  arm64 + x86-64 installer. The release workflow produces a separate arm64
  core because that is the requested release target.
- `make package-retroarch-steam-deck` already cross-compiles an x86-64 Linux
  core with Zig 0.17.0 and packages it with the `.info`, license and
  instructions. The workflow calls that target as-is.
- There were no Linux arm64 or Windows DLL release jobs. These use the same
  `src/libretro/Makefile` target with a native arm64 runner or Windows MinGW
  compiler; no core source or build-system restructuring is needed.

The workflow uses GitHub-hosted [Ubuntu arm64 and macOS arm64 runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).

## Build commands

Run these from the repository root. Build outputs should be kept outside
tracked source files (the examples use `build/release-*`).

### Native macOS arm64 and Linux x86-64 / arm64

```sh
make -C src/libretro build build_dir="$PWD/build/release-native"
```

On macOS, this creates `lotusgb_libretro.dylib`; on Linux, it creates
`lotusgb_libretro.so`. For macOS release builds, use Xcode Command Line Tools
and the workflow's arm64 / macOS 11.0 flags. Linux arm64 should be built on an
arm64 Linux host, as in the workflow.

### Windows x86-64 DLL

The GitHub workflow builds and smoke-loads the DLL on a native Windows runner
using MSYS2/MinGW. An Ubuntu development host can also cross-compile it for a
format check, but that does not replace the Windows runtime smoke test:

```sh
sudo apt-get install g++-mingw-w64-x86-64
make -C src/libretro build_dir="$PWD/build/release-windows" \
  CXX='x86_64-w64-mingw32-g++ -static -static-libgcc -static-libstdc++'
cp build/release-windows/lotusgb_libretro.so \
  build/release-windows/lotusgb_libretro.dll
```

The linker emits a Windows PE DLL; the copy gives it the expected `.dll`
extension. Validate and stage it with:

```sh
python3 tools/verify_core_binary.py --platform windows-x86_64 \
  --core build/release-windows/lotusgb_libretro.dll \
  --info build/release-windows/lotusgb_libretro.info \
  --output build/release-windows/artifact
```

For a native build on the development host, use `make build`. That target also
builds LotusGBA; it is separate from these LotusGB release artifacts.

### Steam Deck

Use the existing package target after installing Zig 0.17.0 at
`build/toolchains/zig-0.17.0/zig` (or set `STEAM_DECK_ZIG`):

```sh
make package-retroarch-steam-deck
```

The package is `build/packages/LotusGB-Steam-Deck-x86_64.zip`; it contains the
core, matching `.info`, instructions, license and manifest. This target builds
the Linux x86-64 ABI used by Steam Deck and does not require a Deck to build.

## Artifact contents and acceptance checks

Each platform job loads the just-built core through the libretro API, runs the
MIT-licensed GB Pong ROM for 120 frames, and requires one valid 160×144 video
callback for each frame. It then checks the binary format/architecture and
that `.info` lists `gb|gbc`. The four platform-specific workflow artifacts
contain the native core, the
matching `.info`, `LICENSE`, `THIRD_PARTY_NOTICES.txt`, `manifest.json` and
`SHA256SUMS`. The Steam Deck artifact is the existing ZIP package and its
SHA-256 sidecar. The workflow
records SHA-256 values. In addition, the existing `CI` workflow runs
`make test` on PRs. These checks establish build and basic dynamic-load/frame
delivery behavior; they do not establish broad game compatibility or full
device acceptance.

Local acceptance on 2026-11-26: the macOS arm64 core built with the workflow
flags, passed `tools/verify_core_binary.py`, and loaded GB Pong for 120 frames
through `tools/smoke_libretro_core.py`; `file` identified it as a Mach-O arm64
dynamic library. Its SHA-256 was
`c9fa677dc839e580ff770da3296ff614ca934fbabc9d2573292ef25e6b29fe04`. The
full local `make build` and `make test` also passed on macOS arm64, including
the GB/CGB, libretro, GBA, netpacket probe, Mooneye protocol, homebrew checker
and PPU frame runner checks.

GitHub Actions acceptance on 2026-11-26: [PR #1's Core Binary Release run](https://github.com/jiayeli-hualien/LotusGB/actions/runs/45735338534)
passed all five platform jobs. Each loaded the target core and ran licensed
GB Pong for 120 frames; the Windows job ran on native Windows, and Steam Deck
ABI smoke ran on Ubuntu x86-64. All five artifacts were downloaded and
checked for expected files, formats, manifests, notices and SHA-256 values.
The [existing PR CI run](https://github.com/jiayeli-hualien/LotusGB/actions/runs/45735338407)
also passed, including `make test` and GBIT instruction comparisons.
Build and smoke success do not establish broad compatibility or prove loading
in RetroArch's GUI. The Steam Deck manifest explicitly keeps device
verification pending; its Ubuntu smoke test does not replace loading the
artifact on an actual Deck. The existing native and Steam Deck runtime
evidence is tracked separately in [Steam Deck checks](steam-deck-check.md) and
[macOS packaging](macos-package.md).

The macOS artifact is an arm64 development binary. It is not Developer ID
signed or notarized. The Windows binary is a MinGW-built x86-64 DLL, and this
workflow does not run Windows RetroArch. Linux arm64 depends on GitHub's
arm64-hosted Ubuntu runner. The Steam Deck target remains x86-64; ARM Linux
receives a separate native artifact. The workflow only uploads Actions
artifacts and never creates a formal GitHub Release.
