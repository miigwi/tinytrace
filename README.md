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

Three front buttons instead of touch: **D0** previous · **D2** next · **D1**
refresh (long-press → idle/mascot). The onboard **NeoPixel** is the beacon —
green / amber / red for the worst severity on the current screen, blue when the
data is stale. Glance at the desk: is the light red?

## Screens

Built on-device from DQL, refreshed every 60 s into RAM (buttons switch instantly).

| Screen | Query |
|---|---|
| **active problems** | `fetch dt.davis.problems` — active, deduplicated (24 h), top 8 |
| **golden signals** | top-worst service (by failures/2h): LAT p99 · TPS · ERR rate · 4xx |
| **last logs** | `fetch logs \| filter loglevel in {ERROR, WARN}` (1 h) |
| **idle** | mascot + active-problem count |

The panel blanks after 5 min untouched (any button wakes it); it keeps querying
while asleep so the beacon stays honest.

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

## Provisioning

First boot (or hold **D1** at reset) starts a captive portal:

1. Join the open WiFi **`dynaglance-setup`**.
2. Open **`192.168.4.1`** (most phones pop it automatically).
3. Enter WiFi, the tenant URL (`https://<env>.apps.dynatrace.com`), and the token.

It's stored in NVS and survives reflashes. Nothing is baked into the firmware.

Create the token at
[myaccount.dynatrace.com → Platform tokens](https://myaccount.dynatrace.com/platformTokens),
scoped **read-only**:

```
storage:logs:read   storage:metrics:read   storage:events:read   storage:buckets:read
```

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
`DG_ROTATION` (screen orientation), `DG_TZ` (POSIX timezone for the on-panel
clock), `DG_LONGPRESS_MS`, and `DG_SLEEP_MS`.

## Security

Tinytrace holds a real, if scoped, secret. It's **read-only, revocable, stored
in NVS, and never in the firmware**. By default TLS validates the tenant against
a **pinned root-CA bundle** ([certs.h](firmware/src/certs.h)) — `setInsecure`
lives only behind the `DG_TLS_INSECURE` bring-up flag. A stolen gadget = a
revocable read-only token you kill from the tokens page.

## What's verified

| | |
|---|---|
| ✅ | Builds (`pio run -e tinytrace`) |
| ✅ | On hardware: captive-portal provisioning, WiFi, SNTP |
| ✅ | On hardware: TLS + platform-token auth against a live Grail tenant |
| ✅ | On hardware: live active-problems / golden-signals / last-logs screens |
| ⬜ | Golden-signals 4xx query + marquee + local-TZ refinements — compile-clean, pending a confirmation flash |

## Layout

```
firmware/
  platformio.ini    env: tinytrace (board adafruit_feather_esp32s3_reversetft)
  src/
    main.cpp        app loop: provision → connect → refresh loop
    config.*        NVS store + captive-portal provisioning
    net.*           WiFi + SNTP
    dt.*            DQL client over TLS (execute + poll)
    dt_screens.*    the four DQL screen builders
    render.*        list + golden-signals (sparkline) renderer, marquee
    hal.h / board.cpp   buttons → actions, NeoPixel beacon, power rail
    model.h         Screen/Row model
    certs.h         pinned root-CA bundle
```
