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

## Command surface (planned — MC-CDI-015)

```
ping              frame             get_registers      read_mem / write_mem
history           get_frame         frame_range        frame_timeseries
pause             continue          run_frames N       quit
dispatch_miss_info                  os9_call_log       sector_log
mcd212_state      cdic_state        slave_state        screenshot
frame_diff        memory_diff       first_divergence   framebuf_diff
```

Reverse-debugger tiers (per-store / per-block / per-call attribution,
breakpoints, watchpoints, RAM reconstruction) follow the nesrecomp/Genesis
`rdb_*` design once the basic surface is up.

## Oracle commands (CeDImu)

The oracle exposes the same ring + query commands plus emulator-internal probes
(`emu_cpu_regs`, `emu_mcd212_state`, `emu_screenshot`, `framebuf_diff`) so
native-vs-oracle diffing is symmetric.

## Adding a command

1. Add a handler in `runner/src/debug_server.c`; register it in the dispatch
   table.
2. Mirror it on the oracle side if it inspects emulator-internal state.
3. Document it here. Never add a side-channel log instead.
