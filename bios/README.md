# CD-i system ROM inputs

Supply your own player ROM. Firmware is never committed or downloaded automatically.

`cdrtos_discovered.txt` is the platform's observed in-ROM entry catalog. Pass it explicitly to `CdiRecompBios --seeds`; it contains addresses, not guest code.

Generated BIOS C and its byte-identity header belong to an ignored build output or a game's generated directory. See [build instructions](../docs/BUILDING.md). A generated player accepts only the BIOS bytes used for its compilation.

The currently qualified Hotel Mario consumer identifies its supported player ROM in its own DISC.md. Other player/board combinations require their own validation.
