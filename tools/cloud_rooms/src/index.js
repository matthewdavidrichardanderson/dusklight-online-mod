// Room membership and ICE signaling only. This Worker deliberately has no
// peer_reliable, UDP, TURN, or gameplay-forwarding endpoint.
const MAX_PLAYERS = 8;
const MAX_PENDING = 4;
const MAX_MESSAGE_BYTES = 16 * 1024;
const MAX_SIGNAL_BYTES = 12 * 1024;
const HELLO_TIMEOUT_MS = 15_000;
const SETTINGS_TIMEOUT_MS = 30_000;
const ROOM_NAME = /^[A-Za-z0-9 _-]{1,64}$/;
const CLIENT_NAME = /^[^\x00-\x1f\x7f]{1,32}$/;
const CLIENT_ID = /^client_[1-9][0-9]{0,19}$/;
const DEFAULT_SETTINGS = Object.freeze({
  dummy_model: true, sync_flags: true, sync_world: false,
  remote_collision: true, pvp: false,
});

export function normalizedRoom(name) {
  if (typeof name !== "string" || !ROOM_NAME.test(name) || name.trim() !== name ||
      !name.trim()) return null;
  return name.toLowerCase();
}

function settings(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const result = {};
  for (const key of Object.keys(DEFAULT_SETTINGS)) {
    if (typeof value[key] !== "boolean") return null;
    result[key] = value[key];
  }
  result.pvp = result.pvp && result.remote_collision && result.dummy_model;
  return result;
}

function send(ws, value) {
  try { ws.send(JSON.stringify(value)); return true; }
  catch { try { ws.close(1011, "send failed"); } catch {} return false; }
}

function close(ws, code, reason) {
  try { ws.close(code, reason); } catch {}
}

function attachment(ws) {
  try { return ws.deserializeAttachment() ?? {}; } catch { return {}; }
}

function joined(state) {
  return state.getWebSockets().filter(ws => attachment(ws).joined);
}

function member(state, id) {
  return joined(state).find(ws => attachment(ws).id === id);
}

function capabilities(message) {
  const value = message?.capabilities;
  return {
    semantic_visual_v1: value?.semantic_visual_v1 === true,
    semantic_snapshot_delta_v1: value?.semantic_snapshot_delta_v1 === true,
  };
}

function readiness(members) {
  return {
    semantic_visuals_ready: members.every(ws => attachment(ws).capabilities.semantic_visual_v1),
    snapshot_deltas_ready: members.every(ws => attachment(ws).capabilities.semantic_snapshot_delta_v1),
  };
}

function broadcast(members, value, exclude) {
  for (const ws of members) if (ws !== exclude) send(ws, value);
}

async function passwordHash(password, salt) {
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(password),
    "PBKDF2", false, ["deriveBits"]);
  const bits = await crypto.subtle.deriveBits({
    name: "PBKDF2", salt: Uint8Array.from(salt), iterations: 100_000, hash: "SHA-256",
  }, key, 256);
  return Array.from(new Uint8Array(bits));
}

function equalHash(a, b) {
  if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
  let difference = 0;
  for (let i = 0; i < a.length; i++) difference |= a[i] ^ b[i];
  return difference === 0;
}

function newClientId(state) {
  for (let attempt = 0; attempt < 16; attempt++) {
    const number = crypto.getRandomValues(new Uint32Array(2));
    const id = `client_${((BigInt(number[0]) << 32n) | BigInt(number[1])) || 1n}`;
    if (!member(state, id)) return id;
  }
  throw new Error("Could not allocate a unique client ID");
}

function error(ws, reason, fatal = false) {
  send(ws, { type: "error", error: reason });
  if (fatal) close(ws, 4000, reason);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/health") return Response.json({ service: "dusklight-rooms", version: 1 });
    if (request.method !== "GET" || request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
      return new Response("WebSocket upgrade required", { status: 426 });
    if (!url.pathname.startsWith("/room/")) return new Response("Not found", { status: 404 });
    let supplied;
    try { supplied = decodeURIComponent(url.pathname.slice(6)); }
    catch { return new Response("Invalid room name", { status: 400 }); }
    const room = normalizedRoom(supplied);
    if (!room || url.search) return new Response("Invalid room name", { status: 400 });
    const id = env.ROOMS.idFromName(room);
    return env.ROOMS.get(id).fetch(request);
  },
};

export class Room {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.room = null;
    this.serial = Promise.resolve();
    this.ready = state.blockConcurrencyWhile(async () => {
      this.room = await state.storage.get("room") ?? null;
    });
  }

  async exclusive(action) {
    const previous = this.serial;
    let release;
    this.serial = new Promise(resolve => { release = resolve; });
    await previous;
    try { return await action(); }
    finally { release(); }
  }

  async fetch(request) {
    await this.ready;
    return this.exclusive(async () => {
      const sockets = this.state.getWebSockets();
      if (sockets.length >= MAX_PLAYERS + MAX_PENDING ||
          sockets.filter(ws => !attachment(ws).joined).length >= MAX_PENDING)
        return new Response("Room full", { status: 429 });
      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);
      this.state.acceptWebSocket(server);
      server.serializeAttachment({ joined: false, deadline: Date.now() + HELLO_TIMEOUT_MS });
      await this.armAlarm();
      return new Response(null, { status: 101, webSocket: client });
    });
  }

  async armAlarm() {
    const pending = this.state.getWebSockets()
      .map(ws => attachment(ws))
      .filter(item => !item.joined && Number.isFinite(item.deadline) && item.deadline > 0)
      .map(item => item.deadline);
    const deadline = Math.min(...pending, this.room?.pending?.deadline ?? Infinity);
    if (Number.isFinite(deadline)) await this.state.storage.setAlarm(Math.max(Date.now() + 1, deadline));
    else await this.state.storage.deleteAlarm();
  }

  async webSocketMessage(ws, raw) {
    await this.ready;
    if (typeof raw !== "string" || raw.length > MAX_MESSAGE_BYTES ||
        new TextEncoder().encode(raw).length > MAX_MESSAGE_BYTES) {
      error(ws, "invalid_message", true); return;
    }
    let message;
    try { message = JSON.parse(raw); }
    catch { error(ws, "invalid_message", true); return; }
    if (!message || typeof message !== "object" || Array.isArray(message) ||
        typeof message.type !== "string") {
      error(ws, "invalid_message", true); return;
    }
    const own = attachment(ws);
    if (!own.joined) {
      if (message.type !== "hello") { error(ws, "hello_required", true); return; }
      await this.exclusive(() => this.hello(ws, message));
      return;
    }
    if (message.type === "ice_signal") {
      const target = member(this.state, message.target_client_id);
      if (!target || message.target_client_id === own.id ||
          !Number.isInteger(message.kind) || message.kind < 0 || message.kind > 2 ||
          !Number.isSafeInteger(message.generation) || message.generation < 0 ||
          typeof message.data !== "string" || message.data.length > MAX_SIGNAL_BYTES) {
        error(ws, "invalid_ice_signal"); return;
      }
      send(target, { type: "ice_signal", client_id: own.id, kind: message.kind,
        data: message.data, generation: message.generation });
      return;
    }
    if (message.type === "room_settings") {
      await this.exclusive(() => this.changeSettings(ws, message)); return;
    }
    if (message.type === "settings_ready") {
      await this.exclusive(() => this.settingsReady(ws, message)); return;
    }
    if (message.type === "kick") {
      await this.exclusive(async () => {
        if (own.id !== this.room?.owner) { error(ws, "not_owner"); return; }
        const target = member(this.state, message.target_client_id);
        if (!target || target === ws) { error(ws, "player_not_found"); return; }
        send(target, { type: "kicked", reason: "removed_by_host" });
        close(target, 4001, "removed by lobby host");
      });
      return;
    }
    // Fail closed: no peer_reliable, peer_receipt, pose, chat, save, or other
    // game payload is accepted by this service, now or in future versions.
    error(ws, "unsupported_control_type", true);
  }

  async hello(ws, message) {
    const name = message.name;
    const password = message.password;
    const action = message.action;
    if (message.protocol_version !== 3 || typeof name !== "string" ||
        !CLIENT_NAME.test(name) ||
        typeof password !== "string" || password.length < 6 || password.length > 128 ||
        (action !== "create" && action !== "join")) {
      error(ws, "invalid_hello", true); return;
    }
    const members = joined(this.state);
    // An interrupted close/hibernation must not strand a password-protected
    // room with no live members. Only a new host can recreate an empty room.
    if (this.room && !members.length) {
      this.room = null;
      await this.state.storage.delete("room");
    }
    if (members.length >= MAX_PLAYERS) { error(ws, "lobby_full", true); return; }
    if (!this.room) {
      if (action !== "create") { error(ws, "lobby_not_found", true); return; }
      const configured = settings(message.settings);
      if (!configured) { error(ws, "invalid_settings", true); return; }
      const salt = Array.from(crypto.getRandomValues(new Uint8Array(16)));
      this.room = { salt, hash: await passwordHash(password, salt), owner: "",
        settings: configured, generation: 0, pending: null };
    } else {
      const hash = await passwordHash(password, this.room.salt);
      if (!equalHash(hash, this.room.hash)) { error(ws, "invalid_password", true); return; }
      if (action === "create" && members.length) {
        // A reconnecting host may have lost its old socket. Joining an extant
        // room must not silently steal ownership from current members.
        if (member(this.state, this.room.owner)) { error(ws, "lobby_exists", true); return; }
      }
    }
    const id = newClientId(this.state);
    const info = { joined: true, id, name, capabilities: capabilities(message),
      want_puppet: message.want_puppet === true };
    ws.serializeAttachment(info);
    if (!member(this.state, this.room.owner)) this.room.owner = id;
    await this.state.storage.put("room", this.room);
    const all = joined(this.state);
    const ready = readiness(all);
    send(ws, { type: "welcome", protocol_version: 3, room_id: message.room_id,
      client_id: id, owner_client_id: this.room.owner, peers: members.map(other => ({
        client_id: attachment(other).id, name: attachment(other).name,
        want_puppet: attachment(other).want_puppet,
      })), settings: this.room.settings, settings_generation: this.room.generation,
      settings_pending: !!this.room.pending, ...ready });
    broadcast(members, { type: "peer_joined", client_id: id, name,
      want_puppet: info.want_puppet, ...ready });
    await this.armAlarm();
  }

  async changeSettings(ws, message) {
    const own = attachment(ws);
    if (own.id !== this.room?.owner) { error(ws, "not_owner"); return; }
    if (this.room.pending) { error(ws, "settings_busy"); return; }
    const requested = settings(message.settings);
    if (!requested) { error(ws, "invalid_settings"); return; }
    const members = joined(this.state);
    const generation = this.room.generation + 1;
    this.room.pending = { settings: requested, generation,
      participants: members.map(item => attachment(item).id), ready: [],
      deadline: Date.now() + SETTINGS_TIMEOUT_MS };
    await this.state.storage.put("room", this.room);
    broadcast(members, { type: "settings_prepare", generation,
      participants: this.room.pending.participants });
    await this.armAlarm();
  }

  async settingsReady(ws, message) {
    const own = attachment(ws);
    const pending = this.room?.pending;
    if (!pending || !Number.isSafeInteger(message.generation) ||
        message.generation !== pending.generation ||
        !pending.participants.includes(own.id) || pending.ready.includes(own.id)) {
      error(ws, "invalid_settings_ready"); return;
    }
    pending.ready.push(own.id);
    if (pending.ready.length !== pending.participants.length) {
      await this.state.storage.put("room", this.room); return;
    }
    this.room.settings = pending.settings;
    this.room.generation = pending.generation;
    this.room.pending = null;
    await this.state.storage.put("room", this.room);
    const members = joined(this.state);
    broadcast(members, { type: "room_settings", owner_client_id: this.room.owner,
      settings: this.room.settings, settings_generation: this.room.generation,
      ...readiness(members) });
    await this.armAlarm();
  }

  async webSocketClose(ws) {
    await this.exclusive(() => this.leave(ws));
  }

  async webSocketError(ws) {
    close(ws, 1011, "socket error");
    await this.exclusive(() => this.leave(ws));
  }

  async leave(ws) {
    await this.ready;
    const own = attachment(ws);
    if (!own.joined || !this.room) { await this.armAlarm(); return; }
    ws.serializeAttachment({ joined: false });
    const members = joined(this.state);
    if (!members.length) {
      this.room = null;
      await this.state.storage.delete("room");
      await this.armAlarm();
      return;
    }
    if (this.room.pending?.participants.includes(own.id)) {
      // All peers reconnect from a clean generation instead of applying a
      // partial settings transaction after a participant disappeared.
      this.room.pending = null;
      await this.state.storage.put("room", this.room);
      for (const memberSocket of members) close(memberSocket, 4002, "settings interrupted");
      return;
    }
    const previousOwner = this.room.owner;
    if (previousOwner === own.id) this.room.owner = attachment(members[0]).id;
    await this.state.storage.put("room", this.room);
    broadcast(members, { type: "peer_left", client_id: own.id, ...readiness(members) });
    if (previousOwner !== this.room.owner)
      broadcast(members, { type: "owner_changed", owner_client_id: this.room.owner });
    await this.armAlarm();
  }

  async alarm() {
    await this.exclusive(() => this.expire());
  }

  async expire() {
    await this.ready;
    const now = Date.now();
    for (const ws of this.state.getWebSockets()) {
      const own = attachment(ws);
      if (!own.joined && own.deadline && own.deadline <= now) {
        ws.serializeAttachment({ joined: false, deadline: 0 });
        close(ws, 4000, "hello timeout");
      }
    }
    if (this.room?.pending?.deadline <= now) {
      this.room.pending = null;
      await this.state.storage.put("room", this.room);
      for (const ws of joined(this.state)) close(ws, 4002, "settings timeout");
    }
    await this.armAlarm();
  }
}
