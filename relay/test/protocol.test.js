// Validator tests. The interesting half is what gets *rejected*: this is the
// file that decides whether a problem title from someone's tenant can reach a
// third-party relay, and a regression here is silent by nature — telemetry
// would keep flowing and look perfectly healthy.

import test from 'node:test';
import assert from 'node:assert/strict';

import { validate, PROTOCOL_VERSION } from '../src/protocol.js';

const DEVICE = '3f9a2c1b7e4d0568';

function report(overrides = {}) {
  return { v: PROTOCOL_VERSION, device: DEVICE, fw: 'a1b2c3d', ...overrides };
}

function accept(body) {
  const r = validate(body);
  assert.equal(r.ok, true, `expected accept, got: ${r.error}`);
  return r.report;
}

function reject(body, matching) {
  const r = validate(body);
  assert.equal(r.ok, false, 'expected reject, got accept');
  if (matching) assert.match(r.error, matching);
  return r.error;
}

test('a full report round-trips every field', () => {
  const out = accept(
    report({
      uptime_s: 86400,
      refresh_min: 5,
      metrics: {
        battery_pct: 87.5,
        charge_rate_pct_h: -1.2,
        wifi_rssi_dbm: -62,
        free_heap_bytes: 141232,
      },
      refresh: {
        duration_ms: 12480,
        ok: true,
        queries: [
          { q: 'problems', duration_ms: 3100, ok: true },
          { q: 'golden.pick', duration_ms: 1280, ok: true },
          { q: 'golden.signals', duration_ms: 5200, ok: true },
          { q: 'logs', duration_ms: 2900, ok: false, status: 429 },
        ],
      },
      events: [{ e: 'query_failed', q: 'logs', status: 429 }],
    }),
  );

  assert.equal(out.device, DEVICE);
  assert.equal(out.metrics.battery_pct, 87.5);
  assert.equal(out.refresh.duration_ms, 12480);
  assert.equal(out.queries.length, 4);
  assert.deepEqual(out.events, [{ e: 'query_failed', q: 'logs', status: 429 }]);
});

test('a minimal report is accepted, with empty collections', () => {
  const out = accept(report());
  assert.deepEqual(out.metrics, {});
  assert.deepEqual(out.queries, []);
  assert.deepEqual(out.events, []);
  assert.equal(out.refresh, undefined);
});

// --- the privacy boundary -------------------------------------------------

test('unknown keys are dropped, not forwarded', () => {
  const out = accept(
    report({
      problem_title: 'Failure rate increase on checkout',
      metrics: { battery_pct: 50, log_line: 'ERROR user 4711 payment declined' },
    }),
  );
  const serialised = JSON.stringify(out);
  assert.doesNotMatch(serialised, /checkout/);
  assert.doesNotMatch(serialised, /payment declined/);
  assert.equal(out.problem_title, undefined);
  assert.equal(out.metrics.log_line, undefined);
  // ...and the known sibling still came through, so this is a drop and not a
  // wholesale rejection of the object.
  assert.equal(out.metrics.battery_pct, 50);
});

test('query and event names are enums, so no free text rides in on them', () => {
  reject(
    report({ refresh: { duration_ms: 10, ok: true, queries: [{ q: 'Failure rate on checkout', duration_ms: 1, ok: true }] } }),
    /queries\[0\]\.q/,
  );
  reject(report({ events: [{ e: 'ERROR payment declined for user 4711' }] }), /events\[0\]\.e/);
});

test('a MAC-derived device ID does not fit the field', () => {
  // The point of the 16-hex shape is that it has room for a random 64-bit ID
  // and no room for anything MAC-shaped, hashed or not. 48 bits with a fixed
  // vendor OUI in half of them is brute-forceable in seconds, so a hash of it
  // is not anonymisation.
  reject(report({ device: 'F4:12:FA:6B:2C:91' }), /device/);
  reject(report({ device: 'f412fa6b2c91' }), /device/); // 12 hex — too short
  reject(report({ device: DEVICE.toUpperCase() }), /device/); // case is pinned
  reject(report({ device: `${DEVICE}00` }), /device/); // too long
});

test('the firmware stamp cannot be used as a free-text field', () => {
  accept(report({ fw: 'a1b2c3d' }));
  accept(report({ fw: 'a1b2c3d+' })); // dirty tree, per firmware/version.py
  accept(report({ fw: 'nogit' }));
  reject(report({ fw: 'a1b2c3d (checkout is on fire)' }), /fw/);
  reject(report({ fw: '' }), /fw/);
});

// --- shape and range ------------------------------------------------------

test('the protocol version is pinned', () => {
  reject(report({ v: 2 }), /^v:/);
  reject({ device: DEVICE, fw: 'a1b2c3d' }, /^v:/);
});

test('non-objects and null are rejected outright', () => {
  reject(null, /body/);
  reject([], /body/);
  reject('a1b2c3d', /body/);
});

test('out-of-range numbers are rejected rather than clamped', () => {
  // A battery at 900 % is a firmware bug. Clamping it to 100 would hide the bug
  // behind a chart that looks entirely reasonable.
  reject(report({ metrics: { battery_pct: 900 } }), /battery_pct/);
  reject(report({ metrics: { battery_pct: -1 } }), /battery_pct/);
  reject(report({ metrics: { wifi_rssi_dbm: 40 } }), /wifi_rssi_dbm/);
  reject(report({ uptime_s: -5 }), /uptime_s/);
  reject(report({ refresh_min: 0 }), /refresh_min/);
});

test('NaN and Infinity are rejected before they can poison a chart', () => {
  // JSON.parse cannot produce these, but a hand-built object can, and the
  // validator is the last thing between them and a metric value.
  reject(report({ metrics: { battery_pct: NaN } }), /battery_pct/);
  reject(report({ metrics: { battery_pct: Infinity } }), /battery_pct/);
  reject(report({ metrics: { battery_pct: '87.5' } }), /battery_pct/);
});

test('the queries and events arrays are bounded', () => {
  const one = { q: 'problems', duration_ms: 1, ok: true };
  accept(report({ refresh: { duration_ms: 1, ok: true, queries: Array(8).fill(one) } }));
  reject(
    report({ refresh: { duration_ms: 1, ok: true, queries: Array(9).fill(one) } }),
    /at most 8/,
  );
  reject(report({ events: Array(17).fill({ e: 'query_failed' }) }), /at most 16/);
});

test('refresh requires both its fields', () => {
  reject(report({ refresh: { duration_ms: 1 } }), /refresh\.ok/);
  reject(report({ refresh: { ok: true } }), /refresh\.duration_ms/);
  reject(report({ refresh: { duration_ms: 1, ok: 'yes' } }), /refresh\.ok/);
});

test('HTTP status codes are bounded to plausible ones', () => {
  accept(report({ events: [{ e: 'query_failed', status: 599 }] }));
  reject(report({ events: [{ e: 'query_failed', status: 600 }] }), /status/);
});
