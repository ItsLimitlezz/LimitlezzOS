# T-Deck Device Security

This is the first Phase 12 security increment: an optional device PIN verifier
and diagnostics path. It proves the setup/check/clear flow without claiming that
local data is encrypted yet.

Implemented:

- `security status` over the USB serial console.
- `security set <pin>` for a 4-12 digit PIN.
- `security check <pin>` to verify the saved PIN.
- `security clear <pin>` to remove the verifier after a correct PIN.
- `security test` for the bounded verifier selftest.
- Native simulator selftest coverage for set/check/reject/clear behavior.

Still TODO:

- lock-screen or Settings UI for setting and unlocking with the PIN
- data encryption using the PIN-derived secret
- migration of existing plaintext message history, identities, keys, and app data
- recovery wording for forgotten PINs
- secure boot or flash-encryption guidance for advanced builds

## Storage Contract

The firmware stores one verifier file at:

```text
/sd/limitlezz/security.cfg
```

The simulator uses the same filename under its data directory. The format is a
single line:

```text
1|pin-sha256|2048|<16 hex salt>|<64 hex verifier>
```

The PIN itself is never written to disk. The verifier is a salted, iterated
SHA-256 value with the current work factor from `LZ_SECURITY_KDF_ROUNDS`.

This is only an authentication gate for later encrypted storage work. Until the
encrypted store lands, a lost SD card can still reveal plaintext message logs,
identity files, MeshCore keys, Meshtastic PKI keys, and app data.

## Serial Diagnostics

Fresh hardware with no configured PIN:

```text
lz> security status
security: no device PIN set (not configured); encrypted-store=not-enabled
```

Set and verify a PIN:

```text
lz> security set 123456
[ok] device PIN verifier set (data encryption not enabled yet)

lz> security check 123456
[ok] PIN accepted
```

Built-in verifier proof:

```text
lz> security test
PIN verifier selftest: PASS min=4 max=12 rounds=2048
```
