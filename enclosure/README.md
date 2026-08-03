# Tinytrace enclosure — v35

A magnetic-mount case for the **Adafruit ESP32-S3 Reverse TFT Feather**, plus a
pack of snap-on magnetic hats. Authored as a **Bambu Studio** project
([`Tinytrace_v35_print_plate.3mf`](Tinytrace_v35_print_plate.3mf)) — 8 parts
laid out across 6 print plates, ready to slice.

![overview](renders/overview.png)

## Exploded — core enclosure

The main body, the front bezel, and the three button caps:

![exploded core](renders/00_exploded_core.png)

## Parts

| Part | Plate | Notes |
|---|---|---|
| **Main body — magnetic mount** | 1 | body + transparent IO window; tree-organic supports |
| **Front bezel** | 1 | faceplate over the 1.14" display |
| **3D button caps** ×3 | 1 | D0 / D1 / D2 |
| **Main body — no-magnet variant** | 2 | plain body without the mount magnets |
| **Baseball cap** | 3 | magnetic accessory + Dynatrace logo |
| **Bobble beanie** | 4 | magnetic accessory + Dynatrace logo |
| **Loose beanie** | 5 | magnetic accessory + Dynatrace logo |
| **Cowboy hat** | 6 | magnetic accessory + Dynatrace logo |

Each hat conceals a magnet and clips onto the mount body; the logo and accents
are separate-colour parts printed in the same job.

## Printing

Sliced for a **Bambu Lab H2C**, 0.4 mm nozzle, from the bundled project:

| | |
|---|---|
| Layer height | 0.16 mm |
| Walls / infill | 2 walls · 15 % sparse |
| Supports | tree (organic), auto, on build plate only — main enclosure |
| Bed | textured plate |
| Material | PLA (Matte / Basic), **PLA Translucent** for the IO window, PLA Wood accent, **TPU 90A** |
| Colour | multi-material; plates carry filament-change **pauses** (e.g. "PAUSE 286", "PAUSE 9") for the logos and accents |

Plate names in the project spell out the swaps. Open the `.3mf` in Bambu Studio
/ OrcaSlicer to inspect per-plate filament maps and pause layers.

## Renders

[`renders/`](renders/) holds:

- `overview.png` — labelled contact sheet of all six plates.
- `00_exploded_core.png` — generated from the meshes (support-free, colour-coded).
- `01…06_*.png` — the slicer's own 512×512 plate previews, extracted from the `.3mf`.

> The `.3mf` is a print-plate project (parts laid flat for printing), so it
> carries no authored assembled/exploded transforms. The exploded image is a
> composed parts-separated view of the core enclosure; the plate previews show
> each plate as it prints.
