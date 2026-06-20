#!/usr/bin/env python3
"""Build a LimitlezzOS app package as a deterministic ZIP_STORED archive."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path


PACKAGE_MAX_BYTES = 2 * 1024 * 1024
FILE_MAX_BYTES = 256 * 1024
SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]{1,23}$")


def fail(message: str) -> None:
    raise SystemExit(f"[package] {message}")


def safe_package_path(rel: str) -> bool:
    if not rel or rel.startswith("/") or "\\" in rel or ":" in rel:
        return False
    parts = rel.split("/")
    if any(part in {"", ".", ".."} or part.startswith(".") for part in parts):
        return False
    if parts[0] == "data":
        return False
    return True


def collect_files(root: Path) -> list[Path]:
    files = sorted(path for path in root.rglob("*") if path.is_file())
    if not (root / "manifest.json").is_file():
        fail("manifest.json is required at the package root")
    if len(files) > 24:
        fail("too many files; firmware accepts at most 24")
    for path in files:
        rel = path.relative_to(root).as_posix()
        if not safe_package_path(rel):
            fail(f"unsafe package path: {rel}")
        if path.stat().st_size > FILE_MAX_BYTES:
            fail(f"file too large for firmware package: {rel}")
    return files


def read_manifest(root: Path) -> dict:
    try:
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"manifest.json is not valid JSON: {exc}")
    app_id = manifest.get("id")
    if not isinstance(app_id, str) or not SAFE_ID.fullmatch(app_id):
        fail("manifest id is missing or unsafe")
    return manifest


def build_package(root: Path, out: Path, files: list[Path]) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_STORED) as zf:
        for path in files:
            rel = path.relative_to(root).as_posix()
            info = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0o100644 << 16
            zf.writestr(info, path.read_bytes())
    if out.stat().st_size > PACKAGE_MAX_BYTES:
        out.unlink(missing_ok=True)
        fail("package exceeds firmware 2 MB limit")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app_dir", type=Path, help="directory containing manifest.json")
    parser.add_argument("-o", "--out", type=Path, help="output .zip path")
    parser.add_argument("--device-path", help="path the package will have on the T-Deck")
    args = parser.parse_args()

    root = args.app_dir.resolve()
    if not root.is_dir():
        fail(f"not a directory: {root}")
    manifest = read_manifest(root)
    app_id = manifest["id"]
    out = args.out or (Path.cwd() / f"{app_id}.zip")
    files = collect_files(root)
    build_package(root, out, files)

    digest = sha256(out)
    size = out.stat().st_size
    install_path = args.device_path or out.as_posix()
    print(f"package={out}")
    print(f"id={app_id}")
    print(f"bytes={size}")
    print(f"sha256={digest}")
    print(f"serial=app package install {app_id} {install_path} {digest} {size}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
