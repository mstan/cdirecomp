# TCP.md — debug server protocol

The TCP debug server is the only sanctioned debugging interface. If a piece of
state isn't observable over TCP, extend `runner/src/debug_server.c` — never work
around it with printf.

## Ports

- Native runtime: **127.0.0.1:4380**
- Oracle (CeDImu, in-process): **127.0.0.1:4381** (native, +1)

Configurable per build; native and oracle are separate listeners so a single
client can diff them.

Both harnesses accept startup `--stop-frame N`, which parks immediately after
publishing field N for deterministic frame-domain comparison. Like
`--stop-seq`, this is a free-run startup boundary, never a lockstep command.

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
status               -> {ok,insns,blocks,frame,pc,halted,input,miss_count,miss_last,irq_pending}
pause                -> {ok,pause_requested,seq}  freeze CPU at next already-recorded trace entry
video_state          -> {ok,csr1r,csr2r,csr1w,csr2w,dcr1,dcr2,ddr1,ddr2,vsr1,vsr2,dcp1,dcp2,cursor_x,cursor_y,cursor_enabled,cursor_pattern,...}
video_frame          -> {ok,width,height,generation,argb_fnv1a}
frame_hashes [from,count] -> {ok,total,records:[{frame,width,height,argb_fnv1a,mcd_count,mcd_fnv1a}...]} completed-frame ring
video_scanline y     -> {ok,width,height,generation,y,argb}  side-effect-free ARGB8888 row
screenshot --path p  -> client-side BMP/PPM from video_scanline after an immediate one-observer pause
get_registers        -> {ok,pc,sr,usp,d0..d7,a0..a7}
read_mem  addr[,len] -> {ok,addr,bytes:"HEX.."}  side-effect-free (RAM/ROM; "--" for MMIO)
trace     [count]    -> {ok,total,records:[{seq,pc,sr,a7}..]}  block-trace ring tail
set_input mask       -> {ok,input}  dev-only host state; IKAT consumes it on emulated time
emu_ikat_state       -> {ok,input,pointer_ns,cursor_packets,out_remaining,regs}
ikat_events          -> {ok,total,events:[{seq,trace_seq,pc,frame,cycles,type,channel,data}...]}
ciap_events          -> {ok,total,oldest,events:[{seq,trace_seq,pc,frame,cycles,offset,size,write,value}...]}
disc_state           -> {ok,present,sectors,track_mode}
mount_disc path      -> {ok,present,sectors,track_mode}  validates/mounts real CUE/BIN media
eject_disc           -> {ok,present,sectors,track_mode}
dispatch_miss_info   -> {ok,count,last_addr,last_frame,unique:[..]}
quit                 -> {ok,bye}  closes the connection
```

**Planned** (MC-CDI-015, grow as the runtime grows):

```
write_mem         get_frame / frame_range / frame_timeseries
pause / continue / run_frames N      os9_call_log     sector_log
mcd212_state      cdic_state
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
`set_input` is deterministic development instrumentation, separate from the SDL
player frontend. Both feed the same transport-neutral `CDI_INPUT_*` state and
timed IKAT protocol path; neither may inject guest RAM or bypass the IKAT IMR.
The mirrored `video_state` and `video_frame` queries expose device registers and
a canonical framebuffer hash without screenshots or side-channel logs. Media
mutation commands are native-only player-development controls; their guest-
visible effects still occur through the timed IKAT channel-D model.

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
