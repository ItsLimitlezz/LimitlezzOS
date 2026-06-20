# T-Deck MeshCore Companion V0 Protocol

Status: Phase 5 / V0.8 protocol foundation plus an initial firmware serial
smoke surface. The current firmware exposes `companion mc hello`, `status`,
`nodes`, `threads`, `send`, `dm`, and `test` for USB console validation; the
formal `MC0` request/response bridge, live events, BLE transport, and external
app compatibility are still planned.

## Goal

MeshCore companion V0 should expose the MeshCore features that are already
owned by the T-Deck firmware without pretending to be an official MeshCore app
transport. The first target is a small USB serial protocol that lets a purpose
built test tool or LimitlezzOS companion app:

- identify the T-Deck MeshCore identity and current bridge capabilities
- read a snapshot of known MeshCore nodes
- send Public channel text
- send DMs to a known MeshCore name or address
- receive message, node, send-status, and snapshot-change events
- recover cleanly from validation, routing, and radio errors

BLE can reuse the same logical lines later, but USB is the V0 transport.

## Compatibility Boundary

This V0 protocol is not external-app compatible yet. Do not claim that existing
MeshCore phone or desktop apps can connect to LimitlezzOS through this bridge
unless a real MeshCore app protocol is confirmed and mapped.

For V0, supported clients are test tools and purpose-built LimitlezzOS
companion experiments. The T-Deck remains the source of truth for MeshCore
keys, node state, address matching, radio scheduling, and delivery state.

## Transport And Framing

The USB bridge uses line-oriented text after the user or firmware has entered
MeshCore companion mode. The activation UI/serial command is intentionally left
to the firmware implementation, but it must be distinct from Meshtastic
companion mode.

The current implementation step keeps this in the existing serial console as
`companion mc ...` commands so CI and COM8 hardware smokes can validate the
service boundary before the formal host protocol takes ownership of the port.

Formal USB MC0 mode is an explicit mode switch rather than an implicit reuse of
the normal console prompt. The firmware enters MC0 mode with:

```text
companion mc usb on
MC0 1 HELLO proto=0 app=limitlezz-smoke host=windows want=none
MC0 2 STATUS
MC0 3 NODES since=0 limit=5
MC0 99 EXIT
```

Host smoke keeps the mode-entry command and exit line configurable for older
artifacts and later protocol revisions, but it must not treat Meshtastic
companion mode as a fallback.

- Encoding: UTF-8 text lines.
- Line ending: `\n`; firmware should accept `\r\n`.
- Request prefix: `MC0`.
- Maximum inbound line length: advertised by `HELLO` as `max_line`.
- Request ids: host-generated ASCII tokens, 1-12 characters, echoed by the
  device.
- Field format: `key=value` pairs separated by one space.
- String values: percent-encoded UTF-8 bytes. Encode at least space, `%`, `=`,
  `\r`, and `\n`.
- Numbers: decimal unless a field explicitly says hex.
- Booleans: `0` or `1`.
- Addresses and keys: lowercase hex, treated as opaque by the host.

Single-line response:

```text
MC0 <id> OK key=value ...
MC0 <id> ERR code=<code> retry=<0|1> message=<percent-text>
```

Snapshot response:

```text
MC0 <id> BEGIN type=<snapshot-type> rev=<n> count=<n> more=<0|1> cursor=<token>
MC0 <id> <ROW> key=value ...
MC0 <id> END type=<snapshot-type> rev=<n> count=<n> more=<0|1> cursor=<token>
```

Device event:

```text
MC0 EVT <seq> <event-type> key=value ...
```

Events are asynchronous and may appear between request responses unless the
firmware documents a stricter ordering. A host that loses event sequence
continuity must call `STATUS` and refresh affected snapshots.

## Request Surface

### `HELLO`

The host must send `HELLO` before any other request.

Request:

```text
MC0 1 HELLO proto=0 app=limitlezz-test host=windows want=events
```

Response:

```text
MC0 1 OK proto=0 fw=0.8-draft device=tdeck session=84 caps=identity,nodes,status,send_public,send_dm,events max_line=512 max_text=180 event_seq=120 nodes_rev=42 messages_rev=77
```

Required fields:

- `proto`: selected protocol version. V0 is `0`.
- `caps`: comma-separated capability tokens.
- `max_line`: maximum inbound request line the firmware accepts.
- `max_text`: maximum decoded message body bytes accepted for V0 sends.
- `event_seq`: last event sequence number seen by the device.
- `nodes_rev`: current node snapshot revision.
- `messages_rev`: current message/delivery snapshot revision.

If the host requests an unsupported protocol version, the device returns
`ERR code=unsupported_version`.

### `IDENTITY`

Returns the local MeshCore identity as the firmware currently understands it.

Request:

```text
MC0 2 IDENTITY
```

Response:

```text
MC0 2 OK enabled=1 name=Jess addr=6b1d000000000000000000000000000000000000000000000000000000000000 short_id=MC-6b1d0000 role=chat pubkey=6b1d000000000000000000000000000000000000000000000000000000000000 addr_format=meshcore-pubkey-hex advert_ready=1
```

Required fields:

- `enabled`: whether MeshCore is currently enabled.
- `name`: percent-encoded local display name.
- `addr`: local MeshCore address, lowercase 64-hex MeshCore public key, opaque
  to the host except for exact equality.
- `short_id`: optional display-only short identifier. Hosts must not route by it.
- `role`: current local MeshCore role, such as `chat`, `router`, or
  `unknown`.
- `addr_format`: the address encoding advertised for this session.
- `advert_ready`: whether the firmware has enough identity state to advertise.

Private keys must never be exposed through this protocol.

### `STATUS`

Returns bridge, radio, queue, and snapshot status. This is the host's resync
anchor after reconnect, dropped events, or errors.

Request:

```text
MC0 3 STATUS
```

Response:

```text
MC0 3 OK mc=on bridge=usb mc_companion=idle mt_companion=off tdm=active airtime=balanced queue=0 event_seq=120 nodes_rev=42 messages_rev=77
```

Suggested fields:

- `mc`: `on`, `off`, or `disabled`.
- `bridge`: active transport, usually `usb` for V0.
- `mc_companion`: `idle`, `attached`, or `streaming`.
- `mt_companion`: Meshtastic companion state so clients can detect conflicts.
- `tdm`: `active`, `mc_only`, `mt_only`, or `idle`.
- `airtime`: current split-airtime preset.
- `queue`: queued outbound MeshCore sends owned by the firmware.
- `event_seq`, `nodes_rev`, `messages_rev`: resync counters.

### `NODES`

Returns a bounded snapshot of the firmware's known MeshCore nodes.

Request:

```text
MC0 4 NODES since=0 limit=50
```

Response:

```text
MC0 4 BEGIN type=nodes rev=42 count=2 more=0 cursor=end
MC0 4 NODE addr=4f8e21a000000000000000000000000000000000000000000000000000000000 short_id=MC-4f8e21a0 name=Limitlezz role=chat seen_ms=12000 snr=-9 rssi=-112 public_key=present dm=ready
MC0 4 NODE addr=- short_id=MC-12ab9001 name=Hilltop role=router seen_ms=180000 snr=-14 rssi=-118 public_key=missing dm=not_messageable
MC0 4 END type=nodes rev=42 count=2 more=0 cursor=end
```

Request fields:

- `since`: last `nodes_rev` known by the host, or `0` for a full snapshot.
- `limit`: maximum rows requested. Firmware may cap this below the requested
  value; V0 USB mode caps node snapshots at five rows to keep line responses
  bounded.
- `cursor`: optional opaque cursor from a previous `NODES` response.

Node fields:

- `addr`: MeshCore address, lowercase 64-hex public key when known. `-` means
  the firmware has no usable key yet, so `SEND_DM to_addr=...` is impossible.
- `short_id`: display-only short identifier for compact UI labels.
- `name`: percent-encoded display name, if known.
- `role`: `chat`, `router`, `repeater`, `sensor`, or `unknown`.
- `seen_ms`: milliseconds since last heard, or `-1` if unknown.
- `snr`, `rssi`: last RF quality values, or omitted when unknown.
- `public_key`: `present`, `missing`, or `unknown`.
- `dm`: `ready`, `no_key`, `not_messageable`, or `unknown`.

V0 sends DMs only to nodes already known by `addr` or an unambiguous `name`.
It does not include contact import, key exchange control, or remote node
mutation.

### `SEND_PUBLIC`

Queues text for the default MeshCore Public channel or a known room token.

Request:

```text
MC0 5 SEND_PUBLIC room=public text=Hello%20mesh client_mid=pc-0001
```

Immediate response:

```text
MC0 5 OK accepted=1 msg_id=mc-804 queue=1 status=queued
```

Later events:

```text
MC0 EVT 121 tx_status client_mid=pc-0001 msg_id=mc-804 kind=public status=sent
MC0 EVT 122 tx_status client_mid=pc-0001 msg_id=mc-804 kind=public status=delivered
```

Request fields:

- `room`: `public` for V0, or a firmware-known room token later.
- `text`: percent-encoded UTF-8 body.
- `client_mid`: optional host-generated id for de-duplicating retries.

If the command is accepted, later delivery failure is reported as
`tx_status status=failed`; it is not a synchronous `ERR`.

### `SEND_DM`

Queues a MeshCore private message to a known address or an unambiguous known
name. Address is preferred because display names can collide.

By address:

```text
MC0 6 SEND_DM to_addr=4f8e21a000000000000000000000000000000000000000000000000000000000 text=Meet%20at%20camp client_mid=pc-0002
```

By known name:

```text
MC0 7 SEND_DM to_name=Limitlezz text=Copy%20that client_mid=pc-0003
```

Immediate response:

```text
MC0 6 OK accepted=1 msg_id=mc-805 to_addr=4f8e21a000000000000000000000000000000000000000000000000000000000 status=queued
```

Later event:

```text
MC0 EVT 123 tx_status client_mid=pc-0002 msg_id=mc-805 kind=dm to_addr=4f8e21a000000000000000000000000000000000000000000000000000000000 status=delivered
```

V0 name matching rules:

- Match against the firmware's known MeshCore display name and short name.
- Case-insensitive exact match only.
- If zero nodes match, return `ERR code=not_found`.
- If more than one node matches, return `ERR code=ambiguous_name`.
- If the matching node lacks a usable session/key, return `ERR code=no_key`.
- If the node role is not messageable, return `ERR code=not_messageable`.
- If both `to_addr` and `to_name` are supplied, they must identify the same
  node or firmware returns `ERR code=target_mismatch`.

The host does not manage MeshCore private keys or sessions in V0.

### `EVENTS`

Controls live event streaming. A V0 implementation may start events after
`HELLO want=events`, but it should still accept this explicit command.

Request:

```text
MC0 8 EVENTS mode=on types=nodes,messages,tx,status
```

Response:

```text
MC0 8 OK events=on types=nodes,messages,tx,status event_seq=123
```

Supported event types:

```text
MC0 EVT 124 node_upsert addr=4f8e21a000000000000000000000000000000000000000000000000000000000 nodes_rev=43
MC0 EVT 125 snapshot_dirty type=nodes rev=43 reason=node_upsert
MC0 EVT 126 rx_public msg_id=mc-806 from_addr=4f8e21a000000000000000000000000000000000000000000000000000000000 from_name=Limitlezz room=public text=Copy%20CH0.
MC0 EVT 127 rx_dm msg_id=mc-807 from_addr=4f8e21a000000000000000000000000000000000000000000000000000000000 from_name=Limitlezz text=Direct%20copy.
MC0 EVT 128 tx_status client_mid=pc-0002 msg_id=mc-805 kind=dm status=failed reason=ack_timeout retry=1
MC0 EVT 129 status mc=on tdm=active airtime=balanced queue=0
```

Snapshot events are hints. The host should use `STATUS`, `NODES`, or later
message snapshot commands for authoritative state after reconnect.

## Error Semantics

Synchronous `ERR` means the request was not accepted. Accepted sends fail later
through `tx_status`.

Common error codes:

| Code | Meaning | Retry |
| --- | --- | --- |
| `bad_request` | Missing field, malformed id, bad encoding, or invalid value | No |
| `unsupported_version` | Host requested an unsupported `proto` | No |
| `unknown_command` | Verb is not implemented | No |
| `not_ready` | MeshCore state is still starting or identity is unavailable | Yes |
| `meshcore_disabled` | MeshCore is disabled by settings or build gate | No |
| `busy` | Radio/bridge cannot accept more work right now | Yes |
| `not_found` | Address/name is unknown | No |
| `ambiguous_name` | Name matched more than one known node | No |
| `not_messageable` | Node role/session cannot receive DMs | No |
| `no_key` | No usable private-chat key/session for that node | No |
| `text_too_long` | Decoded text exceeds `max_text` or current packet budget | No |
| `rate_limited` | Host is sending too fast | Yes |
| `send_failed` | Firmware rejected the send before queueing | Maybe |
| `internal` | Unexpected firmware failure | Maybe |

Error example:

```text
MC0 10 ERR code=ambiguous_name retry=0 message=Multiple%20nodes%20named%20Limitlezz
```

Rules:

- Every `ERR` includes `code`, `retry`, and `message`.
- `message` is for humans and must not be parsed for behavior.
- `retry=1` means the same request may succeed later; it is not a guarantee.
- If a request is accepted with `OK`, later delivery failures must use
  `tx_status status=failed`.
- Unknown fields are ignored unless they change the meaning of a command.
- Unknown required behavior should be negotiated with `caps`, not guessed.

## V0 Roadmap

1. Document the V0 line protocol and keep it separate from Meshtastic
   companion protocol claims.
2. Add an initial USB serial-console smoke surface for `hello`, `status`,
   `nodes`, `threads`, Public send, DM send, and self-test.
3. Add a USB-only MeshCore companion mode with `HELLO`, `IDENTITY`, `STATUS`,
   and `NODES`.
4. Add `SEND_PUBLIC` and `SEND_DM` using the existing firmware-owned MeshCore
   send paths.
5. Add event streaming for receive, send-status, node-change, and status
   changes.
6. Add snapshot revision counters and reconnect/resync behavior.
7. Mirror the same logical protocol over BLE only after USB behavior is stable.
8. Revisit external-app compatibility only after the real MeshCore app protocol
   is confirmed.

## Validation Checklist

- Default smoke remains the serial-console boundary check:
  `python scripts/mc_companion_usb_smoke.py` runs `companion mc status` and
  `companion mc test`.
- Formal USB MC0 smoke is opt-in:
  `python scripts/mc_companion_usb_smoke.py --mc0-usb` enters the configured
  USB mode, sends `HELLO`, `IDENTITY`, `STATUS`, and `NODES`, asserts
  `MC0 ... OK`, the public-key address format, `BEGIN`, and `END` response
  markers, then exits through the configured `MC0 <id> EXIT` line.
- If firmware lands different command names, use the smoke helper's
  `--mc0-enter-command`, `--mc0-*-template`, `--mc0-*-marker`, and
  `--mc0-exit-template` flags instead of editing firmware or weakening the
  assertions.
- USB serial host can enter MeshCore companion mode without enabling
  Meshtastic companion mode.
- `HELLO`, `IDENTITY`, `STATUS`, and `NODES` work after boot and after
  reconnect.
- Public send queues, emits `tx_status`, and appears in the on-device inbox.
- DM send works by address and by an unambiguous known name.
- DM name collision returns `ambiguous_name` without sending.
- Missing key/session returns `no_key` without sending.
- Dropped or skipped event sequence is recoverable through `STATUS` and
  snapshots.
- On-device MeshCore messaging still works while no companion client is
  attached.
- Meshtastic USB/BLE companion behavior does not regress.
- README/roadmap do not call this official MeshCore app compatibility until
  the real MeshCore app protocol is confirmed.
