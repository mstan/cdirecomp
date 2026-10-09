# Hotel Mario engineering status

Updated 2026-10-09. Work is tracked in central Beads under the framework epic
`beads-ttbl` and game epic `beads-ssy9`.

## Owner-requested checkpoint

Implementation and testing stopped on 2026-10-09 at the owner's request.
All Hotel Mario probes and controllers are stopped. The remaining production
player was closed normally after releasing input and recording its state, so
the original battery file could be flushed. The owner subsequently authorized
extensive notes, README updates and default-branch integration only.
Gameplay implementation and campaign probes remain paused. Issues remain open.

## Owner playtest after integration

On 2026-10-09 the owner requested a manual launch and reported that basic
gameplay works well and the title-screen background now works. Minor visual
flickering remains visible and is tracked as `beads-ssy9.1`, an open game bug.
Its scene, frequency, affected layer, impact and cause have not been
characterized; no investigation or fix was performed during this closeout.
This is owner feedback from early manual play, not full-campaign certification.

The launched indexed LLE executable is the recorded `build/playable-lle`
variant, at speed 1 with DirectSound and the original BIOS/USA CUE. Launch
metadata and stdout/stderr are retained in the product's ignored
`build/playable-lle/manual-play-20261009-095749/` directory. The session is
left under the owner's control; no controller or automated game probe was
resumed. Documentation and Beads were updated for the final session closeout.

## Automated checkpoint results

The latest default LLE build passed 30/30 headless cold launches. Its two
windowed batches remain failed: launch 4 in the first batch and launch 15 in
the second exited with code 0 before their required capture. The DirectSound
attract run lost its connection after the last recorded field 68067 and failed
its evidence gate; three complete circuits are not certified. Earlier 30/30
windowed success belongs to the preceding executable.

Normal-speed one-player input on the indexed default build reached Hotel 1
Stage 4 at field 51281, clearing the first three stages. The separate seeded
two-player build reached Stage 3 for Mario and Stage 5 for Luigi. Its final
checkpoint at field 145679 had zero guest resets and dispatch misses. These
are partial campaign results. All hotels, bosses, ending, correct restored
stage, sustained audio and performance acceptance remain unverified.

All six compiler and eleven runtime component checks passed before stopping.
The final controller helper edits record source identity/device history,
discard stale observations, and implement the original game's midair brake
semantics. They are checkpointed development tooling, not campaign or input
quality certification. No HLE or guest progress writes were introduced.

Framework integration is on `hotel-mario-lle`, including `10859e5`,
`ae8485d` and `2bdd5c5`; the core is pinned to `ddfa4e1`. Product commit
`30d5081` pins the indexed coverage implementation `ae8485d`. The local
preview archive predates that coverage fix and is not a final release.
Evidence remains in ignored `build/tmp/` directories, including
`indexed-lle-headless-matrix-20261009`, both indexed windowed matrix batches,
`indexed-lle-windowed-attract-20261009`, `indexed-lle-oneplayer-20261009`, and
`normal-production-save-20261009/user-stop-checkpoint.json`.

## Corrected bring-up failures

- **Intro backgrounds (`beads-bcha`).** Scene markers on file 1/channel 0
  were discarded while the intro selected channel 15. CIAP now delivers
  file-wide trigger/EOF status separately from channel-selected payloads.
  The real ROM maps these events into record flags and the game flips its
  background pages. All eight intro page changes were observed; the mushroom
  talk scene also renders in a normal-speed windowed run. The previous
  MCD212 displayed-flag hypothesis was disproved by its recorded writers.
- **Buffered audio/stage entry (`beads-6v0p`).** Ordinary ADPCM buffer
  consumption incorrectly set the interrupt that makes the ROM acknowledge
  and re-probe an already-consumed pair. It now signals the normal PCL refill
  interrupt. Memory playback consumes alternating buffers for their actual
  decoded duration, and FINISH waits for them to drain. Direct selected XA
  audio also provides the header notification used by the real driver to
  update `SS.Pos`; its payload follows the audio path.
- **Transport lifecycle (`beads-6v0p`).** Decoder RESET cancels the decoder
  arm, so a delayed IKAT resume cannot restart a completed operation.
  Firmware download exposes its initial idle state independently. Locator
  status no longer competes with the ROM's DATA buffer picker.
- **Windowed scripted input.** SDL polling and developer input now have
  separate producer states. Idle keyboard polling previously cleared a
  button before IKAT's timed sample. Both producers still use the same
  low-level pointer/interrupt path.
- **First stage data record.** Selection now notifies the host from the next
  physical sector header, including the filtered boundary sector. The ROM's
  one-buffer discard callback had consumed the first selected payload and
  left a 24-sector stage load at 23 sectors. No fabricated selection event is
  used. Regular filtered headers also keep the real driver's `SS.Pos` moving
  between selected audio sectors.
- **Death/restart audio.** Decoder RESET previously cancelled the separate
  audio processor, and plain APCR PLAY0 (`$40`) did nothing. The real ROM's
  sound-stop wait then timed out and audio CIL reuse raised `$F4`. Decoder
  RESET now preserves AP playback and notifications; AP reset is separate,
  PLAY0 resumes playback, and its word pointer tracks consumed samples.
  Regression tests exercise reset during playback and sound replacement.

The CIAP firmware model still contains behavior inferred from original ROM
traffic, especially buffer ownership, memory-play gating and physical sector
progress while host buffers are full. These are tested implementations, not
claims of complete silicon/microcode accuracy. Broader applications must
continue to exercise them.

## Static native modules (`beads-z8yh`)

The disc frontend inventories bounded filesystem files, validates OS-9
header parity and CRC, and emits position-independent C for 174 distinct
Hotel Mario executable images. Program and explicitly selected private Subr
export layouts provide code seeds. The original OS-9 loader remains in charge.

Native bindings require an exact full-image SHA-256. Every overlapping CPU
or DMA RAM write revokes the binding; per-instruction epoch checks prevent
an abandoned call from continuing into an unloaded/replaced image. Exact
instruction resume maps cover interrupts and OS-9 trap continuations.
Uncovered code stays on the clean-room interpreter floor. Recorded module
targets retain image identity and can be promoted by offline regeneration.
Coverage now uses a hash index and 262144 cumulative image/offset records.
The previous 16384-record limit lost evidence during sustained attract runs,
and a linear lookup put growing diagnostic work on the interpreter path.
First-seen load provenance and stable pagination remain available; exhaustion
still fails the capture. A 20000-entry regression covers aggregation across
load bases and replacement epochs, and clearing during warm reset.

Synthetic tests cover two relocation bases, PC-relative reads/calls/jumps,
32-bit index values, preserved absolute addresses, guest stacks, trap service
words, asynchronous resumes, overlapping writes, image replacement and stale
tokens. Generation fails for unsupported instructions and module fall-off.

## Remaining playthrough gates (`beads-mq6t`)

- The former attract restart `$FA` failure was corrected by keeping
  `SS.Pos` current through filtered physical-sector headers. The subsequent
  120000-field windowed and 160000-field headless runs had no observed guest
  resets or dispatch misses, but their final coverage ledgers overflowed and
  both evidence gates failed. They are not accepted runs. The fresh normal-speed
  DirectSound run of the indexed coverage build also ended before acceptance,
  as recorded at the checkpoint above.
  `hotelmario_attract_gate.py` now identifies actual nine-demo circuits from
  the original AV map and requires a return to demo 1 after each circuit.
- One-player Stage 1 is reachable through normal shell/menu input. A
  normal-speed windowed run reached field 12000 with working button/direction
  input, zero resets/dispatch misses and zero PCM drops through SDL's dummy
  consumer. This does not certify completion of a level, campaign or audio
  listening quality.
- The current seeded build completed an accelerated field-40000 controller
  probe through repeated deaths/restarts and game over without the prior
  audio CIL error. This establishes lifecycle progress, not a cleared stage
  or legitimate full campaign.
- The current seeded LLE build cleared one-player Hotel 1 Stage 1 through
  normal-speed windowed controller input. The original open-door count reached
  zero and the game advanced to Stage 2 at field 31059, with score 2350 and
  three lives. The development controller reads state and sends timed IKAT
  input; it never writes guest progress. Its earlier door-state interpretation
  was reversed and those earlier attempts do not establish level completion.
- The same normal-speed seeded run also cleared Stage 2 at field 71116
  (score 2900, four lives), and Stage 3 at field 125850 (score 800, five lives
  after original-game continues). It reached field 180000 without resets or
  dispatch misses, retaining 8473 uncovered entries without loss. Its SDL
  dummy audio did drop PCM, so that run does not pass audio acceptance.
- A named original-game save survived a normal window close and appeared in
  the restore menu after a cold boot using the same 32 KiB battery file.
  Correct stage restoration still needs validation. Two-player input reached
  Stage 3 for Mario and Stage 5 for Luigi in a separate seeded LLE run.
- Both legitimate campaigns, all streamed modules/bosses/cutscenes, the
  ending, and original-game save/restore/continue need validation. No guest
  progress writes, forced stages or replacement game logic are permitted.
- The preceding default LLE executable passed all 30 headless and all 30
  windowed normal-speed cold boots to the pixel-exact title. Every run retained
  linked-input provenance and all 174 compiled module identities, with three
  active bindings at the title. These launch gates establish startup only.
  The indexed coverage build passed its new 30-launch headless batch. Both
  new windowed batches ended early with exit code 0 and remain failed; their
  results are retained separately.
- A local runtime-only HotelMarioRecomp preview archive passed the five-file
  allowlist, Release/COSIM OFF build-graph audit, PE import audit and linked
  input provenance checks. Publication and sustained performance acceptance
  remain release gates. Assets, generated C and development tools stay out.

## Reproducible evidence

`tools/hotelmario_scenario.py` boots an isolated player profile, selects Play
CD-i through actual input, records runtime/asset/source identities and device
traffic, and retains a framebuffer, CPU/device state and both RAM banks.
Requested capture points must be reached; guest resets, faults, transport
stalls and the known disc-error screen fail the run. Accelerated runs are
investigation, not normal-speed acceptance.
Use `--require-build-provenance` for acceptance: the executable and every
linked source/generated input must match their build-time hashes. The runtime
also rejects a BIOS that differs from the ROM used to generate its BIOS C.

```powershell
py -3 tools/hotelmario_scenario.py path/to/HotelMarioRecomp.exe bios/cdi490a.rom "disc/Hotel Mario (USA).cue" --output build/tmp/my-run --generated-dir hotelmario/generated --windowed --speed 1 --seconds 300 --stop-frame 12000 --input-script "6000:16,6018:0,6250:16,6270:0,7400:16,7420:0,8000:4,8120:0,8200:16,8220:0,8400:32,8420:0"
py -3 tools/collect_module_seeds.py --evidence build/tmp/my-run/evidence.json --output build/tmp/my-module-seeds.txt
```

The schedule is a reproduction for the tested asset/build, not a general
campaign driver. Evidence directories are local and git-ignored. Historical
incorrect-background comparison frames remain in `docs/issues/`.

## Detailed validation ledger at integration

This ledger supersedes earlier progress notes that describe tests as running.
No game runtime or controller was restarted for this integration. A passing
observation harness establishes its requested capture, not an entire campaign.

### Exact identities and build variants

| Item | Identity / scope |
| --- | --- |
| BIOS | `cdi490a.rom`, 524288 bytes; SHA-256 `0708ec2cbe0528f7511ab2bc84526457e7f6c2bd7d9ca377f969b295d05a0945` |
| USA raw disc | 367147200 bytes, Mode-2/2352; SHA-256 `31cf7c28f4e7e7dcf731633bfcb4a5ae9459794aecfcd3d765df66d2442722f9` |
| Latest indexed default executable | `HotelMarioRecomp/build/playable-lle/HotelMarioRecomp.exe`, 29906954 bytes; SHA-256 `1d330205d80d1b61bb8dc4e12a10b09c6ba0f44d7ce26baa58abe4d6e70682c0` |
| Preceding default executable | SHA-256 `9adc63c3a8e455dd2d806fa6555a2301f76baf18300e839888045eeb436cae34`; its successful windowed matrix is historical evidence for that executable |
| Seeded gameplay executable | `HotelMarioRecomp/build/player-seeded-lle/HotelMarioRecomp.exe`, 34900825 bytes; SHA-256 `beb30f29b3a13f535da842884603b915241b727fdd8a0a9e0b482bea481d01b6`; adds 1205 observed native entry offsets, retains the older coverage ledger |
| Tested game core | `ddfa4e1b090b643a9ee596050e7eb345b8b276a2`, retained by the framework's submodule pin |
| Indexed ledger implementation | `ae8485d942899dad540dcceebd4621d84b00db91`; 262144 records and a 524288-slot hash index |
| Core default-branch merge | `eaeec13f132255429b45314f1568b7e6dec6c2d4`; merges the tested feature into upstream `main` at `921f67b`; six compiler checks pass on this combined core |

The default executable's capture identifies source revision `df442ba` plus
the linked source changes that were subsequently committed as `ae8485d`.
The post-link sidecar hashes linked inputs and the executable; its provenance
is stronger than inferring a build from the working branch name. Later docs
and controller edits do not establish that a new executable was built.
The seeded variant and the indexed default must not be conflated.

### Component checks actually run

The six compiler and eleven runtime checks all passed at the gameplay
checkpoint. Existing full-suite logs are at
`build/recompiler/Testing/Temporary/LastTest.log` and
`build/runner-native/Testing/Temporary/LastTest.log`.

| Suite | Passing CTest names | Meaning and limits |
| --- | --- | --- |
| Compiler | `module_seeds`, `module_entries`, `native_pic_execution`, `native_pic_rejects_falloff`, `os9_module_validation`, `disc_filesystem` | Synthetic relocation, instruction execution/resume and rejection checks; bounded module/filesystem parsing. These do not prove every path in every real module. |
| Runtime | `ikat_transport`, `native_module_binding`, `sha256`, `cdi_nvram`, `scc68070_peripherals`, `mcd212_video`, `m68k_arithmetic`, `cdi_input`, `cdi_audio`, `cdic`, `player_config` | Device/CPU/storage/input/native-lifetime regressions. Passing unit checks are not complete hardware or title compatibility. |
| Coverage regression | Within `native_module_binding` | 20000 distinct even offsets, two load bases, replacement epoch, exact aggregation, stable pagination and warm-reset clearing; no dropped entries. |
| Combined core integration | All six compiler checks listed above | Fresh Release build against merged core `eaeec13`; no full BIOS/disc regeneration or new game execution on that combined core. |

The integration-only compiler build is preserved at
`F:/Projects/_wt-cdirecomp-hotel-core-integration-20261009/build/recompiler-integration`.
It uses an isolated framework checkout at `5f8e1b6` and the merged core.
The production pin stays at `ddfa4e1`; promoting to combined core `eaeec13`
would require regenerated BIOS/game code and fresh product acceptance.

### Startup and attract evidence

All paths in this table are beneath the framework's ignored `build/tmp/`.
Failed batches are retained as failed; independent batches are not combined
into a successful 30-launch claim.

| Evidence directory | Observed result | What remains unproven |
| --- | --- | --- |
| `final-lle-headless-matrix-20261009` | Preceding default passed 30/30 normal-speed cold launches to exact title hash `1b902b2f985afb5f` | Later indexed executable and campaigns |
| `final-lle-windowed-matrix-20261009` | Preceding default passed 30/30 normal-speed windowed cold launches | Current indexed windowed reliability, audio and campaigns |
| `indexed-lle-headless-matrix-20261009` | Latest default passed 30/30; each retained linked provenance, 174 compiled identities and three active title bindings | Windowed behavior, attract and gameplay completion |
| `indexed-lle-windowed-matrix-20261009` | Three launches passed; fourth exited with code 0 before capture, before its first guest instruction; batch failed | Shutdown cause and a complete current-build windowed batch |
| `indexed-lle-windowed-matrix-second-20261009` | Fourteen launches passed; fifteenth exited with code 0 before capture; batch failed | Shutdown cause and a complete current-build windowed batch |
| `header-no-poke-normal-attract-20261009` | Historical windowed observation reached 120000 fields with zero resets/misses/PCM drops | Predates subsequent selection-boundary and IKAT timing changes; no current-build or exact-circuit certification |
| Long preceding-default attract runs | Headless 160000 fields exhausted 16384 coverage records and dropped 3476 entries; windowed 120000 fields dropped 190 entries | Both final evidence gates failed despite no observed guest reset/miss |
| `indexed-lle-windowed-attract-20261009` | DirectSound, speed 1, no input; last sample field 68067; connection refused thereafter, `result.json` has `ok: false` | Required field 85000 capture, complete retained evidence and three complete circuits |

The old coverage failure was diagnostic capacity/hot-path work, not evidence
that deduplication grew with each epoch: records already aggregate image/offset
across epochs. The new hash index fixes that class, and its synthetic regression
passes. The interrupted indexed attract run does not validate its full-cycle
acceptance. Runtime exit code 0 is not itself a successful launch; the causes
of the two windowed exits have not been established.

`hotelmario_attract_gate.py` reads the original disc's `L0_av.map` and derives
the ordered nine-demo circuit. Each accepted circuit must return to demo 1;
three circuits require four observed starts of demo 1. Elapsed fields alone
are not sufficient. The verifier also requires a successful scenario capture,
matching disc/runtime identity and linked provenance, normal pacing, no held
input during attract, no guest reset/dispatch fault and no lost coverage.
Audio acceptance additionally requires a real windowed output driver, decoded
PCM and zero dropped frames. That still does not establish listening quality.
The product's existing short `tools/accept-attract.ps1` probe is not this
three-circuit gate.

### Legitimate gameplay and persistence evidence

The development controller reads original game state and sends `set_input`
through timed IKAT. It does not write guest RAM, edit saves, force stages,
patch guest code or substitute game logic. Ordinary game-over continues were
used; this is not a no-death or no-continue playthrough.

| Evidence | Progress observed | Limits |
| --- | --- | --- |
| `normal-campaign-controller-20261009` | Seeded one-player entered Stages 2, 3 and 4 at fields 31059, 71116 and 125850; final field 180000 had zero resets/misses and 8473 retained uncovered offsets, none lost | Only first three stages cleared; SDL dummy PCM dropped 318656 frames; observation result passed, audio/campaign acceptance did not |
| `indexed-lle-oneplayer-20261009` | Latest default entered Stages 2, 3 and 4 at fields 14113, 18380 and 51281; final controller record field 59407 | Partial ordinary production-profile probe; not the formal full-campaign evidence bundle |
| `normal-production-save-20261009` | Seeded two-player: Mario entered Stages 2 and 3 at fields 29276 and 78351; Luigi entered Stages 2–5 at fields 33299, 62494, 115509 and 124178 | Mario cleared first two stages, Luigi first four; neither completed Hotel 1 or a campaign |
| `normal-production-save-20261009/user-stop-checkpoint.json` | Final field 145679, Mario Stage 3, Luigi Stage 5, zero resets/misses, released input; normal window close followed | Captured partial state, not a full campaign certificate |
| Save/restore screenshots and battery in that profile | Name `AAA` saved through the original menu; normal close flushed a 32768-byte battery; cold boot displayed the slot in the restore menu | Correct active hotel/stage after completing the chooser has not been validated |

The original restore routine first loads highest unlocked hotel/stage and then
uses a chooser to select the active stage. Seeing a named slot, or reading its
unlock values, is not proof that the active stage was restored. The later
two-player run selected new players and must not be counted as restore proof.
The development controller's earlier reversed open/closed tile interpretation
also invalidates any earlier claimed closures. Correct open tiles are `$2000`,
closed tiles `$1000`; advancement follows the original zero-open-door count.

The final helper records source hashes per controller run and periodic device,
coverage and host-clock history, rejects snapshots more than 12 fields stale,
and follows original jump braking semantics: neutral continues a running jump,
opposite direction stops travel, and takeoff facing is retained. These helper
changes are not a certified autonomous campaign driver. They still require
legitimate gameplay observation, especially for new enemy types and bosses.

### Packaging, compatibility and remaining gates

The local `HotelMarioRecomp-current-lle-preview-windows-x64.zip` passed the
exact five-file allowlist, Release/COSIM OFF graph, PE imports and linked-input
provenance audit. SHA-256:
`afc2e4d37da53c685297dc59bc14abd8a48359c5213528b9ee540c87c2a8487f`.
It predates the indexed ledger executable and is a development preview.
No new archive, playable-end-to-end release, tag or publication was produced
by this checkpoint integration. The exact BIOS guard rejected a different
`cdi220b.rom` with exit code 2; entire-disc SHA checking remains acceptance
tooling, while runtime module binding enforces each full image identity.

Still open before an end-to-end claim:

1. Complete current-build 30-launch windowed batch with explained shutdowns.
2. Three actual attract circuits with successful complete evidence and real
   output audio; no discarded coverage or PCM.
3. Both complete legitimate campaigns, every hotel/stage/boss, later streamed
   modules and cutscenes, and the ending. Compilation of 174 identities is not
   proof that all paths execute correctly.
4. Correct hotel/stage restore after cold boot, original continue behavior
   beyond early stages, and supported persistence through normal exit.
5. Audio listening and sustained speed-1 performance, including busy scenes
   and later modules. Dummy PCM delivery is not sound-quality acceptance.
6. Rebuilt current runtime package and repeated package/provenance audit when
   the final executable exists; wider title/platform compatibility remains
   separate work.

LLE remains the default and the correctness floor. No OS-9 HLE, synthetic guest
progress or optional HLE optimization was added. Some device timing/ownership
behavior is inferred from original ROM traffic; these component tests do not
constitute silicon/microcode accuracy. No emulator implementation was an
authority, and BIOS/disc/generated assets remain local and excluded from Git
and runtime packages.

### Integration and handoff

The owner authorized this integration after the stop. Framework commits
`10859e5`, `df23fdf`, `df442ba`, `ae8485d`, `2bdd5c5` and `5f8e1b6` preserve
the implementation, fail-closed captures, launch/stage notes, indexed ledger,
actual attract verifier and stopped checkpoint. Shared-core `main` now
includes `ddfa4e1` through merge `eaeec13`; upstream's newer work is preserved.
Framework `master` retains the tested `ddfa4e1` gitlink. Product `master` pins
the documented framework checkpoint; README status is updated in both repos.
The integration task is `beads-ttbl.1`. Validation owners remain `beads-6v0p`
(transport/input/audio), `beads-bcha` (cutscenes), `beads-z8yh` (native modules)
and `beads-mq6t` (campaign/product). Those validation issues stay open.

Before any future run, obtain the owner's authorization to resume gameplay
work, preserve the stopped profiles, and select the exact intended executable
and generated tree. Keep new evidence separate from these failed/historical
runs. `read_mem` is limited to 4096 bytes per request; larger captures must be
split. To persist battery changes, release input and close the owned window
normally while the guest is running; a paused guest cannot pump its window
close, and debug-server `quit` only disconnects TCP.

Beads updates are durable in the central local database. Its Dolt push has
failed because remote `origin/main` references missing Dolt data; that failure
must not be described as synchronized. Repairing that remote is separate work.

## Windows and Linux package tooling

The owner requested a new Windows/Linux Hotel Mario checkpoint build, using
`../psxrecomp/TombaRecomp` as the release reference. Build task `beads-ssy9.2`
owns the product artifacts; `beads-ttbl.2` owns reusable Linux runtime packaging.
`tools/package_runtime_appimage.py` follows the linuxdeploy/appimagetool
workflow with SHA-pinned official tools, Release/COSIM OFF graph checks,
linked-input verification, ELF/dependency checks, and allowlist/hash comparison
of the staged and extracted AppImage payload. Original assets, generated C,
provenance sidecars and development tools remain outside runtime packages.
The product supplies its Linux build wrapper and AppRun. This packaging work
does not resume full-campaign or visual-flicker investigation. Actual artifact
results are recorded after the builds and package boot checks finish.
