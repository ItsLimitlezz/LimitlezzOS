#!/usr/bin/env python3
"""
Validate the LimitlezzOS split-airtime TDM scheduler over the serial CLI.

This is a hardware smoke probe, not a firmware build step. It defaults to COM8
on Windows and /dev/ttyACM0 elsewhere, but accepts --port for any developer rig.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time
from dataclasses import dataclass

import serial_harness


EXPECTED_SPLITS = {
    "mt": (60, 40, 300, 200),
    "balanced": (50, 50, 250, 250),
    "mc": (40, 60, 200, 300),
}


@dataclass(frozen=True)
class Airtime:
    label: str
    mt_pct: int
    mc_pct: int


@dataclass(frozen=True)
class RfSnapshot:
    mode: str
    mt_dwell_ms: int
    mc_dwell_ms: int
    active: str
    switches: int


class SmokeFailure(RuntimeError):
    pass


def default_port() -> str:
    env_port = os.environ.get("LZ_SERIAL_PORT")
    if env_port:
        return env_port
    return "COM8" if os.name == "nt" else "/dev/ttyACM0"


def parse_airtime(text: str) -> Airtime:
    match = re.search(r"airtime:\s*(.*?)\s+MT\s+(\d+)%\s*/\s*MC\s+(\d+)%", text)
    if not match:
        raise SmokeFailure(f"could not parse airtime output:\n{text}")
    return Airtime(match.group(1).strip(), int(match.group(2)), int(match.group(3)))


def parse_rf(text: str) -> RfSnapshot:
    mode = re.search(r"^mode:\s*(.+)$", text, re.MULTILINE)
    dwell = re.search(r"^dwell:\s*Meshtastic\s+(\d+)ms\s*/\s*MeshCore\s+(\d+)ms", text, re.MULTILINE)
    active = re.search(r"^active:\s*(Meshtastic|MeshCore)\b", text, re.MULTILINE)
    switches = re.search(r"^switches:\s*(\d+)", text, re.MULTILINE)
    missing = [
        name
        for name, found in (
            ("mode", mode),
            ("dwell", dwell),
            ("active", active),
            ("switches", switches),
        )
        if not found
    ]
    if missing:
        raise SmokeFailure(f"missing rf field(s) {', '.join(missing)} in:\n{text}")
    return RfSnapshot(
        mode=mode.group(1).strip(),
        mt_dwell_ms=int(dwell.group(1)),
        mc_dwell_ms=int(dwell.group(2)),
        active=active.group(1),
        switches=int(switches.group(1)),
    )


def assert_split(snapshot: RfSnapshot, preset: str) -> None:
    mt_pct, mc_pct, mt_ms, mc_ms = EXPECTED_SPLITS[preset]
    expected_mode = f"SPLIT MT {mt_pct}% / MC {mc_pct}%"
    if not snapshot.mode.startswith(expected_mode):
        raise SmokeFailure(f"expected mode prefix '{expected_mode}', got '{snapshot.mode}'")
    if (snapshot.mt_dwell_ms, snapshot.mc_dwell_ms) != (mt_ms, mc_ms):
        raise SmokeFailure(
            f"expected dwell {mt_ms}/{mc_ms}ms for {preset}, "
            f"got {snapshot.mt_dwell_ms}/{snapshot.mc_dwell_ms}ms"
        )


def assert_airtime(airtime: Airtime, preset: str) -> None:
    mt_pct, mc_pct, _mt_ms, _mc_ms = EXPECTED_SPLITS[preset]
    if (airtime.mt_pct, airtime.mc_pct) != (mt_pct, mc_pct):
        raise SmokeFailure(
            f"expected airtime split {mt_pct}/{mc_pct}% for {preset}, "
            f"got {airtime.mt_pct}/{airtime.mc_pct}%"
        )


def run_cmd(port, command: str, timeout: float) -> str:
    print(f"[tdm] > {command}")
    output = serial_harness.run_command(port, command, timeout)
    print(output.rstrip())
    if "[err] MeshCore is gated" in output:
        raise SmokeFailure(
            "MeshCore is gated in this firmware. Build or flash a MeshCore-enabled "
            "TDM image before running split-airtime smoke."
        )
    return output


def restore_state(port, timeout: float, mt_on: bool, mc_on: bool, preset: str | None) -> None:
    try:
        if preset:
            serial_harness.run_command(port, f"airtime {preset}", timeout)
        serial_harness.run_command(port, f"net mt {'on' if mt_on else 'off'}", timeout)
        serial_harness.run_command(port, f"net mc {'on' if mc_on else 'off'}", timeout)
    except Exception as exc:  # pragma: no cover - best-effort hardware cleanup
        print(f"[tdm] warning: state restore failed: {exc}", file=sys.stderr)


def parse_net_state(text: str) -> tuple[bool, bool]:
    match = re.search(r"networks:\s*Meshtastic\s+(on|off),\s*MeshCore\s+(on|off)", text)
    if not match:
        raise SmokeFailure(f"could not parse network state:\n{text}")
    return match.group(1) == "on", match.group(2) == "on"


def run_smoke(args: argparse.Namespace) -> None:
    print(f"[tdm] opening {args.port} @ {args.baud}")
    with serial_harness.open_port_retry(
        args.port, args.baud, args.timeout, args.open_timeout, args.dtr, args.rts
    ) as port:
        if args.reset:
            serial_harness.pulse_reset(port, args.reset_settle)
        serial_harness.sync_prompt(port, args.boot_timeout)

        initial_net = run_cmd(port, "net", args.timeout)
        initial_mt, initial_mc = parse_net_state(initial_net)
        initial_airtime = parse_airtime(run_cmd(port, "airtime", args.timeout))
        restore_preset = (
            "balanced"
            if (initial_airtime.mt_pct, initial_airtime.mc_pct) == (50, 50)
            else "mc"
            if (initial_airtime.mt_pct, initial_airtime.mc_pct) == (40, 60)
            else "mt"
        )

        try:
            run_cmd(port, "net mt on", args.timeout)
            run_cmd(port, "net mc on", args.timeout)

            for preset in ("mt", "balanced", "mc"):
                airtime = parse_airtime(run_cmd(port, f"airtime {preset}", args.timeout))
                assert_airtime(airtime, preset)
                snapshot = parse_rf(run_cmd(port, "rf", args.timeout))
                assert_split(snapshot, preset)

            parse_airtime(run_cmd(port, "airtime balanced", args.timeout))
            first = parse_rf(run_cmd(port, "rf", args.timeout))
            time.sleep(args.settle)
            second = parse_rf(run_cmd(port, "rf", args.timeout))
            assert_split(second, "balanced")
            if second.switches <= first.switches:
                raise SmokeFailure(
                    f"expected TDM switches to increase after {args.settle:.1f}s; "
                    f"before={first.switches} after={second.switches}"
                )

            run_cmd(port, "net mc off", args.timeout)
            single = parse_rf(run_cmd(port, "rf", args.timeout))
            if single.mode != "Meshtastic 100%":
                raise SmokeFailure(f"expected Meshtastic 100% after net mc off, got '{single.mode}'")

            print("[tdm] split-airtime smoke PASS")
        finally:
            if not args.no_restore:
                restore_state(port, args.timeout, initial_mt, initial_mc, restore_preset)


def selftest() -> None:
    airtime = parse_airtime("airtime: Balanced  MT 50% / MC 50%\n")
    assert_airtime(airtime, "balanced")
    rf1 = parse_rf(
        "mode: SPLIT MT 50% / MC 50% (Balanced)\n"
        "dwell: Meshtastic 250ms / MeshCore 250ms\n"
        "active: Meshtastic  906.875 MHz  BW 250.0  SF11  CR4/5  (slot 111ms left)\n"
        "Meshtastic: 906.875 MHz BW250 SF11   rx 3\n"
        "MeshCore:   910.525 MHz BW62.5 SF7   rx 2\n"
        "switches: 41\n"
    )
    rf2 = parse_rf(
        "mode: SPLIT MT 50% / MC 50% (Balanced)\n"
        "dwell: Meshtastic 250ms / MeshCore 250ms\n"
        "active: MeshCore  910.525 MHz  BW 62.5  SF7  CR4/5  (slot 88ms left)\n"
        "Meshtastic: 906.875 MHz BW250 SF11   rx 3\n"
        "MeshCore:   910.525 MHz BW62.5 SF7   rx 2\n"
        "switches: 44\n"
    )
    assert_split(rf1, "balanced")
    assert_split(rf2, "balanced")
    if rf2.switches <= rf1.switches:
        raise SmokeFailure("selftest switch counter did not increase")
    single = parse_rf(
        "mode: Meshtastic 100%\n"
        "dwell: Meshtastic 250ms / MeshCore 250ms\n"
        "active: Meshtastic  906.875 MHz  BW 250.0  SF11  CR4/5  (slot 0ms left)\n"
        "Meshtastic: 906.875 MHz BW250 SF11   rx 3\n"
        "MeshCore:   910.525 MHz BW62.5 SF7   rx 2\n"
        "switches: 44\n"
    )
    if single.mode != "Meshtastic 100%":
        raise SmokeFailure("selftest failed to parse single-network mode")
    print("[tdm] parser selftest PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description="Smoke-test split-airtime TDM over the LimitlezzOS serial CLI.")
    parser.add_argument("--port", default=default_port())
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--open-timeout", type=float, default=60.0)
    parser.add_argument("--boot-timeout", type=float, default=45.0)
    parser.add_argument("--settle", type=float, default=1.4, help="Seconds to wait between rf samples.")
    parser.add_argument("--dtr", choices=["default", "on", "off"], default="off")
    parser.add_argument("--rts", choices=["default", "on", "off"], default="off")
    parser.add_argument("--reset", action="store_true", help="Pulse reset before waiting for the prompt.")
    parser.add_argument("--reset-settle", type=float, default=1.5)
    parser.add_argument("--no-restore", action="store_true", help="Leave the tested network/airtime state active.")
    parser.add_argument("--selftest", action="store_true", help="Run parser checks without opening serial.")
    args = parser.parse_args()

    try:
        if args.selftest:
            selftest()
        else:
            run_smoke(args)
        return 0
    except SmokeFailure as exc:
        print(f"[tdm] FAIL: {exc}", file=sys.stderr)
        return 2
    except serial_harness.RomDownloadMode as exc:
        print("[tdm] device is in ESP-ROM download mode, not LimitlezzOS", file=sys.stderr)
        print(str(exc).strip(), file=sys.stderr)
        return 3
    except TimeoutError as exc:
        partial = str(exc).strip()
        print("[tdm] timed out waiting for prompt/output", file=sys.stderr)
        if partial:
            print(partial, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
