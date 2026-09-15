# Dockge handoff files

These files are committed with the relay source and sent to one relay operator
alongside the Linux `dusklight_online_relay` executable from the matching
release. They do not use GitHub Container Registry or another relay registry.

Do not run Docker or the relay on the maintainer's development PC. To prepare a
handoff, copy these tracked files to a folder and add the matching Linux relay
executable with the exact filename `dusklight_online_relay`. Send that folder or
archive to the operator. The operator puts it in Dockge's stacks directory and
builds it on the Docker server.

Commit:

- `.gitignore`
- `.env.example`
- `Dockerfile`
- `compose.yaml`
- `run-with-gluetun.sh`
- `README.md`
- `SETUP.md`

Do not commit:

- `dusklight_online_relay`, which is the release binary sent to the operator
- `.env` or any real WireGuard key/configuration
- generated ZIP, TAR or `dist/` contents

Normal relay upgrades do not change the Compose YAML. Send the new Linux
executable; the operator replaces the old file, updates `RELAY_VERSION`, and
uses Dockge's Build/Recreate action. See `SETUP.md` for the operator procedure.

