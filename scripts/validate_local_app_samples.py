#!/usr/bin/env python3
"""Validate and optionally install the SDK 0.1 local app sample pack."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SAMPLES = REPO_ROOT / "examples" / "local-apps"

MANIFEST_MAX_BYTES = 1535
ENTRY_MAX_BYTES = 1024
ACTION_MAX = 2

FIELD_LIMITS = {
    "id": 23,
    "name": 31,
    "version": 15,
    "author": 27,
    "summary": 71,
    "entry": 47,
    "api_version": 11,
    "icon": 19,
}

SUPPORTED_PERMISSIONS = {
    "display",
    "input",
    "storage",
    "mesh_read",
    "mesh_send",
    "system_time",
    "battery",
    "notifications",
    "network_wifi",
}

SUPPORTED_SDKS = {"0.1", "0.1.0"}
SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]+$")
SAFE_COUNTER = re.compile(r"^[A-Za-z0-9_-]{1,19}$")


class ValidationError(RuntimeError):
    pass


def byte_len(value: str) -> int:
    return len(value.encode("utf-8"))


def check_string(manifest: dict[str, object], key: str, required: bool = False) -> str:
    value = manifest.get(key)
    if value is None:
        if required:
            raise ValidationError(f"missing {key}")
        return ""
    if not isinstance(value, str) or not value:
        raise ValidationError(f"{key} must be a non-empty string")
    limit = FIELD_LIMITS[key]
    if byte_len(value) > limit:
        raise ValidationError(f"{key} exceeds firmware limit {limit} bytes")
    return value


def check_entry_name(entry: str) -> None:
    if entry.startswith(("/", "\\")):
        raise ValidationError("entry must be relative")
    if ".." in entry:
        raise ValidationError("entry must not contain '..'")
    if "\\" in entry or ":" in entry:
        raise ValidationError("entry must use safe POSIX-style separators")
    if any(ord(ch) < 32 for ch in entry):
        raise ValidationError("entry contains control characters")


def parse_metadata_line(line: str) -> tuple[str, str] | None:
    stripped = line.strip()
    if stripped.startswith("--"):
        stripped = stripped[2:].strip()
    elif stripped.startswith("#"):
        stripped = stripped[1:].strip()
    for sep in (":", "="):
        if sep in stripped:
            key, value = stripped.split(sep, 1)
            key = key.strip()
            if key in {"title", "status", "body", "text", "action"}:
                return key, value.strip()
    return None


def action_parts(spec: str) -> list[str]:
    return [part.strip() for part in spec.split("|")]


def validate_action(spec: str, permissions: set[str], package: Path) -> None:
    parts = action_parts(spec)
    label = parts[0] if parts else ""
    if not label:
        raise ValidationError("action label is required")
    if byte_len(label) > 23:
        raise ValidationError("action label exceeds firmware limit 23 bytes")
    if len(parts) > 1 and byte_len(parts[1]) > 47:
        raise ValidationError("action status exceeds firmware limit 47 bytes")
    if len(parts) > 2 and byte_len(parts[2]) > 191:
        raise ValidationError("action body exceeds firmware limit 191 bytes")
    effect = parts[3] if len(parts) > 3 else ""
    if effect:
        if byte_len(effect) > 31:
            raise ValidationError("action effect exceeds firmware limit 31 bytes")
        if effect.startswith("counter:") or effect.startswith("count:"):
            key = effect.split(":", 1)[1]
            if not SAFE_COUNTER.fullmatch(key):
                raise ValidationError("counter action uses an unsafe key")
            if "storage" not in permissions:
                raise ValidationError("counter action requires storage permission")
        else:
            raise ValidationError(f"unsupported action effect {effect!r}")
    if "input" not in permissions:
        raise ValidationError(f"{package.name} declares actions without input permission")


def validate_entry(entry_path: Path, permissions: set[str], package: Path) -> None:
    size = entry_path.stat().st_size
    if size == 0:
        raise ValidationError("entry is empty")
    if size > ENTRY_MAX_BYTES:
        raise ValidationError(f"entry exceeds firmware limit {ENTRY_MAX_BYTES} bytes")
    text = entry_path.read_text(encoding="utf-8")
    if "{time}" in text and "system_time" not in permissions:
        raise ValidationError("{time} token requires system_time permission")
    if "{battery}" in text and "battery" not in permissions:
        raise ValidationError("{battery} token requires battery permission")
    actions = 0
    for line in text.splitlines():
        parsed = parse_metadata_line(line)
        if not parsed:
            continue
        key, value = parsed
        if key == "action":
            actions += 1
            if actions > ACTION_MAX:
                raise ValidationError(f"more than {ACTION_MAX} actions")
            validate_action(value, permissions, package)
        elif key == "title" and byte_len(value) > 31:
            raise ValidationError("entry title exceeds firmware limit 31 bytes")
        elif key == "status" and byte_len(value) > 63:
            raise ValidationError("entry status exceeds firmware limit 63 bytes")
        elif key in {"body", "text"} and byte_len(value) > 95:
            raise ValidationError("entry body line exceeds safe sample line limit 95 bytes")


def validate_package(package: Path) -> dict[str, object]:
    manifest_path = package / "manifest.json"
    if not manifest_path.is_file():
        raise ValidationError("missing manifest.json")
    if manifest_path.stat().st_size == 0:
        raise ValidationError("manifest is empty")
    if manifest_path.stat().st_size > MANIFEST_MAX_BYTES:
        raise ValidationError(f"manifest exceeds firmware limit {MANIFEST_MAX_BYTES} bytes")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValidationError(f"manifest JSON error: {exc}") from exc
    if not isinstance(manifest, dict):
        raise ValidationError("manifest must be an object")

    app_id = check_string(manifest, "id", required=True)
    if not SAFE_ID.fullmatch(app_id):
        raise ValidationError("id contains unsafe characters")
    check_string(manifest, "name", required=True)
    check_string(manifest, "version")
    check_string(manifest, "author")
    summary = manifest.get("summary", manifest.get("description", ""))
    if summary:
        if not isinstance(summary, str):
            raise ValidationError("summary/description must be a string")
        if byte_len(summary) > FIELD_LIMITS["summary"]:
            raise ValidationError("summary exceeds firmware limit 71 bytes")
    entry = check_string(manifest, "entry", required=True)
    check_entry_name(entry)
    api_version = check_string(manifest, "api_version") or "0.1"
    if api_version not in SUPPORTED_SDKS:
        raise ValidationError("unsupported SDK")
    check_string(manifest, "icon")
    hue = manifest.get("hue", -1)
    if not isinstance(hue, int) or hue < -1 or hue > 359:
        raise ValidationError("hue must be -1..359")
    permissions_value = manifest.get("permissions")
    if not isinstance(permissions_value, list) or not permissions_value:
        raise ValidationError("permissions must be a non-empty array")
    permissions = set()
    for item in permissions_value:
        if not isinstance(item, str) or item not in SUPPORTED_PERMISSIONS:
            raise ValidationError(f"unsupported permission {item!r}")
        permissions.add(item)
    if "display" not in permissions:
        raise ValidationError("display permission is required for SDK 0.1 samples")

    entry_path = package / entry
    if not entry_path.is_file():
        raise ValidationError("missing entry file")
    validate_entry(entry_path, permissions, package)
    return manifest


def iter_packages(samples_dir: Path) -> list[Path]:
    packages = [p for p in samples_dir.iterdir() if p.is_dir()]
    return sorted(packages, key=lambda p: p.name)


def install_packages(packages: list[Path], install_root: Path, clean: bool) -> None:
    apps_dir = install_root / "apps"
    apps_dir.mkdir(parents=True, exist_ok=True)
    for package in packages:
        dest = apps_dir / package.name
        if dest.exists():
            if not clean:
                raise ValidationError(f"{dest} already exists; pass --clean to replace samples")
            shutil.rmtree(dest)
        shutil.copytree(package, dest, ignore=shutil.ignore_patterns("data"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples-dir", type=Path, default=DEFAULT_SAMPLES)
    parser.add_argument("--install-root", type=Path, help="copy samples under <root>/apps after validation")
    parser.add_argument("--clean", action="store_true", help="replace existing sample folders at --install-root")
    args = parser.parse_args()

    samples_dir = args.samples_dir.resolve()
    if not samples_dir.is_dir():
        print(f"[apps] missing sample directory: {samples_dir}", file=sys.stderr)
        return 2

    packages = iter_packages(samples_dir)
    if not packages:
        print("[apps] no sample packages found", file=sys.stderr)
        return 2

    seen_ids: set[str] = set()
    try:
        for package in packages:
            manifest = validate_package(package)
            app_id = str(manifest["id"])
            if app_id in seen_ids:
                raise ValidationError(f"duplicate app id {app_id}")
            seen_ids.add(app_id)
            print(f"[apps] ok {package.name}: {app_id}")
        if args.install_root:
            install_root = args.install_root.resolve()
            install_packages(packages, install_root, args.clean)
            print(f"[apps] installed {len(packages)} samples under {install_root / 'apps'}")
    except ValidationError as exc:
        print(f"[apps] FAIL: {exc}", file=sys.stderr)
        return 1

    print(f"[apps] sample pack PASS ({len(packages)} packages)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
