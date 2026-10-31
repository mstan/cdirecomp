# CD-i native HLE burndown

Saved at the owner's request on 2026-10-27. The owner subsequently activated
the end-to-end slash goal; individual implementation and qualification have
resumed. On 2026-10-31 the owner authorized independent subagent review after
each remaining candidate, branch PRs for retained changes, and assignment of
the next candidate. Measurement and acceptance gates remain serial.

Owning issue: `beads-oatv`, under framework epic `beads-ttbl`, related to the
central `optimization campaign` (`beads-2wy8.1`) with label
`campaign:optimization-campaign`.

## Objective and implementation boundary

Replace expensive complete jobs with native operations that satisfy the
faithful LLE implementation's observable contract. Existing subsystem,
function and file boundaries are implementation details. A useful HLE job may
span OS services, CPU execution, memory ownership/bookkeeping, device work and
graphics or audio preparation.

AOT executes translated instructions natively; it does not by itself replace
an expensive emulation algorithm. Preserve a build-time selectable faithful
implementation, rather than requiring HLE to retain its internal machinery or
execute the reference alongside the replacement in production.

The owner explicitly authorized this HLE investigation despite older repository
documentation prohibiting OS-9 HLE. Preserve the underlying correctness rule:
no fabricated syscall success, silent stubs, drifting shadow kernel state or
forced guest progress. Use authoritative guest RAM and verified contracts.

## Candidate dispositions

These are complete jobs to evaluate. Related candidates can become one
replacement across OS, memory and device boundaries. They are not fixed
subsystem boundaries or a requirement to adopt every proposed optimization.

| # | Candidate | Work the native replacement would remove | Current status |
| --- | --- | --- | --- |
| 1 | **Qualify the scanout candidate already written** | Repeated decoding, transparency, contribution and composition work across pixels | **Qualified as part of the combined native frame job.** Three alternating Hotel Mario pairs save 23.0-26.0% CPU/field with complete contract equality. Scanout-only performance remains inconclusive. |
| 2 | **Complete frame production and publication** | Repeated framebuffer hashing, copying, diagnostic reconstruction and publication work | **Implemented and qualified with candidate 1.** Exact content identity, retained storage and frontend handoff remove duplicate work without changing field cadence. |
| 3 | **Graphics service to completed drawing job** | Guest drawing preparation, intermediate memory traffic, control-list construction and repeated renderer preparation | **Qualified native queue job; independent review KEEP, final performance neutral/inconclusive.** Real callbacks, queue publication and device/IRQ frontiers match. Reviewer-found debugger stops are fixed and regression-tested. Branch PR handoff is current; historical pre-fix gains are not a current speedup claim. |
| 4 | **Memory operation to fully published result** | Instruction-by-instruction copying/filling plus repeated module invalidation, tag maintenance and dirty tracking | **Open, queued.** Previous gameplay-only attribution did not qualify complete bulk operations. |
| 5 | **File/module load to executable module ready** | Repeated sector handling, copying, validation, relocation, linking, hashing and native binding | **Selected native verification job implemented and qualified in the real load-to-ready pipeline.** Three matched pairs use 23.4–33.3% fewer process cycles during loading; later gameplay is neutral/inconclusive. Kernel I/O, linking and scheduling stay authoritative. |
| 6 | **Frequently executed OS-9 service jobs** | Recompiled kernel algorithms and internal dispatch that a complete native operation could replace | **Open, queued.** Service attribution and boundary contracts are remaining implementation work. |
| 7 | **Native execution spans** | Repeated dispatch, interpreter handoffs, instruction bookkeeping and checks between operations with no observable intervening event | **Implemented and qualified.** Reused the existing observed native catalog, then coalesced legal PIC continuations into shared native bodies. SDK `3955bf3`; incremental 3-pair median CPU reduction 23.77% versus the seeded catalog, with exact canonical guest contracts. |
| 8 | **Complete CD/XA streaming jobs** | Repeated work across sector selection, decoding, buffer ownership, DMA/refill, guest drivers and audio handoff | **Open, queued.** Complete streaming integration and actual seek/channel/refill qualification remain work to perform. |
| 9 | **Whole-board event advancement** | Calling timer, input transport, CD/audio, RTC, raster and pacing machinery after every instruction | **Open, queued.** The naïve bulk-sum counterexample is a design constraint, not a rejection of a correct native span. |
| 10 | **Frame to host presentation** | Redundant frame copies, texture uploads, conversions and presentation preparation | Copy/upload consolidated into 2 and qualified through SDL dummy/software. **Hardware-driver qualification remains open, queued.** |
| 11 | **GPU resident jobs and high-resolution rendering** | CPU rendering at enhanced pixel density, expanded frame uploads, duplicate native/enhanced preparation and unbounded scale-dependent intermediates | **New, open and queued; guidance tracked only.** Implement the owner's GPU rendering guide under `beads-ttbl.3`: required 16x at full native speed with headroom on the owner's PC, desired 32x/64x, later lower-end targets. Hardware qualification can satisfy 10 jointly only after both contracts pass. |

## Current checkpoint

- Current engine branch: `perf/native-drawing-jobs-20261028`; supported consumer
  branch: `HotelMarioRecomp/perf/native-drawing-job-20261028`.
- Qualified engine checkpoints: `a83c317` (frame job), `e92baf8` (coarse native
  catalog), `85c869e` (completed-line jobs). Shared core: `3955bf3`.
- Supported consumer integration: `16353fc` on
  `HotelMarioRecomp/perf/native-jobs-20261028`, owning issue `beads-7t8h`.
- **Reopened at the owner's direction.** The prior closure was premature:
  candidates 3/4/5/6/8/9 and hardware presentation still require actual work.
  Missing workload attribution, contracts and differential fixtures are work
  within this goal. Work serially, checking in with the owner after each.
  Candidate **5** has completed its serial implementation and qualification
  below. The owner resumed the entire remaining set on 2026-10-31;
  **candidate 3 now has independent KEEP review** after implementation,
  objective debugger repair and fresh built-game measurement, classified
  neutral/inconclusive. Branch PR handoff is current. After each independent keep/fix/reject review,
  create branch PRs for retained work and assign the next candidate. Do not
  merge without owner approval. Remaining work is 4, 6, 8, 9, hardware 10 and
  the new GPU rendering candidate 11. Reuse accepted 1/2/5/7 evidence.
- Scanout selection is `-DCDI_VIDEO_BACKEND=LLE` or
  `-DCDI_VIDEO_BACKEND=HLE`. The repository default remains LLE.
- HLE computes joint source/matte color spans, consolidating contribution,
  transparency, composition and diagnostic output. Eligible RLE7 sources feed
  those spans directly; other formats retain the faithful decoders.
- The existing pixel fixtures and all 12 runtime component tests passed.
  The new independent LLE/HLE test exercised 1,024 randomized control cases
  and 15,872 line jobs, comparing consumed bytes, decoded diagnostics,
  composition verdicts, cursor behavior and completed frames.
- The first differential run found a low-width high-8 RLE duplication edge
  case; it was corrected before the passing run.
- Matched combined-job results and their limitations are recorded below. The
  earlier scanout-only package remains inconclusive, rather than an adopted win.
- The existing local consumer directory
  `F:/Projects/HotelMarioRecomp/build/playable-lle` currently contains the
  LLE-selected comparison build. The HLE comparison binary is retained under
  `HLE-frame`. Do not infer a backend from a historical directory name.
- The matched newly built LLE binary and its link-time provenance are retained
  at `G:/Temp/Codex/cdirecomp/hle-pass-20261027/LLE`.
- `tools/scanout_hle_compare.py` is exercised against complete held runtime
  state and all completed-frame records. It retains its own source with each
  run, verifies link-time input/binary identity and compares the fixed gameplay
  interval separately from startup. The final gameplay state verifies an
  active player, enemies and door interactions. SDL mode explicitly excludes
  host-time PCM queue/drop counters while retaining the full decoded PCM hash.
- No release package, delivery folder or native player save was changed.

## Evidence and limits

Retained root: `G:/Temp/Codex/cdirecomp/hle-pass-20261027`.

The corrected runtime sample in `profile-corrected.json` contains 2,552
samples from the existing Hotel Mario build, from boot through field 11,000.
Decode/composition/transparency account for 976 samples (about 38.2%);
framebuffer hashing accounts for 431 (about 16.9%); the device-event update
chain accounts for roughly 5-6%.

These are selection evidence from a mixed boot/early-gameplay interval, not
isolated gameplay benchmarks or before/after performance results. The initial
`profile.json` lacked ASLR-adjusted symbolization and is excluded from
decisions. Media-generation reads are already atomic; an earlier issue note
incorrectly described a mutex there.

Existing full-campaign, windowed reliability, visual flicker, audio listening
and native save-restoration acceptance gaps remain separate. Identical
baseline/candidate failures are shared defects, not introduced regressions.

## Acceptance and burndown rules

1. Reuse the owning issue, campaign and retained evidence. Measure the
   smallest workload needed to select the largest useful next job. Evaluate
   reuse, replacement and consolidation before local tuning.
2. Define each job's external contract, including inputs, output bytes,
   authoritative guest state, memory ownership, events/interrupts, errors and
   native-save effects. Internal helper boundaries do not define the contract.
3. Implement and integrate a coherent replacement, then test it against the
   faithful implementation. Production HLE must not require running LLE
   alongside it. Retain a functioning portable implementation and build-time
   LLE selection.
4. Measure actual built-game CPU/frame cost, throughput and relevant startup,
   stall, memory and frame-time costs. Match assets, workloads, host/compiler,
   features, audio and display settings. Warm and alternate A/B order; retain
   raw individual samples, variation, source/binary identities and settings.
5. Adoption is per game: neutral or faster is acceptable; a reproducible
   regression in one game cannot be offset by gains elsewhere. Treat small
   mixed results within variation as neutral/inconclusive, not proven wins.
6. Label the exact optimization package each game received. Preserve
   machine-readable per-game data in the owning issue and campaign. Do not
   claim untested games or platforms are qualified.
7. After two unsuccessful improvement experiments, reassess the architecture
   and pivot within scope. Avoid accumulating instrumentation or micro-fixes
   without delivering a measured outcome.
8. Record a disposition for every candidate: validated/adopted, consolidated
   into another completed job, rejected with evidence, or deferred with a
   concrete evidence-based reason and remaining prerequisite. Keep partially
   implemented or unqualified work explicit. A documentation pass or passing
   component tests alone does not complete an implementation candidate.
9. Work solo. Use muted/background automated QA and supported TCP interfaces;
   preserve owner focus, saves and settings. Honor the existing owner handoff
   and merge rules when applicable.
10. Keep the resource ledger current and clean agent-created disposable data.
    Retain the bounded evidence and passing builds needed for ongoing work.
    Follow the one-current-build-per-game delivery policy if delivering a
    replacement. Publication of a blog post requires an explicit request.

## Resumption log

Update this file and `beads-oatv` at meaningful decisions, implementation and
qualification milestones. Record campaign results in `beads-2wy8.1`. The
previous central sync attempt encountered another running sync helper; respect
that healthy process and retry through the documented helper when appropriate.

### Resumed qualification

- Reclaimed `beads-oatv` and activated the explicit goal. No CD-i test or build
  remained running from the interrupted request. Other sessions' jobs were
  preserved.
- Validated the comparison harness against held runtime state, retained every
  completed frame and moved expensive state capture outside timing. Original
  harness source is retained with its raw initial run.
- Initial scanout-only warm and retained A/B pairs both matched the complete
  1 MiB RAM image, CPU/device/audio state, final 256 instructions and all
  13,000 completed frames. Input-driven Hotel 1 Stage 1 gameplay was observed;
  the script exercised movement, jumping and doors without guest-state writes.
- Performance of that initial package is **inconclusive**: warm LLE/HLE were
  5.213/4.080 ms per field; the retained pair was 4.954/5.030 ms per field.
  The retained pair's host average CPU occupancy rose from roughly 66% to
  100%. Neither the favorable warm sample nor the small mixed result is an
  adopted performance claim. Raw data:
  `G:/Temp/Codex/cdirecomp/hle-pass-20261027/scanout-first-pair`.
- The gameplay frame history contained 2,857 identical consecutive images in
  3,600 transitions (about 79.4%). This selects a broader frame-production,
  identity, retention and host-handoff implementation, consolidating candidate
  2 with the existing scanout job and the redundant-copy/upload portion of
  candidate 10.
- Implemented exact unchanged-image hash reuse, copying only the changed
  visible prefix while preserving hidden storage, and a coherent publication
  snapshot for content-based frontend copying/upload. Field generation and
  presentation/redraw cadence still advance every field. Differential tests
  cover identical fields, geometry changes, interlaced retention and frontend
  delivered pixels. **Combined built-game qualification remains pending.**
- New binary pairs use `HLE-frame` and `LLE-frame` beneath the retained root.
  The supported consumer build is being rebuilt; inspect its final CMake
  selection rather than relying on its historical directory name.
- A headless Ghidra registry query found no CD-i program. No GUI, writer or
  new database was opened; no analysis or source binary was removed.
- `resource-ledger.json` records retained and disposable paths. New bulk
  evidence remains on G:; native saves and user profiles are untouched.

### Combined native frame job qualified

Evidence: `frame-package-pairs/campaign-evidence.json` beneath the retained
root. Matched Windows x64 / Ryzen 7 9800X3D, GCC 15.2 Release, COSIM OFF,
same 490a BIOS and USA disc, deterministic ordinary-input schedule, software
scanout, fully emulated audio and headless output. Warmed alternating order
LLE/HLE, HLE/LLE, LLE/HLE; measured fields 9,400-13,000:

| Pair | LLE CPU ms/field | HLE CPU ms/field | CPU reduction |
| --- | --- | --- | --- |
| 1 | 5.945638 | 4.399113 | 26.01% |
| 2 | 5.874566 | 4.524740 | 22.98% |
| 3 | 6.044922 | 4.487847 | 25.76% |

The median paired CPU reduction is 25.76%. Each pair, and the warm pair,
matches the complete 1 MiB RAM image, CPU/device/native state, full decoded
audio hash, final instruction tail and all 13,000 framebuffer/control hashes.
This package is **native scanout spans plus exact frame publication/content
reuse**, not an isolated scanout-only speedup. Background host occupancy was
near 100% in every run; uncapped throughput was correspondingly variable.
These are matched process-CPU results under load, not quiet-host latency or
all-hardware performance claims. Sampled peak RSS remained approximately
208 MB in the retained pairs; raw individual values are preserved.

`frame-package-sdl/campaign-evidence.json` exercises the actual frontend's
copy/upload/present and audio-consumer paths using SDL dummy video/software
rendering, without a desktop window. Warm and retained pairs have identical
guest/CPU/device/frame/decoded-PCM contracts; the retained pair is
5.142361 -> 3.434896 ms/field (33.20% reduction). Host-time queue/drop counts
are retained but excluded from semantic equality. This is functional frontend
qualification and one software-backend timing pair, not a D3D/OpenGL result.

All 12 Windows component checks pass. Both LLE and HLE pixel fixtures and the
15,872-job differential contract also pass on Linux x64/WSL2 with GCC 13.3,
`-std=c11 -O2 -Wall -Wextra -Werror`. No assembly or platform-specific native
path was introduced. Full Linux game-runtime qualification remains unclaimed.

### Next-job selection

The always-on census identifies 81,604,896 interpreted instructions at field
13,000, with no dropped PC records. Six PCs in the existing game routine at
`0x2762D8` account for tens of millions of those instructions; its native
module offset is `0x17018`. The retained generated tree
`build/tmp/native-player-seeded-20261008` already contains this routine and
its instruction resumes. Its 174 module identities and source provenance are
available. Qualify that existing work with the current native frame job before
starting a duplicate discovery or manually replacing the game/OS service.

Native-binding evidence does **not** support a speculative lifetime rewrite:
only five bindings and one invalidation occur in the complete 13,000-field
workload; that invalidation retires the old bumper image. The main image
remains bound. Memory/module ownership candidates still require actual cost
attribution, rather than an assumption that image invalidation causes the
large interpreter count. No Ghidra registry key matched either CD-i or Hotel;
literal OS-service replacement would need an exposed authoritative fixture.

### Native execution jobs qualified

The retained 1,205-entry catalog is now reproducible as
`hotelmario/module-seeds.txt` (SHA-256
`37bbc003f41a49968013cd3874f417f795c212805b6da7e2231897c7507443a4`).
These are observed entry offsets bound to full original module hashes, not
generated game code or a relaxation of image/code-lifetime validation.

Reusing that catalog with the current frame job first reduced a retained
incremental pair from 3.842231 to 2.158798 ms/field (43.81%). This one-pair
diagnostic is retained in `seeded-native-entry-probe`; coverage counts alone
were not used to adopt it.

That output still performed 73,286,926 host native dispatches. Inspection found
linear observed instruction continuations emitted as separate native bodies.
The shared-core change `3955bf3311f55e031836f70b5c1839e95b81f9cf`
(`beads-cpjc`, under `beads-rxq`, related to this issue and the campaign)
admits legally reached fall-through entries into the existing canonical-owner
proof only for position-independent asynchronous-resume modules. The final
hard-boundary fixed point, every instruction resume and code-lifetime check
remain. Flat-ROM behavior and the separate Genesis source profile are unchanged.

The new dense-PIC fixture fails the original coarse-execution budget and passes
the new generator. All seven compiler checks and the existing flat-ROM
function-alias integration suite pass. All 174 Hotel Mario modules were
regenerated with the same seed input and compiled; no generated C was edited.

`coarse-native-pairs/campaign-evidence.json` records warmed alternating pairs
against that same seeded catalog with the original per-entry dispatch:

| Pair | Per-entry CPU ms/field | Coarse job CPU ms/field | CPU reduction |
| --- | --- | --- | --- |
| 1 | 2.064112 | 1.573527 | 23.77% |
| 2 | 2.623531 | 1.566840 | 40.28% |
| 3 | 2.352973 | 1.844618 | 21.60% |

The median incremental CPU reduction is 23.77%; host native dispatches fall
to 1,692,743. The latter is supporting evidence, not the performance result.

`combined-native-pairs/campaign-evidence.json` directly measures the complete
package against original-catalog/reference-LLE scanout, rather than adding
the gains from different experiments:

| Pair | Original reference CPU ms/field | Complete native package CPU ms/field | CPU reduction |
| --- | --- | --- | --- |
| 1 | 5.343216 | 2.238074 | 58.11% |
| 2 | 4.978299 | 2.126456 | 57.29% |
| 3 | 5.029593 | 2.031250 | 59.61% |

Median complete-package CPU reduction is **58.11%** for this fixed gameplay
interval. Reference binary size is 29,908,046 bytes; candidate is 33,891,700.
Retained sampled peak RSS is about 207.68 MB versus 211.38-211.40 MB, a roughly
3.7 MB increase. Host CPU occupancy varied and is retained in each raw result;
these are process-CPU measurements, not quiet-host latency or hardware-GPU
claims. Do not extrapolate to untested titles, stages or settings.

All pairs retain exact full RAM, canonical CPU state, device state, PCM hash,
final register/instruction/cycle tail and every framebuffer/control hash.
CPU-tier comparisons park at common instruction-entry sequence **150976126**
immediately after field 13,000. A frame-end hold alone observes the interpreter
after its PC increment and native code before its next entry, producing a
diagnostic PC mismatch despite identical instruction-entry histories. The
initial mismatch is retained in `seeded-native-probe`; it is not passed off as
an accepted comparison. Host implementation instruction/dispatch counts,
native-binding implementation timestamps and IKAT opcode-attribution PC are
explicitly excluded from CPU-tier semantic equality; actual event sequence,
hardware timing/payload and all guest outputs/state remain compared. All raw
diagnostics are retained.

At this checkpoint the owning burndown remained active. The next measurement
selected a completed-line job, recorded below. Frame/CPU job qualification
does not close the separate full-campaign/audio/save acceptance gaps.

### Completed-line native job qualification

Gameplay-only residual sampling of the coarse native build selected scanout:
220/491 samples in decode/composition/transparency/matte, another 26 in frame
publication, versus 46 in the board-event chain. These are perturbed selection
samples, not timing results; retain `profile-residual.json` and
`residual-selection.json` beneath the evidence root.

The native frame job now retains the last completed line's complete input
contract: persistent video controls, addresses, source geometry and the exact
authoritative source bytes consumed. Equal inputs reuse the existing retained
frame/plane/verdict result and restore the decoded-row diagnostics. Every MCD
control instruction, source-pointer update, event and IRQ still executes.
Geometry changes invalidate all line results, including changes back to an
earlier geometry. No guest-write dirty tags or shadow guest state are used.
RGB555's coupled source contract and blinking cursor rows retain ordinary
scanout. The implementation is portable C under the existing build-time HLE
choice; LLE remains available.

The first key included the cursor restart field even when blinking was off.
It matched all differential contracts but reused only 6,480/3,118,800 jobs.
Two retained pairs completed before cancellation: 2.169540 versus 2.196779 and
2.106275 versus 2.133391 CPU ms/field. No benefit was established; these small
differences alone are not a proven regression beyond measurement variation.
This attempt is **not adopted**. Both complete pairs, source diff, executable
and source snapshots are retained in `line-reuse-pairs` / `line-key-attempt`,
with `unadopted-evidence.json` retaining both comparisons. The incomplete third
pair was stopped and excluded once the decision was resolved.

The corrected key excludes only that nonsemantic restart field when blinking
is disabled. It reuses 2,575,818/3,118,800 jobs. `line-reuse-key-pairs` retains
three alternating built-game pairs against the preceding qualified native job:

| Pair | Previous native CPU ms/field | Completed-line job CPU ms/field | CPU reduction |
| --- | --- | --- | --- |
| 1 | 2.042535 | 1.290871 | 36.80% |
| 2 | 2.070839 | 1.238239 | 40.21% |
| 3 | 2.070593 | 1.235873 | 40.31% |

Median incremental CPU reduction is **40.21%**, with complete canonical guest
contracts equal in every warm/retained comparison. Do not add this percentage
to the earlier combined-package percentage. Differential component coverage
now includes 50,688 line jobs and 4,792 observed reuse hits, direct source-byte
mutations, CE rewrites across publication, controls, geometry and retained
interlaced storage. Windows runtime tests and Linux GCC portable LLE/HLE
fixtures/parity pass. Final consumer/frontend integration is recorded below.

### Supported consumer and final direct comparison

The supported `HotelMarioRecomp/tools/build.ps1 -Disc ...` pipeline rebuilt
the current generator, regenerated all 174 executable modules using the tracked
SHA-bound catalog, and rebuilt the consumer. Default consumer video selection
is HLE; `-VideoBackend LLE` retains faithful scanout. `-StaticModuleEntries`
selects static-only discovery; `-ModuleSeeds` supplies an alternative catalog.
Linux build glue uses the same default, with `CDI_VIDEO_BACKEND=LLE` available.
The shared framework's default remains LLE. Native kernel/save behavior is
unchanged; owner settings and saves were preserved.

`final-combined-sdl/campaign-evidence.json` compares that supported build
directly against original-catalog/LLE video, using the actual frontend with
SDL dummy video/software rendering and dummy PCM output:

| Comparison | Original CPU ms/field | Final supported CPU ms/field | CPU reduction |
| --- | --- | --- | --- |
| Warm | 5.224511 | 1.368221 | 73.81% |
| Retained | 5.704861 | 1.399498 | **75.47%** |

Both complete canonical guest contracts pass, including every frame from
1 through 13,000. This direct result is one warmed retained pair; the individual
frame, CPU-coalescing and completed-line packages separately have three
alternating pairs. Do not present the one final pair as a three-pair result or
add incremental percentages. Final binary size is 33,892,868 versus 29,908,046
bytes; sampled peak RSS is 216,723,456 versus 207,187,968 bytes (about 9.5 MB
more). The line cache is bounded to the maximum 560 rows. Host occupancy was
76.8%/80.9%; retain the raw samples and avoid quiet-host latency claims.

The tracked machine-readable index is [hle-campaign-evidence.json](hle-campaign-evidence.json).
It records each package, per-game measurements, source/core/consumer heads,
binary identities, raw evidence/identity hashes, limits and every candidate's
disposition/prerequisite. The raw root retains settings, all samples, RAM,
frames, sources/diffs, binaries, component logs and the resource ledger.
Original link-time sidecars are retained when source snapshots are relocated;
each relocated source must match its original link-time hash.

### Prior deferral notes — reopened, not completion evidence

The post-line-job profile (`profile-final.json`) contains 344 gameplay-only
main-thread samples. It finds 60 in instruction-entry tracing, 47 in line-job
qualification, 32 in frame publication and 40 in board-event advancement.
Named game-native bodies account for 16 samples; named ROM-native bodies for
7. Leaf sampling cannot attribute all indirect service cost or certify other
workloads. It selects useful work; it is not an A/B result.

- **3, graphics/drawing preparation:** the earlier interpreter hotspot at
  module offset `0x17018` processes a dynamic callback queue, including guest
  callback invocation and queue-pointer updates. It is not an established
  single blit/copy contract. The renderer portion is delivered in 1/2 and legal
  native continuations in 7. Before replacing the broader guest job, capture
  a demonstrably expensive drawing/callback operation at its caller boundary,
  including queue ownership/head/tail, callback effects, interrupts and output
  memory; build a boundary differential fixture and an exercising workload.
- **4, memory/publication:** the run has only five native bindings and one
  invalidation, all before the timed interval. Native write bookkeeping has
  13/491 initial and 10/344 final samples; this is not demonstrated repeated
  load/hash churn. A replacement needs a measured bulk-copy/fill/loading case
  plus overlap, partial-fault, interrupt-visible memory and code-invalidation
  contracts at the complete operation boundary.
- **5, loader pipeline:** all five bindings occur by field 7,072; the timed
  gameplay interval contains none. Startup output/state equality is retained,
  but gameplay gains cannot establish loader performance. Prerequisite: matched
  loading/transition measurements covering actual Load/Link/Unlink outcomes,
  relocation, error, unload and authoritative kernel/module state.
- **6, OS-9 services:** current sampling identifies no specific costly service,
  and seven named ROM-body samples do not establish a service ABI. Prerequisite:
  service-boundary cost attribution and a hot-service corpus including success,
  errors, interrupted/reentrant execution, caller registers, kernel structures
  and native-save effects. OS HLE remains an allowed implementation option.
- **8, CD/XA streaming:** existing native jobs decode 2,259 audio sectors and
  6,181,056 sample frames with exact PCM identity. Decode appears in only 2/491
  initial samples. Remaining CDIC work includes observable buffer-pointer and
  IRQ advancement. Prerequisites: a seek/channel-switch/refill workload,
  DMA/IRQ/locator/ownership timelines, normal-speed PCM continuity qualification
  and the event-span contract in 9. Existing audio-listening certification
  remains open under the separate game acceptance work.
- **9, board advancement:** a bulk-cycle sum fails a concrete timing identity:
  24 four-cycle quanta advance slave/CDIC time by **6,336 ns**, while a single
  96-cycle quantum advances it by **6,355 ns**. `slave_increment_time` can also
  activate CDIC before its update in the same quantum. Timer IRQs and RTC/raster
  phases remain observable. Prerequisites: earliest-event/frontier APIs across
  timer, IKAT, CDIC and raster; preserve summed per-quantum truncated time and
  RTC/raster phase; flush prior elapsed time before MMIO/visible effects;
  preserve activation/order/IRQ boundaries. Deadline, seek, audio and IRQ
  differential fixtures and a built-game span A/B must qualify that job.
- **10, hardware presentation extension:** redundant copy/upload is delivered
  and frontend-qualified. A hardware-driver optimization still needs a matched
  actual-window/backend profile, resize/expose behavior and pixel readback,
  while preserving owner focus and presentation settings. Dummy/software
  evidence does not establish GPU-driver performance.

Future native OS/memory/drawing jobs should compare caller-visible inputs,
outputs, authoritative guest state, errors and hardware time/events at the
complete job boundary. They may change interior instruction execution. The
exact instruction-history comparison used in this pass is appropriate to its
CPU-preserving frame jobs and faithful AOT changes; it does not constrain
future HLE to retain the reference algorithm.

### Reopened serial work: candidate 5

Owner rejected the premature deferral closure and requested actual work,
serially with a check-in after each candidate. Candidate 5 is active; the
other unfinished investigations remain queued. Owning shared-core dispatch
hook issue: `beads-i8bk` (core `cd1e5e2`).

Actual loading attribution and a held live kernel call identify verification
of the 112,984-byte main module. Its SR is `$2000`; it admits interrupts and
performs real process scheduling checks every 256 words. The prototype uses
native polynomial folding over a ready verification span, preserves the
original device-time quanta, and materializes original CPU state at interrupt
or debugger frontiers. Scheduling, file I/O, module registration/linking and
errors remain authoritative kernel operations. Unsupported firmware, memory
shapes and instruction-observation modes retain faithful execution.

The first prototype exposed a sign-extended MOVEQ constant error and is not
adopted. After correction, 100,000 independent polynomial/register/flag checks
pass on Windows and Linux. A production-object fixture compares the native
job with the actual recompiled firmware in 74 cases, covering exact timed
interrupt frontiers, unsupported firmware, odd-source fallback and a real
bus error. All 13 runtime tests, seven compiler tests and the existing
function-alias integration check pass. The production source also passes
Linux GCC strict compilation with HLE both enabled and disabled.

Matched built-game comparisons reach the actual main module's first native
entry, `$25F312`, at field 1,145 after real loading, verification and linking.
They compare every CPU register/flag, full 1 MiB guest RAM, video/audio/device
state, native binding/unbinding events and every completed frame. The gameplay
extension passes through field 13,000, including all five observed module
bindings and the actual unload at field 2,634. A deliberately corrupted copy
of the main-module payload exercises the real kernel's CRC rejection at
`$406B4C`: both builds return error `$E8` with carry set and identical complete
contracts. The original disc was never modified; the disposable copy is
removed after retaining its byte-change recipe, hashes and raw results.

Short-interval process CPU time proved too coarsely quantized: an identical
build's loading CPU samples differed by up to 38%. After reassessing those
measurements, the harness retains raw Windows process-cycle counts as well as
CPU time, with both processes restricted to logical CPU 15. Only task-created
processes receive that affinity. No cycle-to-time conversion is used.

| Pair | Reference process cycles | Native process cycles | Reduction |
| --- | --- | --- | --- |
| 1 | 474,821,260 | 316,634,143 | 33.32% |
| 2 | 481,901,888 | 369,119,827 | 23.40% |
| 3 | 495,179,075 | 376,439,108 | 23.98% |

These three warmed, alternating pairs measure fields 1,000 through the first
native module entry. Median loading cycle reduction is **23.98%**, versus
roughly 1.6–5.2% variation in two identical-build loading controls. This is
process work during the measured loading interval, not a disc-latency or
whole-game speedup claim. The initial 51% CPU-time probe is not the final claim.

Later gameplay (fields 9,400–13,000) is **neutral/inconclusive**: CPU differences
are −1.05%, +7.32%, +1.00%; process-cycle differences are −8.54%, +8.87%, +0.46%.
Controls and earlier noisy series are all retained. No reproducible gameplay
regression is established, and no gameplay gain is claimed. The supported SDL
dummy/software frontend also matches every contract; its one retained pair is
−4.22% CPU / −3.44% cycles, within control variation. This does not qualify a
hardware GPU driver, other stages, games, or Linux runtime performance.

The supported consumer pipeline builds both `-LoaderBackend LLE` and `HLE`;
HLE is the consumer default, while the generic engine default remains LLE.
Linux uses `CDI_LOADER_BACKEND`. The rebuilt LLE control matches the original
qualified package through field 13,000 and records zero native loader words.
The accepted HLE run verifies 38,986 words in 286 spans, with 112 interrupt
handoffs and 161 kernel scheduling-check handoffs. Those checks are not all
actual process switches. File I/O, allocation, relocation, linking, unload,
registration and errors continue through the real kernel. This is a measured
native verification job integrated into that complete pipeline, not a claim
that every `F$Load` operation has been rewritten.

Implementation: engine `d7a3403`, shared core `cd1e5e2`; complete build-input
provenance `050969a`. Consumer integration: `cf5a4ed`. Private native-job headers
are now included in link-time provenance. The final provenance-only relink is
checked for byte-identical PE sections against the measured and frontend-tested
binaries. Exact identities, raw samples, settings and limitations are retained
in [loader-job-evidence.json](loader-job-evidence.json). Owner saves/settings
and delivery directories were untouched; all automated launches were muted.

Candidate 5's selected job is qualified. The broader burndown stays open;
candidates 3, 4, 6, 8, 9 and hardware presentation have not been started as part
of this serial step. Stop here for owner review.

Central Beads issue updates are saved locally. The required remote push was
allowed to finish and failed after 507.6 seconds; its log/result paths are
retained in `loader-job-evidence.json`. Remote backup sync is an administrative
gap, separate from this candidate's completed engineering qualification.

Evidence: `G:/Temp/Codex/cdirecomp/load-job-20261028`.

### Candidate 3 checkpoint — owner-requested pause, 2026-10-28

Native deferred drawing orchestration is implemented on
`perf/native-drawing-jobs-20261028`, source `faf11af`. It executes the actual
drawing callbacks and publishes authoritative queue/buffer state, preserving
device quanta, IRQs, invalidation and faults. Independent build-time drawing
selection is integrated in the consumer on
`perf/native-drawing-job-20261028`, `a9052e7`. Generic default remains LLE.

All 58 actual generated-reference differential cases pass, including bitmap
painting and repeated CPU materialization followed by continuation. All 13
runtime tests, seven compiler tests and function-alias integration pass.
Final Linux HLE=0/HLE=1 strict compilation passes.

Three warmed alternating same-source HLE/LLE gameplay pairs match complete
CPU/RAM/device/audio state and every frame through 13,000. Process-cycle
reductions are 5.88%, 7.25%, 5.82% (median **5.88%**), against identical-build
controls of +2.86% and −0.43%. Observed CPU reductions are 5.85%, 8.01%, 7.40%,
but CPU controls vary by up to 7.99%, so CPU time alone does not prove the gain.
The final SDL dummy/software warm and retained contracts also match; its
retained −2.78% CPU/−1.73% cycle difference is **neutral/inconclusive** within
control variation. No whole-game, other-title or hardware-driver claim.

Earlier comparisons with the older binary also made rebuilt LLE look faster;
those gains are not attributed to this native job. The first direct native
experiment was mixed. The second publishes only changed registers instead of
copying the full CPU repeatedly; all earlier raw results remain retained.

Machine-readable evidence: [drawing-job-evidence.json](drawing-job-evidence.json).
Raw root: `C:/Users/Matthew/AppData/Local/Temp/Codex/cdirecomp/drawing-job-20261028`.
Code and supported integration are committed locally. The existing three PRs
remain open and unmerged; no candidate-3 PR has been opened. Final review,
narrative/Beads handoff and disposable-scratch cleanup are pending continuation.
Builds, generated sources and immutable snapshots are retained for that handoff.
Owner saves/settings and delivery folders were untouched; QA was muted and
background-only. No other candidate was started. **Work is paused at the
owner's request; resume only after explicit continuation.**

### Candidate 5 completion re-audit and target pool, 2026-10-30

The reactivated bounded goal names candidate 5 only. Reused its accepted
evidence: all five retained binaries and their complete link-time source
inputs verify, the three PR heads are unchanged, and equality was recomputed
from 40 retained raw runs covering loading, corrupt-module errors, later
bindings/unload, gameplay and the SDL software frontend. The 74 production
firmware differential cases and supporting test logs remain intact. No new
game runs, builds or optimization experiments were needed. The measured
loading result remains **23.98% fewer process cycles**; later gameplay remains
neutral/inconclusive. The native verification job is integrated in the real
load-to-ready pipeline, whose I/O, allocation, relocation, linking, unload,
scheduling and errors remain authoritative kernel operations.

Retired another 96,540,256 bytes of obsolete build objects and duplicate
staging outputs. Preserved `hle-build/inputs` at its original path because
passing receipts reference its frozen consumer CMake source. Passing binaries,
raw samples, native saves/settings and the separately paused candidate-3
workspace remain intact. Audit, cleanup and PR evidence are indexed in
[loader-job-evidence.json](loader-job-evidence.json).

Use the installed shared capacity pool for future compatible workloads:
this PC, SERVER and Steam Deck SSH. The client is
`C:/Users/Matthew/.local/bin/benchmark_hosts.py`, invoked with
`C:/Users/Matthew/.pyenv/pyenv-win/versions/3.12.5/python.exe`.
`probe` measures capacity; `run --os windows|linux --owner beads-oatv
--job-file <JSON>` reserves realistic CPU/GPU/RAM/workspace budgets and rotates
compatible targets. Use exact JSON `argv`/`env` on Windows. Both Deck SSH
aliases share one physical-host ledger, with existing identities and strict
host-key checks. Other games, compilers and services do not automatically
exclude a target. Keep every matched pair on one host/configuration and
retain telemetry and identities. Missing required telemetry does not pass;
Deck timing requires a verified Linux build/runtime/backend first.

Within authorized scope, unavailable capacity must not stall preparation:
prepare isolated prototypes, switches, fixtures, frozen baselines and exact
runnable jobs with realistic budgets for later testing. Keep prepared, built,
tested and accepted states distinct, and preserve each item's independent
serial measurement/disposition gate. This does not resume owner-paused work,
permit extra subagents, bypass reservations or qualify unmeasured changes.

All three hosts passed today's declared CPU-only capacity probe. The read-only
evidence audit also completed under a local capacity reservation; this is not
a new game qualification. Historical quiet-host gates and accepted gameplay
checks were not changed or repeated. Adding the pool does not authorize a
new benchmark matrix. **Candidate 5 remains qualified; stop at this serial
checkpoint. Candidate 3 remains paused, and no other candidate was started.**

### Candidate 11: GPU high-resolution rendering

Added at the owner's explicit request on 2026-10-31. This work was **not**
covered by the previous candidate-10 copy/upload and hardware-presentation
entry. Owning issue: `beads-ttbl.3`, related to `beads-oatv` and the optimization
campaign. Source: [GPU-HIGH-RESOLUTION-RENDERING-GUIDE.md](C:/Users/Matthew/Desktop/GPU-HIGH-RESOLUTION-RENDERING-GUIDE.md),
SHA256 `644ec4bb9efa5330351908e78e81ce9df2ebee7f71187c64f3cf19c27f8521bd`.
Status is **tracked only**, with no GPU implementation or high-scale result
claimed. The owner has resumed the remaining burndown; historical pause and
checkpoint notes above describe earlier stops.

The required outcome is **16x internal resolution at full native cadence with
measured headroom on the owner's PC first**. Desired outcomes are 32x and 64x;
16x on Steam Deck and server/lower-end hardware follows later. Set numerical
budgets from this engine's baseline. The guide's historical approximately 90%
figure is unverified and must never be used as a target or a claimed result.

The implementation must satisfy the complete job contract:

1. Publish compact immutable raster/draw jobs at a natural boundary. Keep CPU
   preparation independent of scale; share the same preparation with exact
   native output. Reuse the existing frame, line and source-identity work.
2. Keep source data resident on the GPU and upload jobs plus changed source
   ranges. Invalidate for every guest CPU/DMA writer, reset, state restore and
   rollback. Never upload an expanded enhanced frame each field.
3. Draw ordered visible work on the GPU at the selected density. Bound targets
   and intermediates by the job/tile rather than a full scale-squared shadow
   arena. Admit high scales using actual memory/texture limits and degrade
   presentation gracefully with a logged reason.
4. Preserve native framebuffer, guest-memory and canonical state/hash results
   exactly at every scale and against the pre-change build. Preserve all guest
   framebuffer readbacks and render-to-texture behavior. Enhanced output stays
   outside canonical serialization, hashes and guest reads; invalidate its
   caches/history on current-build save/load, rollback and reset.
5. Keep enhancements local to presentation/netplay display settings by default.
   Any GPU result feeding simulation requires integer-exact cross-vendor and
   backend qualification plus mixed-peer rollback, a common profile selected
   before frame 0 and a software fallback. Do not invent asset detail.
6. Keep the same-contract portable CPU fallback and build/run it on another
   compatible target. Gate optional host features without breaking other
   supported builds. Old save-state compatibility is not required.

Qualification must retain matched built-game before/after evidence, exact
source/binary identities, settings, raw samples and host load/capacity records:

- Establish CPU cost, per-pass **GPU timestamps**, upload bytes and process/GPU
  memory at 1x, 2x, 4x and the current maximum. Separate upscaling overhead from
  native-pass cost; use GPU timestamps at 1x/4x/maximum before and after.
- Prove CPU preparation and per-frame upload do not grow with scale, while
  native pixels/RAM/state remain exact. Actually exercise current-build
  save/load, rollback, reset, guest readbacks and real window resize.
- Qualify required 16x speed/headroom and bounded high-scale memory/admission
  on the owner's PC first. Keep desired/lower-end results distinct from the
  required acceptance. Enlarging a native SDL texture alone does not establish
  the guide's internal-resolution and resident-job contract.
- Read the in-house reference commits in `F:/Projects/gbarecomp/gbarecomp`:
  `4edc278`, `ba32c62`, `f646f81`, `ffa4f2f`, `4ea8f55`, `a385e0b`, `4261f07`.
  Design from hardware documentation, this ecosystem and that in-house work;
  do not copy, translate or cite third-party renderer source or embed the
  independent accuracy oracle into the shipping path.

Candidate 10's remaining hardware-driver checks may be consolidated into this
implementation only after the shared actual-window/backend profile, resize,
expose and pixel-readback contracts pass. Neither dummy/software evidence nor
this added checklist completes either candidate. Apply the independent final
subagent keep/fix/reject review and branch-PR handoff before moving on.

### Candidate 3 final independent review, 2026-10-31

Independent review found that a delayed instruction stop activated inside a
drawing callback, or a future-PC stop armed during a device clock, could be
skipped by native orchestration. Both failures were reproduced through the
real TCP debugger before repair. Source `49c6b21` publishes an observation epoch
after stop changes and checks it before each private operation. It materializes
the exact CPU prefix and resumes faithful execution at the current operation,
with code invalidation retaining priority. Both held-state regression cases
now match registers, full 1 MiB RAM and retained device state; each verifies
actual native admission and one faithful fallback. The 58 production/reference
cases and final Linux HLE=0/HLE=1 strict checks also pass.

Fresh matched fixed-source comparisons use the supported LLE and HLE builds,
both with the previous qualified frame/loader packages. Three headless pairs
have process-cycle differences **−2.45%, +9.50%, +7.44%**; the contemporaneous
identical-LLE control varies **+10.01%**. CPU differences are −2.51%, +8.89%,
+7.29%, against +8.72% control variation. The SDL dummy/software retained pair
is +6.47% CPU/+6.35% cycles. All results are **neutral/inconclusive** within
control variation: neither positive medians nor the earlier pre-fix gain
establish a current speedup. No reproducible regression is shown.

All eight final warm/retained comparisons match full guest contracts and every
frame through 13,000. The independent reviewer recomputed raw comparisons,
RAM hashes, identities, activity and lease evidence and recommends **KEEP**
under the owner's neutral-per-game rule. This is a bounded complete native
queue service with exact-identity admission, real callbacks, authoritative
state and an independent faithful fallback; retention is not a speedup claim.
Consumer integration `2123474` pins the fix. Passing source/binary identities,
review and final raw evidence are indexed in
[drawing-job-evidence.json](drawing-job-evidence.json). No PR is merged.

The concrete [ready-to-test queue](hle-ready-to-test.json) separates preparation,
builds, tests and acceptance. Candidate 4 already has an exact loading-phase
profiling job and realistic resource budget prepared; it has not been run or
implemented. Remaining candidates retain their individual measurement and
independent final review gates. Continue from candidate 3's PR handoff to the
next candidate without repeating accepted gameplay checks.
