# Debugging contract

Use the native TCP server and always-on rings; see TCP.md. Optional external emulators are black-box behavioral comparators. Hardware specifications, the original guest binary and author-owned source are the implementation authorities.

1. Identify the actual engine/core/game commits, generated inputs, configuration and executable.
2. Check native dispatch misses before investigating downstream behavior.
3. Align native and reference runs by a guest event, instruction sequence or completed field rather than host wall time.
4. Compare state and walk backwards to the first divergence. Trace the writer or executed edge that introduced it.
5. Classify the defect: shared instruction semantics, discovery/emission, runtime/timing, memory/OS state, device behavior or game metadata.
6. Fix the source or generator input, regenerate and exercise the affected behavior. Never patch generated guest C.

Relevant CD-i boundaries include exception/TRAP return PCs, OS-9 module relocation, MCD212 control/scanout, CIAP sectors/XA and IKAT input. Record expected/actual state and the first differing instruction/event in private issue evidence.

Use isolated disposable profiles and saves in automated work; mute speaker output while retaining audio emulation. Observe/input through TCP without moving or activating desktop windows. Never open native save-file dialogs during QA. Native saves and settings must survive cleanup; older save-state compatibility is not required.
