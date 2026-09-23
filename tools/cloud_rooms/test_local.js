// Run this against `wrangler dev --local` in another terminal. It exercises
// the actual Worker/Durable Object runtime, not just the protocol class.
import assert from "assert";

const base = process.env.ROOMS_TEST_URL ?? "http://127.0.0.1:8787";
const response = await fetch(`${base}/health`);
assert.equal(response.status, 200);
assert.equal((await response.json()).service, "dusklight-rooms");

class Client {
  constructor(socket) {
    this.socket = socket;
    this.messages = [];
    this.waiters = [];
    socket.addEventListener("message", event => {
      const value = JSON.parse(event.data);
      const index = this.waiters.findIndex(waiter => waiter.type === value.type);
      if (index < 0) this.messages.push(value);
      else this.waiters.splice(index, 1)[0].resolve(value);
    });
  }
  send(value) { this.socket.send(JSON.stringify(value)); }
  next(type) {
    const index = this.messages.findIndex(value => value.type === type);
    if (index >= 0) return Promise.resolve(this.messages.splice(index, 1)[0]);
    return new Promise((resolve, reject) => {
      const waiter = { type, resolve };
      this.waiters.push(waiter);
      setTimeout(() => {
        const position = this.waiters.indexOf(waiter);
        if (position >= 0) { this.waiters.splice(position, 1); reject(new Error(`Timed out: ${type}`)); }
      }, 5000).unref();
    });
  }
  close() { this.socket.close(); }
}

async function connect(room) {
  const socket = new WebSocket(`${base.replace(/^http/, "ws")}/room/${encodeURIComponent(room)}`);
  await new Promise((resolve, reject) => {
    socket.addEventListener("open", resolve, { once: true });
    socket.addEventListener("error", reject, { once: true });
  });
  return new Client(socket);
}

const settings = { dummy_model: true, sync_flags: true, sync_world: false,
  remote_collision: true, pvp: false };
const room = `Local Test ${Date.now()}`;
const password = "local-secret-password";
const hello = (action, name) => ({ type: "hello", protocol_version: 3,
  action, name, room_id: room, password, settings, want_puppet: true,
  capabilities: { semantic_visual_v1: true, semantic_snapshot_delta_v1: true } });

const host = await connect(room);
host.send(hello("create", "Host"));
const hostWelcome = await host.next("welcome");
assert.equal(hostWelcome.owner_client_id, hostWelcome.client_id);

const guest = await connect(room);
guest.send(hello("join", "Guest"));
const guestWelcome = await guest.next("welcome");
assert.equal(guestWelcome.peers[0].client_id, hostWelcome.client_id);
assert.equal((await host.next("peer_joined")).client_id, guestWelcome.client_id);

host.send({ type: "ice_signal", target_client_id: guestWelcome.client_id,
  kind: 1, generation: 0, data: "test candidate" });
assert.equal((await guest.next("ice_signal")).client_id, hostWelcome.client_id);

host.send({ type: "room_settings", settings: { ...settings, sync_flags: false } });
const hostPrepare = await host.next("settings_prepare");
assert.equal(hostPrepare.generation, 1);
assert.equal((await guest.next("settings_prepare")).participants.length, 2);
host.send({ type: "settings_ready", generation: 1 });
guest.send({ type: "settings_ready", generation: 1 });
assert.equal((await host.next("room_settings")).settings.sync_flags, false);
assert.equal((await guest.next("room_settings")).settings_generation, 1);

// The old gameplay relay envelope is prohibited even for a valid room member.
guest.send({ type: "peer_reliable", recipients: [{ id: hostWelcome.client_id,
  sequence: 1 }], body: { type: "chat", text: "must not forward" } });
assert.equal((await guest.next("error")).error, "unsupported_control_type");
await host.next("peer_left");
assert.equal(host.messages.some(value => value.type === "peer_reliable"), false);
host.close();
guest.close();
console.log("Local Cloudflare Worker integration passed");
