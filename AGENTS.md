# Development notes

## Project scope

LotusGB is a C++ Game Boy emulator project. The current implementation focuses on the CPU; it is not yet a complete system emulator. Keep CPU behavior testable through its public interfaces so memory, timing, and future system components can be integrated later.

## Build and checks

From the repository root:

- `make build` builds the project.
- `make test` runs the CPU interface and instruction unit tests. These tests do not require GBIT.
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
- Treat hardware documentation and targeted tests as the source of truth for Game Boy behavior. GBIT is a useful integration check, but investigate disagreements instead of adding compatibility workarounds without evidence.
- Model CPU progress in M-cycle steps unless a demonstrated observable timing requirement needs finer clock detail.
- Preserve the distinction between STOP and HALT state and test their wake-up behavior when changing those paths.
