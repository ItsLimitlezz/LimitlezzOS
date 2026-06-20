#!/usr/bin/env python3
"""
Smoke-test MeshCore USB companion v0 serial-console commands.

The helper is intentionally conservative: it defaults to the implemented
`companion mc ...` command names, but every command and marker can be
overridden while the firmware command surface is still settling. Use
`--mc0-usb` to probe the formal raw MC0 request/response mode.
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
    decode,
    open_port_retry,
    pulse_reset,
    run_command,
    sync_prompt,
    write_line,
)
from serial_harness import serial  # pyserial module imported by the harness


DEFAULT_STATUS_COMMAND = "companion mc status"
DEFAULT_TEST_COMMAND = "companion mc test"
DEFAULT_PUBLIC_TEMPLATE = "companion mc send {text}"
DEFAULT_MC0_ENTER_COMMAND = "companion mc usb on"
DEFAULT_MC0_HELLO_ID = "1"
DEFAULT_MC0_IDENTITY_ID = "2"
DEFAULT_MC0_STATUS_ID = "3"
DEFAULT_MC0_NODES_ID = "4"
DEFAULT_MC0_EXIT_ID = "99"
DEFAULT_MC0_HELLO_TEMPLATE = "MC0 {id} HELLO proto=0 app=limitlezz-smoke host=windows want=none"
DEFAULT_MC0_IDENTITY_TEMPLATE = "MC0 {id} IDENTITY"
DEFAULT_MC0_STATUS_TEMPLATE = "MC0 {id} STATUS"
DEFAULT_MC0_NODES_TEMPLATE = "MC0 {id} NODES since=0 limit=5"
DEFAULT_MC0_EXIT_TEMPLATE = "MC0 {id} EXIT"
DEFAULT_STATUS_MARKERS = ["mccomp: status", "MeshCore", "MC companion"]
DEFAULT_TEST_MARKERS = ["PASS"]
DEFAULT_PUBLIC_MARKERS = ["[ok]", "sent", "queued"]
ERROR_MARKERS = ("[err]", "unknown command", "Unknown command", "usage:")
MC0_ERROR_MARKERS = (" ERR code=", " ERR ")
FALLBACK_HINTS = (
    "USB companion mode:",
    "BLE companion:",
    "not present",
    "not implemented",
)
PROMPT_TEXT = "lz> "


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


def expand_markers(values: list[str] | None) -> list[str]:
    markers: list[str] = []
    for value in values or []:
        markers.extend(split_markers(value))
    return markers


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


def validate_template_fields(option: str, template: str, allowed: set[str]) -> None:
    field_names = {
        name
        for _, name, _, _ in Formatter().parse(template)
        if name is not None and name != ""
    }
    unsupported = sorted(field_names - allowed)
    if unsupported:
        names = ", ".join(unsupported)
        allowed_names = ", ".join(f"{{{name}}}" for name in sorted(allowed)) or "no fields"
        raise SystemExit(f"{option} only supports {allowed_names}; unsupported: {names}")


def validate_public_template(template: str) -> None:
    validate_template_fields("--public-command-template", template, {"text"})


def format_mc0_template(option: str, template: str, request_id: str) -> str:
    validate_template_fields(option, template, {"id"})
    return template.format(id=request_id)


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


def default_marker_override(custom: list[str] | None, defaults: list[str]) -> list[str]:
    if custom is None:
        return list(defaults)
    return expand_markers(custom)


def build_mc0_specs(args: argparse.Namespace) -> list[CommandSpec]:
    hello_command = format_mc0_template(
        "--mc0-hello-template", args.mc0_hello_template, args.mc0_hello_id
    )
    identity_command = format_mc0_template(
        "--mc0-identity-template", args.mc0_identity_template, args.mc0_identity_id
    )
    status_command = format_mc0_template(
        "--mc0-status-template", args.mc0_status_template, args.mc0_status_id
    )
    nodes_command = format_mc0_template(
        "--mc0-nodes-template", args.mc0_nodes_template, args.mc0_nodes_id
    )
    return [
        CommandSpec(
            "mc0 hello",
            hello_command,
            default_marker_override(
                args.mc0_hello_marker,
                [f"MC0 {args.mc0_hello_id} OK"],
            ),
        ),
        CommandSpec(
            "mc0 identity",
            identity_command,
            default_marker_override(
                args.mc0_identity_marker,
                [
                    f"MC0 {args.mc0_identity_id} OK",
                    "addr_format=meshcore-pubkey-hex",
                    "pubkey=",
                ],
            ),
        ),
        CommandSpec(
            "mc0 status",
            status_command,
            default_marker_override(
                args.mc0_status_marker,
                [f"MC0 {args.mc0_status_id} OK"],
            ),
        ),
        CommandSpec(
            "mc0 nodes",
            nodes_command,
            default_marker_override(
                args.mc0_nodes_marker,
                [f"MC0 {args.mc0_nodes_id} BEGIN", f"MC0 {args.mc0_nodes_id} END"],
            ),
        ),
    ]


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


def has_all_markers(output: str, markers: list[str]) -> bool:
    return all(marker in output for marker in markers)


def output_has_response_boundary(output: str, markers: list[str]) -> bool:
    if not markers:
        return True
    if PROMPT_TEXT in markers and PROMPT_TEXT in output:
        return True
    return output.endswith("\n")


def fail_on_error_markers(label: str, output: str, allow_error_output: bool) -> None:
    if allow_error_output:
        return
    for marker in ERROR_MARKERS:
        if marker in output:
            raise SmokeFailure(f"[mc-smoke] {label} reported error marker {marker!r}")
    for marker in MC0_ERROR_MARKERS:
        if marker in output:
            raise SmokeFailure(f"[mc-smoke] {label} reported MC0 error marker {marker!r}")


def assert_output(spec: CommandSpec, output: str, allow_error_output: bool) -> None:
    fail_on_error_markers(f"command {spec.command!r}", output, allow_error_output)

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


def assert_mc0_output(spec: CommandSpec, output: str, allow_error_output: bool) -> None:
    fail_on_error_markers(spec.label, output, allow_error_output)
    if not has_all_markers(output, spec.markers):
        missing = ", ".join(repr(marker) for marker in spec.markers if marker not in output)
        raise SmokeFailure(f"[mc-smoke] missing MC0 marker(s) for {spec.command!r}: {missing}")


def read_until_markers_or_idle(
    port: serial.Serial,
    markers: list[str],
    timeout: float,
    idle_timeout: float,
    label: str,
    allow_error_output: bool,
) -> str:
    end = time.monotonic() + timeout
    idle_deadline = time.monotonic() + idle_timeout
    buf = bytearray()
    while time.monotonic() < end:
        chunk = port.read(1)
        if chunk:
            buf.extend(chunk)
            idle_deadline = time.monotonic() + idle_timeout
            output = decode(bytes(buf))
            fail_on_error_markers(label, output, allow_error_output)
            if has_all_markers(output, markers) and output_has_response_boundary(output, markers):
                return output
        else:
            if not markers and time.monotonic() >= idle_deadline:
                return decode(bytes(buf))
            time.sleep(0.03)

    output = decode(bytes(buf))
    if markers:
        missing = ", ".join(repr(marker) for marker in markers if marker not in output)
        detail = f"[mc-smoke] timed out waiting for {label}; missing marker(s): {missing}"
    else:
        detail = f"[mc-smoke] timed out waiting for {label}"
    if output.strip():
        detail = "\n".join([detail, "[mc-smoke] partial output:", output.rstrip()])
    raise SmokeFailure(detail)


def run_specs(port: serial.Serial, specs: list[CommandSpec], args: argparse.Namespace) -> None:
    for spec in specs:
        marker_note = ", ".join(repr(marker) for marker in spec.markers) or "none"
        print(f"[mc-smoke] > {spec.command}  [{spec.label}; expect: {marker_note}]")
        output = run_command(port, spec.command, args.timeout)
        if output.strip():
            print(output.rstrip())
        assert_output(spec, output, args.allow_error_output)


def run_mc0_specs(port: serial.Serial, specs: list[CommandSpec], args: argparse.Namespace) -> None:
    # These command names are intentionally configurable so older artifacts or
    # later protocol revisions can still be probed without weakening checks.
    entered = False
    if args.mc0_enter_command:
        enter_markers = default_marker_override(args.mc0_enter_marker, [])
        enter_spec = CommandSpec("mc0 enter", args.mc0_enter_command, enter_markers)
        marker_note = ", ".join(repr(marker) for marker in enter_spec.markers) or "none"
        print(f"[mc-smoke] > {enter_spec.command}  [{enter_spec.label}; expect: {marker_note}]")
        port.reset_input_buffer()
        write_line(port, enter_spec.command)
        output = read_until_markers_or_idle(
            port,
            enter_spec.markers,
            args.mc0_enter_timeout,
            args.mc0_idle_timeout,
            enter_spec.label,
            args.allow_error_output,
        )
        if output.strip():
            print(output.rstrip())
        assert_mc0_output(enter_spec, output, args.allow_error_output)
        if args.mc0_enter_settle > 0:
            time.sleep(args.mc0_enter_settle)
        entered = True

    failure: SmokeFailure | None = None
    try:
        for spec in specs:
            marker_note = ", ".join(repr(marker) for marker in spec.markers) or "none"
            print(f"[mc-smoke] > {spec.command}  [{spec.label}; expect: {marker_note}]")
            write_line(port, spec.command)
            output = read_until_markers_or_idle(
                port,
                spec.markers,
                args.timeout,
                args.mc0_idle_timeout,
                spec.label,
                args.allow_error_output,
            )
            if output.strip():
                print(output.rstrip())
            assert_mc0_output(spec, output, args.allow_error_output)
    except SmokeFailure as exc:
        failure = exc

    exit_failure: SmokeFailure | None = None
    if entered and args.mc0_exit_template:
        exit_command = format_mc0_template(
            "--mc0-exit-template", args.mc0_exit_template, args.mc0_exit_id
        )
        exit_markers = default_marker_override(
            args.mc0_exit_marker,
            [f"MC0 {args.mc0_exit_id} OK", PROMPT_TEXT],
        )
        exit_spec = CommandSpec("mc0 exit", exit_command, exit_markers)
        marker_note = ", ".join(repr(marker) for marker in exit_spec.markers) or "none"
        print(f"[mc-smoke] > {exit_spec.command}  [{exit_spec.label}; expect: {marker_note}]")
        try:
            write_line(port, exit_spec.command)
            output = read_until_markers_or_idle(
                port,
                exit_spec.markers,
                args.mc0_exit_timeout,
                args.mc0_idle_timeout,
                exit_spec.label,
                args.allow_error_output,
            )
            if output.strip():
                print(output.rstrip())
            assert_mc0_output(exit_spec, output, args.allow_error_output)
        except SmokeFailure as exc:
            exit_failure = exc
            print(str(exc), file=sys.stderr)

    if failure is not None:
        raise failure
    if exit_failure is not None:
        raise exit_failure


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
    mc0 = parser.add_argument_group("formal MC0 USB mode")
    mc0.add_argument(
        "--mc0-usb",
        action="store_true",
        help=(
            "Opt into the formal MC0 USB protocol smoke instead of the "
            "default serial-console status/test smoke."
        ),
    )
    mc0.add_argument(
        "--mc0-enter-command",
        default=DEFAULT_MC0_ENTER_COMMAND,
        help=(
            "Console command used to enter formal MeshCore USB mode. "
            "Override this when testing older artifacts or protocol revisions."
        ),
    )
    mc0.add_argument(
        "--mc0-enter-marker",
        action="append",
        help=(
            "Optional marker to require after the enter command. Repeat or use "
            "MARKER1|MARKER2. No marker is required by default because the "
            "command may take ownership of the port without printing a prompt."
        ),
    )
    mc0.add_argument(
        "--mc0-enter-timeout",
        type=float,
        default=5.0,
        help="Seconds to wait for optional output from the MC0 enter command.",
    )
    mc0.add_argument(
        "--mc0-enter-settle",
        type=float,
        default=0.2,
        help="Seconds to wait after the enter command before sending MC0 requests.",
    )
    mc0.add_argument(
        "--mc0-idle-timeout",
        type=float,
        default=0.5,
        help="Seconds of quiet serial input that ends an optional MC0 read.",
    )
    mc0.add_argument("--mc0-hello-id", default=DEFAULT_MC0_HELLO_ID)
    mc0.add_argument("--mc0-identity-id", default=DEFAULT_MC0_IDENTITY_ID)
    mc0.add_argument("--mc0-status-id", default=DEFAULT_MC0_STATUS_ID)
    mc0.add_argument("--mc0-nodes-id", default=DEFAULT_MC0_NODES_ID)
    mc0.add_argument("--mc0-exit-id", default=DEFAULT_MC0_EXIT_ID)
    mc0.add_argument(
        "--mc0-hello-template",
        default=DEFAULT_MC0_HELLO_TEMPLATE,
        help="MC0 HELLO line template; supports {id}.",
    )
    mc0.add_argument(
        "--mc0-identity-template",
        default=DEFAULT_MC0_IDENTITY_TEMPLATE,
        help="MC0 IDENTITY line template; supports {id}.",
    )
    mc0.add_argument(
        "--mc0-status-template",
        default=DEFAULT_MC0_STATUS_TEMPLATE,
        help="MC0 STATUS line template; supports {id}.",
    )
    mc0.add_argument(
        "--mc0-nodes-template",
        default=DEFAULT_MC0_NODES_TEMPLATE,
        help="MC0 NODES line template; supports {id}.",
    )
    mc0.add_argument(
        "--mc0-hello-marker",
        action="append",
        help="Override HELLO expected marker(s). Defaults to 'MC0 <hello-id> OK'.",
    )
    mc0.add_argument(
        "--mc0-identity-marker",
        action="append",
        help=(
            "Override IDENTITY expected marker(s). Defaults to OK, "
            "addr_format=meshcore-pubkey-hex, and pubkey=."
        ),
    )
    mc0.add_argument(
        "--mc0-status-marker",
        action="append",
        help="Override STATUS expected marker(s). Defaults to 'MC0 <status-id> OK'.",
    )
    mc0.add_argument(
        "--mc0-nodes-marker",
        action="append",
        help=(
            "Override NODES expected marker(s). Defaults to both "
            "'MC0 <nodes-id> BEGIN' and 'MC0 <nodes-id> END'."
        ),
    )
    mc0.add_argument(
        "--mc0-exit-template",
        default=DEFAULT_MC0_EXIT_TEMPLATE,
        help=(
            "MC0 line used to leave formal USB mode and return to the console; "
            "supports {id}. Set to an empty string to skip the exit attempt."
        ),
    )
    mc0.add_argument(
        "--mc0-exit-marker",
        action="append",
        help="Override exit expected marker(s). Defaults to MC0 OK plus the LimitlezzOS prompt.",
    )
    mc0.add_argument(
        "--mc0-exit-timeout",
        type=float,
        default=10.0,
        help="Seconds to wait for the MC0 exit response or console prompt.",
    )
    args = parser.parse_args()

    specs = build_mc0_specs(args) if args.mc0_usb else build_command_specs(args)
    try:
        port, initial = open_console(args)
        with port:
            if initial.strip():
                print(initial.rstrip())
            if args.mc0_usb:
                run_mc0_specs(port, specs, args)
            else:
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
