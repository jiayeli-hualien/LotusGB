# Raspberry Pi / Recalbox validation

LotusGB builds and runs as an AArch64 libretro core on the tested Recalbox
system. A headless run confirms core loading, frame and audio callbacks, and
scripted input. Actual RetroArch video, sound, controls, and save handling on
the device still need an interactive check; this is not yet a completed
Recalbox release target.

## Tested system and result (2026-10-26)

- Recalbox 10.1.1 on AArch64, kernel `6.12.25-v8`, RetroArch 1.22.2.
- Recalbox uses glibc 2.39 and provides `libstdc++.so.6`. The cross-built core
  requires at most `GLIBC_2.32` and `GLIBCXX_3.4.29`.
- The included MIT-licensed GB Pong ROM completed 120 frames twice in one
  process. The runner checked 120 video callbacks in each repetition and
  reported 96,437 stereo audio frames from the final repetition.
- A second 120-frame run held Up over frames 60–119. Its final RGB framebuffer
  differed from neutral input. The neutral and Up framebuffer SHA-256 values
  each matched a run of source revision `7245297` on Ubuntu x86-64:

  | Input | Final framebuffer SHA-256 |
  | --- | --- |
  | Neutral | `5f0cf7bc3d4987633b33b6aab129b4e90128ac69eea7939d5fef90db09a6f3fd` |
  | Up, frames 60–119 | `4f31d2049eed9711be73aa232f4f7104701e4b97ee4c0d6ac7fab64db604827d` |

These results cover the headless libretro callbacks for this ROM and input
script. The audio count does not establish audible quality. The matching
framebuffer hashes establish cross-architecture reproducibility for these
particular runs, not hardware accuracy or general game compatibility.

## Install for manual RetroArch loading

The tested device's root filesystem is read-only, so `/usr/lib/libretro/` is
not a writable install location. Recalbox's EmulationStation config generator
also constructs core paths under `/usr/lib/libretro/` and appends
`_libretro.so`; adding a file to the share does not register it as the GB
system's selected core.

For manual selection in RetroArch, the core and its metadata are installed in
the writable user core directory:

```text
/recalbox/share/system/.config/retroarch/cores/lotusgb_libretro.so
/recalbox/share/system/.config/retroarch/cores/lotusgb_libretro.info
```

The device's `retroarchcustom.cfg` now sets both `libretro_directory` and
`libretro_info_path` to that directory. Its original contents were saved to
`/recalbox/share/lotusgb-test/retroarchcustom.cfg.before-lotusgb`. In the
RetroArch menu, load the LotusGB core, then load
`/recalbox/share/lotusgb-test/gb-pong.gb`. This manual-load setup has not yet
been checked through the physical display and controller.

To undo the user-core install, restore the saved config and remove the two
installed files:

```sh
cp /recalbox/share/lotusgb-test/retroarchcustom.cfg.before-lotusgb \
   /recalbox/share/system/configs/retroarch/retroarchcustom.cfg
rm /recalbox/share/system/.config/retroarch/cores/lotusgb_libretro.so \
   /recalbox/share/system/.config/retroarch/cores/lotusgb_libretro.info
```

A separate RetroArch process launched over SSH loaded the LotusGB core and
ROM, then failed while switching the KMS display mode because EmulationStation
was still using the display. Launch through Recalbox's normal frontend path
before judging actual video or audio; do not start a second RetroArch process
beside EmulationStation.

## Reproduce the headless check

Use an AArch64 Linux GNU C++17 cross compiler. On a developer machine where
`aarch64-linux-gnu-g++` is available, build only the libretro targets; the
top-level `make build` also builds host-side GBIT tools.

```sh
make -C src/libretro build rom-test-runner \
  build_dir="$PWD/build/recalbox-aarch64" CXX=aarch64-linux-gnu-g++
file build/recalbox-aarch64/lotusgb_libretro.so
```

Copy the ARM64 core, its `.info` file, runner and included GB Pong ROM to a
separate test directory in Recalbox's writable share. The share is exFAT on
the tested device and mounts transferred files without execute permission.
Copy the runner and core into executable `/tmp` before using them:

```sh
ssh root@recalbox 'mkdir -p /recalbox/share/lotusgb-test'
scp build/recalbox-aarch64/lotusgb_libretro.so \
    build/recalbox-aarch64/lotusgb_libretro.info \
    build/recalbox-aarch64/libretro_rom_runner \
    tests/roms/gb-pong/gb-pong.gb \
    root@recalbox:/recalbox/share/lotusgb-test/
ssh root@recalbox '
  mkdir -p /tmp/lotusgb-test
  cp /recalbox/share/lotusgb-test/* /tmp/lotusgb-test/
  chmod 755 /tmp/lotusgb-test/libretro_rom_runner \
            /tmp/lotusgb-test/lotusgb_libretro.so
  cd /tmp/lotusgb-test
  ./libretro_rom_runner ./lotusgb_libretro.so ./gb-pong.gb 120 \
    ./pong.rgb ./pong.srm --repeat 2
  printf "60 120 UP\n" > up.txt
  ./libretro_rom_runner ./lotusgb_libretro.so ./gb-pong.gb 120 \
    ./pong-up.rgb ./pong-up.srm ./up.txt
  sha256sum ./pong.rgb ./pong-up.rgb
'
```

`/tmp` is cleared by a device reboot. The test directory under
`/recalbox/share` remains until removed. This procedure does not register a
core in EmulationStation or modify the system's installed core files.

## Next acceptance step

Run LotusGB through the device's actual RetroArch frontend with GB Pong.
Check visible rendering and button response, listen for continuous audio and
startup noise, and verify that exiting and relaunching content works. Then
check an appropriately licensed battery-save game through the frontend.
Record the Recalbox version, core build revision, and observed results before
marking the platform target complete.
