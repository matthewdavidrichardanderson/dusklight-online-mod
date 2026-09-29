import assert from "assert";
import { createHmac, pbkdf2Sync, randomBytes } from "node:crypto";
import { Room, normalizedRoom } from "./src/index.js";

class Socket {
  constructor(state) {
    this.state = state; this.sent = []; this.closed = false;
    const nonce = randomBytes(16).toString("hex");
    const salt = state.getWebSockets().some(ws => ws.info.joined) ?
      state.storage.values.get("room")?.salt : randomBytes(16).toString("hex");
    this.info = { joined: false, nonce, salt };
    this.sent.push({ type: "auth_challenge", nonce, salt });
  }
  serializeAttachment(value) { this.info = value; }
  deserializeAttachment() { return this.info; }
  send(value) { this.sent.push(JSON.parse(value)); }
  close(code, reason) { this.closed = true; this.closeCode = code; this.closeReason = reason; }
  take(type) { return this.sent.find(value => value.type === type); }
}

class Storage {
  values = new Map();
  async get(key) { return this.values.get(key); }
  async put(key, value) { this.values.set(key, structuredClone(value)); }
  async delete(key) { this.values.delete(key); }
  async setAlarm(value) { this.alarm = value; }
  async deleteAlarm() { this.alarm = null; }
}

class State {
  sockets = [];
  storage = new Storage();
  blockConcurrencyWhile(action) { return action(); }
  getWebSockets() { return this.sockets.filter(ws => !ws.closed); }
  add() { const ws = new Socket(this); this.sockets.push(ws); return ws; }
}

const settings = { dummy_model: true, sync_flags: true, sync_world: false,
  remote_collision: true, pvp: false };
const hello = (socket, action, code = "secret-password") => {
  const { nonce, salt } = socket.take("auth_challenge");
  const verifier = pbkdf2Sync(code, Buffer.from(
    "dusklight-lobby-code-v1:example lobby\0"), 100_000, 32, "sha256");
  const key = createHmac("sha256", verifier).update(Buffer.concat([
    Buffer.from("dusklight-room-key-v1:"), Buffer.from(salt, "hex")])).digest();
  const proof = createHmac("sha256", key)
    .update("dusklight-room-auth-v1:" + nonce).digest("hex");
  return {
    type: "hello", action, protocol_version: 5, room_id: "Example Lobby",
    name: action === "create" ? "Host" : "Guest", code_proof: proof,
    ...(action === "create" ? { code_verifier: verifier.toString("hex") } : {}),
    settings, want_puppet: true, capabilities: {
      semantic_visual_v1: true, semantic_snapshot_delta_v1: true,
    },
  };
};

assert.equal(normalizedRoom("Example Lobby"), "example lobby");
assert.equal(normalizedRoom("../room"), null);
assert.equal(normalizedRoom(" lobby "), null);

const state = new State();
const room = new Room(state, {});
await room.ready;
const host = state.add();
const hostHello = hello(host, "create");
assert.equal(JSON.stringify(hostHello).includes("secret-password"), false);
await room.webSocketMessage(host, JSON.stringify(hostHello));
const welcome = host.take("welcome");
assert.ok(welcome.client_id.startsWith("client_"));
assert.equal(welcome.owner_client_id, welcome.client_id);
assert.equal(welcome.settings.sync_flags, true);
assert.match(welcome.mesh_key, /^[0-9a-f]{64}$/);
assert.equal(JSON.stringify(state.storage.values.get("room")).includes("secret-password"), false);

const wrong = state.add();
await room.webSocketMessage(wrong, JSON.stringify(hello(wrong, "join", "not-the-password")));
assert.equal(wrong.take("error").error, "bad_lobby_code");
assert.equal(wrong.closed, true);

const replay = state.add();
const replayHello = hello(replay, "join");
replayHello.code_proof = hostHello.code_proof;
await room.webSocketMessage(replay, JSON.stringify(replayHello));
assert.equal(replay.take("error").error, "bad_lobby_code");

const guest = state.add();
await room.webSocketMessage(guest, JSON.stringify(hello(guest, "join")));
const guestWelcome = guest.take("welcome");
assert.equal(guestWelcome.mesh_key, welcome.mesh_key);
const guestId = guestWelcome.client_id;
assert.notEqual(guestId, welcome.client_id);
assert.equal(host.take("peer_joined").client_id, guestId);

await room.webSocketMessage(host, JSON.stringify({ type: "ice_signal", target_client_id: guestId,
  kind: 1, generation: 0, data: "candidate" }));
assert.equal(guest.take("ice_signal").client_id, welcome.client_id);

// The service must never become a gameplay relay, even if a client tries to
// send exactly the old relay protocol's reliable-body message.
const before = guest.sent.length;
await room.webSocketMessage(host, JSON.stringify({ type: "peer_reliable",
  recipients: [{ id: guestId, sequence: 1 }], body: { type: "chat", text: "no" } }));
assert.equal(guest.sent.length, before);
assert.equal(host.take("error").error, "unsupported_control_type");

// A separate fixture exercises room-settings barriers without closing host.
const state2 = new State();
const room2 = new Room(state2, {});
await room2.ready;
const owner2 = state2.add();
await room2.webSocketMessage(owner2, JSON.stringify(hello(owner2, "create")));
const peer2 = state2.add();
await room2.webSocketMessage(peer2, JSON.stringify(hello(peer2, "join")));
const changed = { ...settings, sync_flags: false };
await room2.webSocketMessage(owner2, JSON.stringify({ type: "room_settings", settings: changed }));
assert.equal(owner2.take("settings_prepare").generation, 1);
assert.equal(peer2.take("settings_prepare").participants.length, 2);
await room2.webSocketMessage(owner2, JSON.stringify({ type: "settings_ready", generation: 1 }));
assert.equal(owner2.take("room_settings"), undefined);
await room2.webSocketMessage(peer2, JSON.stringify({ type: "settings_ready", generation: 1 }));
assert.equal(owner2.take("room_settings").settings.sync_flags, false);
assert.equal(peer2.take("room_settings").settings_generation, 1);
assert.equal(state2.storage.values.get("room").generation, 1);

// Hibernation replaces the class instance while preserving sockets and
// storage. A new join must still see the same room and settings generation.
const rehydrated = new Room(state2, {});
await rehydrated.ready;
const late = state2.add();
await rehydrated.webSocketMessage(late, JSON.stringify(hello(late, "join")));
assert.equal(late.take("welcome").settings_generation, 1);
assert.equal(late.take("welcome").settings.sync_flags, false);
await rehydrated.webSocketClose(late);

await room2.webSocketClose(owner2);
assert.equal(peer2.take("owner_changed").owner_client_id, peer2.take("welcome").client_id);
await room2.webSocketClose(peer2);
assert.equal(state2.storage.values.has("room"), false);

const state3 = new State();
const room3 = new Room(state3, {});
await room3.ready;
const firstHost = state3.add();
const secondHost = state3.add();
await Promise.all([
  room3.webSocketMessage(firstHost, JSON.stringify(hello(firstHost, "create"))),
  room3.webSocketMessage(secondHost, JSON.stringify(hello(secondHost, "create"))),
]);
assert.ok(firstHost.take("welcome"));
assert.equal(secondHost.take("error").error, "lobby_exists");
assert.equal(state3.storage.values.get("room").owner, firstHost.take("welcome").client_id);

const invalidName = state3.add();
await room3.webSocketMessage(invalidName, JSON.stringify({ ...hello(invalidName, "join"), name: null }));
assert.equal(invalidName.take("error").error, "invalid_hello");

const staleState = new State();
staleState.storage.values.set("room", structuredClone(state3.storage.values.get("room")));
const staleRoom = new Room(staleState, {});
await staleRoom.ready;
const replacement = staleState.add();
await staleRoom.webSocketMessage(replacement,
  JSON.stringify(hello(replacement, "create", "different-password")));
assert.ok(replacement.take("welcome"));
assert.equal(staleState.storage.values.get("room").owner,
  replacement.take("welcome").client_id);

const idle = state3.add();
idle.serializeAttachment({ joined: false, deadline: Date.now() - 1 });
await room3.alarm();
assert.equal(idle.closed, true);
assert.ok(!state3.storage.alarm || state3.storage.alarm > Date.now());

console.log("Cloud room protocol tests passed");
