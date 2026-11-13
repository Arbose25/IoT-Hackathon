const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const keys = [
  "event_id", "boot_id", "sequence_no", "kind", "captured_at", "clock_quality",
  "temperature_c", "humidity_pct", "dht_ok", "rfid_ok", "flame", "watchdog_ok",
  "uptime_ms", "wifi_rssi_dbm", "free_heap_bytes", "reset_reason", "firmware",
];
const maxBytes = 2048;
const safetyKeys = ["heat_index_c", "safety", "door", "fire_latched", "panic_latched"];
const reply = (status: number, result: string) => Response.json({ result }, { status });
const integer = (v: unknown, min: number, max: number) =>
  typeof v === "number" && Number.isSafeInteger(v) && v >= min && v <= max;
const decimal = (v: unknown, min: number, max: number) =>
  typeof v === "number" && Number.isFinite(v) && v >= min && v <= max;

function valid(p: Record<string, unknown>) {
  if (!p || typeof p !== "object") return false;
  const level3 = p.firmware === "level3";
  const expected = level3 ? [...keys, ...safetyKeys] : keys;
  if (Object.keys(p).length !== expected.length || !expected.every((key) => Object.hasOwn(p, key))) return false;
  if (level3 && (
    !(p.heat_index_c === null || decimal(p.heat_index_c, -20, 160)) ||
    typeof p.safety !== "string" || !["SAFE", "WARNING", "DANGER", "FAULT", "FIRE", "PANIC"].includes(p.safety) ||
    typeof p.fire_latched !== "boolean" || typeof p.panic_latched !== "boolean" ||
    p.door !== (p.fire_latched ? "UNLOCKED" : "LOCKED")
  )) return false;
  return typeof p.event_id === "string" && uuid.test(p.event_id) &&
    typeof p.boot_id === "string" && uuid.test(p.boot_id) &&
    integer(p.sequence_no, 0, 4294967295) &&
    (p.kind === "telemetry" || p.kind === "boot" || (level3 && (
      (p.kind === "fire" && p.fire_latched) || (p.kind === "panic" && p.panic_latched) ||
      (p.kind === "fire_reset" && !p.fire_latched) ||
      (p.kind === "panic_clear" && !p.panic_latched) || p.kind === "reset_denied"
    ))) &&
    (p.clock_quality === "unknown" ? p.captured_at === null :
      p.clock_quality === "synced" && typeof p.captured_at === "string" &&
      /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(p.captured_at) &&
      Number.isFinite(Date.parse(p.captured_at)) && Date.parse(p.captured_at) <= Date.now() + 300000) &&
    ["dht_ok", "rfid_ok", "flame", "watchdog_ok"].every((key) => typeof p[key] === "boolean") &&
    (p.dht_ok ? decimal(p.temperature_c, -40, 125) && decimal(p.humidity_pct, 0, 100) :
      p.temperature_c === null && p.humidity_pct === null) &&
    integer(p.uptime_ms, 0, 4294967295) && integer(p.wifi_rssi_dbm, -127, 0) &&
    integer(p.free_heap_bytes, 0, 16777216) && integer(p.reset_reason, 0, 100) &&
    (level3 || p.firmware === "level2");
}

Deno.serve(async (request: Request) => {
  if (request.method !== "POST") return reply(405, "method_not_allowed");
  const token = request.headers.get("X-Device-Token") ?? "";
  const device = request.headers.get("X-Device-ID") ?? "";
  const room = request.headers.get("X-Room-Code") ?? "";
  if (!/^[a-f0-9]{64}$/i.test(token) || !uuid.test(device) || !/^[a-z0-9_-]{1,32}$/i.test(room)) {
    return reply(401, "unauthorized");
  }
  if (request.headers.get("Content-Type")?.split(";")[0] !== "application/json") {
    return reply(400, "invalid_content_type");
  }
  if (Number(request.headers.get("Content-Length")) > maxBytes) return reply(413, "too_large");
  try {
    const reader = request.body?.getReader();
    if (!reader) return reply(400, "invalid_body");
    const bytes = new Uint8Array(maxBytes);
    let length = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (length + value.length > maxBytes) {
        await reader.cancel();
        return reply(413, "too_large");
      }
      bytes.set(value, length);
      length += value.length;
    }
    let payload;
    try {
      payload = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(0, length)));
    } catch {
      return reply(400, "invalid_json");
    }
    if (!valid(payload)) return reply(400, "invalid_telemetry");

    const hash = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token));
    const digest = Array.from(new Uint8Array(hash), (byte) => byte.toString(16).padStart(2, "0")).join("");
    const key = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
    const url = Deno.env.get("SUPABASE_URL");
    if (!url || !key) return reply(503, "backend_unavailable");
    const response = await fetch(url + "/rest/v1/rpc/classroom_ingest", {
      method: "POST",
      headers: { apikey: key, Authorization: "Bearer " + key, "Content-Type": "application/json" },
      body: JSON.stringify({ p_device: device, p_room: room, p_digest: digest, p: payload }),
      signal: AbortSignal.timeout(2500),
    });
    if (!response.ok) return reply(503, "backend_unavailable");
    const result = await response.json();
    const statuses: Record<string, number> = {
      accepted: 201, duplicate: 200, unauthorized: 401, conflict: 409, limited: 429,
    };
    return reply(statuses[result] ?? 503, result);
  } catch {
    return reply(503, "backend_unavailable");
  }
});
