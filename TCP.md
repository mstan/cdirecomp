# TCP.md — debug server protocol

The TCP debug server is the only sanctioned debugging interface. If a piece of
state isn't observable over TCP, extend `runner/src/debug_server.c` — never work
around it with printf.

## Ports

- Native runtime: **127.0.0.1:4380**
- Oracle (CeDImu, in-process): **127.0.0.1:4381** (native, +1)

Configurable per build; native and oracle are separate listeners so a single
client can diff them.

## Transport

- TCP localhost, line-based, one command per line terminated by `\n`.
- JSON request preferred: `{"cmd":"get_frame","frame":1234,"id":7}`; bare
  commands accepted for the simplest cases (`ping\n`). One client at a time.
- Single-line JSON response: `{"ok":true,...}` / `{"ok":false,"error":"..."}`.

## Always-on ring buffer

`debug_server.c` records a frame snapshot every frame into a 36000-entry ring
(~10 min @ 60 Hz). All retroactive inspection reads from this ring — you never
arm capture at probe time. Per frame (target shape; being filled in as the
runtime grows, MC-CDI-015):

- SCC68070 registers (D0-D7, A0-A7, SR, PC, USP)
- MCD212 plane A/B + region control state, display status
- CDIC state (current sector, audio status, IRQ flags)
- SLAVE input report
- A system-RAM snapshot + last recompiled function name
- OS-9 call counter / last service

## Command surface

**Live now** (threaded server in `debug_server.c`):

```
ping                 -> {ok,pong}
status               -> {ok,insns,blocks,frame,pc,miss_count,miss_last,irq_pending}
get_registers        -> {ok,pc,sr,usp,d0..d7,a0..a7}
read_mem  addr[,len] -> {ok,addr,bytes:"HEX.."}  side-effect-free (RAM/ROM; "--" for MMIO)
trace     [count]    -> {ok,total,records:[{seq,pc,sr,a7}..]}  block-trace ring tail
dispatch_miss_info   -> {ok,count,last_addr,last_frame,unique:[..]}
quit                 -> {ok,bye}  closes the connection
```

**Planned** (MC-CDI-015, grow as the runtime grows):

```
write_mem         get_frame / frame_range / frame_timeseries     screenshot
pause / continue / run_frames N      os9_call_log     sector_log
mcd212_state      cdic_state         slave_state
frame_diff        memory_diff        first_divergence  framebuf_diff
```

Reverse-debugger tiers (per-store / per-block / per-call attribution,
breakpoints, watchpoints, RAM reconstruction) follow the nesrecomp/Genesis
`rdb_*` design once the basic surface is up.

## Oracle (CeDImu) — live

`oracle/cdi_oracle.cpp` links the wxWidgets-free CeDImu core and serves the SAME
surface on **:4381**: `ping status get_registers read_mem trace
dispatch_miss_info quit` (status carries `"oracle":true`; `dispatch_miss_info` is
always 0 — an interpreter never misses). Both sides capture one trace entry per
executed instruction, so the PC streams are index-alignable and
`tools/first_divergence.py` pages both from seq 0 to find the first mismatch.
Emulator-internal probes (`emu_mcd212_state`, `emu_screenshot`, `framebuf_diff`)
are added as the device models come up.

Build (needs the mingw64 cmake — the devkitPro cmake on PATH mangles Windows
paths; see docs/ARCHITECTURE.md):

```
C:/msys64/mingw64/bin/cmake.exe -S oracle -B build/oracle -G Ninja -DCMAKE_BUILD_TYPE=Release
C:/msys64/mingw64/bin/cmake.exe --build build/oracle -j
build/oracle/CdiOracle.exe bios/cdi490a.rom --steps 100000 --hold
```

## Adding a command

1. Add a handler in `runner/src/debug_server.c`; register it in the dispatch
   table.
2. Mirror it on the oracle side if it inspects emulator-internal state.
3. Document it here. Never add a side-channel log instead.
