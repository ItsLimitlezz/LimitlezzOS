#!/usr/bin/env python3
"""
Check T-Deck firmware flash and static-RAM budgets.

The flash budget is measured from firmware.bin against the configured OTA app
slot in partitions.csv. Static RAM is parsed from PlatformIO's size report when
provided.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


DEFAULT_MAX_FIRMWARE_BYTES = 2_200_000
DEFAULT_MAX_STATIC_RAM_BYTES = 307_200


def parse_size(value: str) -> int:
    raw = value.strip().lower()
    if raw.startswith("0x"):
        return int(raw, 16)
    mult = 1
    if raw.endswith("k"):
        raw = raw[:-1]
        mult = 1024
    elif raw.endswith("m"):
        raw = raw[:-1]
        mult = 1024 * 1024
    return int(raw, 10) * mult


def app_slot_size(partitions: Path, app_partition: str) -> int:
    fallback: int | None = None
    for line in partitions.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 5 or parts[0].lower() == "name":
            continue
        name, kind, _subtype, _offset, size = parts[:5]
        if kind == "app" and fallback is None:
            fallback = parse_size(size)
        if name == app_partition:
            if kind != "app":
                raise SystemExit(f"partition {app_partition!r} is {kind!r}, not app")
            return parse_size(size)
    if fallback is not None and not app_partition:
        return fallback
    raise SystemExit(f"app partition {app_partition!r} not found in {partitions}")


SIZE_RE = re.compile(
    r"^(RAM|Flash):\s+\[[^\]]*\]\s+([0-9.]+)%\s+"
    r"\(used\s+([0-9]+)\s+bytes\s+from\s+([0-9]+)\s+bytes\)",
    re.IGNORECASE,
)


def parse_platformio_size_report(path: Path | None) -> dict[str, int | float]:
    if path is None:
        return {}
    metrics: dict[str, int | float] = {}
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        match = SIZE_RE.search(line.strip().lstrip("\ufeff"))
        if not match:
            continue
        label = match.group(1).lower()
        metrics[f"{label}_pct"] = float(match.group(2))
        metrics[f"{label}_used_bytes"] = int(match.group(3))
        metrics[f"{label}_total_bytes"] = int(match.group(4))
    return metrics


def pct(part: int, whole: int) -> float:
    return (part / whole) * 100.0 if whole else 0.0


def render_markdown(report: dict) -> str:
    rows = [
        ("firmware.bin", report["firmware_bytes"], report["max_firmware_bytes"], report["firmware_ok"]),
        ("OTA slot", report["firmware_bytes"], report["ota_slot_bytes"], True),
    ]
    if report.get("static_ram_bytes") is not None:
        rows.append(("static RAM", report["static_ram_bytes"], report["max_static_ram_bytes"], report["static_ram_ok"]))

    out = [
        "## T-Deck firmware budget",
        "",
        "| Metric | Used | Budget | Status |",
        "| --- | ---: | ---: | --- |",
    ]
    for name, used, budget, ok in rows:
        status = "pass" if ok else "fail"
        out.append(f"| {name} | {used:,} bytes | {budget:,} bytes | {status} |")
    out.extend(
        [
            "",
            f"- firmware.bin uses {report['firmware_slot_pct']:.1f}% of the OTA app slot.",
            f"- result: {'pass' if report['ok'] else 'fail'}",
        ]
    )
    return "\n".join(out) + "\n"


def write_key_values(path: Path, report: dict) -> None:
    keys = [
        "budget_status",
        "firmware_bytes",
        "max_firmware_bytes",
        "ota_slot_bytes",
        "firmware_slot_pct",
        "static_ram_bytes",
        "max_static_ram_bytes",
    ]
    lines: list[str] = []
    for key in keys:
        value = report.get(key)
        if value is None:
            continue
        lines.append(f"{key}={value}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_report(args: argparse.Namespace) -> dict:
    firmware = Path(args.firmware)
    if not firmware.exists():
        raise SystemExit(f"firmware binary not found: {firmware}")
    partitions = Path(args.partitions)
    if not partitions.exists():
        raise SystemExit(f"partition table not found: {partitions}")

    firmware_bytes = firmware.stat().st_size
    slot_bytes = app_slot_size(partitions, args.app_partition)
    size_metrics = parse_platformio_size_report(Path(args.size_report) if args.size_report else None)
    if args.size_report and "ram_used_bytes" not in size_metrics:
        raise SystemExit(f"RAM usage line not found in size report: {args.size_report}")
    static_ram = size_metrics.get("ram_used_bytes")
    static_ram_total = size_metrics.get("ram_total_bytes")

    report = {
        "ok": True,
        "budget_status": "pass",
        "firmware_bytes": firmware_bytes,
        "max_firmware_bytes": args.max_firmware_bytes,
        "ota_slot_bytes": slot_bytes,
        "firmware_slot_pct": round(pct(firmware_bytes, slot_bytes), 2),
        "firmware_ok": firmware_bytes <= args.max_firmware_bytes,
        "static_ram_bytes": static_ram,
        "static_ram_total_bytes": static_ram_total,
        "max_static_ram_bytes": args.max_static_ram_bytes,
        "static_ram_ok": True if static_ram is None else static_ram <= args.max_static_ram_bytes,
        "size_report": str(args.size_report) if args.size_report else None,
    }
    report["ok"] = bool(report["firmware_ok"] and report["static_ram_ok"])
    report["budget_status"] = "pass" if report["ok"] else "fail"
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Check T-Deck firmware size and RAM budgets.")
    parser.add_argument("--firmware", default=Path(".pio") / "build" / "tdeck" / "firmware.bin")
    parser.add_argument("--partitions", default=Path("partitions.csv"))
    parser.add_argument("--app-partition", default="ota_0")
    parser.add_argument("--size-report", help="PlatformIO `pio run -t size` output to parse for RAM use.")
    parser.add_argument("--max-firmware-bytes", type=int, default=DEFAULT_MAX_FIRMWARE_BYTES)
    parser.add_argument("--max-static-ram-bytes", type=int, default=DEFAULT_MAX_STATIC_RAM_BYTES)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--markdown-out", type=Path)
    parser.add_argument("--manifest-out", type=Path)
    args = parser.parse_args()

    report = build_report(args)
    markdown = render_markdown(report)
    print(markdown, end="")

    if args.json_out:
        args.json_out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.markdown_out:
        args.markdown_out.write_text(markdown, encoding="utf-8")
    if args.manifest_out:
        write_key_values(args.manifest_out, report)

    if not report["ok"]:
        print("T-Deck budget check failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
