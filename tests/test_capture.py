"""Check capture ranges and truthful I/O failures against the production writer."""
from pathlib import Path
import argparse
import os
import struct
import subprocess
import tempfile
import wave

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="cdi-capture-") as temp:
    work = Path(temp)
    exe = work / ("capture.exe" if os.name == "nt" else "capture")
    subprocess.run([args.compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                    "-I", str(root / "runner/include"), str(root / "tests/capture_harness.c"),
                    "-o", str(exe)], check=True)

    def run(mode, expected=0):
        folder = work / mode
        folder.mkdir()
        result = subprocess.run([str(exe), mode], cwd=folder, capture_output=True)
        assert result.returncode == expected, (mode, result.returncode, result.stderr)
        if expected:
            assert b"cdi capture:" in result.stderr
            assert b"done fields_written=" not in result.stderr
        return folder

    ranged = run("range")
    with wave.open(str(ranged / "audio.wav")) as stream:
        assert (stream.getframerate(), stream.getnchannels(), stream.getsampwidth()) == (37800, 2, 2)
        raw = stream.readframes(stream.getnframes())
    first = int(37800 * 2 / 59.94)
    split = int(37800 * 3 / 59.94) - first
    last = int(37800 * 4 / 59.94) - first
    assert struct.unpack("<" + "h" * (len(raw) // 2), raw) == (3, -3) * split + (4, -4) * (last - split)
    frames = sorted((ranged / "frames").glob("*.ppm"))
    assert [p.name for p in frames] == ["frame_000003.ppm", "frame_000004.ppm"]
    assert all(p.read_bytes() == b"P6\n1 1\n255\n\x12\x34\x56" for p in frames)
    silent = run("silence")
    with wave.open(str(silent / "audio.wav")) as stream:
        assert stream.getnframes() == int(37800 * 4 / 59.94)
        assert set(stream.readframes(stream.getnframes())) == {0}
    image_only = run("image-only")
    assert len(list((image_only / "frames").glob("*.ppm"))) == 200
    assert not (image_only / "audio.wav").exists()
    for mode in ("short-write", "seek", "flush", "close-wav", "close-frame", "bad-wav", "bad-frame"):
        run(mode, 2)
    if os.name != "nt":
        run("full-device", 2)
    print("Capture PCM range, silence, image-only, frame pixels and output failures passed.")
