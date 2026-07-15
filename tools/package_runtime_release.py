#!/usr/bin/env python3
"""Build and audit the runtime-only cdirecomp Windows release archive."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile


ALLOWED_IMPORTS = {"kernel32.dll", "msvcrt.dll", "ws2_32.dll", "sdl2.dll"}
FORBIDDEN_MARKERS = (b"cedimu", b"clown68000", b"cdioracle", b"cdi_oracle")
FORBIDDEN_BUILD_MARKERS = (
    "cedimu", "clown68000", "clowncommon", "cdi_oracle", "oracle/cdi_oracle",
)
FORBIDDEN_SUFFIXES = {
    ".rom", ".bin", ".cue", ".iso", ".map", ".obj", ".o", ".pdb",
    ".lib", ".exp", ".ilk", ".log", ".py", ".pyc",
}


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def audit_runtime(runtime: Path) -> None:
    objdump = shutil.which("objdump")
    if not objdump:
        fail("objdump is required to audit the PE architecture and imports")
    result = subprocess.run(
        [objdump, "-f", "-p", str(runtime)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    lowered = result.stdout.lower()
    if "pei-x86-64" not in lowered:
        fail("CdiRuntime.exe is not a Windows x86-64 PE executable")
    imports = {
        match.group(1).lower()
        for match in re.finditer(r"DLL Name:\s*([^\s]+)", result.stdout)
    }
    unexpected = sorted(imports - ALLOWED_IMPORTS)
    missing = sorted({"kernel32.dll", "sdl2.dll"} - imports)
    if unexpected or missing:
        fail(
            "unexpected runtime imports; unexpected="
            f"{unexpected}, missing={missing}, observed={sorted(imports)}"
        )
    binary = runtime.read_bytes().lower()
    leaked = [marker.decode("ascii") for marker in FORBIDDEN_MARKERS
              if marker in binary]
    if leaked:
        fail(f"development/oracle marker found in runtime: {leaked}")


def audit_build_graph(runtime: Path) -> None:
    build_dir = runtime.resolve().parent
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        fail(f"release CMake cache is missing beside runtime: {cache}")
    cache_text = cache.read_text(encoding="utf-8", errors="replace")
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache_text:
        fail("runtime was not produced by a CMake Release configuration")
    if "CDI_COSIM_BUILD:BOOL=OFF" not in cache_text:
        fail("runtime release must be built with CDI_COSIM_BUILD=OFF")

    ninja = shutil.which("ninja")
    if not ninja:
        fail("ninja is required to audit the native target build graph")
    commands = subprocess.run(
        [ninja, "-C", str(build_dir), "-t", "commands", "CdiRuntime"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout.lower().replace("\\", "/")
    leaked = [marker for marker in FORBIDDEN_BUILD_MARKERS if marker in commands]
    if leaked:
        fail(f"development/oracle source or library leaked into build graph: {leaked}")


def audit_archive(archive: Path, expected: set[str], prefix: str) -> None:
    with zipfile.ZipFile(archive) as bundle:
        names = set(bundle.namelist())
        if names != expected:
            fail(
                "release archive differs from its explicit allowlist; "
                f"missing={sorted(expected - names)}, extra={sorted(names - expected)}"
            )
        for name in names:
            relative = name.removeprefix(prefix)
            lowered = relative.lower()
            if Path(lowered).suffix in FORBIDDEN_SUFFIXES:
                fail(f"forbidden release file: {name}")
            if any(marker.decode("ascii") in lowered
                   for marker in FORBIDDEN_MARKERS):
                fail(f"development/oracle name leaked into release: {name}")
        runtime_data = bundle.read(prefix + "CdiRuntime.exe").lower()
        if any(marker in runtime_data for marker in FORBIDDEN_MARKERS):
            fail("development/oracle marker leaked into archived runtime")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--runtime", type=Path,
                        default=Path("build/runner-release/CdiRuntime.exe"))
    parser.add_argument("--sdl", type=Path,
                        default=Path("build/runner-release/SDL2.dll"))
    parser.add_argument("--config", type=Path,
                        default=Path("player.cfg.example"))
    parser.add_argument("--readme", type=Path,
                        default=Path("RUNTIME-README.md"))
    parser.add_argument("--notices", type=Path,
                        default=Path("THIRD-PARTY-NOTICES.md"))
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    version = args.version.removeprefix("v")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        fail("--version must use MAJOR.MINOR.PATCH")
    archive = args.out or Path("dist") / f"cdirecomp-{version}-windows-x64.zip"
    sources = {
        "CdiRuntime.exe": args.runtime,
        "SDL2.dll": args.sdl,
        "player.cfg.example": args.config,
        "README.md": args.readme,
        "THIRD-PARTY-NOTICES.md": args.notices,
    }
    missing = [str(path) for path in sources.values() if not path.is_file()]
    if missing:
        fail("missing allowlisted release input(s): " + ", ".join(missing))
    if args.runtime.name.lower() != "cdiruntime.exe":
        fail("--runtime must be the native CdiRuntime.exe target")
    if args.sdl.name.lower() != "sdl2.dll":
        fail("--sdl must be SDL2.dll")

    audit_build_graph(args.runtime)
    audit_runtime(args.runtime)
    prefix = f"cdirecomp-{version}/"
    expected = {prefix + name for name in sources}
    archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
        for destination, source in sources.items():
            bundle.write(source, prefix + destination)
    audit_archive(archive, expected, prefix)

    checksum = sha256(archive)
    checksum_path = archive.with_suffix(archive.suffix + ".sha256")
    checksum_path.write_text(f"{checksum}  {archive.name}\n", encoding="ascii")
    print("PASS: Release/OFF native build graph contains no oracle/emulator input")
    print(f"PASS: runtime imports are allowlisted: {sorted(ALLOWED_IMPORTS)}")
    print(f"PASS: archive contains exactly {len(expected)} allowlisted files")
    print(f"WROTE: {archive}")
    print(f"SHA256: {checksum}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
