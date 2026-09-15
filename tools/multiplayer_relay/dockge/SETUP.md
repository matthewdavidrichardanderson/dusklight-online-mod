# Dusklight Online relay for Dockge

This folder contains a Linux relay executable and a reusable Dockge Compose
stack. Dockge builds the small runtime image on the operator's server. The relay
runs only through Gluetun's Proton VPN network namespace.

## Requirements

- A Linux amd64 Docker server with Dockge
- Docker Compose 2.23.1 or newer
- `/dev/net/tun` enabled on the server
- A paid Proton VPN plan with port forwarding

## First installation

1. Copy this entire folder to
   `/opt/stacks/dusklight-online-relay` on the Docker server. Confirm that the
   included Linux executable is named exactly `dusklight_online_relay`.
2. In Proton VPN, open **Downloads -> WireGuard configuration**. Generate a
   configuration for a P2P server with **NAT-PMP (Port Forwarding)** enabled.
   Do not enable Moderate NAT at the same time.
3. Open the downloaded configuration and copy the value after `PrivateKey =`.
   This is not the normal Proton account password. Keep it private.
4. In Dockge, choose **Scan Stacks Folder**, then open
   `dusklight-online-relay`. The included `compose.yaml` should appear.
5. In Dockge's environment editor, enter the values from `.env.example`, using
   the real private key and a `RELAY_VERSION` matching the executable.
6. Select **Build/Recreate** (or deploy with the build option) and start the
   stack. The build checks that the executable really matches `RELAY_VERSION`.
7. In the `vpn` log, wait for `port forwarded is ...`.
8. In the `relay` log, copy the newest line beginning `Relay code: TP1-` and
   give that code to all players.

No router port forward and no Docker `ports:` mapping are required. Proton
assigns a random UDP port. If the VPN reconnects with a different IP or port,
the relay restarts automatically and prints a new code. Existing lobbies end
and players must use the new code.

## Updating the relay

1. Stop the relay after its current lobby has finished.
2. Replace `/opt/stacks/dusklight-online-relay/dusklight_online_relay` with the
   new Linux relay executable you received.
3. Change `RELAY_VERSION` in Dockge to the matching tag, such as `v2.6.0`.
4. Select **Build/Recreate**, start the stack, and check the `relay` log for its
   new code. A version mismatch deliberately fails the image build.

The Compose YAML normally stays unchanged. Replace the setup files only when the
release notes explicitly say the Dockge template changed. For rollback, restore
the previous executable, restore its `RELAY_VERSION`, and rebuild.

## Troubleshooting

- If `vpn` never reports a forwarded port, verify the Proton plan, private key,
  NAT-PMP option, P2P server selection and `/dev/net/tun` availability.
- If `relay` stays at `Waiting for Gluetun`, resolve the `vpn` log error first.
- Use `RELAY_VERBOSE=on` only while diagnosing relay behavior.
- Never send or commit the WireGuard private key.

