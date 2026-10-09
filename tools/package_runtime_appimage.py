#!/usr/bin/env python3
"""Package and audit a runtime-only Linux x86_64 CD-i AppImage.

Uses the pinned linuxdeploy/appimagetool workflow used by TombaRecomp. Build
tools, original assets, generated C and provenance sidecars remain outside the
AppDir. Both the staged and extracted payload are audited.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.request

from build_provenance import verify
from package_runtime_release import (
    FORBIDDEN_MARKERS, FORBIDDEN_SUFFIXES, audit_build_graph, sha256,
)


TOOLS = {
    "linuxdeploy": (
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage",
        "8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1",
    ),
    "appimagetool": (
        "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage",
        "95cbe7cce9717fce90c484e34052ee7c7f1d7635b33c12525b4776826a7d29b6",
    ),
}


def run(*command: str, **kwargs) -> str:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {command}\n{result.stdout}")
    return result.stdout


def audit_elf(path: Path, *, library: bool = False) -> None:
    header = run("readelf", "-h", str(path))
    if "ELF64" not in header or "Advanced Micro Devices X86-64" not in header:
        raise ValueError(f"not a Linux x86_64 ELF: {path}")
    if library and "(SONAME)" not in run("readelf", "-d", str(path)):
        raise ValueError(f"bundled library has no SONAME: {path}")


def audit_appdir(root: Path, product: str) -> dict:
    support = {
        "AppRun", ".DirIcon", f"{product}.desktop", f"{product}.svg",
        f"usr/bin/{product}",
        f"usr/share/applications/{product}.desktop",
        f"usr/share/icons/hicolor/scalable/apps/{product}.svg",
        "usr/share/cdirecomp/README.md",
        "usr/share/cdirecomp/THIRD-PARTY-NOTICES.md",
        "usr/share/cdirecomp/player.cfg.example",
    }
    manifest = {}
    for path in sorted(root.rglob("*")):
        if path.is_dir() and not path.is_symlink():
            continue
        relative = path.relative_to(root).as_posix()
        library = bool(re.fullmatch(r"usr/lib/[^/]+\.so(?:\.\d+)*", relative))
        if relative not in support and not library:
            raise ValueError(f"file outside runtime allowlist: {relative}")
        if path.suffix.lower() in FORBIDDEN_SUFFIXES:
            raise ValueError(f"forbidden release file: {relative}")
        if any(marker.decode() in relative.lower() for marker in FORBIDDEN_MARKERS):
            raise ValueError(f"development/oracle file: {relative}")
        if path.is_symlink():
            resolved = path.resolve(strict=True)
            if not resolved.is_relative_to(root.resolve()):
                raise ValueError(f"symlink leaves AppDir: {relative}")
            manifest[relative] = {"symlink": os.readlink(path)}
        elif path.is_file():
            if library:
                audit_elf(path, library=True)
            manifest[relative] = {"sha256": sha256(path),
                                  "executable": bool(path.stat().st_mode & 0o111)}
        else:
            raise ValueError(f"unsupported staged object: {relative}")
    for required in ("AppRun", f"usr/bin/{product}", "usr/share/cdirecomp/README.md",
                     "usr/share/cdirecomp/THIRD-PARTY-NOTICES.md",
                     "usr/share/cdirecomp/player.cfg.example"):
        if required not in manifest:
            raise ValueError(f"missing runtime resource: {required}")
    runtime = root / "usr/bin" / product
    audit_elf(runtime)
    if any(marker in runtime.read_bytes().lower() for marker in FORBIDDEN_MARKERS):
        raise ValueError("development/oracle marker in runtime")
    linkage = run("ldd", str(runtime))
    if "not found" in linkage:
        raise ValueError(f"unresolved packaged dependency:\n{linkage}")
    return manifest


def fetch_tool(name: str, directory: Path) -> Path:
    url, expected = TOOLS[name]
    directory.mkdir(parents=True, exist_ok=True)
    tool = directory / f"{name}-x86_64.AppImage"
    if not tool.exists() or sha256(tool) != expected:
        temporary = tool.with_suffix(".download")
        urllib.request.urlretrieve(url, temporary)
        if sha256(temporary) != expected:
            raise ValueError(f"{name} download differs from pinned SHA-256")
        temporary.replace(tool)
    tool.chmod(0o755)
    return tool


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product", choices=("cdirecomp", "hotelmario"), default="cdirecomp")
    parser.add_argument("--version", required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--apprun", type=Path, required=True)
    parser.add_argument("--readme", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--notices", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--tools-dir", type=Path,
                        default=Path.home() / ".cache/recomp-appimage-tools")
    args = parser.parse_args()
    version = args.version.removeprefix("v")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        parser.error("version must use MAJOR.MINOR.PATCH")
    product = "HotelMarioRecomp" if args.product == "hotelmario" else "CdiRuntime"
    runtime = args.runtime.resolve(strict=True)
    if runtime.name != product:
        parser.error(f"runtime must be the native {product} target")
    for path in (args.apprun, args.readme, args.config, args.notices):
        if not path.is_file():
            parser.error(f"missing runtime resource: {path}")
    if args.out.exists():
        parser.error(f"output exists; select a fresh path: {args.out}")
    audit_build_graph(runtime)
    provenance = verify(runtime)
    audit_elf(runtime)
    directory = Path(tempfile.mkdtemp(prefix=f"appimage-{version}-", dir=runtime.parent))
    appdir = directory / "AppDir"
    (appdir / "usr/bin").mkdir(parents=True)
    payload = appdir / "usr/share/cdirecomp"
    payload.mkdir(parents=True)
    shutil.copy2(runtime, appdir / "usr/bin" / product)
    shutil.copy2(args.apprun, appdir / "AppRun")
    (appdir / "AppRun").chmod(0o755)
    for source, name in ((args.readme, "README.md"), (args.config, "player.cfg.example"),
                         (args.notices, "THIRD-PARTY-NOTICES.md")):
        shutil.copy2(source, payload / name)
    (appdir / f"{product}.desktop").write_text(
        "[Desktop Entry]\nType=Application\n"
        f"Name={product}\nComment=CD-i LLE development preview\n"
        f"Exec={product}\nIcon={product}\nCategories=Game;\n"
        "Terminal=false\nStartupNotify=true\n", encoding="utf-8")
    # Original vector badge; no game artwork or disc-derived resource.
    (appdir / f"{product}.svg").write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">'
        '<rect width="256" height="256" rx="40" fill="#25354a"/>'
        '<circle cx="128" cy="128" r="88" fill="#e2e7ed"/>'
        '<circle cx="128" cy="128" r="22" fill="#25354a"/>'
        '<path d="M54 174h148v28H54z" fill="#e64b45"/></svg>', encoding="utf-8")
    (appdir / ".DirIcon").symlink_to(f"{product}.svg")
    deploy = fetch_tool("linuxdeploy", args.tools_dir)
    appimagetool = fetch_tool("appimagetool", args.tools_dir)
    env = os.environ | {"NO_STRIP": "1", "ARCH": "x86_64"}
    (directory / "linuxdeploy.log").write_text(run(
        str(deploy), "--appimage-extract-and-run", "--appdir", str(appdir),
        "--executable", str(appdir / "usr/bin" / product),
        "--desktop-file", str(appdir / f"{product}.desktop"),
        "--icon-file", str(appdir / f"{product}.svg"), env=env))
    staged = audit_appdir(appdir, product)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    output = args.out.resolve()
    (directory / "appimagetool.log").write_text(run(
        str(appimagetool), "--appimage-extract-and-run", str(appdir), str(output), env=env))
    output.chmod(0o755)
    extracted = directory / "verify-extracted"
    extracted.mkdir()
    run(str(output), "--appimage-extract", cwd=extracted)
    packaged = audit_appdir(extracted / "squashfs-root", product)
    if staged != packaged:
        raise ValueError("AppImage payload differs from audited AppDir")
    versions = run("readelf", "--version-info", str(appdir / "usr/bin" / product))
    required_glibc = sorted(set(re.findall(r"GLIBC_([\d.]+)", versions)),
                            key=lambda version: tuple(map(int, version.split("."))))
    checksum = sha256(output)
    Path(str(output) + ".sha256").write_text(f"{checksum}  {output.name}\n", encoding="ascii")
    Path(str(output) + ".audit.json").write_text(json.dumps({
        "scope": "runtime packaging only; no full campaign claim",
        "version": version, "sha256": checksum,
        "linked_runtime_sha256": provenance["runtime_sha256"],
        "packaged_runtime_sha256": sha256(appdir / "usr/bin" / product),
        "runtime_glibc_versions": required_glibc, "files": packaged,
        "tool_sha256": {name: item[1] for name, item in TOOLS.items()},
    }, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: linked provenance, Release/COSIM OFF graph, ELF/dependency and payload audits ({len(packaged)} files)")
    print(f"WROTE: {output}\nSHA256: {checksum}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
