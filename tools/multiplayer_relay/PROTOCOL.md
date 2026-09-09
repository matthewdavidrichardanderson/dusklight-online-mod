# Multiplayer Relay Protocol

This document defines the contract between the game client and standalone
relay.

## Framing and limits

- Reliable transport: KCP over UDP; no transport encryption.
- Encoding: one UTF-8 JSON object per line.
- Current wire version: `2`.
- Maximum encoded input line: 512 KiB, excluding the newline.
- Maximum queued output per client: 8 MiB.
- Maximum clients in one room: 8.
- Maximum room name: 64 bytes.
- Maximum nickname: 32 bytes.
- Minimum password: 6 bytes.
- Maximum password: 128 bytes.
- A client must complete `hello` within 10 seconds.

The UDP transport does not encrypt the room password. Internet deployment requires a
trusted private network or a later encrypted transport phase.

## Transport framing and ownership

The native connection adapter owns the UDP socket and maps validated endpoints
onto logical peer IDs. KCP owns no socket, address resolution, traversal or
connection establishment. Its output callback and input method are the boundary
for another carrier, such as a future libjuice adapter; libjuice is not included
or enabled by this change.

The native adapter services protocol work in the background, with serialized
access to its state. Loading or paused game updates therefore do not stop ACKs
or keepalives. Completed messages are queued; game-state handling stays on the
existing game-update hook. The worker is joined before socket/mod teardown.

- `DUC1`: fixed 21-byte native session control (type byte, little-endian 64-bit
  client nonce and server session generation). Open/challenge/confirm/ready
  establish return routability; keepalive and close maintain the session.
  This handshake is not cryptographic authentication. Existing lobby/password
  checks still run in `hello` after establishment.
- `DUR1`: reliable datagram, followed by a little-endian 64-bit generation,
  16-bit ordered group and KCP bytes. Maximum datagram size is 1200 bytes.
  Stale generations and unknown peers/groups are rejected before KCP input.
- `DMPU`: existing visual framing. It is dispatched separately, never fed into
  KCP. The existing token validator controls visual endpoint rebinding.

Reliable application messages are length-prefixed inside the KCP byte stream
(32-bit little-endian size), with a 2 MiB frame limit. Only complete frames are
exposed to the JSON layer. The existing JSON line and room limits still apply.
All current reliable messages use one ordered group so a save replacement does
not overtake or get overtaken by related progression messages. The reliability
API supports independent groups, but gameplay is not split across them yet.

Both payload paths pass through one scheduler: 512 KiB/s per peer and 4 MiB/s
aggregate, charged with 64 bytes of encapsulation allowance per datagram.
Realtime and reliable queues alternate; each is bounded to 256 KiB and expires
packets after 100 ms. Dropped reliable datagrams remain unacknowledged in KCP
and are retransmitted. Recovery uses the bounded 32-segment window on links
with stable measured RTT. When smoothed RTT exceeds the session baseline by
more than max(50 ms, 25%), KCP congestion-window throttling and exponential
retry backoff resume. Stable links use 1.5x retry backoff. Before RTT is known,
a four-segment startup floor avoids serializing small events on overseas links.
The shared rate/burst budgets and receiver flow control apply in either mode.
Native handshake/keepalive control is fixed-size and independently rate-limited.
Reliable send queues are bounded; 30 seconds without ACK progress reports failure
rather than silently skipping events. Native sessions time out after 15 seconds
without valid receive activity; handshakes time out after 10 seconds.

The relay and clients must be upgraded together. No old TCP fallback exists.

## Relay code

The standalone relay prints a relay code at startup. This is a client-side
bootstrap code containing the operator's advertised public host and port; it is
not a room identifier and contains no lobby password. The operator gives the
same code to lobby creators and joiners. Room name, password, nickname, and
host/join intent are supplied separately in `hello`.

## UDP visual channel

Reliable and visual traffic share the configured UDP port. `welcome` includes
a connection-specific `udp_token`. The client presents that token in a UDP
registration packet; the relay then binds the authenticated `client_id` to the
observed source IP and port. Visual, remote-object, and acknowledgement
datagrams are accepted only from that registered endpoint.

The relay routes pose and Midna chunks only to room members who requested those
visuals and skips known cross-stage recipients. Pose acknowledgements are
targeted back to the original sender. Unregistered or sender-spoofed datagrams
are discarded, and authenticated UDP input is rate limited.

## Connection sequence

Clients explicitly create or join rooms. A successful connection is:

1. Client sends `hello` with `action` set to `create` or `join`.
2. Relay sends `welcome` containing the assigned `client_id`, room owner,
   current room settings, and existing peers.
3. Relay sends `peer_joined` to the existing peers.
4. Gameplay messages can be routed.
5. Relay sends `peer_left` when a joined client disconnects.

`hello` fields:

| Field | Type | Required |
| --- | --- | --- |
| `type` | string (`hello`) | yes |
| `protocol_version` | integer (`2`) | yes |
| `action` | string (`create` or `join`) | yes |
| `room_id` | string | yes |
| `password` | string (at least 6 bytes) | yes |
| `name` | string | yes |
| `settings` | object | required for `create` |
| `session_id` | string | no; currently client metadata |
| `want_puppet` | boolean | no; reserved for preference routing |
| `want_midna` | boolean | no; reserved for preference routing |
| `capabilities.semantic_visual_v1` | boolean | no |
| `capabilities.semantic_snapshot_delta_v1` | boolean | no |

Calling `hello` again after joining returns `already_joined`; it cannot move a
socket between rooms.

`welcome` includes `semantic_visuals_ready` and `snapshot_deltas_ready`.
Each readiness field is true only when every room member advertises the
corresponding capability. Missing snapshot-delta support keeps clients on
complete semantic snapshots.

Nicknames are not connection identities and may be duplicated. Every joined
socket receives a unique opaque `client_id`, which is used for routing.

Creating an existing room returns `lobby_exists`. Joining a missing room
returns `lobby_not_found`; joining with a different password returns
`bad_password`.

The creator is the logical room owner. If the owner disconnects, ownership
passes to the oldest remaining connection and the relay broadcasts
`owner_changed`.

## Relay-handled messages

| Message | Behavior |
| --- | --- |
| `hello` | Validate and join/create a room. |
| `ping` | Reply to the sender with `pong`. |
| `pose` | Broadcast a sanitized pose envelope to other room members. |
| `reliable` | Deduplicate a bounded sequence window, broadcast, then `ack`. |
| `sync_request` | Route only to `target_client_id`. |
| `room_settings` | Owner-only update; validate and broadcast normalized settings. |

For every routed client message, the relay overwrites `client_id` with the
authenticated connection ID. Clients cannot impersonate another room member.
Targets are only valid when they belong to the sender's room.

## Protocol 2 gameplay routing

The following messages may be broadcast or may include `target_client_id`:

| Area | Message types |
| --- | --- |
| World flags | `event_bit`, `tbox_bit`, `switch_bit`, `room_switch_bit`, `item_bit`, `dungeon_item_bit` |
| Inventory/progression | `save_snapshot`, `item_get`, `rando_item_get`, `item_first_bit`, `collect_crystal`, `collect_mirror`, `dark_clear_lv`, `transform_lv`, `region_bit`, `collect`, `visited_room`, `letter_get`, `ooccoo_state`, `collect_smell` |
| Counts and slots | `key_num`, `light_drop_num`, `light_drop_get_flag`, `max_life_update`, `bottle_slots`, `bomb_bag_slot`, `rupee_count`, `rupee_delta`, `poe_count`, `malo_fundraising`, `charlo_offering`, `fish_record` |
| Peer status/preferences | `presence`, `progression_state`, `puppet_preference`, `midna_preference` |
| Visual/PvP | `midna_pose`, `pvp_hit` |
| Ganondorf encounter | `ganondorf_owner_claim`, `ganondorf_owner`, `ganondorf_hit`, `ganondorf_reaction`, `ganondorf_player_damage`, `ganondorf_state` |

The relay transports these payloads but does not interpret or persist their
gameplay state. Late join synchronization remains client-to-client.

Semantic rendering snapshots use UDP packet type 7. The relay does not
decode snapshot deltas; clients select a snapshot acknowledged by every
eligible receiver and broadcast one delta against that explicitly named
baseline.

## Room settings

The owner sends all current values in one `room_settings` message:

- `dummy_model`
- `sync_flags`
- `sync_world`
- `remote_collision`
- `pvp`

Every value must be boolean. The relay forces `pvp` off when
`remote_collision` is off. Non-owner updates return `owner_only`. Current
settings are included in every `welcome`, so late joiners use the room's
settings rather than their local defaults.


### Reliable line compression (relay 2.1.0)

Lines of JSON at least 1 KiB may be encoded as `Z1` followed by lowercase hex
of one Zstandard frame, then the existing newline delimiter. Compression is used
only if the complete encoded line is smaller. Uncompressed JSON remains valid.
Both encoded and expanded input are limited to 512 KiB; truncated, concatenated,
invalid, and oversized frames are rejected. The decoded JSON enters the existing
routing and ownership validation unchanged. Compression does not alter KCP
ordering, acknowledgement, retransmission, or congestion control.

Deploy this relay together with the matching Online build. Relay 2.0.0 cannot
read compressed lines. Pose datagrams are unchanged.
