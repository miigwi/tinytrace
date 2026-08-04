# Interactive assembly guide — source

The interactive build guide embedded on the site (and openable standalone at
[`index.html`](index.html)). This is the **Claude Design** project source, kept
here so the guide is versioned and rebuildable — not a one-off bundle.

| File | What it is |
|---|---|
| `index.html` | the DC component source (`<x-dc>` markup + an inline `DCLogic` class) |
| `support.js` | the Claude Design runtime that hydrates the component in the browser |
| `explode-stage.js` | the three.js 3D stage (exploded / step-by-step, orbit, callouts) |
| `tinytrace_geo.bin` | the enclosure + parts geometry the stage renders |

**Runtime dependency:** the stage imports **three.js r184 from unpkg** at load
(`https://unpkg.com/three@0.184.0/…`), so the guide needs network access to
render the 3D view. The rest (geometry, UI) is served from this folder.
