// Translating a validated device report into Dynatrace ingest calls.
//
// The relay exists so the device never has to speak Dynatrace's wire formats.
// Dynatrace accepts OTLP only as HTTP with protobuf — gRPC and OTLP/JSON are
// both unsupported — and a protobuf encoder on an ESP32-S3 is a real chunk of
// work for a device that is already blocking 10-16 s per refresh. Here it is a
// text format and a JSON array.
//
//   Metrics → POST /api/v2/metrics/ingest   (Metrics v2 line protocol, text)
//   Logs    → POST /api/v2/logs/ingest      (JSON array)
//
// Traces are deliberately not here yet: the refresh and per-query timings ride
// in as metrics, which answers "how long does a refresh take, and which query
// dominates" without the protobuf. Spans (step 5 of the issue) add the causal
// nesting on top of the same payload, no protocol change.

const METRIC_PREFIX = 'tinytrace';

// Dimension values in the line protocol would need escaping for spaces, commas
// and quotes. Rather than escape, we assert: the protocol validator only ever
// produces enums, hex IDs and git shas, so anything failing this is a bug in
// the validator, and dropping the line is better than shipping a malformed one.
const SAFE_DIM_RE = /^[A-Za-z0-9_.:+-]+$/;

function dims(pairs) {
  const parts = [];
  for (const [key, value] of Object.entries(pairs)) {
    if (value === undefined || value === null) continue;
    const v = String(value);
    if (!SAFE_DIM_RE.test(v)) continue;
    parts.push(`${key}=${v}`);
  }
  return parts.join(',');
}

// A gauge line. Timestamps are deliberately omitted so Dynatrace stamps at
// ingest: the device's clock comes from SNTP and may not have settled, and a
// device that boots with a 1970 clock would otherwise write points into the far
// past where nobody will ever look at them.
function gauge(key, dimensions, value) {
  return `${METRIC_PREFIX}.${key},${dims(dimensions)} ${value}`;
}

function counter(key, dimensions, delta) {
  return `${METRIC_PREFIX}.${key},${dims(dimensions)} count,delta=${delta}`;
}

// buildMetricLines turns a validated report into Metrics v2 line protocol.
// Exported separately from the sending so it can be asserted on directly in
// tests without a network round trip.
export function buildMetricLines(report) {
  const base = { 'device.id': report.device, fw: report.fw };
  const lines = [];

  for (const [name, value] of Object.entries(report.metrics)) {
    // battery_pct → tinytrace.battery_pct. Metric keys stay as the protocol
    // names them, so a field is greppable from firmware to chart.
    lines.push(gauge(name, base, value));
  }

  if (report.uptime_s !== undefined) lines.push(gauge('uptime_s', base, report.uptime_s));
  if (report.refresh_min !== undefined) {
    lines.push(gauge('refresh_min', base, report.refresh_min));
  }

  if (report.refresh) {
    lines.push(gauge('refresh.duration_ms', base, report.refresh.duration_ms));
    // A counter split by result, rather than a 0/1 gauge: "how often does this
    // device fail to refresh" is the question, and a rate over a counter
    // survives the device being asleep for the gaps in between.
    lines.push(counter('refresh.count', { ...base, result: report.refresh.ok ? 'ok' : 'error' }, 1));
  }

  for (const q of report.queries) {
    const qd = { ...base, query: q.q };
    lines.push(gauge('query.duration_ms', qd, q.duration_ms));
    lines.push(counter('query.count', { ...qd, result: q.ok ? 'ok' : 'error' }, 1));
  }

  for (const e of report.events) {
    lines.push(counter('event.count', { ...base, event: e.e, query: e.q }, 1));
  }

  return lines;
}

// buildLogEvents turns reported error events into Log ingest records. Errors
// only, and every field is an enum or an ID — the log content is assembled from
// the event name, never from anything the device typed.
export function buildLogEvents(report, now = Date.now()) {
  return report.events.map((e) => {
    const rec = {
      timestamp: new Date(now).toISOString(),
      'log.source': 'tinytrace',
      severity: 'ERROR',
      content: `tinytrace ${e.e}`,
      'device.id': report.device,
      'firmware.build': report.fw,
      event: e.e,
    };
    if (e.q !== undefined) rec.query = e.q;
    if (e.status !== undefined) rec['http.status_code'] = e.status;
    return rec;
  });
}

async function post(env, path, contentType, body) {
  const url = `${env.DT_URL.replace(/\/+$/, '')}${path}`;
  const res = await fetch(url, {
    method: 'POST',
    headers: {
      Authorization: `Api-Token ${env.DT_TOKEN}`,
      'Content-Type': contentType,
    },
    body,
    // The device has already been answered by the time this runs, so a slow
    // tenant costs the relay a little CPU time and costs the panel nothing.
    signal: AbortSignal.timeout(10_000),
  });
  if (!res.ok) {
    // Read a bounded slice: enough to identify the failure in `wrangler tail`,
    // not enough for a chatty error body to dominate the log.
    const detail = (await res.text().catch(() => '')).slice(0, 200);
    throw new Error(`${path} -> ${res.status} ${detail}`);
  }
  return res;
}

// forward ships a validated report to Dynatrace. It resolves to a per-signal
// outcome rather than throwing, because a failure here must stay invisible to
// the device: the panel has its answer already, and one signal failing should
// not suppress the other.
export async function forward(env, report, now = Date.now()) {
  const results = { metrics: 'skipped', logs: 'skipped' };

  const lines = buildMetricLines(report);
  if (lines.length) {
    try {
      await post(env, '/api/v2/metrics/ingest', 'text/plain; charset=utf-8', lines.join('\n'));
      results.metrics = 'ok';
    } catch (err) {
      results.metrics = `error: ${err.message}`;
    }
  }

  const events = buildLogEvents(report, now);
  if (events.length) {
    try {
      await post(env, '/api/v2/logs/ingest', 'application/json; charset=utf-8', JSON.stringify(events));
      results.logs = 'ok';
    } catch (err) {
      results.logs = `error: ${err.message}`;
    }
  }

  return results;
}
