# Encrypted Cloudflare room channel

The mod and Worker must be deployed together. Updated clients require the
encrypted `/room/v2/` endpoint; neither side falls back to plaintext room
messages. The room membership protocol inside the channel remains version 5.
The official Dusklight WebSocket SDK is used without host changes.

## Routing and key exchange

The URL is `wss://<service>/room/v2/<route>`. The route is the lowercase hex
HMAC-SHA256 of the ASCII-lowercased lobby name, with the public domain separator
`dusklight-cloud-room-route-v1` as the HMAC key. This is a deterministic lookup
digest, not a secret or a password hash. It preserves case-insensitive discovery
and keeps the literal lobby name out of URLs. The actual name is sent inside
the encrypted hello and checked against the route by the Worker.

Each socket gets a fresh ephemeral X25519 key pair on both ends. The Worker
sends `{"type":"secure_hello","version":1,"public_key":"<64 hex>"}`;
the client replies with `secure_ready` in the same format. WSS authenticates
this exchange. Lobby codes are never used as encryption keys. All-zero shared
secrets and malformed keys are rejected.

Traffic keys use HKDF-SHA256. The extract salt is the concatenation of UTF-8
`dusklight-cloud-channel-v1:`, the 64 ASCII route characters, the raw 32-byte
client public key, and the raw 32-byte server public key. The input key material
is the X25519 shared secret. Each 32-byte key is the first HKDF expand block:
HMAC-SHA256(extracted key, `client-to-server` or `server-to-client` followed by
byte `01`). Ephemeral private keys are discarded after exchange.

The native implementation uses the bundled Monocypher and existing HMAC
implementation. The Worker uses Web Crypto X25519 and native `node:crypto`
HMAC/ChaCha20-Poly1305 with `nodejs_compat`. No extra crypto package or SDK
export is required.

## Encrypted records

All subsequent messages, in both directions, are text frames containing:

```text
DCR1:<lowercase hex(sequence || ciphertext || authentication tag)>
```

The sequence is eight bytes, unsigned big endian, starting at zero in each
direction. Each record uses RFC 8439 ChaCha20-Poly1305 with a 12-byte nonce:
four zero bytes followed by the sequence. Associated data is ASCII `DCR1:`
followed by the raw eight sequence bytes. The authentication tag is 16 bytes.
Records must arrive in sequence; malformed, modified, repeated, reordered,
or plaintext records close the connection. There is no renegotiation or
plaintext fallback. Sequence values must be less than `2^53 - 1`.

Plaintext remains capped at 16,384 UTF-8 bytes. Hex framing increases the
maximum WebSocket frame to 32,821 bytes; both the client SDK options and Worker
enforce the corresponding limits. The first encrypted server message is the
existing authentication challenge. Lobby names, code verifiers/proofs, peer
keys, membership notifications, settings, and ICE signaling are encrypted.
Gameplay continues to use the existing encrypted peer mesh.

The Worker stores traffic keys and counters in WebSocket attachments so
hibernation preserves the channel. Nonces are persisted before sending.
Message processing, room changes, and broadcasts are serialized together to
avoid reusing counters across awaited operations. Reconnects always establish
fresh keys and reject ciphertext from the prior connection.

## Tests

Build Online with `DUSKLIGHT_ONLINE_BUILD_TESTS=ON`. The CTest suites
`dusklight_online_cloud_cipher` and `dusklight_online_cloud_room_transport`
cover encryption, limits, replay/tamper rejection, reconnects, and the native
Cloud Room mesh. Use the existing direct/relay tests for regression coverage.

From this directory, with Node 22 or newer:

```sh
npm test
```

Set `CLOUD_CIPHER_TEST_EXE` to the built `dusklight_online_cloud_cipher_test`
executable (including `.exe` on Windows) to run the JavaScript crypto suite
against the actual C++ implementation in both client and server roles.

For an actual local Worker/Durable Object runtime, start:

```sh
npx wrangler dev --local --ip 127.0.0.1 --port 8787
```

Then run `npm run test:local`. `ROOMS_TEST_URL` overrides the default
`http://127.0.0.1:8787`. With `CLOUD_CIPHER_TEST_EXE` set, this integration test
uses native C++ encryption against the Worker, including credential wire
inspection, wrong codes, case-insensitive joins, large ICE frames, settings,
reconnects, tampering, replay, and plaintext rejection. It does not launch the
game or deploy to Cloudflare.
