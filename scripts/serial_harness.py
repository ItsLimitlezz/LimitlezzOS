#!/usr/bin/env python3
"""
Serial smoke harness for LimitlezzOS T-Deck builds.

Defaults to COM8 because that is the current hardware validation port for this
workspace. Override with --port or LZ_SERIAL_PORT when testing another board.
"""
from __future__ import annotations

import argparse
import os
import sys
import time

try:
    import serial
except ImportError as exc:  # pragma: no cover - host setup guard
    raise SystemExit("pyserial is required. PlatformIO installs it, or run: pip install pyserial") from exc


PROMPT = b"lz> "
DEFAULT_COMMANDS = ["id", "sys", "net", "rf", "stats", "wifi", "companion test"]
DEFAULT_EXPECT = {
    "id": "identity:",
    "sys": "battery:",
    "net": "networks:",
    "rf": "mode:",
    "stats": "radio:",
    "wifi": "wifi:",
    "companion test": "PASS",
}
ROM_DOWNLOAD_MARKERS = ("waiting for download", "DOWNLOAD(USB/UART0)", "DOWNLOAD")


class RomDownloadMode(RuntimeError):
    pass


def decode(data: bytes) -> str:
    return data.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")


def read_until_prompt(port: serial.Serial, timeout: float) -> bytes:
    end = time.monotonic() + timeout
    buf = bytearray()
    while time.monotonic() < end:
        chunk = port.read(1)
        if chunk:
            buf.extend(chunk)
            text = decode(bytes(buf))
            if any(marker in text for marker in ROM_DOWNLOAD_MARKERS):
                raise RomDownloadMode(text)
            if PROMPT in buf:
                return bytes(buf)
        else:
            time.sleep(0.03)
    raise TimeoutError(decode(bytes(buf)))


def write_line(port: serial.Serial, text: str) -> None:
    port.write((text + "\n").encode("utf-8"))
    port.flush()


def open_port(name: str, baud: int, timeout: float, dtr: str, rts: str) -> serial.Serial:
    port = serial.Serial()
    port.port = name
    port.baudrate = baud
    port.timeout = 0.1
    port.write_timeout = timeout
    port.rtscts = False
    port.dsrdtr = False
    if dtr != "default":
        port.dtr = dtr == "on"
    if rts != "default":
        port.rts = rts == "on"
    port.open()
    return port


def open_port_retry(name: str, baud: int, timeout: float, open_timeout: float, dtr: str, rts: str) -> serial.Serial:
    end = time.monotonic() + open_timeout
    last_error: Exception | None = None
    while True:
        try:
            return open_port(name, baud, timeout, dtr, rts)
        except serial.SerialException as exc:
            last_error = exc
            if time.monotonic() >= end:
                print(f"[serial] open failed for {name}: {exc}", file=sys.stderr)
                raise
            time.sleep(0.5)


def pulse_reset(port: serial.Serial, settle: float) -> None:
    # Keep GPIO0 released and pulse EN/reset through RTS. Some Windows CDC
    # drivers briefly disconnect here; the caller retries the port open.
    port.dtr = False
    port.rts = True
    time.sleep(0.2)
    port.rts = False
    time.sleep(settle)
    port.reset_input_buffer()


def sync_prompt(port: serial.Serial, timeout: float) -> str:
    port.reset_input_buffer()
    write_line(port, "")
    try:
        return decode(read_until_prompt(port, timeout))
    except TimeoutError:
        write_line(port, "")
        return decode(read_until_prompt(port, timeout))


def run_command(port: serial.Serial, command: str, timeout: float) -> str:
    port.reset_input_buffer()
    write_line(port, command)
    return decode(read_until_prompt(port, timeout))


def main() -> int:
    parser = argparse.ArgumentParser(description="Smoke-test the LimitlezzOS serial CLI.")
    parser.add_argument("--port", default=os.environ.get("LZ_SERIAL_PORT", "COM8"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--open-timeout", type=float, default=60.0, help="Seconds to wait for a COM port to appear.")
    parser.add_argument("--boot-timeout", type=float, help="Seconds to wait for the first LimitlezzOS prompt.")
    parser.add_argument("--commands", nargs="*", default=DEFAULT_COMMANDS)
    parser.add_argument("--open-only", action="store_true", help="Only verify the port can be opened.")
    parser.add_argument("--dtr", choices=["default", "on", "off"], default="off")
    parser.add_argument("--rts", choices=["default", "on", "off"], default="off")
    parser.add_argument("--reset", action="store_true", help="Pulse reset before waiting for the prompt.")
    parser.add_argument("--reset-settle", type=float, default=1.5, help="Seconds to wait after reset pulse.")
    parser.add_argument("--no-expect", action="store_true", help="Do not assert default command markers.")
    args = parser.parse_args()

    if not args.open_only and not args.commands:
        print("[serial] no commands requested; refusing to report a false PASS", file=sys.stderr)
        return 2

    boot_timeout = args.boot_timeout if args.boot_timeout is not None else args.timeout
    print(f"[serial] opening {args.port} @ {args.baud}")
    boot_deadline = time.monotonic() + boot_timeout
    last_rom = ""
    rom_notice_printed = False
    prompt_notice_printed = False
    disconnect_notice_printed = False
    try:
        while True:
            remaining_boot = max(0.1, boot_deadline - time.monotonic())
            with open_port_retry(args.port, args.baud, args.timeout,
                                 min(args.open_timeout, remaining_boot),
                                 args.dtr, args.rts) as port:
                if args.open_only:
                    print("[serial] open ok")
                    return 0

                if not prompt_notice_printed:
                    print("[serial] waiting for LimitlezzOS prompt")
                    prompt_notice_printed = True
                try:
                    if args.reset:
                        pulse_reset(port, args.reset_settle)
                    initial = sync_prompt(port, remaining_boot)
                except RomDownloadMode as exc:
                    last_rom = str(exc).strip()
                    if time.monotonic() >= boot_deadline:
                        raise
                    if not rom_notice_printed:
                        print("[serial] ESP-ROM download mode detected; press RESET or power-cycle the T-Deck, still waiting...")
                        rom_notice_printed = True
                    time.sleep(1.0)
                    continue
                except serial.SerialException as exc:
                    if time.monotonic() >= boot_deadline:
                        print(f"[serial] serial error on {args.port}: {exc}", file=sys.stderr)
                        return 1
                    if not disconnect_notice_printed:
                        print(f"[serial] {args.port} disconnected during USB boot handoff; waiting for it to return...")
                        disconnect_notice_printed = True
                    time.sleep(1.0)
                    continue

                if initial.strip():
                    print(initial.rstrip())

                for command in args.commands:
                    print(f"[serial] > {command}")
                    output = run_command(port, command, args.timeout)
                    print(output.rstrip())
                    if not args.no_expect:
                        marker = DEFAULT_EXPECT.get(command)
                        if marker and marker not in output:
                            print(f"[serial] missing expected marker for '{command}': {marker}", file=sys.stderr)
                            return 2

                print("[serial] smoke PASS")
                return 0
    except serial.SerialException as exc:
        print(f"[serial] serial error on {args.port}: {exc}", file=sys.stderr)
        return 1
    except RomDownloadMode as exc:
        partial = (last_rom or str(exc)).strip()
        print("[serial] device is in ESP-ROM download mode, not LimitlezzOS", file=sys.stderr)
        if partial:
            print("[serial] ROM output:", file=sys.stderr)
            print(partial, file=sys.stderr)
        print("[serial] press RESET or power-cycle the T-Deck, then rerun the smoke with -SkipUpload.", file=sys.stderr)
        return 3
    except TimeoutError as exc:
        if last_rom:
            print("[serial] device is in ESP-ROM download mode, not LimitlezzOS", file=sys.stderr)
            print("[serial] ROM output:", file=sys.stderr)
            print(last_rom, file=sys.stderr)
            print("[serial] press RESET or power-cycle the T-Deck, then rerun the smoke with -SkipUpload.", file=sys.stderr)
            return 3
        partial = str(exc).strip()
        print("[serial] timed out waiting for prompt/output", file=sys.stderr)
        if partial:
            print("[serial] partial output:", file=sys.stderr)
            print(partial, file=sys.stderr)
        print(f"[serial] requested port: {args.port}", file=sys.stderr)
        print("[serial] hint: make sure the device is running LimitlezzOS text-console mode, not companion mode.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
