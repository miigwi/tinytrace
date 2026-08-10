// The device→relay wire protocol, v1, and the validator that enforces it.
//
// This file is the privacy boundary. The device holds things that must never
// leave it — tenant URL, platform token, WiFi SSID, local IP — and the panels
// carry real problem titles and log lines out of the user's own tenant. The
// rule that makes that safe is structural rather than a matter of care at the
// call site:
//
//   NO FREE TEXT CROSSES THIS FILE.
//
// Every string in the accepted payload is either a fixed enum (query names,
// event names) or a fixed-shape identifier (16 hex digits, a git short sha).
// There is no field a problem title could be put into, so a firmware bug or a
// careless future field cannot exfiltrate screen content — the shape rejects
// it. Unknown keys are dropped rather than forwarded, for the same reason.
//
// Numbers are range-checked too, not because a rogue number leaks anything but
// because they become metric values and a NaN or an Infinity poisons a chart.

export const PROTOCOL_VERSION = 1;

// The four DQL queries a refresh runs — three screen builders, but the golden
// signals screen picks the worst service first and then queries it, which is
// where "four queries of two round trips each" comes from. Names are part of
// the wire protocol: they become a metric dimension, so adding one here and in
// the firmware is a deliberate, reviewable act.
export const QUERY_NAMES = ['problems', 'golden.pick', 'golden.signals', 'logs'];

// Error events the device may report. Errors only — never payloads. Each is a
// condition the fleet view is meant to answer questions about.
export const EVENT_NAMES = [
  'query_failed',      // a DQL query returned non-200 or failed to parse
  'query_timeout',     // the poll loop gave up
  'wifi_rejoin_failed',// the association did not come back after light sleep
  'tenant_unreachable',// TLS/connect failed before any HTTP status
  'clock_sync_failed', // SNTP did not settle
];

// A device ID is a random 64-bit value generated once, on consent, and kept in
// NVS. It is deliberately NOT derived from the MAC: hashing a MAC is not
// anonymisation, since 48 bits with a fixed vendor OUI in half of them is
// brute-forceable in seconds. 16 lowercase hex digits, nothing else.
const DEVICE_ID_RE = /^[0-9a-f]{16}$/;

// The firmware stamp is the git short sha the image was built from, with a
// trailing "+" when the working tree was dirty, or the literal "nogit". Pinning
// the shape here means the one field that looks like free text cannot be used
// as one. Keep in sync with firmware/version.py.
const FW_RE = /^(nogit|[0-9a-f]{7,40}\+?)$/;

// Numeric fields: [min, max]. Anything outside is dropped rather than clamped —
// a battery at 900 % is a firmware bug, and silently clamping it to 100 would
// hide the bug behind a plausible chart.
const METRIC_RANGES = {
  battery_pct: [0, 100],
  charge_rate_pct_h: [-200, 200],
  wifi_rssi_dbm: [-120, 0],
  free_heap_bytes: [0, 8 * 1024 * 1024],
};

const MAX_QUERIES = 8;   // a refresh runs 4; leave room without allowing a flood
const MAX_EVENTS = 16;
const MAX_DURATION_MS = 10 * 60 * 1000;
const MAX_UPTIME_S = 10 * 365 * 24 * 3600;

// Refresh cadences the picker offers, in minutes. Anything else is a bug.
const REFRESH_MIN_RANGE = [1, 60];

class Reject extends Error {}

function fail(msg) {
  throw new Reject(msg);
}

function num(value, field, [min, max]) {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    fail(`${field}: expected a finite number`);
  }
  if (value < min || value > max) fail(`${field}: ${value} outside [${min}, ${max}]`);
  return value;
}

function int(value, field, range) {
  const n = num(value, field, range);
  if (!Number.isInteger(n)) fail(`${field}: expected an integer`);
  return n;
}

function bool(value, field) {
  if (typeof value !== 'boolean') fail(`${field}: expected a boolean`);
  return value;
}

function enumerated(value, field, allowed) {
  if (typeof value !== 'string' || !allowed.includes(value)) {
    fail(`${field}: expected one of ${allowed.join(', ')}`);
  }
  return value;
}

function object(value, field) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    fail(`${field}: expected an object`);
  }
  return value;
}

// validate takes a parsed JSON body and returns a clean, fully-typed report
// built key by key from scratch. It never returns any part of the input object
// itself, so nothing can ride along in a key the validator did not look at.
//
// Returns { ok: true, report } or { ok: false, error }.
export function validate(body) {
  try {
    const b = object(body, 'body');

    if (b.v !== PROTOCOL_VERSION) fail(`v: expected ${PROTOCOL_VERSION}`);

    if (typeof b.device !== 'string' || !DEVICE_ID_RE.test(b.device)) {
      fail('device: expected 16 lowercase hex digits');
    }
    if (typeof b.fw !== 'string' || !FW_RE.test(b.fw)) {
      fail('fw: expected a git short sha, optionally suffixed "+", or "nogit"');
    }

    const report = { v: PROTOCOL_VERSION, device: b.device, fw: b.fw, metrics: {}, queries: [], events: [] };

    if (b.uptime_s !== undefined) report.uptime_s = int(b.uptime_s, 'uptime_s', [0, MAX_UPTIME_S]);
    if (b.refresh_min !== undefined) {
      report.refresh_min = int(b.refresh_min, 'refresh_min', REFRESH_MIN_RANGE);
    }

    if (b.metrics !== undefined) {
      const m = object(b.metrics, 'metrics');
      // Iterate the allowlist, not the input: a key we do not know about is
      // simply never read, so it cannot reach Dynatrace by any path.
      for (const [name, range] of Object.entries(METRIC_RANGES)) {
        if (m[name] === undefined) continue;
        report.metrics[name] = num(m[name], `metrics.${name}`, range);
      }
    }

    if (b.refresh !== undefined) {
      const r = object(b.refresh, 'refresh');
      report.refresh = {
        duration_ms: int(r.duration_ms, 'refresh.duration_ms', [0, MAX_DURATION_MS]),
        ok: bool(r.ok, 'refresh.ok'),
      };
      if (r.queries !== undefined) {
        if (!Array.isArray(r.queries)) fail('refresh.queries: expected an array');
        if (r.queries.length > MAX_QUERIES) fail(`refresh.queries: at most ${MAX_QUERIES}`);
        report.queries = r.queries.map((raw, i) => {
          const q = object(raw, `refresh.queries[${i}]`);
          const out = {
            q: enumerated(q.q, `refresh.queries[${i}].q`, QUERY_NAMES),
            duration_ms: int(q.duration_ms, `refresh.queries[${i}].duration_ms`, [0, MAX_DURATION_MS]),
            ok: bool(q.ok, `refresh.queries[${i}].ok`),
          };
          if (q.status !== undefined) {
            out.status = int(q.status, `refresh.queries[${i}].status`, [0, 599]);
          }
          return out;
        });
      }
    }

    if (b.events !== undefined) {
      if (!Array.isArray(b.events)) fail('events: expected an array');
      if (b.events.length > MAX_EVENTS) fail(`events: at most ${MAX_EVENTS}`);
      report.events = b.events.map((raw, i) => {
        const e = object(raw, `events[${i}]`);
        const out = { e: enumerated(e.e, `events[${i}].e`, EVENT_NAMES) };
        if (e.q !== undefined) out.q = enumerated(e.q, `events[${i}].q`, QUERY_NAMES);
        if (e.status !== undefined) out.status = int(e.status, `events[${i}].status`, [0, 599]);
        return out;
      });
    }

    return { ok: true, report };
  } catch (err) {
    if (err instanceof Reject) return { ok: false, error: err.message };
    throw err;
  }
}
