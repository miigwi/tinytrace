// End-to-end tests over the fetch handler, driven with plain Request objects.
// Node 22 has Request/Response/fetch built in, so this needs no Workers
// emulator and no dependencies — `node --test` and nothing else.

import test from 'node:test';
import assert from 'node:assert/strict';

import worker from '../src/index.js';

const DEVICE = '3f9a2c1b7e4d0568';
const FLEET_KEY = 'tt_fleet_0123456789abcdef';

const BODY = {
  v: 1,
  device: DEVICE,
  fw: 'a1b2c3d',
  uptime_s: 3600,
  refresh_min: 5,
  metrics: { battery_pct: 87.5 },
  refresh: { duration_ms: 12480, ok: true, queries: [{ q: 'problems', duration_ms: 3100, ok: true }] },
};

// A ctx that runs waitUntil work eagerly and hands back a promise to await, so
// a test can assert on what was forwarded without sleeping.
function testCtx() {
  const pending = [];
  return {
    ctx: { waitUntil: (p) => pending.push(p) },
    settled: () => Promise.all(pending),
    get count() { return pending.length; },
  };
}

// Metrics ingest answers 202 with a JSON summary; log ingest answers 204 with
// no body.
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

function env(overrides = {}) {
  return {
    FLEET_KEY,
    DT_URL: 'https://abc12345.live.dynatrace.com',
    DT_TOKEN: 'dt0c01.SECRET',
    ...overrides,
  };
}

function post(body, { key = FLEET_KEY, headers = {} } = {}) {
  const h = { 'Content-Type': 'application/json', ...headers };
  if (key !== null) h.Authorization = `Bearer ${key}`;
  return new Request('https://relay.example/v1/telemetry', {
    method: 'POST',
    headers: h,
    body: typeof body === 'string' ? body : JSON.stringify(body),
  });
}

async function send(request, e = env()) {
  const t = testCtx();
  const res = await worker.fetch(request, e, t.ctx);
  await t.settled();
  return { res, body: await res.json(), forwarded: t.count };
}

// --- routing --------------------------------------------------------------

test('health is unauthenticated and leaks no configuration', async () => {
  const res = await worker.fetch(
    new Request('https://relay.example/health'),
    env(),
    testCtx().ctx,
  );
  assert.equal(res.status, 200);
  const body = await res.json();
  assert.deepEqual(body, { ok: true, protocol: 1 });
  // Nothing about the tenant or either secret should be inferable from here.
  assert.doesNotMatch(JSON.stringify(body), /dynatrace|dt0c01|tt_fleet/i);
});

test('unknown paths 404 and the wrong method 405', async () => {
  const notFound = await worker.fetch(new Request('https://relay.example/'), env(), testCtx().ctx);
  assert.equal(notFound.status, 404);

  const wrongMethod = await worker.fetch(
    new Request('https://relay.example/v1/telemetry'),
    env(),
    testCtx().ctx,
  );
  assert.equal(wrongMethod.status, 405);
});

// --- auth -----------------------------------------------------------------

test('a missing, malformed or wrong fleet key is rejected', async () => {
  for (const key of [null, '', 'wrong-key', `${FLEET_KEY}x`, FLEET_KEY.slice(0, -1)]) {
    const { res } = await send(post(BODY, { key }));
    assert.equal(res.status, 401, `key ${JSON.stringify(key)} should not authenticate`);
  }

  const noBearer = await worker.fetch(
    new Request('https://relay.example/v1/telemetry', {
      method: 'POST',
      headers: { Authorization: FLEET_KEY },
      body: JSON.stringify(BODY),
    }),
    env(),
    testCtx().ctx,
  );
  assert.equal(noBearer.status, 401);
});

test('an unconfigured relay refuses rather than falling open', async () => {
  // Accepting everything when FLEET_KEY is unset would look healthy right up
  // until it forwarded a stranger's traffic onto our billed ingest.
  const { res } = await send(post(BODY, { key: 'anything' }), env({ FLEET_KEY: undefined }));
  assert.equal(res.status, 503);
});

test('authentication happens before any forwarding', async () => {
  const stub = stubFetch();
  try {
    const { forwarded } = await send(post(BODY, { key: 'wrong-key' }));
    assert.equal(forwarded, 0);
    assert.equal(stub.calls.length, 0);
  } finally {
    stub.restore();
  }
});

// --- body handling --------------------------------------------------------

test('an accepted report is answered 202 and forwarded', async () => {
  const stub = stubFetch();
  try {
    const { res, body, forwarded } = await send(post(BODY));
    assert.equal(res.status, 202);
    assert.deepEqual(body, { ok: true });
    assert.equal(forwarded, 1);
    assert.equal(stub.calls.length, 1); // metrics only — this report has no events
    assert.match(stub.calls[0].url, /\/api\/v2\/metrics\/ingest$/);
  } finally {
    stub.restore();
  }
});

test('malformed JSON is a 400, not a 500', async () => {
  const { res, body } = await send(post('{"v":1,'));
  assert.equal(res.status, 400);
  assert.match(body.error, /malformed JSON/);
});

test('a rejected report explains itself and is not forwarded', async () => {
  const stub = stubFetch();
  try {
    const { res, body, forwarded } = await send(post({ ...BODY, device: 'nope' }));
    assert.equal(res.status, 400);
    assert.match(body.error, /device/);
    assert.equal(forwarded, 0);
    assert.equal(stub.calls.length, 0);
  } finally {
    stub.restore();
  }
});

test('an oversized body is refused by header and by actual length', async () => {
  const big = 'x'.repeat(9 * 1024);

  const byHeader = await send(post(BODY, { headers: { 'Content-Length': String(9 * 1024) } }));
  assert.equal(byHeader.res.status, 413);

  // Chunked requests arrive with no Content-Length, so the header check is an
  // early out rather than the enforcement.
  const byLength = await send(post(JSON.stringify({ ...BODY, pad: big })));
  assert.equal(byLength.res.status, 413);
});

// --- rate limiting --------------------------------------------------------

test('the rate limiter is keyed on the validated device ID', async () => {
  const seen = [];
  const e = env({ RATE_LIMITER: { limit: async ({ key }) => (seen.push(key), { success: true }) } });
  const stub = stubFetch();
  try {
    await send(post(BODY), e);
    assert.deepEqual(seen, [DEVICE]);
  } finally {
    stub.restore();
  }
});

test('a rate-limited device gets 429 and nothing is forwarded', async () => {
  const e = env({ RATE_LIMITER: { limit: async () => ({ success: false }) } });
  const stub = stubFetch();
  try {
    const { res, forwarded } = await send(post(BODY), e);
    assert.equal(res.status, 429);
    assert.equal(forwarded, 0);
    assert.equal(stub.calls.length, 0);
  } finally {
    stub.restore();
  }
});

test('an invalid report is rejected before it can consume rate-limit budget', async () => {
  let called = false;
  const e = env({ RATE_LIMITER: { limit: async () => ((called = true), { success: true }) } });
  const { res } = await send(post({ ...BODY, device: 'F4:12:FA:6B:2C:91' }), e);
  assert.equal(res.status, 400);
  assert.equal(called, false);
});

test('the relay works without a rate limiter binding', async () => {
  const stub = stubFetch();
  try {
    const { res } = await send(post(BODY), env({ RATE_LIMITER: undefined }));
    assert.equal(res.status, 202);
  } finally {
    stub.restore();
  }
});

// --- failure isolation ----------------------------------------------------

test('a dead Dynatrace tenant still answers the device 202', async () => {
  // Telemetry must never degrade the panel. The device is answered before the
  // forward is attempted, so a dead relay-to-tenant hop cannot stretch the
  // refresh window or surface on the panel as "tenant unreachable".
  const stub = stubFetch(async () => { throw new Error('connection reset'); });
  try {
    const { res, body } = await send(post(BODY));
    assert.equal(res.status, 202);
    assert.deepEqual(body, { ok: true });
  } finally {
    stub.restore();
  }
});

test('the ingest token never appears in a response to the device', async () => {
  const stub = stubFetch(async () => new Response('dt0c01.SECRET rejected', { status: 401 }));
  try {
    for (const req of [post(BODY), post(BODY, { key: 'wrong' }), post('{')]) {
      const res = await worker.fetch(req, env(), testCtx().ctx);
      const text = await res.text();
      assert.doesNotMatch(text, /dt0c01|tt_fleet|dynatrace/i);
    }
  } finally {
    stub.restore();
  }
});
