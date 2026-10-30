# macOS development setup

LotusGB builds natively on macOS with Apple Clang. The project does not need
Linux, X11, or a separate GUI toolkit for its CPU and libretro-core checks.

## Tools

Install Xcode or the Xcode Command Line Tools, then confirm the compiler and
build tools are available:

```sh
xcode-select -p
clang++ --version
make --version
python3 --version
```

The current build uses C++17, GNU Make, and Python 3. Homebrew is optional; no
additional Homebrew package is needed when these commands are already present.
The Python decoder generator uses only the standard library.

## Checkout and GBIT setup

Keep the GBIT fork beside this repository, at `../gbit`, and use the revision
pinned by CI. From the LotusGB repository root:

```sh
git clone https://github.com/jiayeli-hualien/gbit.git ../gbit
git -C ../gbit checkout 40c5247b818a7c524e3c3e75fe0fde900a4ee8e9
make -C ../gbit LDFLAGS='-L. -lgbit'
```

The GBIT fork's default executable link flags contain GNU ld's `-rpath`
syntax, which Apple's linker rejects. The `LDFLAGS` override omits that
Linux-specific option; it does not change the pinned source.

Use the [standard testing workflow](testing.md) for the required build and
test sequence. Its GBIT integration check on macOS uses the dynamic loader
path from the build directory:

```sh
(cd build && DYLD_LIBRARY_PATH="../../gbit:.${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" ./gbit_test)
```

Build outputs stay under `build/`; external ROM suites remain outside the
repository as described in the validation documentation.

## Graphical testing

The repository builds a libretro core, not a standalone GUI application.
Install a macOS RetroArch frontend separately if you want to test controller,
video, and audio behavior interactively. Each macOS build produces
`build/lotusgb_libretro.dylib` alongside the `.so` used by local ROM runners.
Load the `.dylib` directly, or update the core in the default RetroArch user
directory with:

```sh
make install-retroarch-core-macos
```

The installation target builds the core first and replaces
`~/Library/Application Support/RetroArch/cores/lotusgb_libretro.dylib`.
If RetroArch uses a different core directory, pass
`RETROARCH_CORE_DIR="/path/to/cores"` to the command. Fully quit and restart
RetroArch after installing so it loads the updated library.
