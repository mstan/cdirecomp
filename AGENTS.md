# Working on cdirecomp

- Read PRINCIPLES.md and DEBUG.md before changing instruction, device or runtime behavior.
- Track substantive work in the private central Beads database under the CD-i framework epic. Keep private intake, session notes and raw run evidence outside this repository.
- The engine owns reusable CD-i behavior. Game manifests, module seeds, title-specific validation and generated sources belong to separate game projects.
- The shared 68000 frontend is the pinned external/m68k-recomp-core submodule. Change its implementation there, and update its pin deliberately; do not recreate local forwarding copies.
- Fix source or generator inputs; never edit generated guest C.
- Use hardware specifications and author-owned source as implementation authorities. Optional emulators are black-box behavioral comparators, isolated from production targets and packages.
- Use the TCP interface for unattended game observation and input. Mute unattended tests, isolate their preferences/saves, and preserve the desktop and all owner data. Never open native save-file pickers during QA.
- Default source-only validation: cmake -S . -B build; cmake --build build --parallel 2; ctest --test-dir build --output-on-failure.
- Consumer builds supply CDI_BIOS_GENERATED_DIR and CDI_GAME_GENERATED_DIR explicitly. Keep LLE implementations functioning and qualify native replacements against their contracts.
- A production package contains the player, required redistributable dependencies, launcher assets and notices. It contains no recompiler, oracle, firmware, disc, generated guest source or development evidence.
