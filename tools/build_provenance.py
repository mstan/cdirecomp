"""Verify a local link-time provenance sidecar against its binary and inputs."""
import hashlib
import json
from pathlib import Path


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify(runtime: Path) -> dict:
    sidecar = Path(str(runtime) + ".build.json")
    data = json.loads(sidecar.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or not data.get("inputs"):
        raise ValueError(f"invalid build provenance: {sidecar}")
    if data.get("runtime_sha256") != digest(runtime):
        raise ValueError("runtime differs from its link-time provenance")
    for item in data["inputs"]:
        path = Path(item["path"])
        if not path.is_file() or digest(path) != item["sha256"]:
            raise ValueError(f"runtime input changed since linking: {path}")
    return data
