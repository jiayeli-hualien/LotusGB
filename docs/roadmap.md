# Development roadmap

This roadmap keeps the next work in dependency order. Prefer small changes with a focused unit test or integration check. Revisit the order when implementation reveals a real dependency; do not rewrite the CPU just to make the design look cleaner.

## Completed foundation

- CPU instruction execution advances in M-cycle steps.
- Decoder metadata is compiled into typed C++ tables from the CSV source files.
- CPU interface and instruction unit tests run through `make test`.
- GitHub Actions builds the project, runs unit tests, and checks instructions against a pinned GBIT fork.
- Regression coverage includes DAA, `ADD SP,e8` flags, EI delay, and several stack/jump memory-cycle sequences.
- Polymorphic interfaces used through owning pointers now have virtual destructors. `GBCPU` marks memory/decoder dependencies as borrowed references, and each GBIT adapter owns its dependencies and helper independently.
- A regression test creates two GBIT adapters, destroys one, and verifies that the other remains usable.

## Next work, in order

1. **Finish CPU-local STOP/HALT behavior.** Keep STOP and HALT as separate CPU states. Verify STOP's fetch/padding and wake behavior against reliable hardware references or tests, then decide whether the GBIT-only `STOP_Workaround` is still justified. Do not model interrupt-driven wake-up as complete until an interrupt request/enable interface exists. Cover observable state and bus cycles with CPU tests.
2. **Define the system clock and bus contract.** Before adding peripherals, decide how CPU M-cycles advance shared time and how reads/writes are presented to the system bus. Keep the current M-cycle model unless a tested device behavior requires T-cycle stepping. Add tests that make CPU bus accesses visible at the right cycle boundaries.
3. **Add a real address bus and cartridge boundary.** Replace flat/mock memory in the emulator path with address-region routing, then add cartridge ROM access and mapper support. Keep the GBIT memory adapter as test infrastructure rather than the system memory implementation.
4. **Add timer and interrupt components.** Connect timer state, interrupt request/enable registers, CPU IME, HALT wake-up, and interrupt entry through explicit interfaces. Test the interactions together, especially delayed EI and HALT edge cases.
5. **Add the PPU and input path.** Build a headless system loop that advances CPU, timer, and PPU on one shared clock; then add input state and a minimal frame output. Keep rendering/UI separate from emulation state.
6. **Add audio and cartridge persistence.** These can be developed after the system loop and memory map exist, with component-level tests and suitable ROM or hardware test suites.
7. **Add the libretro adapter.** Do this after a headless system can load a cartridge, advance frames, and expose input/video/audio callbacks. Keep RetroArch-specific lifecycle and callbacks outside CPU and hardware components.

## Validation rule

For each step, state what externally observable behavior changes, add a focused regression test at the narrowest useful boundary, and run the relevant unit or integration checks. Use GBIT as one CPU comparison source; investigate disagreements instead of copying behavior without hardware evidence.
