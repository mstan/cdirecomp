#!/usr/bin/env python3
"""Synthetic integration test for conservative 68000 function aliasing."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


def be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def main() -> int:
    repo = Path(str(__file__).replace("\\", "/")).resolve().parents[1]
    default_exe = repo / "build" / "recompiler" / "CdiRecompFrontendFixture.exe"
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", type=Path, default=default_exe)
    args = parser.parse_args()
    exe = args.recompiler.resolve()
    if not exe.is_file():
        raise SystemExit(f"frontend fixture executable not found: {exe}")

    with tempfile.TemporaryDirectory(prefix="cdi_alias_test_") as td:
        root = Path(td)
        (root / "generated").mkdir()

        rom = bytearray(b"\xFF" * 0x400)
        be32(rom, 0, 0x00FFFE00)
        be32(rom, 4, 0x00000200)
        rom[0x100:0x200] = b" " * 0x100
        rom[0x120:0x12A] = b"ALIAS TEST"

        # A branch-reachable callable suffix must share the enclosing body.
        rom[0x200:0x20C] = bytes.fromhex(
            "7000"      # moveq #0,d0
            "6004"      # bra.s $208
            "4e71"      # unreachable nop
            "ffff"      # unreachable illegal/data word
            "4e71"      # $208 alias entry: nop
            "4e75"      # rts
        )

        # An unbounded walk that reaches an illegal MC68000 encoding is unsafe
        # alias evidence, so these two starts must remain hard boundaries.
        rom[0x220:0x226] = bytes.fromhex(
            "4e71"      # $220 nop
            "4e71"      # $222 configured entry
            "1040"      # illegal MOVEA.B D0,A0
        )

        # Two legal instruction streams may overlap at different even decode
        # boundaries. The interior stream stays canonical (not an alias of the
        # outer ORI.L-sized instruction), while its own linear suffix may alias.
        rom[0x240:0x24A] = bytes.fromhex(
            "0079205f74001018"  # $240 ori.w #$205f,$74001018
            "4e75"              # $248 rts
        )

        # A nested RTE/RTR must unwind every generated JSR frame to the
        # depth-zero trampoline; skip-RTS remains a one-frame unwind.
        rom[0x260:0x268] = bytes.fromhex(
            "4eb900000270"      # jsr $270
            "4e75"              # rts
        )
        rom[0x270:0x272] = bytes.fromhex("4e73")  # rte

        # ROXR.L needs a 33-bit X:Dn ring. Cover both immediate and register
        # count forms so codegen cannot regress to uint32_t X<<32 or ignore the
        # runtime count register.
        rom[0x280:0x284] = bytes.fromhex("e2904e75")  # roxr.l #1,d0; rts
        rom[0x290:0x294] = bytes.fromhex("e2b04e75")  # roxr.l d1,d0; rts

        # A paired MOVE.W/JMP PC-indexed offset table may dispatch backward
        # into handlers owned by an earlier function range. Those handler PCs
        # must be promoted to real entries instead of remaining unresolved
        # hybrid interior targets.
        rom[0x2A0:0x2A2] = bytes.fromhex("4e75")
        rom[0x2B0:0x2B8] = bytes.fromhex(
            "7201" "4e75"       # case 0
            "7202" "4e75"       # case 1
        )
        # Group-$B mode bits 001 are CMPM's special encoding, not an
        # address-register-direct EOR. Exercise the exact form used by the
        # CD-RTOS string comparator that exposed the overlap.
        rom[0x2C0:0x2C4] = bytes.fromhex("b50b4e75")  # cmpm.b (a3)+,(a2)+; rts
        # Byte predecrement/postincrement on A7 uses a two-byte stack step.
        rom[0x2D0:0x2D6] = bytes.fromhex(
            "1f07"               # move.b d7,-(a7)
            "101f"               # move.b (a7)+,d0
            "4e75"               # rts
        )
        # Same-register CMPM makes the architectural read/increment ordering
        # observable: destination must read from old A7+2, not old A7.
        rom[0x2D6:0x2DA] = bytes.fromhex("bf0f4e75")
        # Extended-arithmetic edge cases found by the BIOS co-sim.
        rom[0x2DA:0x2DE] = bytes.fromhex("40824e75")  # negx.l d2; rts
        rom[0x2DE:0x2E2] = bytes.fromhex("d1824e75")  # addx.l d2,d0; rts
        rom[0x300:0x310] = bytes.fromhex(
            "d040"               # add.w d0,d0
            "303b0006"           # move.w table(pc,d0.w),d0
            "4efb0002"           # jmp table(pc,d0.w)
            "ffa6" "ffaa"       # table -> $2B0, $2B4
            "ffff"               # next offset resolves odd: table end
        )

        # OS-9 TRAP #0 has an inline service selector that is data, not an
        # instruction. Its post-RTE continuation must remain inside the
        # canonical caller and be available in the async native resume map.
        rom[0x320:0x32C] = bytes.fromhex(
            "7000"               # moveq #0,d0
            "4e40"               # trap #0
            "0053"               # inline I$WritLn service selector (data)
            "6602"               # $326 bne.s $32A
            "7001"               # moveq #1,d0
            "4e75"               # rts
        )
        (root / "alias.bin").write_bytes(rom)
        (root / "code_addrs.txt").write_text(
            "000200\n000202\n000204\n000208\n00020A\n000220\n000222\n"
            "000240\n000242\n000244\n000246\n000248\n"
            "000260\n000266\n000270\n000280\n000282\n000290\n000292\n"
            "0002A0\n0002B0\n0002B2\n0002B4\n0002B6\n0002C0\n0002C2\n"
            "0002D0\n0002D2\n0002D4\n0002D6\n0002D8\n"
            "0002DA\n0002DC\n0002DE\n0002E0\n"
            "000300\n000302\n000306\n"
            "000320\n000322\n000326\n000328\n00032A\n",
            encoding="ascii",
        )
        (root / "game.toml").write_text(
            """[game]
output_prefix = "alias_test"
code_addrs_file = "code_addrs.txt"
function_aliases = true
async_resume_entries = true
trap0_inline_service_word = true

[functions]
extra = [0x000200, 0x000206, 0x000208, 0x000220, 0x000222,
         0x000240, 0x000242, 0x000246, 0x000260, 0x000270,
         0x000280, 0x000290, 0x0002A0, 0x0002C0, 0x0002D0, 0x0002D6,
         0x0002DA, 0x0002DE,
         0x000300, 0x000320]

[ram_layout]
game_mode = 0
vint_runcount = 0
vint_routine = 0
plc_pending = 0
initial_ssp = 0x00FFFE00
vbla_stack = 0
intr_stack = 0
player_object = 0
level_modes = []
""",
            encoding="ascii",
        )

        dump = root / "functions.txt"
        proc = subprocess.run(
            [str(exe), "alias.bin", "--game", "game.toml",
             "--dump-functions", str(dump)],
            cwd=root,
            text=True,
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if proc.returncode:
            raise AssertionError(
                f"frontend fixture failed ({proc.returncode}):\n{proc.stdout}"
            )

        full = (root / "generated" / "alias_test_full.c").read_text(
            encoding="utf-8"
        )
        dispatch = (root / "generated" / "alias_test_dispatch.c").read_text(
            encoding="utf-8"
        )
        manifest = dump.read_text(encoding="utf-8")

        assert "void func_resume_000200(uint32_t _entry)" in full
        assert "void func_000200(void) { func_resume_000200(0x000200u); }" in full
        assert "void func_000208(void) { func_resume_000200(0x000208u); }" in full
        assert "# alias 000208 -> 000200" in manifest

        assert "void func_000220(void) {" in full
        assert "void func_000222(void) {" in full
        assert "# alias 000222" not in manifest

        assert "void func_000240(void) {" in full
        assert "void func_000242(void) {" in full
        assert "void func_000246(void) {" in full
        assert "# alias 000242 -> 000240" not in manifest
        assert "Unsupported summary:" in proc.stdout
        assert "TOTAL                            0" in proc.stdout
        assert "Overlapping-decode entry audit: 2 warning(s)" in proc.stdout

        assert "if (g_rte_resume) return; /* RTE/RTR: unwind to depth-zero trampoline */" in full
        assert "return; } /* skip-RTS: one-level propagation (pre-pop) */" in full

        assert "uint64_t _wide = (uint64_t)_sv | (_x << 32);" in full
        assert "uint32_t _raw_cnt = g_cpu.D[1] & 63u;" in full
        assert "uint32_t _wide = ((uint32_t)_sv) | (_x << 32);" not in full

        cmpm = full[full.index("/* $0002C0 */"):full.index("/* $0002C2 */")]
        assert "_cmpm_s = (uint8_t)m68k_read8(g_cpu.A[3])" in cmpm
        assert "_cmpm_d = (uint8_t)m68k_read8(g_cpu.A[2])" in cmpm
        assert "g_cpu.A[3] += 1;" in cmpm
        assert "g_cpu.A[2] += 1;" in cmpm

        predec_a7 = full[full.index("/* $0002D0 */"):full.index("/* $0002D2 */")]
        postinc_a7 = full[full.index("/* $0002D2 */"):full.index("/* $0002D4 */")]
        assert "g_cpu.A[7] -= 2;" in predec_a7
        assert "g_cpu.A[7] += 2;" in postinc_a7

        cmpm_a7 = full[full.index("/* $0002D6 */"):full.index("/* $0002D8 */")]
        source_read = cmpm_a7.index("_cmpm_s = (uint8_t)m68k_read8(g_cpu.A[7])")
        source_inc = cmpm_a7.index("g_cpu.A[7] += 2;", source_read)
        dest_read = cmpm_a7.index("_cmpm_d = (uint8_t)m68k_read8(g_cpu.A[7])")
        dest_inc = cmpm_a7.index("g_cpu.A[7] += 2;", dest_read)
        assert source_read < source_inc < dest_read < dest_inc, cmpm_a7
        assert cmpm_a7.count("g_cpu.A[7] += 2;") == 2

        negx = full[full.index("/* $0002DA */"):full.index("/* $0002DC */")]
        assert "int _zold = (g_cpu.SR >> 2) & 1;" in negx
        assert "uint64_t _subtrahend = (uint64_t)(uint32_t)" in negx
        assert "if (_subtrahend != 0)" in negx
        assert "== (uint32_t)0x80000000u && !_x" in negx

        addx = full[full.index("/* $0002DE */"):full.index("/* $0002E0 */")]
        assert "uint64_t _full = (uint64_t)_fa + (uint64_t)_fb + (uint64_t)_fx;" in addx
        assert "if (!_fr && _zold)" in addx
        assert "if (_full >> 32)" in addx

        assert "{ 0x0002B0u, func_0002B0 }" in dispatch
        assert "{ 0x0002B4u, func_0002B4 }" in dispatch
        audit = (root / "generated" / "alias_test_dispatch_audit.log").read_text(
            encoding="utf-8"
        )
        assert "INTERIOR_UNRESOLVED      0" in audit
        assert "$000306  in func $000300  base $00030A  kind=fallback_hybrid/offset_table" in audit

        # Explicit roots do not override the literal code-address oracle.
        assert "func_000206" not in full
        assert "func_000206" not in dispatch
        assert "{ 0x000200u, func_000200 }" in dispatch
        assert "{ 0x000208u, func_000208 }" in dispatch

        # Every emitted instruction boundary has a native resume row owned by
        # its canonical body, without manufacturing another callable function.
        assert "{ 0x000202u, func_resume_000200 }" in dispatch
        assert "void func_000202(void)" not in full
        assert "int game_dispatch_has_addr(uint32_t addr)" in dispatch

        assert "case 0x000326u: goto label_000326;" in full
        assert "{ 0x000326u, func_resume_000320 }" in dispatch
        assert "/* $000324 */" not in full
        assert "void func_000326(void)" not in full

        # The shared frontend defaults this platform-specific facility off.
        # Re-run the same fixture with it disabled and retain the mature
        # Genesis alias shape (one shared body only where aliases exist).
        cfg_path = root / "game.toml"
        cfg_path.write_text(
            cfg_path.read_text(encoding="ascii").replace(
                "async_resume_entries = true",
                "async_resume_entries = false",
            ),
            encoding="ascii",
        )
        proc_off = subprocess.run(
            [str(exe), "alias.bin", "--game", "game.toml"],
            cwd=root,
            text=True,
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if proc_off.returncode:
            raise AssertionError(
                f"frontend fixture (async resume off) failed "
                f"({proc_off.returncode}):\n{proc_off.stdout}"
            )
        full_off = (root / "generated" / "alias_test_full.c").read_text(
            encoding="utf-8"
        )
        dispatch_off = (root / "generated" / "alias_test_dispatch.c").read_text(
            encoding="utf-8"
        )
        assert "static void func_body_000200(uint32_t _entry)" in full_off
        assert "func_resume_" not in full_off
        assert "func_resume_" not in dispatch_off

    print("function_aliases: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
