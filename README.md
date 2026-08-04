# Tinytrace

A tiny standalone Dynatrace desk panel: **active problems, golden signals, and
recent logs** on a 1.14" screen you can glance at instead of opening a tab. It
runs on an **Adafruit ESP32-S3 Reverse TFT Feather** and talks **directly** to
your Dynatrace Grail tenant over HTTPS — no companion app, no sidecar. Plug it
into any USB charger and it just works.

```
   ┌────────────────────────────┐
   │● checkout          22:14   │   header dot + service, local time
   │LAT   128 ms  ▁▂▃▅▇▆  ▲     │
   │TPS   2.4k    ▃▃▄▅▄▅  ►     │   golden signals for the top-worst
   │ERR   3.1 %   ▁▁▂▁▃█  ▲     │   service: LAT p99 · TPS · ERR · 4xx
   │4xx   0.4     ▁▁▁▂▁▁  ►     │
   └────────────────────────────┘
```

## Launcher

One firmware image, a menu on every reset. The three front buttons drive
everything: **D0** up · **D2** down · **D1** select.

| Entry | What it does |
|---|---|
| **TINYTRACE** | the Dynatrace desk panel (live if configured, else demo) |
| **TRACE RUNNER** | a one-button noir endless runner — offline, just for fun |
| **SETTINGS** | → Config Portal · Reset Settings · Back |

Reset returns you to the launcher; that's how you leave an app.

## Tinytrace panel

Built on-device from DQL, refreshed every 60 s into RAM (buttons switch instantly).
In the panel: **D0** previous · **D2** next · **D1** refresh (long-press →
idle/mascot). The onboard **NeoPixel** is the beacon — green / amber / red for
the worst severity on the current screen, blue when the data is stale.

| Screen | Query |
|---|---|
| **active problems** | `fetch dt.davis.problems` — active, deduplicated (24 h), top 8 |
| **golden signals** | top-worst service (by failures/2h): LAT p99 · TPS · ERR rate · 4xx |
| **last logs** | `fetch logs \| filter loglevel in {ERROR, WARN}` (1 h) |
| **idle** | mascot + active-problem count |

The panel blanks after 5 min untouched (any button wakes it); it keeps querying
while asleep so the beacon stays honest.

**Demo mode.** If no WiFi is selected/reachable, or no tenant is configured, the
panel boots **non-connected** and shows a canned "DEMO MODE" set of the four
screens instead of opening a portal. Configure it (below) to go live.

## Trace Runner

A one-button noir endless runner, ported in spirit from
[trace-runner](https://github.com/miigwi/trace-runner): Agent Decoder runs the
graveyard shift and you keep the SLO alive. **D1 = jump, held = higher.** Clear
gaps, steam vents and patrol drones; grab data shards and origami unicorns; a
hit costs SLO (the NeoPixel is the SLO beacon) and 0 % ends the shift with a
rank. Best score persists to NVS. Offline — no WiFi or tenant needed.

## Configuration (WiFi & Dynatrace)

**SETTINGS → Config Portal** brings up a captive portal:

1. Join the open WiFi **`tinytrace-setup`**.
2. Open **`192.168.4.1`** (most phones pop it automatically).
3. Manage **WiFi networks** and **Dynatrace tenants** — each as a list you can
   add to, pick which one is in use, or reset. Then **Apply & Restart**.

Multiple networks and tenants can be stored; the device connects to the selected
pair on boot. Everything lives in NVS and survives reflashes — nothing is baked
into the firmware. **SETTINGS → Reset Settings** wipes all networks and tenants
back to the non-connected state.

Create the token at
[myaccount.dynatrace.com → Platform tokens](https://myaccount.dynatrace.com/platformTokens),
scoped **read-only**:

```
storage:logs:read   storage:metrics:read   storage:events:read   storage:buckets:read
```

## Why standalone (and why all-DQL)

On a Grail tenant, logs are DQL-only — the classic `/api/v2/logs/search` is
deprecated — so the device needs a DQL client no matter what. Given that, **one
read-only platform token + all-DQL** is the least code and makes a true
appliance: no laptop that has to be awake for the thing on your desk to work.

```
  ESP32-S3 Feather ──HTTPS──▶  {env}.apps.dynatrace.com
   read-only token             /platform/storage/query/v1/query:execute → poll
   (in NVS)                     refresh every 60s; buttons switch cached screens
```

## Hardware

| Item | Price | Notes |
|---|---|---|
| **Adafruit ESP32-S3 Reverse TFT Feather** ([#5691](https://www.adafruit.com/product/5691)) | ~€25 | 1.14" 240×135 ST7789, 3 buttons, NeoPixel, USB-C. The only required part. |
| USB-C cable + 5 V supply | — | Any phone charger. |
| LiPo battery (optional) | — | JST-PH. **Mind the polarity** — many third-party cells are wired opposite Adafruit's. |

## Flashing

```bash
cd firmware && pio run -e tinytrace -t upload
```

The S3's native USB can't auto-reset into the bootloader while an app is
crashing, so hold it in the ROM bootloader by hand:

```
hold BOOT → tap RESET → release BOOT, then upload → tap RESET to run the app
```

> ⚠ **Use Adafruit_ST7789, not TFT_eSPI.** TFT_eSPI's `init()` crash-loops on
> this S3 board regardless of pin/clock/MISO config; the vendor driver
> initialises cleanly. (Hard-won — don't switch it back.)

Tunables live as `build_flags` in [platformio.ini](firmware/platformio.ini):
`TT_ROTATION` (screen orientation), `TT_TZ` (POSIX timezone for the on-panel
clock), `TT_LONGPRESS_MS`, and `TT_SLEEP_MS`.

## Security

Tinytrace holds a real, if scoped, secret. It's **read-only, revocable, stored
in NVS, and never in the firmware**. By default TLS validates the tenant against
a **pinned root-CA bundle** ([certs.h](firmware/src/certs.h)) — `setInsecure`
lives only behind the `TT_TLS_INSECURE` bring-up flag. A stolen gadget = a
revocable read-only token you kill from the tokens page.

## What's verified

| | |
|---|---|
| ✅ | Builds (`pio run -e tinytrace`) |
| ✅ | On hardware (original single-app firmware): captive-portal provisioning, WiFi, SNTP |
| ✅ | On hardware: TLS + platform-token auth against a live Grail tenant |
| ✅ | On hardware: live active-problems / golden-signals / last-logs screens |
| ⬜ | Launcher, Trace Runner, multi-network/tenant Settings, and demo mode — compile-clean, **not yet flashed** |

## Layout

```
firmware/
  platformio.ini      env: tinytrace (board adafruit_feather_esp32s3_reversetft)
  src/
    main.cpp          boot launcher: menu → TINYTRACE / TRACE RUNNER / SETTINGS
    app.h             app entry points (tinytraceRun / tracerunnerRun)
    tinytrace_app.*   the panel: connect → live refresh loop, or demo mode
    game_tracerunner.cpp  one-button endless runner (own canvas + sprites)
    demo.*            canned "DEMO MODE" screens for non-connected boots
    config.*          Settings store (WiFi + tenant lists in NVS) + captive portal
    net.*             WiFi + SNTP
    dt.*              DQL client over TLS (execute + poll)
    dt_screens.*      the four DQL screen builders
    render.*          list + golden-signals (sparkline) renderer, marquee
    hal.h / board.cpp buttons → actions, NeoPixel beacon, power rail
    model.h           Screen/Row model
    certs.h           pinned root-CA bundle
```
