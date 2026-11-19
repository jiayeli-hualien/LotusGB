# Development notes

## Project scope

LotusGB is a C++ DMG/CGB emulator with a RetroArch libretro core. The repository also contains LotusGBA, a separate experimental Game Boy Advance core with its own CPU, system and libretro adapter. The project prioritizes useful game ROM compatibility and a good play experience on modern hosts and networks, while leaving the game ROM itself untouched whenever possible. Hardware behavior remains an important reference and should be tested through public component, system and frontend interfaces, but exact hardware fidelity is not an absolute requirement when a documented emulator-side compatibility behavior better serves supported games. Any intentional divergence must be explained in a nearby code comment and in the relevant design/evidence document, with tests for its guest-visible behavior. Do not conflate focused tests or bounded game checkpoints with broad game compatibility; use the roadmap and per-system evidence documents for current scope.

## Build and checks

From the repository root:

- `make build` builds the project.
- `make test` runs the CPU, hardware-component, system, libretro, framebuffer, homebrew and GBA tests. It does not require downloaded ROM suites; the pinned GBIT comparison is a separate integration check.
- `make test-roms` optionally runs available Blargg CH3 ROMs from
  `../test-roms/dmg_sound` through the built libretro core. It writes adapted
  copies, screenshots, logs, and a report under `build/test-roms/`; check the
  screenshots manually for the on-screen result. This local workflow is not
  part of GitHub CI.
- `make test-mooneye MOONEYE_SUITE_DIR=/path/to/extracted-suite` optionally
  runs selected Mooneye PPU and interrupt timing ROMs. ROMs stay outside the
  repository; logs and a report go under `build/test-roms/mooneye/`. This local
  workflow is not part of GitHub CI.
- The GBIT integration executable expects the `gbit` repository beside this repository. Build it with `make -C ../gbit`, then run the test executable from `build/`:

  ```sh
  cd build
  LD_LIBRARY_PATH="../../gbit:.${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ./gbit_test
  ```

GitHub Actions builds the pinned `jiayeli-hualien/gbit` revision, builds LotusGB, runs `make test`, and then runs the GBIT instruction tests. Keep local integration instructions consistent with `.github/workflows/ci.yml`.

## ROM test policy

- Tests committed to the repository or run in GitHub CI must use ROMs with clear terms that allow the intended testing and redistribution. Check the ROM binary's terms separately from its source code, tools, and assets.
- Use clearly licensed homebrew ROMs for reproducible game-level repository and CI checks.
- Validate commercial-game playability privately and manually. Keep commercial ROMs, saves, screenshots, audio captures, and other game-derived artifacts out of the repository and CI. Public documentation may record concise results without including those files or captures.

## Error handling

- Do not silently swallow unexpected errors or report success after an operation fails. Propagate the failure, report it through the relevant interface/logging boundary, or make the test fail.
- If a failure is intentionally ignored (for example, an optional frontend capability is unavailable), document why and test the intended fallback. Do not turn exceptions or failed operations into success-looking default values.
- At C and libretro callback boundaries, catch exceptions before they cross the ABI, log useful context, and set an explicit failure state when execution cannot continue.

## Documentation

- Prefer an answer-first structure based on the Minto Pyramid Principle: state
  the current conclusion, group the main points, then give supporting evidence
  and details. Adapt the structure to the document's purpose; keep procedures
  step-by-step and historical records chronological. See
  [`docs/documentation-guide.md`](docs/documentation-guide.md).
- Keep status claims tied to evidence. Distinguish implemented behavior from
  behavior covered by tests, game compatibility, and hardware verification.
  Prefer linking to one canonical status description over copying it into
  multiple documents.

## Decoder tables

`src/lib/include/instruction_table.csv` and `src/lib/include/decode_table.csv` are the editable opcode metadata. `tools/generate_decode_tables.py` converts them into `src/lib/include/generated_decode_tables.hpp`; runtime code uses the generated C++ table and does not parse CSV files. Do not edit the generated header by hand. When changing the CSV metadata or generator, regenerate the header with:

```sh
python3 tools/generate_decode_tables.py
```

Commit the generated header together with the CSV or generator change so builds use the matching decoder data.

## CPU changes

- Add focused unit tests in `src/unit_test/` for externally observable CPU behavior, such as registers, flags, cycle count, memory access, and interrupt state.
- Use hardware documentation and targeted tests to understand Game Boy behavior. GBIT is a useful integration check. When evidence shows a difference is needed for ROM compatibility or playability, a deliberate compatibility behavior is acceptable; explain the guest-visible difference and preserve it with a focused regression test.
- Model CPU progress in M-cycle steps unless a demonstrated observable timing requirement needs finer clock detail.
- Preserve the distinction between STOP and HALT state and test their wake-up behavior when changing those paths.
