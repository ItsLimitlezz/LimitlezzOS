#!/usr/bin/env python3
"""Smoke-test the LimitlezzOS MC0 BLE companion service.

Requires bleak on the host running the smoke:
    python -m pip install bleak
"""
from __future__ import annotations

import argparse
import asyncio
import sys
import time


DEFAULT_SERVICE_UUID = "b8f13f20-6a6f-4f8d-8b0d-4d4330000001"
DEFAULT_RX_UUID = "b8f13f20-6a6f-4f8d-8b0d-4d4330000002"
DEFAULT_TX_UUID = "b8f13f20-6a6f-4f8d-8b0d-4d4330000003"
DEFAULT_NAME_PREFIX = "Limitlezz-MC0"


class BleSmokeFailure(RuntimeError):
    pass


def split_markers(value: str) -> list[str]:
    return [part.strip() for part in value.split("|") if part.strip()]


def default_specs(args: argparse.Namespace) -> list[tuple[str, str, list[str]]]:
    specs = [
        (
            "hello",
            "MC0 1 HELLO proto=0 app=limitlezz-ble-smoke host=windows want=none",
            ["MC0 1 OK", "event_seq=", "nodes_rev=", "messages_rev="],
        ),
        (
            "identity",
            "MC0 2 IDENTITY",
            ["MC0 2 OK", "addr_format=meshcore-pubkey-hex", "pubkey="],
        ),
        (
            "status",
            "MC0 3 STATUS",
            ["MC0 3 OK", "bridge=ble", "event_seq=", "nodes_rev=", "messages_rev="],
        ),
        ("nodes", "MC0 4 NODES since=0 limit=5", ["MC0 4 BEGIN", "MC0 4 END"]),
        ("threads", "MC0 5 THREADS since=0 limit=5", ["MC0 5 BEGIN", "MC0 5 END"]),
        (
            "events on",
            "MC0 6 EVENTS mode=on types=nodes,messages,tx,status",
            ["MC0 6 OK", "events=on", "event_seq=", "nodes_rev=", "messages_rev="],
        ),
    ]
    if args.mc0_tx_smoke:
        specs.append(
            (
                "send public tx event",
                (
                    "MC0 8 SEND_PUBLIC room=public text="
                    f"{args.mc0_send_public_text} client_mid={args.mc0_send_public_client_mid}"
                ),
                [
                    "MC0 8 OK",
                    "accepted=1",
                    f"client_mid={args.mc0_send_public_client_mid}",
                    "MC0 EVT",
                    "tx_status",
                    "kind=public",
                ],
            )
        )
    specs.append(
        (
            "events off",
            "MC0 7 EVENTS mode=off",
            ["MC0 7 OK", "events=off", "event_seq=", "nodes_rev=", "messages_rev="],
        )
    )
    return specs


async def find_device(args: argparse.Namespace, bleak) -> object:
    if args.address:
        return args.address
    print(f"[mc-ble] scanning for {args.name_prefix!r} / {args.service_uuid}")
    devices = await bleak.BleakScanner.discover(
        timeout=args.scan_timeout,
        service_uuids=[args.service_uuid],
    )
    matches = []
    for device in devices:
        name = getattr(device, "name", None) or ""
        details = getattr(device, "details", None)
        address = getattr(device, "address", "")
        if name.startswith(args.name_prefix):
            matches.append(device)
        elif args.service_uuid.lower() in str(details).lower():
            matches.append(device)
        elif args.verbose:
            print(f"[mc-ble] saw {address} {name}")
    if not matches:
        raise BleSmokeFailure(
            f"no MC0 BLE companion advertising as {args.name_prefix!r}; "
            "enable it with `companion mc ble on` on COM8"
        )
    if len(matches) > 1:
        print("[mc-ble] multiple matches, using the first:")
        for device in matches:
            print(f"  {getattr(device, 'address', '')} {getattr(device, 'name', '')}")
    return matches[0]


async def read_until_markers(client, tx_uuid: str, queue: asyncio.Queue[str],
                             markers: list[str], timeout: float) -> str:
    end = time.monotonic() + timeout
    output = ""
    while time.monotonic() < end:
        try:
            item = queue.get_nowait()
            if item:
                output += item
        except asyncio.QueueEmpty:
            pass
        try:
            data = await client.read_gatt_char(tx_uuid)
            if data:
                output += bytes(data).decode("utf-8", errors="replace")
        except Exception:
            pass
        if all(marker in output for marker in markers):
            return output
        await asyncio.sleep(0.15)
    missing = ", ".join(repr(marker) for marker in markers if marker not in output)
    detail = f"[mc-ble] timed out waiting for markers: {missing}"
    if output.strip():
        detail = "\n".join([detail, "[mc-ble] partial output:", output.rstrip()])
    raise BleSmokeFailure(detail)


async def run(args: argparse.Namespace) -> int:
    try:
        import bleak
    except ImportError as exc:
        raise BleSmokeFailure("bleak is required: python -m pip install bleak") from exc

    device = await find_device(args, bleak)
    queue: asyncio.Queue[str] = asyncio.Queue()

    def on_tx(_sender, data: bytearray) -> None:
        queue.put_nowait(bytes(data).decode("utf-8", errors="replace"))

    async with bleak.BleakClient(device, timeout=args.connect_timeout) as client:
        if not client.is_connected:
            raise BleSmokeFailure("BLE connect failed")
        print("[mc-ble] connected")
        await client.start_notify(args.tx_uuid, on_tx)
        for label, command, markers in default_specs(args):
            marker_text = ", ".join(repr(marker) for marker in markers)
            print(f"[mc-ble] > {command}  [{label}; expect: {marker_text}]")
            await client.write_gatt_char(args.rx_uuid, (command + "\n").encode("utf-8"))
            output = await read_until_markers(client, args.tx_uuid, queue, markers, args.timeout)
            if output.strip():
                print(output.rstrip())
        await client.stop_notify(args.tx_uuid)
    print("[mc-ble] smoke PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Smoke-test MeshCore MC0 BLE companion mode.")
    parser.add_argument("--address", help="BLE address to connect directly; skips scan.")
    parser.add_argument("--name-prefix", default=DEFAULT_NAME_PREFIX)
    parser.add_argument("--service-uuid", default=DEFAULT_SERVICE_UUID)
    parser.add_argument("--rx-uuid", default=DEFAULT_RX_UUID)
    parser.add_argument("--tx-uuid", default=DEFAULT_TX_UUID)
    parser.add_argument("--scan-timeout", type=float, default=12.0)
    parser.add_argument("--connect-timeout", type=float, default=20.0)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--mc0-tx-smoke", action="store_true")
    parser.add_argument("--mc0-send-public-text", default="mc0%20ble%20smoke")
    parser.add_argument("--mc0-send-public-client-mid", default="mc0-ble-smoke-tx")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except BleSmokeFailure as exc:
        print(str(exc), file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
