// Tests for the Dynatrace translation. These assert on the exact wire text,
// because the failure mode of a metrics line is not an exception — Dynatrace
// accepts the request and quietly drops the malformed lines, so a bug here
// shows up as a chart that is simply empty.

import test from 'node:test';
import assert from 'node:assert/strict';

import { buildMetricLines, buildLogEvents, forward } from '../src/dynatrace.js';

const DEVICE = '3f9a2c1b7e4d0568';

const REPORT = {
  v: 1,
  device: DEVICE,
  fw: 'a1b2c3d',
  uptime_s: 86400,
  refresh_min: 5,
  metrics: { battery_pct: 87.5, wifi_rssi_dbm: -62 },
  refresh: { duration_ms: 12480, ok: true },
  queries: [
    { q: 'problems', duration_ms: 3100, ok: true },
    { q: 'logs', duration_ms: 2900, ok: false, status: 429 },
  ],
  events: [{ e: 'query_failed', q: 'logs', status: 429 }],
};

const EMPTY = { v: 1, device: DEVICE, fw: 'a1b2c3d', metrics: {}, queries: [], events: [] };

test('metric lines carry the device dimensions and the right payload type', () => {
  const lines = buildMetricLines(REPORT);

  assert.ok(lines.includes(`tinytrace.battery_pct,device.id=${DEVICE},fw=a1b2c3d 87.5`));
  assert.ok(lines.includes(`tinytrace.wifi_rssi_dbm,device.id=${DEVICE},fw=a1b2c3d -62`));
  assert.ok(lines.includes(`tinytrace.uptime_s,device.id=${DEVICE},fw=a1b2c3d 86400`));
  assert.ok(lines.includes(`tinytrace.refresh.duration_ms,device.id=${DEVICE},fw=a1b2c3d 12480`));
  assert.ok(
    lines.includes(`tinytrace.refresh.count,device.id=${DEVICE},fw=a1b2c3d,result=ok count,delta=1`),
  );
});

test('per-query timings are split by query name and result', () => {
  const lines = buildMetricLines(REPORT);
  const base = `device.id=${DEVICE},fw=a1b2c3d`;

  assert.ok(lines.includes(`tinytrace.query.duration_ms,${base},query=problems 3100`));
  assert.ok(lines.includes(`tinytrace.query.count,${base},query=problems,result=ok count,delta=1`));
  assert.ok(lines.includes(`tinytrace.query.duration_ms,${base},query=logs 2900`));
  assert.ok(lines.includes(`tinytrace.query.count,${base},query=logs,result=error count,delta=1`));
});

test('an absent optional dimension is omitted, not written as undefined', () => {
  const lines = buildMetricLines({ ...EMPTY, events: [{ e: 'wifi_rejoin_failed' }] });
  assert.deepEqual(lines, [
    `tinytrace.event.count,device.id=${DEVICE},fw=a1b2c3d,event=wifi_rejoin_failed count,delta=1`,
  ]);
});

test('no line ever contains a space inside its dimensions', () => {
  // A space terminates the dimension list in the line protocol, so an unescaped
  // one silently truncates the metric. The validator guarantees the inputs, and
  // this asserts the guarantee holds all the way to the wire.
  for (const line of buildMetricLines(REPORT)) {
    const [head] = line.split(' ');
    assert.doesNotMatch(head, /[",]\s/);
    assert.match(head, /^[A-Za-z0-9_.:+=,-]+$/);
  }
});

test('a report with nothing to say produces no lines and no logs', () => {
  assert.deepEqual(buildMetricLines(EMPTY), []);
  assert.deepEqual(buildLogEvents(EMPTY), []);
});

test('log events are built from the enum, never from device text', () => {
  const [rec] = buildLogEvents(REPORT, Date.parse('2026-08-10T12:00:00Z'));
  assert.deepEqual(rec, {
    timestamp: '2026-08-10T12:00:00.000Z',
    'log.source': 'tinytrace',
    severity: 'ERROR',
    content: 'tinytrace query_failed',
    'device.id': DEVICE,
    'firmware.build': 'a1b2c3d',
    event: 'query_failed',
    query: 'logs',
    'http.status_code': 429,
  });
});

// --- forwarding -----------------------------------------------------------

// Metrics ingest answers 202 with a JSON summary of accepted/rejected lines;
// log ingest answers 204 with no body.
const ingestOk = (url) =>
  url.endsWith('/metrics/ingest')
    ? new Response(JSON.stringify({ linesOk: 1, linesInvalid: 0 }), { status: 202 })
    : new Response(null, { status: 204 });

function stubFetch(handler = async (url) => ingestOk(url)) {
  const calls = [];
  const original = globalThis.fetch;
  globalThis.fetch = async (url, init) => {
    calls.push({ url, init });
    return handler(url, init);
  };
  return { calls, restore: () => { globalThis.fetch = original; } };
}

const ENV = { DT_URL: 'https://abc12345.live.dynatrace.com', DT_TOKEN: 'dt0c01.SECRET' };

test('forward posts metrics and logs with the ingest token', async () => {
  const stub = stubFetch();
  try {
    const result = await forward(ENV, REPORT);
    assert.deepEqual(result, { metrics: 'ok', logs: 'ok' });

    const [metrics, logs] = stub.calls;
    assert.equal(metrics.url, 'https://abc12345.live.dynatrace.com/api/v2/metrics/ingest');
    assert.equal(metrics.init.headers.Authorization, 'Api-Token dt0c01.SECRET');
    assert.match(metrics.init.headers['Content-Type'], /^text\/plain/);
    assert.match(metrics.init.body, /^tinytrace\./);

    assert.equal(logs.url, 'https://abc12345.live.dynatrace.com/api/v2/logs/ingest');
    assert.equal(JSON.parse(logs.init.body)[0].event, 'query_failed');
  } finally {
    stub.restore();
  }
});

test('a trailing slash on DT_URL does not produce a double slash', async () => {
  const stub = stubFetch();
  try {
    await forward({ ...ENV, DT_URL: 'https://abc12345.live.dynatrace.com/' }, REPORT);
    assert.equal(stub.calls[0].url, 'https://abc12345.live.dynatrace.com/api/v2/metrics/ingest');
  } finally {
    stub.restore();
  }
});

test('one signal failing does not suppress the other', async () => {
  const stub = stubFetch(async (url) =>
    url.endsWith('/metrics/ingest')
      ? new Response('bad metric line', { status: 400 })
      : ingestOk(url),
  );
  try {
    const result = await forward(ENV, REPORT);
    assert.match(result.metrics, /^error: .*400/);
    assert.equal(result.logs, 'ok');
  } finally {
    stub.restore();
  }
});

test('a thrown transport error is captured, not propagated', async () => {
  // forward() resolves rather than rejects: a dead tenant must never become an
  // exception on a path the device is waiting behind.
  const stub = stubFetch(async () => { throw new Error('connection reset'); });
  try {
    const result = await forward(ENV, REPORT);
    assert.match(result.metrics, /connection reset/);
    assert.match(result.logs, /connection reset/);
  } finally {
    stub.restore();
  }
});
