# Dusklight Online

Online multiplayer for *The Legend of Zelda: Twilight Princess* running on
[Dusklight](https://github.com/TwilitRealm/dusklight).

Play together through a direct connection or relay server. The mod includes
remote players, shared progression and world state, player collision, optional
PvP, name tags, multiplayer audio, and manual save synchronization.

## Installation

1. Download `dusklight_online.dusk` from the latest release.
2. Place it in Dusklight's `mods` folder.
3. Start Dusklight and enable **Dusklight Online** from the Mods menu.
4. Open the Online menu to host or join a game.

Players should use the same mod version. Back up important save files before
using progression synchronization while the mod is in alpha.

## Connections

- **Direct:** one player hosts and shares the generated invite code.
- **Relay:** connect through a standalone relay when a direct connection is
  unavailable. Relay setup is documented in
  [`tools/multiplayer_relay/README.md`](tools/multiplayer_relay/README.md).

## Building

The project uses CMake and downloads its pinned dependencies automatically.

```sh
cmake -S . -B build
cmake --build build --config RelWithDebInfo --target dusklight_online_package
```

The package is written to `build/mods/dusklight_online.dusk` (or the
generator-specific equivalent).

## Credits

Based on [TwilitRealm/Dusklight](https://github.com/TwilitRealm/dusklight) and
its original multiplayer work. Dusklight Online is maintained by mdra5000.

This project does not include the game or its copyrighted assets. You must
provide your own copy of *Twilight Princess*.

## License

Project code is released under [CC0 1.0 Universal](LICENSE.md). The bundled
Alegreya SC and Inter fonts remain under the
[SIL Open Font License 1.1](res/FONT_LICENSES.txt). Notices for compiled
third-party libraries are in
[`res/THIRD_PARTY_LICENSES.txt`](res/THIRD_PARTY_LICENSES.txt).
