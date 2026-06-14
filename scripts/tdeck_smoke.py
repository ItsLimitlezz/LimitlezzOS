#!/usr/bin/env python3
"""
Build, optionally flash, and smoke-test a LimitlezzOS T-Deck over serial.

The runner is intentionally cross-platform. Windows maintainers can use COM8,
while Linux/macOS developers can pass /dev/ttyACM0, /dev/ttyUSB0, or the port
exported in LZ_SERIAL_PORT.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


DEFAULT_COMMANDS = ["id", "sys", "net", "rf", "stats", "wifi", "companion test"]


def default_port() -> str:
    env_port = os.environ.get("LZ_SERIAL_PORT")
    if env_port:
        return env_port
    return "COM8" if os.name == "nt" else "/dev/ttyACM0"


def run(cmd: list[str], cwd: Path) -> None:
    print("[smoke] " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def platformio_core_dir() -> Path:
    override = os.environ.get("PLATFORMIO_CORE_DIR")
    if override:
        return Path(override)
    return Path.home() / ".platformio"


def find_esptool() -> Path:
    root = platformio_core_dir() / "packages" / "tool-esptoolpy"
    candidates = list(root.rglob("esptool.py")) if root.exists() else []
    if not candidates:
        raise FileNotFoundError(
            "Could not find PlatformIO's esptool.py. Run `pio run -e tdeck` once "
            "or install the espressif32 platform."
        )
    return candidates[0]


def find_boot_app0(artifact_dir: Path | None = None) -> Path:
    if artifact_dir is not None:
        bundled = artifact_dir / "boot_app0.bin"
        if bundled.exists():
            return bundled

    root = platformio_core_dir() / "packages" / "framework-arduinoespressif32"
    candidates = list(root.rglob("boot_app0.bin")) if root.exists() else []
    if not candidates:
        raise FileNotFoundError(
            "Could not find boot_app0.bin in PlatformIO's Arduino ESP32 package."
        )
    return candidates[0]


def require_artifacts(project_dir: Path, env_name: str, artifact_dir: Path | None) -> tuple[Path, Path, Path, Path]:
    build_dir = artifact_dir if artifact_dir is not None else project_dir / ".pio" / "build" / env_name
    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    firmware = build_dir / "firmware.bin"
    boot_app0 = find_boot_app0(artifact_dir)
    missing = [p for p in (bootloader, partitions, firmware, boot_app0) if not p.exists()]
    if missing:
        names = "\n  ".join(str(p) for p in missing)
        raise FileNotFoundError(f"Missing flash artifacts:\n  {names}")
    return bootloader, partitions, boot_app0, firmware


def nostub_upload(project_dir: Path, env_name: str, port: str, baud: int, artifact_dir: Path | None) -> None:
    bootloader, partitions, boot_app0, firmware = require_artifacts(project_dir, env_name, artifact_dir)
    esptool = find_esptool()
    run(
        [
            sys.executable,
            str(esptool),
            "--chip",
            "esp32s3",
            "--port",
            port,
            "--baud",
            str(baud),
            "--before",
            "default_reset",
            "--after",
            "hard_reset",
            "--no-stub",
            "write_flash",
            "--flash_mode",
            "dio",
            "--flash_freq",
            "80m",
            "--flash_size",
            "16MB",
            "0x0",
            str(bootloader),
            "0x8000",
            str(partitions),
            "0xe000",
            str(boot_app0),
            "0x10000",
            str(firmware),
        ],
        cwd=project_dir,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Build/flash and smoke-test LimitlezzOS T-Deck serial CLI.")
    parser.add_argument("--project-dir", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--port", default=default_port())
    parser.add_argument("--env", default="tdeck")
    parser.add_argument("--baud", type=int, default=115200, help="Serial CLI baud rate.")
    parser.add_argument("--upload-baud", type=int, default=115200, help="Baud rate for --no-stub-upload.")
    parser.add_argument("--skip-upload", action="store_true")
    parser.add_argument("--no-stub-upload", action="store_true", help="Use esptool.py --no-stub after building.")
    parser.add_argument("--skip-build", action="store_true", help="With --no-stub-upload, flash existing artifacts.")
    parser.add_argument("--artifact-dir", help="Directory containing firmware.bin, bootloader.bin, and partitions.bin.")
    parser.add_argument("--no-reset-after-upload", action="store_true", help="Do not pulse serial reset before smoke.")
    parser.add_argument("--reset", action="store_true", help="Pulse reset even when --skip-upload is used.")
    parser.add_argument("--open-timeout", type=float, default=60.0)
    parser.add_argument("--boot-timeout", type=float, default=45.0)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--no-expect", action="store_true")
    parser.add_argument("--commands", nargs="*", default=DEFAULT_COMMANDS)
    args = parser.parse_args()

    project_dir = Path(args.project_dir).resolve()
    artifact_dir = Path(args.artifact_dir).resolve() if args.artifact_dir else None
    if not args.skip_upload:
        if args.no_stub_upload:
            if not args.skip_build:
                run(["pio", "run", "-e", args.env], cwd=project_dir)
            nostub_upload(project_dir, args.env, args.port, args.upload_baud, artifact_dir)
        else:
            if args.skip_build or artifact_dir is not None:
                raise SystemExit("--skip-build/--artifact-dir require --no-stub-upload")
            run(["pio", "run", "-e", args.env, "-t", "upload", "--upload-port", args.port], cwd=project_dir)

    harness = project_dir / "scripts" / "serial_harness.py"
    cmd = [
        sys.executable,
        str(harness),
        "--port",
        args.port,
        "--baud",
        str(args.baud),
        "--open-timeout",
        str(args.open_timeout),
        "--boot-timeout",
        str(args.boot_timeout),
        "--timeout",
        str(args.timeout),
    ]
    if args.no_expect:
        cmd.append("--no-expect")
    if args.reset or (not args.skip_upload and not args.no_reset_after_upload):
        cmd.append("--reset")
    cmd.append("--commands")
    cmd.extend(args.commands)
    run(cmd, cwd=project_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
