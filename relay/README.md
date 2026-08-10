# tinytrace relay

Opt-in device telemetry, relayed to Dynatrace.

```
device ──HTTPS + JSON + fleet key──▶ Cloudflare Worker ──ingest token──▶ Dynatrace
```

This is step 1 of [#11](https://github.com/miigwi/tinytrace/issues/11): the
relay comes first because it *defines the wire protocol*, and it can be
exercised end to end with `curl` before a single board is flashed.

**Nothing on the device side exists yet.** No firmware in this repo sends
anything to this relay, and telemetry is default-off by design — see
[What is left](#what-is-left).

## Why a relay at all

The alternative is baking a Dynatrace ingest token into a public repo, which
publishes a write credential against a tenant. Shipping binaries only does not
help: `strings firmware.bin | grep dt0` finds it in seconds. Ingest is billed,
so abuse costs money, and GitHub secret scanning would likely revoke the token
anyway.

So whatever is baked into the firmware has to be **cheap to revoke and useless
on its own**. The fleet key is exactly that — it grants "may knock on the
relay" and nothing more. It cannot read a tenant, cannot write to one, and
rotating it never touches the ingest token.

The relay also buys the device out of Dynatrace's wire formats. Dynatrace
accepts OTLP only as HTTP with protobuf — gRPC and OTLP/JSON are both
unsupported — and a protobuf encoder on an ESP32-S3 is real work for a device
already blocking 10–16 s per refresh. At the edge it is a text format and a
JSON array.

## The privacy boundary

The device holds things that must never leave it, and the panels carry real
problem titles and log lines out of the user's own tenant. The rule that makes
that safe is structural, not a matter of care at each call site:

> **No free text crosses [`src/protocol.js`](src/protocol.js).**

Every string in an accepted payload is either a fixed enum (query names, event
names) or a fixed-shape identifier (16 hex digits, a git short sha). There is no
field a problem title *could* be put into, so a firmware bug or a carelessly
added future field cannot exfiltrate screen content — the shape rejects it.
Unknown keys are dropped rather than forwarded, for the same reason.

Concretely, the relay refuses:

| | |
|---|---|
| `"events":[{"e":"Failure rate increase on checkout"}]` | not in the event enum |
| `"device":"F4:12:FA:6B:2C:91"` | not 16 hex digits |
| `"fw":"a1b2c3d (checkout is on fire)"` | not a git short sha |
| `{"problem_title":"…"}` | unknown key, dropped |

**Never sent, and unrepresentable in the protocol:** tenant URL, platform token,
WiFi SSID, local IP, and any screen content. Only counts, durations, status
codes.

**The device ID is not derived from the MAC.** Hashing a MAC is not
anonymisation: 48 bits, half of it a fixed vendor OUI, is brute-forceable in
seconds. It is a random 64-bit value generated once on consent and kept in NVS.

## Protocol

### `POST /v1/telemetry`

`Authorization: Bearer <fleet key>`, JSON body, 8 KiB cap.

```json
{
  "v": 1,
  "device": "3f9a2c1b7e4d0568",
  "fw": "a1b2c3d",
  "uptime_s": 86400,
  "refresh_min": 5,
  "metrics": {
    "battery_pct": 87.5,
    "charge_rate_pct_h": -1.2,
    "wifi_rssi_dbm": -62,
    "free_heap_bytes": 141232
  },
  "refresh": {
    "duration_ms": 12480,
    "ok": true,
    "queries": [
      { "q": "problems",       "duration_ms": 3100, "ok": true },
      { "q": "golden.pick",    "duration_ms": 1280, "ok": true },
      { "q": "golden.signals", "duration_ms": 5200, "ok": true },
      { "q": "logs",           "duration_ms": 2900, "ok": false, "status": 429 }
    ]
  },
  "events": [{ "e": "query_failed", "q": "logs", "status": 429 }]
}
```

Only `v`, `device` and `fw` are required. Everything else is optional, so a
device can report what it has.

`q` is one of `problems`, `golden.pick`, `golden.signals`, `logs` — the four
DQL queries a refresh runs. (Three screen builders, but the golden-signals
screen picks the worst service first and then queries it, which is where "four
queries of two round trips each" comes from.)

`e` is one of `query_failed`, `query_timeout`, `wifi_rejoin_failed`,
`tenant_unreachable`, `clock_sync_failed`. Errors only, never payloads.

`fw` is the `TT_BUILD` stamp from
[`firmware/version.py`](../firmware/version.py) — a git short sha, `+` if the
tree was dirty, or `nogit`.

**Responses.** `202 {"ok":true}` accepted · `400` failed validation, with the
offending field named · `401` bad fleet key · `413` over 8 KiB · `429` rate
limited · `503` relay not configured.

Out-of-range numbers are **rejected, not clamped**: a battery at 900 % is a
firmware bug, and clamping it to 100 hides the bug behind a plausible chart.

### `GET /health`

Unauthenticated, reveals no configuration. `200 {"ok":true,"protocol":1}`.

## What reaches Dynatrace

**Metrics** → `/api/v2/metrics/ingest`, Metrics v2 line protocol. Every point
carries `device.id` and `fw` dimensions:

```
tinytrace.battery_pct,device.id=3f9a2c1b7e4d0568,fw=a1b2c3d 87.5
tinytrace.refresh.duration_ms,device.id=3f9a2c1b7e4d0568,fw=a1b2c3d 12480
tinytrace.refresh.count,device.id=3f9a2c1b7e4d0568,fw=a1b2c3d,result=ok count,delta=1
tinytrace.query.duration_ms,device.id=3f9a2c1b7e4d0568,fw=a1b2c3d,query=golden.signals 5200
tinytrace.query.count,device.id=3f9a2c1b7e4d0568,fw=a1b2c3d,query=logs,result=error count,delta=1
tinytrace.event.count,device.id=3f9a2c1b7e4d0568,fw=a1b2c3d,event=query_failed,query=logs count,delta=1
```

Timestamps are deliberately omitted so Dynatrace stamps at ingest — the device
clock comes from SNTP and may not have settled, and a device booting with a 1970
clock would otherwise write points into the far past where nobody will look.

**Logs** → `/api/v2/logs/ingest`, JSON array, errors only. Content is assembled
from the event enum, never from anything the device typed.

**Traces** are not emitted yet. Per-query durations ride in as metrics, which
already answers *how long does a refresh take and which query dominates* —
the most valuable part, given a refresh measured 10–16 s dominated by Grail
computing the query rather than by TLS. Spans add the causal nesting on top of
the same payload and need no protocol change; that is step 5.

## Failure isolation

**Telemetry must never degrade the panel.** The device is answered `202` as soon
as its report validates; forwarding happens afterwards in `waitUntil`. A slow or
dead tenant cannot stretch the refresh window or surface on the panel as "tenant
unreachable". Forward failures are logged to `wrangler tail` and dropped — there
is no retry queue, because a queue eventually costs the device a wake-up.

The two signals are independent: metrics failing does not suppress logs.

## Running it

No dependencies, no install — Node 22 has `Request`/`Response`/`fetch` built in.

```bash
npm test                        # 38 tests, zero dependencies
FLEET_KEY=dev-key npm run serve # serve the Worker over plain HTTP on :8787
```

```console
$ curl -sS localhost:8787/health
{"ok":true,"protocol":1}

$ curl -sS -X POST localhost:8787/v1/telemetry \
    -H 'Authorization: Bearer dev-key' -H 'Content-Type: application/json' \
    -d '{"v":1,"device":"3f9a2c1b7e4d0568","fw":"a1b2c3d","metrics":{"battery_pct":87.5}}'
{"ok":true}

$ curl -sS -X POST localhost:8787/v1/telemetry \
    -H 'Authorization: Bearer dev-key' -H 'Content-Type: application/json' \
    -d '{"v":1,"device":"F4:12:FA:6B:2C:91","fw":"a1b2c3d"}'
{"error":"device: expected 16 lowercase hex digits"}
```

[`dev/serve.js`](dev/serve.js) is a local harness, not the deploy path —
`wrangler dev` runs the real workerd. The harness exists so the protocol can be
poked at with nothing but the tools already on the machine.

## Deploying

```bash
npx wrangler secret put FLEET_KEY   # baked into firmware; revocable, useless alone
npx wrangler secret put DT_TOKEN    # Dynatrace ingest token; never leaves the edge
npx wrangler deploy
```

Set `DT_URL` in [`wrangler.toml`](wrangler.toml) to your tenant. The ingest
token needs `metrics.ingest` and `logs.ingest` — and only those.

Rate limiting is a per-device 10/minute binding. The device reports once per
refresh and the fastest cadence the picker offers is one minute, so that is
roughly an order of magnitude of headroom for a well-behaved device — enough to
absorb a reboot loop reporting on every boot, low enough that a leaked fleet key
cannot run up an ingest bill from one device ID.

## What is left

Steps 2–6 of [#11](https://github.com/miigwi/tinytrace/issues/11), all device-side
or hardware-verified:

2. **Add the Google Trust Services root** to
   [`firmware/src/certs.h`](../firmware/src/certs.h). `workers.dev` is issued by
   GTS; the bundle currently pins Amazon, Starfield, DigiCert and ISRG. Without
   it the handshake fails as a bare `-1` connection error that points nowhere
   near TLS. *(Not done here: it depends on the custom-domain decision below,
   and needs a hardware handshake to verify.)*
3. **`SETTINGS → Usage Data`**, opt-in, default off, ID generated on consent.
4. **Metrics** piggybacked on the existing refresh, so no extra wake-up.
5. **Traces**, then **error logs**.
6. **Measure the added power cost.** A second host means a second TLS
   handshake — guessed at 1–2 s on a 10–16 s refresh, perhaps +1–2 mA at a
   five-minute cadence. That is an estimate, and estimates in this area have a
   poor track record on this project; the existing fuel-gauge logger can settle
   it.

### Open questions, and how this answers them

- **Does the Worker live in this repo?** It does, here in `relay/`, on the
  grounds that the protocol has to stay in step with the firmware and a split
  repo makes that a cross-repo change. Its secrets never enter the repo either
  way — they live in `wrangler secret`. If its lifecycle diverges, moving it out
  later is a directory move.
- **`*.workers.dev` or a custom domain?** Still open, and it decides step 2:
  `workers.dev` means the GTS root, a custom domain means whichever CA issues
  it. Worth settling before the cert work rather than after.
- **Retention and cardinality.** One time series per device per metric does not
  scale forever. `device.id` is the only unbounded dimension; `fw` is bounded by
  releases and `query`/`event`/`result` by their enums.
