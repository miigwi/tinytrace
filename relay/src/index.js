// The tinytrace telemetry relay.
//
//   device ──HTTPS + JSON + fleet key──▶ this Worker ──ingest token──▶ Dynatrace
//
// A relay is not optional. The alternative is baking a Dynatrace ingest token
// into a public repo, which publishes a write credential against a tenant.
// Shipping binaries only does not help either — `strings firmware.bin | grep
// dt0` finds it in seconds. Ingest is billed, so abuse costs money, and secret
// scanning would likely revoke the token anyway.
//
// So whatever is baked into the firmware has to be cheap to revoke and useless
// on its own. The fleet key is exactly that: it grants "may knock on the
// relay" and nothing more. It cannot read a tenant, cannot write to one, and
// rotating it never touches the ingest token.
//
// Routes:
//   GET  /health        unauthenticated liveness, reveals no configuration
//   POST /v1/telemetry  the only ingest path; Bearer fleet key

import { validate, PROTOCOL_VERSION } from './protocol.js';
import { forward } from './dynatrace.js';

// A report is a few hundred bytes. The cap is here so an unauthenticated caller
// cannot make us buffer megabytes before we get as far as checking their key.
const MAX_BODY_BYTES = 8 * 1024;

function json(status, body) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json; charset=utf-8' },
  });
}

// Constant-time comparison. The Workers runtime offers
// crypto.subtle.timingSafeEqual, but a few lines of portable JS keeps the
// module runnable under plain `node --test` with no runtime shim — the tests
// are the main thing standing between this and a device we cannot reflash
// easily, so they should not need a Workers emulator to run.
function secretEquals(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string') return false;
  const ab = new TextEncoder().encode(a);
  const bb = new TextEncoder().encode(b);
  // Length is not itself secret enough to be worth hiding, but comparing over
  // the longer of the two keeps the loop count independent of where they first
  // differ, which is the part that matters.
  let diff = ab.length ^ bb.length;
  const n = Math.max(ab.length, bb.length);
  for (let i = 0; i < n; i++) diff |= (ab[i] ?? 0) ^ (bb[i] ?? 0);
  return diff === 0;
}

function bearer(request) {
  const header = request.headers.get('Authorization') || '';
  const match = /^Bearer (.+)$/.exec(header);
  return match ? match[1] : null;
}

async function handleTelemetry(request, env, ctx) {
  if (!env.FLEET_KEY) {
    // Refuse rather than fall open. An unconfigured relay that accepts
    // everything would look healthy right up until it forwarded a stranger's
    // traffic onto our billed ingest.
    console.error('FLEET_KEY is not configured; refusing all telemetry');
    return json(503, { error: 'relay not configured' });
  }
  if (!secretEquals(bearer(request), env.FLEET_KEY)) {
    return json(401, { error: 'unauthorized' });
  }

  const declared = Number(request.headers.get('Content-Length'));
  if (Number.isFinite(declared) && declared > MAX_BODY_BYTES) {
    return json(413, { error: 'payload too large' });
  }

  const raw = await request.text();
  // Chunked requests arrive without a Content-Length, so the header check above
  // is an early out, not the enforcement.
  if (raw.length > MAX_BODY_BYTES) return json(413, { error: 'payload too large' });

  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return json(400, { error: 'malformed JSON' });
  }

  const result = validate(parsed);
  if (!result.ok) return json(400, { error: result.error });
  const report = result.report;

  // Rate limit per device, after validation so the key is a real device ID and
  // not something a caller made up to fan out across buckets. The binding is
  // optional: `wrangler dev` without it should still be usable.
  if (env.RATE_LIMITER) {
    const { success } = await env.RATE_LIMITER.limit({ key: report.device });
    if (!success) return json(429, { error: 'rate limited' });
  }

  // Answer now, forward after. Telemetry must never degrade the panel: the
  // device gets a small 202 as soon as its report is accepted, and a slow or
  // dead Dynatrace tenant cannot stretch the refresh window or surface on the
  // panel as "tenant unreachable". Failures past this point are dropped, with
  // no retry queue — a queue would eventually cost the device a wake-up.
  ctx.waitUntil(
    forward(env, report).then((r) => {
      if (r.metrics.startsWith?.('error') || r.logs.startsWith?.('error')) {
        console.error(`forward ${report.device}: metrics=${r.metrics} logs=${r.logs}`);
      }
    }),
  );

  return json(202, { ok: true });
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (request.method === 'GET' && url.pathname === '/health') {
      return json(200, { ok: true, protocol: PROTOCOL_VERSION });
    }

    if (url.pathname !== '/v1/telemetry') return json(404, { error: 'not found' });
    if (request.method !== 'POST') {
      return json(405, { error: 'method not allowed' });
    }

    try {
      return await handleTelemetry(request, env, ctx);
    } catch (err) {
      console.error(`unhandled: ${err?.stack || err}`);
      return json(500, { error: 'internal error' });
    }
  },
};
