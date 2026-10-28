# CD-i native HLE burndown

Saved at the owner's request on 2026-10-27. The owner subsequently activated
the end-to-end slash goal; individual implementation and qualification have
resumed, working solo.

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

## Remaining candidates

These are complete jobs to evaluate. Related candidates can become one
replacement across OS, memory and device boundaries. They are not ten fixed
subsystem boundaries or a requirement to adopt every proposed optimization.

| # | Candidate | Work the native replacement would remove | Current status |
| --- | --- | --- | --- |
| 1 | **Qualify the scanout candidate already written** | Repeated decoding, transparency, contribution and composition work across pixels | **Qualified as part of the combined native frame job.** Three alternating Hotel Mario pairs save 23.0-26.0% CPU/field with complete contract equality. Scanout-only performance remains inconclusive. |
| 2 | **Complete frame production and publication** | Repeated framebuffer hashing, copying, diagnostic reconstruction and publication work | **Implemented and qualified with candidate 1.** Exact content identity, retained storage and frontend handoff remove duplicate work without changing field cadence. |
| 3 | **Graphics service to completed drawing job** | Guest drawing preparation, intermediate memory traffic, control-list construction and repeated renderer preparation | Broader architectural candidate. Could cross OS, memory and video together; requires workload attribution. |
| 4 | **Memory operation to fully published result** | Instruction-by-instruction copying/filling plus repeated module invalidation, tag maintenance and dirty tracking | Concrete repeated machinery exists; its gameplay cost remains unmeasured. |
| 5 | **File/module load to executable module ready** | Repeated sector handling, copying, validation, relocation, linking, hashing and native binding | Coherent cross-subsystem candidate. Preserve authoritative guest loader/kernel state and observable transport behavior. |
| 6 | **Frequently executed OS-9 service jobs** | Recompiled kernel algorithms and internal dispatch that a complete native operation could replace | Substantially unexplored because this repository previously prohibited OS HLE. Select services by measured cost. |
| 7 | **Native execution spans** | Repeated dispatch, interpreter handoffs, instruction bookkeeping and checks between operations with no observable intervening event | **Implemented and qualified.** Reused the existing observed native catalog, then coalesced legal PIC continuations into shared native bodies. SDK `3955bf3`; incremental 3-pair median CPU reduction 23.77% versus the seeded catalog, with exact canonical guest contracts. |
| 8 | **Complete CD/XA streaming jobs** | Repeated work across sector selection, decoding, buffer ownership, DMA/refill, guest drivers and audio handoff | Existing pieces are native; the opportunity is consolidating the complete job. Benefit unmeasured. |
| 9 | **Whole-board event advancement** | Calling timer, input transport, CD/audio, RTC, raster and pacing machinery after every instruction | Concrete candidate, lower priority: roughly 5-6% of the sampled execution. Preserve event ordering and interrupt boundaries. |
| 10 | **Frame to host presentation** | Redundant frame copies, texture uploads, conversions and presentation preparation | **Redundant copy/upload portion consolidated into candidate 2.** SDL dummy/software frontend A/B passes exact guest/frame/PCM contracts; one retained pair saves 33.2% CPU/field. Hardware-driver presentation cost remains unqualified and needs its own actual-window/backend profile. |

## Current checkpoint

- Branch: `perf/native-scanout-hle-20261027`, based on framework `f3c8d57`.
- The native frame implementation and comparison harness are being committed
  as a coherent qualified checkpoint. Preserve the retained A/B inputs.
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

The owning burndown remains active. Frame/CPU job qualification does not close
the other candidate investigations or the existing full-campaign/audio/save
acceptance gaps. The next step is to profile the remaining built execution
cost and select the next complete job, including whether unchanged source and
register state can avoid a whole scanout job, or whether board-event spans are
now the larger opportunity. Neither hypothesis is an adopted improvement.

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
It matched all differential contracts but reused only 6,480/3,118,800 jobs;
the retained pair was 2.169540 versus 2.196779 CPU ms/field. This attempt is
**not adopted**. Its complete pair, source diff, executable and source snapshots
are retained in `line-reuse-pairs` / `line-key-attempt`; later partial samples
were stopped and excluded once the decision was resolved.

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
fixtures/parity pass. Final consumer/frontend integration remains pending.
