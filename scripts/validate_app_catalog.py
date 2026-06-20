#!/usr/bin/env python3
"""Validate a LimitlezzOS network app catalog index."""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from urllib.parse import urlparse


SCHEMA = "limitlezz.app.catalog.v1"
SUPPORTED_API_VERSIONS = {"0.1", "0.1.0"}
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
TARGETS = {"tdeck", "sim"}
MAX_PACKAGE_BYTES = 2 * 1024 * 1024
MAX_SCREENSHOTS = 4

SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]{1,23}$")
SAFE_VERSION = re.compile(r"^[0-9][0-9A-Za-z_.+-]{0,15}$")
SAFE_ICON = re.compile(r"^[A-Za-z0-9_-]{1,19}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def is_https_url(value: str) -> bool:
    parsed = urlparse(value)
    return parsed.scheme == "https" and bool(parsed.netloc)


def add_error(errors: list[str], path: str, message: str) -> None:
    errors.append(f"{path}: {message}")


def require_string(obj: dict, key: str, path: str, errors: list[str],
                   max_len: int | None = None) -> str:
    value = obj.get(key)
    if not isinstance(value, str) or not value.strip():
        add_error(errors, f"{path}.{key}", "required non-empty string")
        return ""
    if max_len is not None and len(value) > max_len:
        add_error(errors, f"{path}.{key}", f"too long (max {max_len})")
    return value


def require_int(obj: dict, key: str, path: str, errors: list[str]) -> int | None:
    value = obj.get(key)
    if not isinstance(value, int) or isinstance(value, bool):
        add_error(errors, f"{path}.{key}", "required integer")
        return None
    return value


def validate_permissions(app: dict, path: str, errors: list[str]) -> None:
    perms = app.get("permissions")
    if not isinstance(perms, list) or not perms:
        add_error(errors, f"{path}.permissions", "required non-empty string array")
        return
    seen: set[str] = set()
    for i, perm in enumerate(perms):
        if not isinstance(perm, str):
            add_error(errors, f"{path}.permissions[{i}]", "must be a string")
            continue
        if perm not in SUPPORTED_PERMISSIONS:
            add_error(errors, f"{path}.permissions[{i}]", f"unsupported permission {perm!r}")
        if perm in seen:
            add_error(errors, f"{path}.permissions[{i}]", f"duplicate permission {perm!r}")
        seen.add(perm)
    if "display" not in seen:
        add_error(errors, f"{path}.permissions", "must include display")


def validate_compat(app: dict, path: str, api_version: str, errors: list[str]) -> None:
    compat = app.get("compatibility")
    if not isinstance(compat, dict):
        add_error(errors, f"{path}.compatibility", "required object")
        return

    api_versions = compat.get("api_versions")
    if not isinstance(api_versions, list) or not api_versions:
        add_error(errors, f"{path}.compatibility.api_versions", "required non-empty string array")
    else:
        seen_api = {v for v in api_versions if isinstance(v, str)}
        for i, value in enumerate(api_versions):
            if not isinstance(value, str):
                add_error(errors, f"{path}.compatibility.api_versions[{i}]", "must be a string")
            elif value not in SUPPORTED_API_VERSIONS:
                add_error(errors, f"{path}.compatibility.api_versions[{i}]", f"unsupported SDK {value!r}")
        if api_version and api_version not in seen_api:
            add_error(errors, f"{path}.compatibility.api_versions", "must include app api_version")

    targets = compat.get("targets")
    if not isinstance(targets, list) or not targets:
        add_error(errors, f"{path}.compatibility.targets", "required non-empty string array")
    else:
        for i, target in enumerate(targets):
            if not isinstance(target, str):
                add_error(errors, f"{path}.compatibility.targets[{i}]", "must be a string")
            elif target not in TARGETS:
                add_error(errors, f"{path}.compatibility.targets[{i}]", f"unsupported target {target!r}")

    min_os = compat.get("min_os")
    if min_os is not None and (not isinstance(min_os, str) or not SAFE_VERSION.match(min_os)):
        add_error(errors, f"{path}.compatibility.min_os", "must be a compact version string")


def validate_screenshots(app: dict, path: str, errors: list[str]) -> None:
    screenshots = app.get("screenshots", [])
    if screenshots is None:
        return
    if not isinstance(screenshots, list):
        add_error(errors, f"{path}.screenshots", "must be an array")
        return
    if len(screenshots) > MAX_SCREENSHOTS:
        add_error(errors, f"{path}.screenshots", f"too many screenshots (max {MAX_SCREENSHOTS})")
    for i, shot in enumerate(screenshots):
        spath = f"{path}.screenshots[{i}]"
        if not isinstance(shot, dict):
            add_error(errors, spath, "must be an object")
            continue
        url = require_string(shot, "url", spath, errors, 180)
        if url and not is_https_url(url):
            add_error(errors, f"{spath}.url", "must be https")
        width = require_int(shot, "width", spath, errors)
        height = require_int(shot, "height", spath, errors)
        if width is not None and width <= 0:
            add_error(errors, f"{spath}.width", "must be positive")
        if height is not None and height <= 0:
            add_error(errors, f"{spath}.height", "must be positive")
        digest = shot.get("sha256")
        if digest is not None and (not isinstance(digest, str) or not SHA256.match(digest)):
            add_error(errors, f"{spath}.sha256", "must be lowercase 64-character hex")


def validate_app(app: object, index: int, ids: set[str], errors: list[str]) -> None:
    path = f"apps[{index}]"
    if not isinstance(app, dict):
        add_error(errors, path, "must be an object")
        return

    app_id = require_string(app, "id", path, errors, 23)
    if app_id:
        if not SAFE_ID.match(app_id) or app_id == "." or ".." in app_id:
            add_error(errors, f"{path}.id", "unsafe id")
        if app_id in ids:
            add_error(errors, f"{path}.id", f"duplicate id {app_id!r}")
        ids.add(app_id)

    require_string(app, "name", path, errors, 31)
    version = require_string(app, "version", path, errors, 15)
    if version and not SAFE_VERSION.match(version):
        add_error(errors, f"{path}.version", "must be a compact version string")
    require_string(app, "author", path, errors, 27)
    require_string(app, "summary", path, errors, 71)
    require_string(app, "description", path, errors, 240)

    api_version = require_string(app, "api_version", path, errors, 11)
    if api_version and api_version not in SUPPORTED_API_VERSIONS:
        add_error(errors, f"{path}.api_version", f"unsupported SDK {api_version!r}")
    validate_permissions(app, path, errors)

    icon = require_string(app, "icon", path, errors, 19)
    if icon and not SAFE_ICON.match(icon):
        add_error(errors, f"{path}.icon", "must be a symbolic token")
    hue = require_int(app, "hue", path, errors)
    if hue is not None and not (-1 <= hue <= 359):
        add_error(errors, f"{path}.hue", "must be -1 or 0..359")

    package_url = require_string(app, "package_url", path, errors, 180)
    if package_url and not is_https_url(package_url):
        add_error(errors, f"{path}.package_url", "must be https")
    package_sha = require_string(app, "package_sha256", path, errors, 64)
    if package_sha and not SHA256.match(package_sha):
        add_error(errors, f"{path}.package_sha256", "must be lowercase 64-character hex")
    package_size = require_int(app, "package_bytes", path, errors)
    if package_size is not None and package_size <= 0:
        add_error(errors, f"{path}.package_bytes", "must be positive")
    if package_size is not None and package_size > MAX_PACKAGE_BYTES:
        add_error(errors, f"{path}.package_bytes", f"must be <= {MAX_PACKAGE_BYTES}")

    validate_compat(app, path, api_version, errors)
    validate_screenshots(app, path, errors)


def validate_catalog(path: Path) -> list[str]:
    try:
        catalog = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [f"{path}: invalid JSON: {exc}"]

    errors: list[str] = []
    if not isinstance(catalog, dict):
        return ["catalog root: must be an object"]

    schema = catalog.get("schema")
    if schema != SCHEMA:
        add_error(errors, "schema", f"must be {SCHEMA!r}")

    generated_at = catalog.get("generated_at")
    if generated_at is not None and not isinstance(generated_at, str):
        add_error(errors, "generated_at", "must be a string when present")

    apps = catalog.get("apps")
    if not isinstance(apps, list):
        add_error(errors, "apps", "required array")
        return errors

    ids: set[str] = set()
    for i, app in enumerate(apps):
        validate_app(app, i, ids, errors)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate LimitlezzOS app catalog index JSON.")
    parser.add_argument("catalog", nargs="+", type=Path)
    args = parser.parse_args()

    failed = False
    for path in args.catalog:
        errors = validate_catalog(path)
        if errors:
            failed = True
            print(f"[catalog] {path}: FAIL", file=sys.stderr)
            for error in errors:
                print(f"  - {error}", file=sys.stderr)
        else:
            print(f"[catalog] {path}: ok")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
