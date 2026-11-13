const assert = require("node:assert/strict");
const { createHash, randomUUID } = require("node:crypto");
const { readFileSync } = require("node:fs");
const { stripTypeScriptTypes } = require("node:module");
const vm = require("node:vm");

let handler;
let outcome = "accepted";
let calls = 0;
const token = "a".repeat(64);
const device = randomUUID();
const payload = {
  event_id: randomUUID(), boot_id: randomUUID(), sequence_no: 1,
  kind: "telemetry", captured_at: null, clock_quality: "unknown",
  temperature_c: 26, humidity_pct: 55, dht_ok: true, rfid_ok: true,
  flame: false, watchdog_ok: true, uptime_ms: 5000, wifi_rssi_dbm: -50,
  free_heap_bytes: 100000, reset_reason: 1, firmware: "level2",
};
vm.runInNewContext(stripTypeScriptTypes(readFileSync(
  "supabase/functions/classroom-ingest/index.ts", "utf8")), {
  Deno: { serve: (fn) => { handler = fn; }, env: { get: () => "test" } },
  Response, TextEncoder, TextDecoder, crypto: globalThis.crypto, AbortSignal,
  fetch: async (_, request) => {
    calls++;
    const args = JSON.parse(request.body);
    assert.equal(args.p_device, device);
    assert.equal(args.p_room, "A-101");
    assert.equal(args.p_digest, createHash("sha256").update(token).digest("hex"));
    assert.equal(args.p.event_id, payload.event_id);
    assert.ok(!request.body.includes(token));
    return Response.json(outcome);
  },
});

function post(body, secret = token) {
  return handler(new Request("https://example.test", {
    method: "POST", headers: {
      "content-type": "application/json", "x-device-token": secret,
      "x-device-id": device, "x-room-code": "A-101",
    }, body,
  }));
}

async function run() {
  assert.equal((await post(JSON.stringify(payload), "")).status, 401);
  assert.equal((await post(JSON.stringify({ ...payload, humidity_pct: 101 }))).status, 400);
  assert.equal((await post("x".repeat(3000))).status, 413);
  assert.equal(calls, 0);
  for (const [result, status] of Object.entries({
    accepted: 201, duplicate: 200, unauthorized: 401, conflict: 409, limited: 429,
  })) {
    outcome = result;
    const response = await post(JSON.stringify(payload));
    assert.equal(response.status, status);
    assert.deepEqual(await response.json(), { result });
  }
  assert.equal(calls, 5);
  outcome = "accepted";
  const fire = { ...payload, firmware: "level3", kind: "fire", heat_index_c: null,
    safety: "FIRE", door: "UNLOCKED", fire_latched: true, panic_latched: false };
  assert.equal((await post(JSON.stringify(fire))).status, 201);
  assert.equal((await post(JSON.stringify({ ...fire, door: "LOCKED" }))).status, 400);
  assert.equal((await post(JSON.stringify({ ...fire, kind: "fire_reset" }))).status, 400);
  assert.equal(calls, 6);
  console.log("Edge handler validation and acknowledgment tests passed (mock RPC).");
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
