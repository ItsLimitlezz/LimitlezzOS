#!/usr/bin/env python3
"""
Smoke-test MeshCore USB companion v0 serial-console commands.

The helper is intentionally conservative: it defaults to the implemented
`companion mc ...` command names, but every command and marker can be
overridden while the firmware command surface is still settling.
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass, field
from string import Formatter

from serial_harness import (
    RomDownloadMode,
    open_port_retry,
    pulse_reset,
    run_command,
    sync_prompt,
)
from serial_harness import serial  # pyserial module imported by the harness


DEFAULT_STATUS_COMMAND = "companion mc status"
DEFAULT_TEST_COMMAND = "companion mc test"
DEFAULT_PUBLIC_TEMPLATE = "companion mc send {text}"
DEFAULT_STATUS_MARKERS = ["mccomp: status", "MeshCore", "MC companion"]
DEFAULT_TEST_MARKERS = ["PASS"]
DEFAULT_PUBLIC_MARKERS = ["[ok]", "sent", "queued"]
ERROR_MARKERS = ("[err]", "unknown command", "Unknown command", "usage:")
FALLBACK_HINTS = (
    "USB companion mode:",
    "BLE companion:",
    "not present",
    "not implemented",
)


@dataclass
class CommandSpec:
    label: str
    command: str
    markers: list[str] = field(default_factory=list)


class SmokeFailure(RuntimeError):
    def __init__(self, message: str, exit_code: int = 2) -> None:
        super().__init__(message)
        self.exit_code = exit_code


def split_markers(value: str) -> list[str]:
    markers = [part.strip() for part in value.split("|")]
    return [marker for marker in markers if marker]


def parse_expectations(values: list[str] | None) -> dict[str, list[str]]:
    expectations: dict[str, list[str]] = {}
    for value in values or []:
        if "=" not in value:
            raise SystemExit(
                f"invalid --expect value {value!r}; expected COMMAND=MARKER "
                "or COMMAND=MARKER1|MARKER2"
            )
        command, marker_text = value.split("=", 1)
        command = command.strip()
        markers = split_markers(marker_text)
        if not command or not markers:
            raise SystemExit(f"invalid --expect value {value!r}; command and marker are required")
        expectations.setdefault(command, []).extend(markers)
    return expectations


def validate_public_template(template: str) -> None:
    field_names = {
        name
        for _, name, _, _ in Formatter().parse(template)
        if name is not None and name != ""
    }
    unsupported = sorted(field_names - {"text"})
    if unsupported:
        names = ", ".join(unsupported)
        raise SystemExit(f"--public-command-template only supports {{text}}; unsupported: {names}")


def add_markers(
    specs: list[CommandSpec],
    command: str,
    markers: list[str],
    no_default_expect: bool,
) -> None:
    if no_default_expect:
        return
    for spec in specs:
        if spec.command == command:
            spec.markers.extend(marker for marker in markers if marker not in spec.markers)
            return


def build_command_specs(args: argparse.Namespace) -> list[CommandSpec]:
    if args.commands is not None:
        specs = [CommandSpec(f"command {idx}", command) for idx, command in enumerate(args.commands, 1)]
    else:
        specs = [
            CommandSpec("status", args.status_command),
            CommandSpec("test", args.test_command),
        ]

    if args.public_text is not None:
        validate_public_template(args.public_command_template)
        specs.append(
            CommandSpec(
                "public",
                args.public_command_template.format(text=args.public_text),
            )
        )

    if not specs:
        raise SystemExit("[mc-smoke] no commands requested; refusing to report a false PASS")

    add_markers(specs, args.status_command, args.status_marker, args.no_default_expect)
    add_markers(specs, args.test_command, args.test_marker, args.no_default_expect)
    if args.public_text is not None:
        add_markers(specs, specs[-1].command, args.public_marker, args.no_default_expect)

    explicit_expectations = parse_expectations(args.expect)
    for spec in specs:
        spec.markers.extend(explicit_expectations.get(spec.command, []))

    unused = sorted(set(explicit_expectations) - {spec.command for spec in specs})
    if unused:
        names = "\n  ".join(unused)
        raise SystemExit(f"--expect was provided for command(s) not in this smoke run:\n  {names}")

    return specs


def open_console(args: argparse.Namespace) -> tuple[serial.Serial, str]:
    boot_timeout = args.boot_timeout if args.boot_timeout is not None else args.open_timeout
    boot_deadline = time.monotonic() + boot_timeout
    last_rom = ""
    rom_notice_printed = False
    prompt_notice_printed = False
    disconnect_notice_printed = False

    print(f"[mc-smoke] opening {args.port} @ {args.baud}")
    while True:
        remaining_boot = max(0.1, boot_deadline - time.monotonic())
        port = open_port_retry(
            args.port,
            args.baud,
            args.timeout,
            min(args.open_timeout, remaining_boot),
            args.dtr,
            args.rts,
        )
        try:
            if not prompt_notice_printed:
                print("[mc-smoke] waiting for LimitlezzOS prompt")
                prompt_notice_printed = True
            if args.reset:
                pulse_reset(port, args.reset_settle)
            initial = sync_prompt(port, remaining_boot)
            return port, initial
        except RomDownloadMode as exc:
            last_rom = str(exc).strip()
            port.close()
            if time.monotonic() >= boot_deadline:
                raise
            if not rom_notice_printed:
                print(
                    "[mc-smoke] ESP-ROM download mode detected; press RESET or power-cycle "
                    "the T-Deck, still waiting..."
                )
                rom_notice_printed = True
            time.sleep(1.0)
        except serial.SerialException as exc:
            port.close()
            if time.monotonic() >= boot_deadline:
                raise SmokeFailure(f"[mc-smoke] serial error on {args.port}: {exc}", 1) from exc
            if not disconnect_notice_printed:
                print(
                    f"[mc-smoke] {args.port} disconnected during USB boot handoff; "
                    "waiting for it to return..."
                )
                disconnect_notice_printed = True
            time.sleep(1.0)
        except TimeoutError as exc:
            port.close()
            if last_rom:
                message = "\n".join(
                    [
                        "[mc-smoke] device is in ESP-ROM download mode, not LimitlezzOS",
                        "[mc-smoke] ROM output:",
                        last_rom,
                        "[mc-smoke] press RESET or power-cycle the T-Deck, then rerun the smoke.",
                    ]
                )
                raise SmokeFailure(message, 3) from exc
            partial = str(exc).strip()
            lines = ["[mc-smoke] timed out waiting for the LimitlezzOS prompt"]
            if partial:
                lines.extend(["[mc-smoke] partial output:", partial])
            lines.extend(
                [
                    f"[mc-smoke] requested port: {args.port}",
                    "[mc-smoke] hint: make sure the device is running text-console mode.",
                ]
            )
            raise SmokeFailure("\n".join(lines), 1) from exc


def assert_output(spec: CommandSpec, output: str, allow_error_output: bool) -> None:
    if not allow_error_output:
        for marker in ERROR_MARKERS:
            if marker in output:
                raise SmokeFailure(
                    f"[mc-smoke] command {spec.command!r} reported error marker {marker!r}"
                )

    if not spec.markers:
        return

    if any(marker in output for marker in spec.markers):
        return

    marker_list = ", ".join(repr(marker) for marker in spec.markers)
    lines = [
        f"[mc-smoke] missing expected marker for {spec.command!r}: one of {marker_list}",
    ]
    if any(hint in output for hint in FALLBACK_HINTS):
        lines.append(
            "[mc-smoke] the command may not be implemented yet or may have fallen "
            "back to the existing companion status path"
        )
    lines.append(
        "[mc-smoke] use --status-command/--test-command/--commands or "
        "--expect to match the firmware surface, or --no-default-expect for "
        "prompt-only probing"
    )
    raise SmokeFailure("\n".join(lines))


def run_specs(port: serial.Serial, specs: list[CommandSpec], args: argparse.Namespace) -> None:
    for spec in specs:
        marker_note = ", ".join(repr(marker) for marker in spec.markers) or "none"
        print(f"[mc-smoke] > {spec.command}  [{spec.label}; expect: {marker_note}]")
        output = run_command(port, spec.command, args.timeout)
        if output.strip():
            print(output.rstrip())
        assert_output(spec, output, args.allow_error_output)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Smoke-test MeshCore companion USB v0 commands over the LimitlezzOS serial console."
    )
    parser.add_argument("--port", default=os.environ.get("LZ_SERIAL_PORT", "COM8"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=30.0, help="Seconds to wait for each command response.")
    parser.add_argument("--open-timeout", type=float, default=60.0, help="Seconds to wait for a COM port to appear.")
    parser.add_argument("--boot-timeout", type=float, help="Seconds to wait for the first LimitlezzOS prompt.")
    parser.add_argument("--dtr", choices=["default", "on", "off"], default="off")
    parser.add_argument("--rts", choices=["default", "on", "off"], default="off")
    parser.add_argument("--reset", action="store_true", help="Pulse reset before waiting for the prompt.")
    parser.add_argument("--reset-settle", type=float, default=1.5, help="Seconds to wait after reset pulse.")
    parser.add_argument("--status-command", default=DEFAULT_STATUS_COMMAND)
    parser.add_argument("--test-command", default=DEFAULT_TEST_COMMAND)
    parser.add_argument(
        "--commands",
        nargs="*",
        help="Replace the default status/test command list. Pair with --expect for custom markers.",
    )
    parser.add_argument("--public-text", help="Append a public MeshCore smoke send command with this text.")
    parser.add_argument("--public-command-template", default=DEFAULT_PUBLIC_TEMPLATE)
    parser.add_argument("--status-marker", action="append", default=list(DEFAULT_STATUS_MARKERS))
    parser.add_argument("--test-marker", action="append", default=list(DEFAULT_TEST_MARKERS))
    parser.add_argument("--public-marker", action="append", default=list(DEFAULT_PUBLIC_MARKERS))
    parser.add_argument(
        "--expect",
        action="append",
        help="Add a marker assertion as COMMAND=MARKER or COMMAND=MARKER1|MARKER2.",
    )
    parser.add_argument("--no-default-expect", action="store_true", help="Do not assert built-in planned markers.")
    parser.add_argument("--allow-error-output", action="store_true", help="Do not fail on [err]/usage/unknown markers.")
    args = parser.parse_args()

    specs = build_command_specs(args)
    try:
        port, initial = open_console(args)
        with port:
            if initial.strip():
                print(initial.rstrip())
            run_specs(port, specs, args)
        print("[mc-smoke] smoke PASS")
        return 0
    except serial.SerialException as exc:
        print(f"[mc-smoke] serial error on {args.port}: {exc}", file=sys.stderr)
        return 1
    except RomDownloadMode as exc:
        partial = str(exc).strip()
        print("[mc-smoke] device is in ESP-ROM download mode, not LimitlezzOS", file=sys.stderr)
        if partial:
            print("[mc-smoke] ROM output:", file=sys.stderr)
            print(partial, file=sys.stderr)
        print("[mc-smoke] press RESET or power-cycle the T-Deck, then rerun the smoke.", file=sys.stderr)
        return 3
    except TimeoutError as exc:
        partial = str(exc).strip()
        print("[mc-smoke] timed out waiting for prompt/output", file=sys.stderr)
        if partial:
            print("[mc-smoke] partial output:", file=sys.stderr)
            print(partial, file=sys.stderr)
        print(f"[mc-smoke] requested port: {args.port}", file=sys.stderr)
        return 1
    except SmokeFailure as exc:
        print(str(exc), file=sys.stderr)
        return exc.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
