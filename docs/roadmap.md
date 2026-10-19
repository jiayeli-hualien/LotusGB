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
- CPU-local STOP and HALT states are separate; STOP consumes its second byte, ignores its value, and resumes fetching only after an explicit wake request.
- The CPU stepping interface defines one M-cycle as four T-cycles; an instruction test checks its data read and next-opcode fetch in their respective M-cycles.

## Delivery milestones

The first major delivery goal is running a Game Boy ROM in RetroArch on Ubuntu. Build the shared emulator core first; do not make a standalone GUI a prerequisite.

1. **Headless Game Boy core.** Implement the shared clock and bus, address map and cartridge loading, timer and interrupts, then a minimal PPU and input path. Acceptance: headless tests can load a test ROM, advance frames, provide input, and inspect generated video output.
2. **Ubuntu RetroArch core.** Add the libretro adapter after the headless core can run a cartridge. Acceptance: build a Linux libretro shared library, load it in RetroArch on Ubuntu, start a ROM, display frames, and receive input. Audio can follow after this first playable milestone.
3. **Audio and compatibility.** Add audio output and cartridge persistence, then improve timing and visual compatibility with focused tests and reference games. Use Game Boy F-1 Race as one PPU visual target.
4. **Steam Deck.** Build and verify the libretro core in RetroArch on SteamOS, including controls, display scaling, and audio.
5. **Raspberry Pi with Recalbox.** Build and verify the core for the supported Raspberry Pi model and Recalbox environment, including controls, video, and audio.
6. **Windows.** Add a Windows build and CI job for the libretro core, then verify it in a Windows RetroArch installation.

## Next work, in order

1. **Integrate the shared system clock and bus.** Keep CPU stepping at one M-cycle (four T-cycles) for now, and make a system scheduler the time authority for CPU and peripherals. Make memory transactions visible at defined cycle boundaries; move reset's initial opcode fetch into the same bus/timing contract. Refine CPU/bus timing at T-cycle granularity when PPU or bus behavior tests require it.
2. **Add a real address bus and cartridge boundary.** Replace flat/mock memory in the emulator path with address-region routing, then add cartridge ROM access and mapper support. Keep the GBIT memory adapter as test infrastructure rather than the system memory implementation.
3. **Add timer and interrupt components.** Connect timer state, interrupt request/enable registers, CPU IME, HALT wake-up, and interrupt entry through explicit interfaces. Test the interactions together, especially delayed EI and HALT edge cases.
4. **Add the PPU and input path.** Build a headless system loop that advances CPU, timer, and PPU on one shared clock; then add input state and a minimal frame output. Keep rendering/UI separate from emulation state. Use Game Boy F-1 Race as one visual compatibility target when a frame can be rendered.
5. **Add audio and cartridge persistence.** These can be developed after the system loop and memory map exist, with component-level tests and suitable ROM or hardware test suites.
6. **Add the libretro adapter.** Do this after a headless system can load a cartridge, advance frames, and expose input/video/audio callbacks. Keep RetroArch-specific lifecycle and callbacks outside CPU and hardware components.

## Validation rule

For each step, state what externally observable behavior changes, add a focused regression test at the narrowest useful boundary, and run the relevant unit or integration checks. Use GBIT as one CPU comparison source; investigate disagreements instead of copying behavior without hardware evidence.

## Platform support

Use Linux as the primary development and CI platform for now. Steam Deck, Raspberry Pi running Recalbox, and Windows are target platforms. Build the portable emulator core and libretro adapter first, then add platform-specific build and integration checks. Keep code and build configuration portable where practical; defer dedicated checks for each target until the core and libretro adapter are established.
