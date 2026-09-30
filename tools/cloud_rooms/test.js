import assert from "assert";
import { createHmac, pbkdf2Sync, randomBytes } from "node:crypto";
import { Room, normalizedRoom } from "./src/index.js";
import worker from "./src/index.js";
import { createSession, exchange, seal, open, roomRoute } from "./src/cloud_cipher.js";

class Socket {
  constructor(state) {
    this.state = state; this.sent = []; this.closed = false;
    this.wire = [];
    const nonce = randomBytes(16).toString("hex");
    const salt = state.getWebSockets().some(ws => ws.info.joined) ?
      state.storage.values.get("room")?.salt : randomBytes(16).toString("hex");
    const route = roomRoute("Example Lobby");
    const secure = createSession();
    this.client = createSession();
    this.info = { joined: false, nonce, salt, route, secure, deadline: Date.now() + 15000 };
    this.ready = Promise.all([exchange(this.client, secure.publicKey, route, true),
      exchange(secure, this.client.publicKey, route)]).then(() => {
      this.send(seal(secure, JSON.stringify({ type: "auth_challenge", nonce, salt })));
    });
  }
  serializeAttachment(value) { this.info = structuredClone(value); }
  deserializeAttachment() { return structuredClone(this.info); }
  send(value) { this.wire.push(value); this.sent.push(JSON.parse(open(this.client, value))); }
  close(code, reason) { this.closed = true; this.closeCode = code; this.closeReason = reason; }
  take(type) { return this.sent.find(value => value.type === type); }
}

const deliver = (room, socket, raw) => room.webSocketMessage(socket, seal(socket.client, raw));

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
  acceptWebSocket(ws) { this.sockets.push(ws); }
  async add() { const ws = new Socket(this); this.sockets.push(ws); await ws.ready; return ws; }
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
const host = await state.add();
const hostHello = hello(host, "create");
assert.equal(JSON.stringify(hostHello).includes("secret-password"), false);
await deliver(room, host, JSON.stringify(hostHello));
const welcome = host.take("welcome");
assert.ok(welcome.client_id.startsWith("client_"));
assert.equal(welcome.owner_client_id, welcome.client_id);
assert.equal(welcome.settings.sync_flags, true);
assert.match(welcome.mesh_key, /^[0-9a-f]{64}$/);
assert.equal(JSON.stringify(state.storage.values.get("room")).includes("secret-password"), false);

const wrong = await state.add();
await deliver(room, wrong, JSON.stringify(hello(wrong, "join", "not-the-password")));
assert.equal(wrong.take("error").error, "bad_lobby_code");
assert.equal(wrong.closed, true);

const replay = await state.add();
const replayHello = hello(replay, "join");
replayHello.code_proof = hostHello.code_proof;
await deliver(room, replay, JSON.stringify(replayHello));
assert.equal(replay.take("error").error, "bad_lobby_code");

const guest = await state.add();
await deliver(room, guest, JSON.stringify(hello(guest, "join")));
const guestWelcome = guest.take("welcome");
assert.equal(guestWelcome.mesh_key, welcome.mesh_key);
const guestId = guestWelcome.client_id;
assert.notEqual(guestId, welcome.client_id);
assert.equal(host.take("peer_joined").client_id, guestId);

await deliver(room, host, JSON.stringify({ type: "ice_signal", target_client_id: guestId,
  kind: 1, generation: 0, data: "candidate" }));
assert.equal(guest.take("ice_signal").client_id, welcome.client_id);

// The service must never become a gameplay relay, even if a client tries to
// send exactly the old relay protocol's reliable-body message.
const before = guest.sent.length;
await deliver(room, host, JSON.stringify({ type: "peer_reliable",
  recipients: [{ id: guestId, sequence: 1 }], body: { type: "chat", text: "no" } }));
assert.equal(guest.sent.length, before);
assert.equal(host.take("error").error, "unsupported_control_type");

// A separate fixture exercises room-settings barriers without closing host.
const state2 = new State();
const room2 = new Room(state2, {});
await room2.ready;
const owner2 = await state2.add();
await deliver(room2, owner2, JSON.stringify(hello(owner2, "create")));
const peer2 = await state2.add();
await deliver(room2, peer2, JSON.stringify(hello(peer2, "join")));
const changed = { ...settings, sync_flags: false };
await deliver(room2, owner2, JSON.stringify({ type: "room_settings", settings: changed }));
assert.equal(owner2.take("settings_prepare").generation, 1);
assert.equal(peer2.take("settings_prepare").participants.length, 2);
await deliver(room2, owner2, JSON.stringify({ type: "settings_ready", generation: 1 }));
assert.equal(owner2.take("room_settings"), undefined);
await deliver(room2, peer2, JSON.stringify({ type: "settings_ready", generation: 1 }));
assert.equal(owner2.take("room_settings").settings.sync_flags, false);
assert.equal(peer2.take("room_settings").settings_generation, 1);
assert.equal(state2.storage.values.get("room").generation, 1);

// Hibernation replaces the class instance while preserving sockets and
// storage. A new join must still see the same room and settings generation.
const rehydrated = new Room(state2, {});
await rehydrated.ready;
const late = await state2.add();
await deliver(rehydrated, late, JSON.stringify(hello(late, "join")));
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
const firstHost = await state3.add();
const secondHost = await state3.add();
await Promise.all([
  deliver(room3, firstHost, JSON.stringify(hello(firstHost, "create"))),
  deliver(room3, secondHost, JSON.stringify(hello(secondHost, "create"))),
]);
assert.ok(firstHost.take("welcome"));
assert.equal(secondHost.take("error").error, "lobby_exists");
assert.equal(state3.storage.values.get("room").owner, firstHost.take("welcome").client_id);

const invalidName = await state3.add();
await deliver(room3, invalidName, JSON.stringify({ ...hello(invalidName, "join"), name: null }));
assert.equal(invalidName.take("error").error, "invalid_hello");

const staleState = new State();
staleState.storage.values.set("room", structuredClone(state3.storage.values.get("room")));
const staleRoom = new Room(staleState, {});
await staleRoom.ready;
const replacement = await staleState.add();
await deliver(staleRoom, replacement,
  JSON.stringify(hello(replacement, "create", "different-password")));
assert.ok(replacement.take("welcome"));
assert.equal(staleState.storage.values.get("room").owner,
  replacement.take("welcome").client_id);

const idle = await state3.add();
idle.serializeAttachment({ joined: false, deadline: Date.now() - 1 });
await room3.alarm();
assert.equal(idle.closed, true);
assert.ok(!state3.storage.alarm || state3.storage.alarm > Date.now());

// Requests queued while hello awaits its proof must not reset record counters.
const concurrentState = new State();
const concurrentRoom = new Room(concurrentState, {});
const concurrent = await concurrentState.add();
await Promise.all([
  deliver(concurrentRoom, concurrent, JSON.stringify(hello(concurrent, "create"))),
  deliver(concurrentRoom, concurrent, JSON.stringify({ type: "room_settings", settings })),
]);
assert.ok(concurrent.take("welcome"));
assert.ok(concurrent.take("settings_prepare"));
assert.equal(concurrent.info.secure.rx, 2);
assert.equal(concurrent.info.secure.tx, 3); // challenge, welcome, prepare
assert.equal(new Set(concurrent.wire.map(wire => wire.slice(5, 21))).size, concurrent.wire.length);

// Exercise the public URL boundary without a platform WebSocket upgrade.
const routed = [];
const env = { ROOMS: {
  idFromName: name => { routed.push(name); return name; },
  get: () => ({ fetch: () => new Response("routed") }),
} };
const route = roomRoute("Example Lobby");
assert.equal(route, "7d5fcc1f68e533bc791932206e8fa26d2e54a640049697dd1360a2732ba697d1");
assert.equal(route, roomRoute("eXAMPLE lOBBY"));
for (const [path, expected] of [
  ["/room/Example%20Lobby", 426], ["/room/v2/Example%20Lobby", 400],
  [`/room/v2/${route}?code=secret`, 400], [`/room/v2/${route.toUpperCase()}`, 400],
  [`/room/v2/${route}`, 200],
]) {
  const response = await worker.fetch(new Request("https://rooms.test" + path,
    { headers: { Upgrade: "websocket" } }), env);
  assert.equal(response.status, expected, path);
}
assert.deepEqual(routed, ["v2:" + route]);

// Minimal platform adapter: unlike the earlier membership fixtures, this
// exercises Room.fetch and the production key exchange before every hello.
class WireSocket {
  info = {};
  wire = [];
  sent = [];
  serializeAttachment(value) { this.info = structuredClone(value); }
  deserializeAttachment() { return structuredClone(this.info); }
  send(value) {
    this.wire.push(value);
    this.sent.push(JSON.parse(value.startsWith("DCR1:") ? open(this.client, value) : value));
  }
  close(code, reason) { this.closed = true; this.closeCode = code; this.closeReason = reason; }
  take(type) { return this.sent.find(value => value.type === type); }
}
const RealResponse = globalThis.Response;
globalThis.Response = class extends RealResponse {
  constructor(body, init) {
    super(body, init?.status === 101 ? { status: 200 } : init);
    this.webSocket = init?.webSocket;
  }
};
globalThis.WebSocketPair = class { constructor() { this[0] = {}; this[1] = new WireSocket(); } };
const secureState = new State();
const secureRoom = new Room(secureState, {});
async function accept(state = secureState, room = secureRoom) {
  await room.fetch(new Request(`https://rooms.test/room/v2/${route}`));
  return state.sockets.at(-1);
}
async function handshake(socket, room = secureRoom) {
  socket.client = createSession();
  await exchange(socket.client, socket.take("secure_hello").public_key, route, true);
  await room.webSocketMessage(socket, JSON.stringify({ type: "secure_ready", version: 1,
    public_key: socket.client.publicKey }));
  assert.ok(socket.take("auth_challenge"));
  assert.equal(socket.info.secure.privateKey, undefined);
  assert.ok(JSON.stringify(socket.info).length < 2048, "hibernation attachment fits platform limit");
  return socket;
}
async function connect(state = secureState, room = secureRoom) {
  return handshake(await accept(state, room), room);
}

const secureHost = await connect();
const secureHello = hello(secureHost, "create");
const savedHello = seal(secureHost.client, JSON.stringify(secureHello));
await secureRoom.webSocketMessage(secureHost, savedHello);
assert.ok(secureHost.take("welcome"));
for (const wire of secureHost.wire.slice(1)) {
  assert.ok(wire.startsWith("DCR1:"));
  for (const sensitive of ["Example Lobby", "secret-password", secureHello.code_verifier,
    secureHost.take("welcome").mesh_key]) assert.equal(wire.includes(sensitive), false);
}
// Each malformed connection must fail without gaining room membership.
for (const handshake of [
  { type: "hello", protocol_version: 5 }, { type: "secure_ready", version: 0 },
  { type: "secure_ready", version: 1, public_key: "0".repeat(64) },
  { type: "secure_ready", version: 1, public_key: "z".repeat(64) }, null,
]) {
  const socket = await accept();
  await secureRoom.webSocketMessage(socket, JSON.stringify(handshake));
  assert.equal(socket.closed, true);
  assert.equal(socket.info.joined, false);
}
for (const attack of ["plaintext", "tamper", "cross-session", "oversize", "binary", "expired", "room-mismatch"]) {
  const socket = await connect();
  let message = hello(socket, "join");
  if (attack === "room-mismatch") message.room_id = "Other Lobby";
  let wire = seal(socket.client, JSON.stringify(message));
  const validWire = wire;
  if (attack === "plaintext") wire = JSON.stringify(message);
  if (attack === "tamper") wire = wire.slice(0, -1) + (wire.at(-1) === "0" ? "1" : "0");
  if (attack === "cross-session") wire = savedHello;
  if (attack === "oversize") wire = "DCR1:" + "00".repeat(17000);
  if (attack === "binary") wire = new Uint8Array([1, 2]);
  if (attack === "expired") socket.info.deadline = Date.now() - 1;
  await secureRoom.webSocketMessage(socket, wire);
  assert.equal(socket.closed, true, attack);
  assert.equal(socket.info.joined, false, attack);
  const sentBeforeRetry = socket.wire.length;
  await secureRoom.webSocketMessage(socket, validWire);
  assert.equal(socket.info.joined, false, `${attack}: queued record after rejection`);
  assert.equal(socket.wire.length, sentBeforeRetry);
}
const replaySocket = await connect();
const joinWire = seal(replaySocket.client, JSON.stringify(hello(replaySocket, "join")));
await secureRoom.webSocketMessage(replaySocket, joinWire);
assert.ok(replaySocket.take("welcome"));
await secureRoom.webSocketMessage(replaySocket, joinWire);
assert.equal(replaySocket.closeReason, "invalid_encrypted_message");
await secureRoom.webSocketClose(replaySocket);

// The exact same socket resumes with its next nonce after hibernation.
const secureRehydrated = new Room(secureState, {});
const beforeHibernation = secureHost.info.secure.tx;
await deliver(secureRehydrated, secureHost, JSON.stringify({ type: "room_settings", settings }));
assert.equal(secureHost.info.secure.tx, beforeHibernation + 1);
assert.ok(secureHost.take("settings_prepare"));
await deliver(secureRehydrated, secureHost, JSON.stringify({ type: "settings_ready", generation: 1 }));
assert.equal(secureHost.take("room_settings").settings_generation, 1);
assert.equal(secureHost.closed, undefined);

// A guest can open before the host creates the room, with its key exchange
// either before or after creation. Hibernation must preserve pending challenges.
for (const earlyHandshake of [true, false]) {
  const creationState = new State();
  let creationRoom = new Room(creationState, {});
  const waitingGuest = await accept(creationState, creationRoom);
  if (earlyHandshake) await handshake(waitingGuest, creationRoom);
  creationRoom = new Room(creationState, {});
  const creator = await connect(creationState, creationRoom);
  await deliver(creationRoom, creator, JSON.stringify(hello(creator, "create")));
  assert.ok(creator.take("welcome"));
  if (!earlyHandshake) await handshake(waitingGuest, creationRoom);
  await deliver(creationRoom, waitingGuest, JSON.stringify(hello(waitingGuest, "join")));
  assert.ok(waitingGuest.take("welcome"), "correct code must survive concurrent room creation");
  assert.equal(waitingGuest.take("welcome").mesh_key, creator.take("welcome").mesh_key);
  assert.notEqual(waitingGuest.take("auth_challenge").nonce, creator.take("auth_challenge").nonce);
  const invalid = await connect(creationState, creationRoom);
  await deliver(creationRoom, invalid, JSON.stringify(hello(invalid, "join", "wrong-code")));
  assert.equal(invalid.take("error").error, "bad_lobby_code");
}

// Expired or rejected sockets cannot supply the next room's provisional salt.
const pendingState = new State();
const pendingRoom = new Room(pendingState, {});
const expiredPending = await accept(pendingState, pendingRoom);
expiredPending.info.deadline = Date.now() - 1;
const closingPending = await accept(pendingState, pendingRoom);
closingPending.info.closing = true;
const freshPending = await connect(pendingState, pendingRoom);
assert.notEqual(freshPending.info.salt, expiredPending.info.salt);
assert.notEqual(freshPending.info.salt, closingPending.info.salt);

// A non-owner must not close its recipient by causing forwarded JSON to grow.
const boundsState = new State();
const boundsRoom = new Room(boundsState, {});
const boundsHost = await connect(boundsState, boundsRoom);
await deliver(boundsRoom, boundsHost, JSON.stringify(hello(boundsHost, "create")));
const boundsGuest = await connect(boundsState, boundsRoom);
await deliver(boundsRoom, boundsGuest, JSON.stringify(hello(boundsGuest, "join")));
// Deterministic, valid member IDs exercise the worst-case forwarding expansion.
boundsHost.info.id = "client_1";
boundsRoom.room.owner = boundsHost.info.id;
boundsGuest.info.id = "client_18446744073709551615";
const inputSignal = data => `{"type":"ice_signal","target_client_id":"${boundsHost.info.id}",` +
  `"kind":1,"generation":1e15,"data":${JSON.stringify(data)}}`;
const outputSignal = data => JSON.stringify({ type: "ice_signal", client_id: boundsGuest.info.id,
  kind: 1, data, generation: 1e15 });
function fillSignal(serialize, character, bytes) {
  const remaining = bytes - Buffer.byteLength(serialize(""));
  const width = Buffer.byteLength(JSON.stringify(character)) - 2;
  return character.repeat(Math.floor(remaining / width)) + "a".repeat(remaining % width);
}
for (const character of ["é", "\0"]) {
  const data = fillSignal(inputSignal, character, 16 * 1024);
  const raw = inputSignal(data);
  assert.equal(Buffer.byteLength(raw), 16 * 1024);
  assert.ok(Buffer.byteLength(outputSignal(data)) > 16 * 1024);
  const received = boundsHost.wire.length;
  const nonceBefore = boundsHost.info.secure.tx;
  await deliver(boundsRoom, boundsGuest, raw);
  assert.equal(boundsGuest.sent.at(-1).error, "invalid_ice_signal");
  assert.equal(boundsHost.wire.length, received, "oversized forwarding must not reach its target");
  assert.equal(boundsHost.info.secure.tx, nonceBefore, "rejection must not consume the target nonce");
  assert.equal(boundsHost.closed, undefined);
  assert.equal(boundsGuest.closed, undefined);
}
const tooManyUtf8Bytes = "é".repeat(6 * 1024 + 1);
await deliver(boundsRoom, boundsGuest, inputSignal(tooManyUtf8Bytes));
assert.equal(boundsGuest.sent.at(-1).error, "invalid_ice_signal");
// Both the final JSON boundary and the UTF-8 data boundary remain usable.
for (const data of [fillSignal(outputSignal, "\0", 16 * 1024), "é".repeat(6 * 1024)]) {
  await deliver(boundsRoom, boundsGuest, inputSignal(data));
  assert.equal(boundsHost.sent.at(-1).type, "ice_signal");
  assert.equal(boundsHost.sent.at(-1).data, data);
  assert.equal(boundsHost.closed, undefined);
}
await deliver(boundsRoom, boundsHost, JSON.stringify({ type: "ice_signal",
  target_client_id: boundsGuest.info.id, kind: 1, generation: 0, data: "still connected" }));
assert.equal(boundsGuest.sent.at(-1).data, "still connected");
globalThis.Response = RealResponse;
delete globalThis.WebSocketPair;

console.log("Cloud room protocol, encryption, hibernation, and rejection tests passed");
