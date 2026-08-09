# Tinytrace

**Website: [miigwi.github.io/tinytrace](https://miigwi.github.io/tinytrace/)**

A tiny standalone Dynatrace desk panel: **active problems, golden signals, and
recent logs** on a 1.14" screen you can glance at instead of opening a tab. It
runs on an **Adafruit ESP32-S3 Reverse TFT Feather** and talks **directly** to
your Dynatrace Grail tenant over HTTPS — no companion app, no sidecar. Plug it
into any USB charger and it just works.

```
   ┌────────────────────────────┐
   │● checkout           ▮ 87%  │   header dot + service, battery
   │LAT   128 ms  ▁▂▃▅▇▆  ▲     │
   │TPS   2.4k    ▃▃▄▅▄▅  ►     │   golden signals for the top-worst
   │ERR   3.1 %   ▁▁▂▁▃█  ▲     │   service: LAT p99 · TPS · ERR · 4xx
   │4xx   0.4     ▁▁▁▂▁▁  ►     │
   └────────────────────────────┘
```

## Launcher

One firmware image, a menu on every reset. The three front buttons drive
everything: **D2** up · **D0** down · **D1** select.

| Entry | What it does |
|---|---|
| **TINYTRACE** | the Dynatrace desk panel (live if configured, else demo) |
| **TRACE RUNNER** | a one-button noir endless runner — offline, just for fun |
| **SETTINGS** | → Refresh Rate · Config Portal · Reset Settings · Back |

Reset returns you to the launcher; that's how you leave an app.

## Tinytrace panel

Built on-device from DQL, refreshed every **5 minutes** into RAM (buttons switch
instantly). The cadence is set on the device under **SETTINGS → Refresh Rate** —
no portal, no laptop — because it is what battery life mostly turns on.
In the panel: **D2** previous · **D0** next · **D1** refresh (long-press →
idle/mascot). The onboard **NeoPixel** is the beacon — green / amber / red for
the worst severity on the current screen, blue when the data is stale.

**Battery.** Every screen carries a battery cell and percentage in the header
(top-right of the mascot on the idle screen), read from the onboard **MAX17048
fuel gauge** over I2C — this board has no analog VBAT divider, so the gauge is
the only source. It fills and colours with the charge: teal, amber under 30 %,
red under 15 %, and **blue with a bolt while charging**. With no cell attached
the indicator disappears entirely rather than showing a fake reading, and the
service name reclaims the space.

Three limits, all hardware. The charger's CHG LED isn't wired to a GPIO, so
charging is inferred from the gauge's charge-rate register — a *full* battery on
USB settles to ~0 %/h and reads as "not charging", which is true but isn't the
same as unplugged. With no cell attached the charger drives the BAT pin to
~4.2 V, so "absent" and "full" are genuinely indistinguishable in software. And
that rate register is heavily filtered and reset when the gauge is initialised,
so **the bolt takes a few minutes after boot to appear** — plug in and it will
look uncharging for a while before catching up.

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

## Battery life

Measured on hardware — an ESP32-S3 Reverse TFT Feather on a 1200 mAh LiPo,
associated to WiFi, live against a Grail tenant, panel blanked. Discharge was
read from the onboard fuel gauge over multi-hour runs.

| Refresh cadence | Draw | Runtime |
|---|---|---|
| every 1 min | ~74 mA | ~16 h |
| **every 5 min** (default) | **~34 mA** | **~33 h** |
| every 10 min | ~29 mA | ~41 h |
| never (idle floor) | ~24 mA | ~50 h |

**SETTINGS → Refresh Rate** offers 1 / 2 / 5 / 10 / 15 / 30 min and shows the
estimate for each, so the trade is visible where you make it.

Two things dominate, and both are now set for endurance rather than freshness:

- **How often it queries.** A refresh is four DQL queries of two round trips
  each and blocks for 10–16 s, mostly waiting on Grail rather than on TLS —
  reusing the TLS session was measured and would save almost nothing.
- **CPU clock.** `TT_CPU_MHZ` defaults to **80** (the floor that still runs
  WiFi): ~24 mA idle against ~41 mA at 240. The lower clock does stretch each
  query, which at a one-minute cadence cancelled the saving exactly — at five
  minutes the idle term dominates and it wins clearly.

> A caveat on measuring this yourself: a freshly charged cell sheds surface
> charge for the better part of an hour and the gauge reads that as
> consumption — it inflated our early figures by 3×. Start from ~85 %, not
> 100 %, and discard the first 40 minutes.

Going below the ~24 mA floor needs light sleep, which the Arduino framework
compiles out (`CONFIG_PM_ENABLE` is unset); it would take an ESP-IDF build.

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
`TT_ROTATION` (screen orientation), `TT_TZ` (POSIX timezone for the device
clock — no longer drawn on the panel, but it still sets system local time),
`TT_LONGPRESS_MS`, `TT_SLEEP_MS`, `TT_CPU_MHZ` (80 by default — see
[Battery life](#battery-life)), and `TT_REFRESH_MIN_DEFAULT` (the cadence used
until one is chosen on-device).

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
| ✅ | On hardware: the boot launcher — D2/D0 move, D1 selects — and demo mode |
| ✅ | On hardware: Settings → Config Portal, multi-network/tenant provisioning, WiFi, SNTP |
| ✅ | On hardware: TLS + platform-token auth against a live Grail tenant |
| ✅ | On hardware: live active-problems / golden-signals / last-logs screens |
| ✅ | On hardware: Trace Runner |
| ✅ | On hardware: battery indicator (MAX17048) — level, and charging cross-checked against the CHG LED |
| ✅ | On hardware: battery-life figures above, from multi-hour fuel-gauge runs |
| ✅ | On hardware: SETTINGS → Refresh Rate — picked, persisted to NVS, read back after restart |
| ✅ | On hardware: menu scrolling for lists longer than the panel |

Everything above is verified on the current launcher firmware.

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
