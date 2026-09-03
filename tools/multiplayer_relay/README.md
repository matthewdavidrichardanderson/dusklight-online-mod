# Dusklight Online Relay

The relay is maintained and released with Dusklight Online. It groups clients
into password-protected lobbies and routes reliable gameplay messages over TCP
plus latency-sensitive visual traffic over authenticated UDP.

The server does not require Dusklight, the game, the mod SDK, or a game
installation at runtime. It does not persist saves or gameplay state.

## Download and run on Windows

Extract the relay release ZIP and keep `dusklight_online_relay.exe` beside the
optional `dusklight_online_relay_launcher.exe`. Open the launcher, enter the
public IP address or hostname and forwarded port, then share the displayed relay
code with players. The launcher remembers those values for the next run.

The command-line equivalent is:

```powershell
.\dusklight_online_relay.exe `
    --host 0.0.0.0 `
    --port 34197 `
    --public-host relay.example.com `
    --public-port 34197 `
    --verbose
```

Open the selected port for both TCP and UDP. `--host` is the local bind address;
`--public-host` is the endpoint encoded into the relay code and does not need to
be a local interface.

The launcher writes verbose output to
`%APPDATA%\TwilitRealm\Dusklight\relay\relay.log`. Its **Open Log Folder** button
opens that location.

## Using the relay in game

The relay operator gives every player the same relay code. In Dusklight Online,
use **Online → Relay** and enter that code. A host creates a named lobby and the
other players join it with the same lobby name and password.

The lobby creator owns its settings. If that player leaves, the oldest remaining
client becomes the owner. Nicknames are display labels and do not need to be
unique; routing uses opaque client identifiers.

## Relay-only build

The relay has its own build entry point inside the Online repository, so it can
be built without compiling the mod or SDK.

Requirements:

- CMake 3.24 or newer
- A C++20 compiler
- Ninja when using the supplied preset
- Python 3 for integration tests

From `tools/multiplayer_relay`:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cpack --config ../../build/relay-release/CPackConfig.cmake
```

The distributable archive is written to `build/relay-release/package`. CMake
fetches the pinned `nlohmann/json` 3.12.0 dependency when it is not installed.
Windows release binaries use the static MSVC runtime and do not require a
separate Visual C++ redistributable installation.

The relay also remains a normal target of the main Online build:

```powershell
cmake --build .\build --config RelWithDebInfo --target dusklight_online_relay
```

## Verification

The relay-only test suite checks invite codes, three-client lobby behavior,
framing, validation, authenticated TCP/UDP routing, owner transfer, settings,
reliable acknowledgements, supported gameplay messages, and version reporting.

Per-packet tracing is disabled by default because pose traffic is extremely
verbose. Set `DUSK_MP_RELAY_PACKET_TRACE=1` when a packet-size trace is needed.

## Compatibility and upgrades

Clients and relays currently use protocol version 2. Relay capabilities are
negotiated independently so unsupported visual features can fall back safely.
Replace and restart the relay whenever an Online release adds a packet type or
server capability. Existing lobbies end when the process exits because they are
kept only in memory.

See [the protocol reference](PROTOCOL.md), [deployment guide](DEPLOYMENT.md),
and [protocol maintenance checklist](SYNCING.md).

## Security

Lobby passwords are sent over plain TCP. Use throwaway passwords and deploy on a
trusted private network or behind an encrypted tunnel. Do not reuse account
passwords.

Project code is released under the repository's CC0 license. The packaged relay
also includes [third-party notices](THIRD_PARTY_NOTICES.md) for its MIT-licensed
JSON dependency.
