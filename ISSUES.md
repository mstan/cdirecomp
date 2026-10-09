# Hotel Mario engineering status

Updated 2026-10-09. Work is tracked in central Beads under the framework epic
`beads-ttbl` and game epic `beads-ssy9`.

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

Synthetic tests cover two relocation bases, PC-relative reads/calls/jumps,
32-bit index values, preserved absolute addresses, guest stacks, trap service
words, asynchronous resumes, overlapping writes, image replacement and stale
tokens. Generation fails for unsupported instructions and module fall-off.

## Remaining playthrough gates (`beads-mq6t`)

- The former attract restart `$FA` failure was corrected by keeping
  `SS.Pos` current through filtered physical-sector headers. A normal-speed
  windowed build reached field 120000 without reset, dispatch miss or PCM
  drops. That evidence predates the latest selection/audio changes; final
  build acceptance and exact complete-cycle identification remain pending.
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
- Both legitimate campaigns, all streamed modules/bosses/cutscenes, the
  ending, and original-game save/restore/continue need validation. No guest
  progress writes, forced stages or replacement game logic are permitted.
- The current default LLE executable passed all 30 headless and all 30
  windowed normal-speed cold boots to the pixel-exact title. Every run retained
  linked-input provenance and all 174 compiled module identities, with three
  active bindings at the title. These launch gates establish startup only.
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
