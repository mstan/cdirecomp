"""The BIOS frontend must emit to the requested project, from any cwd."""
import pathlib
import subprocess
import sys
import tempfile

exe = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="cdi-bios-output-") as directory:
    root = pathlib.Path(directory)
    rom = bytearray(b"\xff" * 512)
    rom[:8] = bytes.fromhex("0007fff000400100")
    rom[256:260] = bytes.fromhex("4e7160fc")  # nop; bra back to nop
    source = root / "synthetic.rom"
    source.write_bytes(rom)
    output = root / "project output"
    subprocess.run([str(exe), str(source), "--emit", "--out", str(output)],
                   cwd=root, check=True, capture_output=True)
    for filename in ("cdrtos_full.c", "cdrtos_dispatch.c", "bios_identity.h"):
        assert (output / filename).is_file(), filename
    assert not (root / "bios").exists()
    assert subprocess.run([str(exe), str(source), "--emit"], cwd=root,
                          capture_output=True).returncode != 0
    assert subprocess.run([str(exe), str(source), "--out"], cwd=root,
                          capture_output=True).returncode != 0
