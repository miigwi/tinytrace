(() => {
const THREE_URL = 'https://unpkg.com/three@0.184.0/build/three.module.js';
const GEO_URL = (typeof window !== 'undefined' && window.__resources && window.__resources.geoBin) || 'tinytrace_geo.bin';

// Offline bundles hand us blob URLs; three.module.js's internal './three.core.js'
// specifier can't resolve against a blob, so patch it to the core blob and re-blob.
async function loadThree() {
  const R = (typeof window !== 'undefined' && window.__resources) || {};
  if (!R.threeModule || !R.threeCore) return import(/* @vite-ignore */ THREE_URL);
  const src = await (await fetch(R.threeModule)).text();
  const patched = src.replace(/(['"])\.\/three\.core\.js\1/g, JSON.stringify(R.threeCore));
  const url = URL.createObjectURL(new Blob([patched], { type: 'text/javascript' }));
  return import(/* @vite-ignore */ url);
}

const MAT = {
  shell:      { color: 0xb8b2a8, rough: 0.85, metal: 0.02 },
  strap:      { color: 0xb8b2a8, rough: 0.85, metal: 0.02 },
  io_window:  { color: 0x9fd0e0, rough: 0.25, metal: 0.0, opacity: 0.45 },
  bezel:      { color: 0xb8b2a8, rough: 0.85, metal: 0.02 },
  btn_d0:     { color: 0xd9702f, rough: 0.6,  metal: 0.02 },
  btn_d1:     { color: 0xd9702f, rough: 0.6,  metal: 0.02 },
  btn_d2:     { color: 0xd9702f, rough: 0.6,  metal: 0.02 },
  pcb:        { color: 0x14503a, rough: 0.55, metal: 0.15 },
  jst_plug:   { color: 0xe8e4dc, rough: 0.5,  metal: 0.05 },
  jst_slot:   { color: 0x8d9298, rough: 0.5,  metal: 0.1 },
  lipo:       { color: 0x2c3033, rough: 0.6,  metal: 0.1 },
  lipo_face:  { color: 0xc4c8cc, rough: 0.35, metal: 0.5 },
  lipo_tape:  { color: 0xc39a3e, rough: 0.55, metal: 0.05 },
  hat:        { color: 0xa87c52, rough: 0.9,  metal: 0.0 },
  hat_logo:   { color: 0x24272b, rough: 0.6,  metal: 0.05 }
};

const GROUPS = [
  { id: 'shell',   num: 1, meshes: ['shell', 'io_window'], axis: [0, 0, 0], d: 0, dir: [1, 0.15] },
  { id: 'battery', num: 2, meshes: ['lipo', 'lipo_face', 'lipo_tape'], axis: [0, -0.62, 0.785], d: 44,
    stepAxis: [0, -0.30, 0.954], dir: [0.95, 0.7] },
  { id: 'pcb',     num: 3, meshes: ['pcb', 'jst_plug', 'jst_slot'], axis: [0, -1, 0], d: 43, dir: [-0.45, -1] },
  { id: 'buttons', num: 4, meshes: ['btn_d0', 'btn_d1', 'btn_d2'], axis: [0, -1, 0], d: 47, dir: [-0.75, 0.75] },
  { id: 'bezel',   num: 5, meshes: ['bezel'], axis: [0, -1, 0], d: 64, dir: [-1, 0.15] },
  { id: 'hat',     num: 6, meshes: ['hat', 'hat_logo'], axis: [0, 0, 1], d: 22, dir: [1, -0.32] }
];
const STEP_OFF = { battery: 50, pcb: 30, buttons: 30, bezel: 30, hat: 22 };
// JST lead: pouch top face -> rear of the white plug on the board
const CABLE = [
  { color: 0xa8281d, a: [11.6, 31.3, -4.2], b: [16.5, 23.2, 16.0] },
  { color: 0x191b1d, a: [8.9, 31.3, -4.2], b: [14.9, 23.2, 16.0] }
];

let geoPromise = null;
const loadGeo = async (THREE) => {
  if (!geoPromise) geoPromise = (async () => {
    const buf = await (await fetch(GEO_URL)).arrayBuffer();
    const hlen = new DataView(buf).getUint32(0, true);
    const head = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 4, hlen)).replace(/\0+$/, ''));
    const pBase = 4 + hlen, iBase = pBase + head.posBytes;
    const geos = {};
    for (const p of head.parts) {
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.BufferAttribute(new Float32Array(buf, pBase + p.pOff, p.vCount * 3), 3));
      g.setIndex(new THREE.BufferAttribute(new Uint32Array(buf, iBase + p.iOff, p.tCount * 3), 1));
      g.computeBoundingBox();
      geos[p.name] = g;
    }
    return geos;
  })();
  return geoPromise;
};

class ExplodeStage extends HTMLElement {
  static get observedAttributes() { return ['mode', 'explode', 'step', 'highlight', 'view', 'strap', 'accent']; }

  constructor() {
    super();
    this._state = { mode: 'explode', explode: 1, step: 1, highlight: '', view: 'iso', strap: false, accent: '#d9702f' };
    this._zoomMul = 1;
    this._sph = { theta: 0.62, phi: 1.16 };
    this.attachShadow({ mode: 'open' });
    this.shadowRoot.innerHTML = `
      <style>
        :host { display: block; position: relative; overflow: hidden; width: 100%; height: 100%; }
        canvas { display: block; width: 100%; height: 100%; cursor: grab; }
        canvas.drag { cursor: grabbing; }
        .ov { position: absolute; inset: 0; pointer-events: none; }
        svg { position: absolute; inset: 0; width: 100%; height: 100%; }
        .badge { position: absolute; width: 29px; height: 29px; margin: -14.5px 0 0 -14.5px;
          border-radius: 50%; display: grid; place-items: center; font-family: var(--badge-font, inherit);
          font-size: 14px; font-weight: 600; line-height: 1;
          color: #fff; background: #2f3237; transition: background .18s, transform .18s; }
        .badge.hot { background: var(--acc, #d9702f); transform: scale(1.16); }
        .badge.dim { background: #9d9a94; }
        .hint { display: none; }
      </style>
      <canvas></canvas>
      <div class="ov"><svg></svg></div>
`;
    this._canvas = this.shadowRoot.querySelector('canvas');
    this._svg = this.shadowRoot.querySelector('svg');
    this._ov = this.shadowRoot.querySelector('.ov');
  }

  attributeChangedCallback(n, o, v) {
    if (n === 'explode') this._state.explode = Math.max(0, Math.min(1, parseFloat(v) || 0));
    else if (n === 'step') this._state.step = parseInt(v, 10) || 1;
    else if (n === 'strap') this._state.strap = v === 'true' || v === '' || v === '1';
    else this._state[n] = v == null ? '' : v;
    if (n === 'accent') this.style.setProperty('--acc', this._state.accent);
    if (n === 'view') {
      const p = { iso: [0.62, 1.16], front: [0, 1.5708], right: [1.5708, 1.5708],
                  left: [-1.5708, 1.5708], back: [3.1416, 1.5708], top: [0.0001, 0.2] }[String(this._state.view).split('#')[0]];
      if (p) { this._sph.theta = p[0]; this._sph.phi = p[1]; this._zoomMul = 1; }
    }
    this._dirty = true;
    if (this.renderer && document.hidden) { this._frame(true); this._frame(true); }
  }
  set mode(v) { this.setAttribute('mode', v); }
  set explode(v) { this.setAttribute('explode', v); }
  set step(v) { this.setAttribute('step', v); }
  set highlight(v) { this.setAttribute('highlight', v || ''); }
  set view(v) { this.setAttribute('view', v); }
  set strap(v) { this.setAttribute('strap', v ? 'true' : 'false'); }
  set accent(v) { this.setAttribute('accent', v); }

  async connectedCallback() {
    if (this._booted) return;
    this._booted = true;
    const THREE = await loadThree();
    this.THREE = THREE;
    const geos = await loadGeo(THREE);
    this._build(THREE, geos);
    this._loop();
  }

  _build(THREE, geos) {
    const scene = new THREE.Scene();
    scene.background = null;
    this.scene = scene;

    const pivot = new THREE.Group();
    const rot = new THREE.Group();
    rot.rotation.x = -Math.PI / 2;   // model Z-up -> three Y-up; model -Y (front) -> three +Z
    pivot.add(rot);
    scene.add(pivot);
    this.pivot = pivot; this.rot = rot;

    this.meshes = {};
    for (const name in MAT) {
      if (!geos[name]) continue;
      const m = MAT[name];
      const mat = new THREE.MeshStandardMaterial({
        color: m.color, roughness: m.rough, metalness: m.metal, flatShading: true,
        transparent: true, opacity: m.opacity != null ? m.opacity : 1,
        depthWrite: m.opacity == null
      });
      mat.userData.base = { color: new THREE.Color(m.color), opacity: m.opacity != null ? m.opacity : 1 };
      const mesh = new THREE.Mesh(geos[name], mat);
      mesh.name = name;
      rot.add(mesh);
      this.meshes[name] = mesh;
    }
    this.cable = new THREE.Group();
    this.cable.userData.pickable = false;
    for (const c of CABLE) {
      const mat = new THREE.MeshStandardMaterial({ color: c.color, roughness: 0.5, metalness: 0.02 });
      const m = new THREE.Mesh(new THREE.BufferGeometry(), mat);
      m.name = 'lead'; m.userData.pickable = false; m.userData.spec = c;
      this.cable.add(m);
    }
    rot.add(this.cable);

    // insertion arrow
    const acc = new THREE.MeshBasicMaterial({ color: 0xd9702f });
    this.arrowMat = acc;
    const shaft = new THREE.Mesh(new THREE.CylinderGeometry(0.9, 0.9, 12, 16), acc);
    shaft.position.y = 6;
    const tip = new THREE.Mesh(new THREE.ConeGeometry(2.4, 5.5, 20), acc);
    tip.position.y = 14.5;
    const arrow = new THREE.Group();
    arrow.add(shaft); arrow.add(tip);
    arrow.visible = false;
    scene.add(arrow);
    this.arrow = arrow;

    scene.add(new THREE.HemisphereLight(0xffffff, 0x9a958c, 1.5));
    const k = new THREE.DirectionalLight(0xffffff, 2.1); k.position.set(0.7, 1.15, 0.9); scene.add(k);
    const f = new THREE.DirectionalLight(0xffffff, 0.75); f.position.set(-0.9, 0.35, -0.55); scene.add(f);
    const r = new THREE.DirectionalLight(0xfff4e6, 0.55); r.position.set(0.1, -0.6, -0.9); scene.add(r);

    this.cam = new THREE.PerspectiveCamera(31, 1, 1, 4000);
    this.renderer = new THREE.WebGLRenderer({ canvas: this._canvas, antialias: true, alpha: true, preserveDrawingBuffer: true });
    this.renderer.setPixelRatio(Math.min(devicePixelRatio, 2));

    this._dist = 320; this._distT = 320;
    this._ctr = new THREE.Vector3(); this._ctrT = new THREE.Vector3();
    this._badges = {};
    this._lines = {};
    this._ray = new THREE.Raycaster();
    this._term = {};
    for (const g of GROUPS) {
      const b = document.createElement('div');
      b.className = 'badge'; b.textContent = g.num;
      this._ov.appendChild(b); this._badges[g.id] = b;
      const ln = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      ln.setAttribute('fill', 'none'); ln.setAttribute('stroke', '#2f3237');
      ln.setAttribute('stroke-width', '1'); ln.setAttribute('stroke-dasharray', '3 3');
      this._svg.appendChild(ln); this._lines[g.id] = ln;
    }
    this._bindInput();
    new ResizeObserver(() => { this._resize(); }).observe(this);
    this._resize();
    this._dirty = true;
    this._frame(true);
  }

  _bindInput() {
    const c = this._canvas;
    let down = false, px = 0, py = 0;
    c.addEventListener('pointerdown', e => { down = true; px = e.clientX; py = e.clientY; c.classList.add('drag'); c.setPointerCapture(e.pointerId); });
    c.addEventListener('pointermove', e => {
      if (!down) return;
      this._sph.theta -= (e.clientX - px) * 0.0072;
      this._sph.phi = Math.max(0.22, Math.min(Math.PI - 0.22, this._sph.phi - (e.clientY - py) * 0.0062));
      px = e.clientX; py = e.clientY; this._dirty = true;
    });
    const up = () => { down = false; c.classList.remove('drag'); };
    c.addEventListener('pointerup', up); c.addEventListener('pointercancel', up);
    c.addEventListener('wheel', e => {
      e.preventDefault();
      this._zoomMul = Math.max(0.35, Math.min(2.6, this._zoomMul * (1 + e.deltaY * 0.0011)));
      this._dirty = true;
    }, { passive: false });
  }

  _resize() {
    if (!this.renderer) return;
    const w = this.clientWidth || 800, h = this.clientHeight || 600;
    this.renderer.setSize(w, h, false);
    this.cam.aspect = w / h; this.cam.updateProjectionMatrix();
    this._dirty = true;
  }

  _targets() {
    const s = this._state, out = {};
    const stepMode = s.mode === 'steps';
    for (const g of GROUPS) {
      let off = 0, vis = true;
      if (stepMode) {
        if (g.num > s.step) vis = false;
        else if (g.num === s.step) off = STEP_OFF[g.id] || 0;
      } else {
        off = g.d * s.explode;
      }
      out[g.id] = { off, vis, ax: (stepMode && g.stepAxis) ? g.stepAxis : g.axis };
    }
    return out;
  }

  _loop() {
    const tick = () => { this._raf = requestAnimationFrame(tick); this._frame(false); };
    tick();
  }

  _frame(snap) {
    {
      const THREE = this.THREE;
      const s = this._state, t = this._targets();
      const K = snap ? 1 : 0.16;
      const lp = (a, b) => a + (b - a) * K;

      // per-group offsets, lerped
      this._off = this._off || {};
      let moving = false;
      for (const g of GROUPS) {
        const cur = this._off[g.id] == null ? t[g.id].off : this._off[g.id];
        const nxt = lp(cur, t[g.id].off);
        if (Math.abs(nxt - t[g.id].off) > 0.01) moving = true;
        this._off[g.id] = nxt;
        const ax = t[g.id].ax;
        for (const n of g.meshes) {
          const m = this.meshes[n]; if (!m) continue;
          m.position.set(ax[0] * nxt, ax[1] * nxt, ax[2] * nxt);
          m.visible = t[g.id].vis;
        }
      }
      if (this.meshes.strap) {
        this.meshes.strap.visible = s.strap && t.shell.vis;
        this.meshes.strap.position.copy(this.meshes.shell.position);
      }

      // JST lead follows both the pouch and the board
      const bo = this.meshes.lipo.position, jo = this.meshes.jst_plug.position;
      const cvis = t.battery.vis && t.pcb.vis;
      this.cable.visible = cvis;
      const sig = [bo.y, bo.z, jo.y, jo.z].map(n => Math.round(n * 4)).join(',');
      if (cvis && sig !== this._cableSig) {
        this._cableSig = sig;
        for (const m of this.cable.children) {
          const sp = m.userData.spec;
          const A = new THREE.Vector3(sp.a[0] + bo.x, sp.a[1] + bo.y, sp.a[2] + bo.z);
          const B = new THREE.Vector3(sp.b[0] + jo.x, sp.b[1] + jo.y, sp.b[2] + jo.z);
          // As the pouch and board separate, bow the lead outboard past the board's
          // right edge so it stays readable; seated (bow 0) it routes inside the shell.
          const bow = 21 * Math.min(1, new THREE.Vector3().subVectors(bo, jo).length() / 22);
          const curve = new THREE.CatmullRomCurve3([
            A,
            A.clone().add(new THREE.Vector3(0.5 + bow * 0.75, 0.2, 5.5)),
            B.clone().add(new THREE.Vector3(0.5 + bow, 1.0, 3.9)),
            B], false, 'catmullrom', 0.5);
          m.geometry.dispose();
          m.geometry = new THREE.TubeGeometry(curve, 40, 0.62, 8, false);
        }
      }

      // highlight / dim
      const hot = s.highlight || (s.mode === 'steps' ? (GROUPS.find(g => g.num === s.step) || {}).id : '');
      for (const g of GROUPS) {
        const isHot = hot && g.id === hot;
        const dim = hot && !isHot;
        for (const n of g.meshes.concat(g.id === 'shell' ? ['strap'] : [])) {
          const m = this.meshes[n]; if (!m) continue;
          const base = m.material.userData.base;
          m.material.opacity = lp(m.material.opacity, dim ? base.opacity * 0.28 : base.opacity);
          m.material.depthWrite = m.material.opacity > 0.92;
          const target = isHot ? new THREE.Color(base.color).lerp(new THREE.Color(s.accent), 0.34) : base.color;
          m.material.color.lerp(target, K);
        }
      }
      this.arrowMat.color.set(s.accent);

      // arrow in step mode
      const cg = GROUPS.find(g => g.num === s.step);
      if (s.mode === 'steps' && cg && cg.d) {
        const box = new THREE.Box3();
        for (const n of cg.meshes) { const m = this.meshes[n]; if (m && m.visible) box.expandByObject(m); }
        if (!box.isEmpty()) {
          const c = box.getCenter(new THREE.Vector3());
          const cga = t[cg.id].ax;
          const dir = new THREE.Vector3(cga[0], cga[1], cga[2]).applyQuaternion(this.rot.quaternion).negate();
          const upOff = cg.id === 'hat' ? new THREE.Vector3(box.max.x - c.x + 12, 0, 0) : new THREE.Vector3(0, box.max.y - c.y + 9, 0);
          this.arrow.position.copy(c.add(upOff)).sub(dir.clone().multiplyScalar(20));
          this.arrow.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), dir);
          this.arrow.visible = true;
        }
      } else this.arrow.visible = false;

      // fit camera to visible content
      const vb = new THREE.Box3();
      for (const g of GROUPS) for (const n of g.meshes) { const m = this.meshes[n]; if (m && m.visible) vb.expandByObject(m); }
      if (this.meshes.strap && this.meshes.strap.visible) vb.expandByObject(this.meshes.strap);
      if (!vb.isEmpty()) {
        this._ctrT.copy(vb.getCenter(new THREE.Vector3()));
        const { theta, phi } = this._sph;
        const u = new THREE.Vector3(Math.sin(phi) * Math.sin(theta), Math.cos(phi), Math.sin(phi) * Math.cos(theta));
        const rt = new THREE.Vector3().crossVectors(new THREE.Vector3(0, 1, 0), u).normalize();
        const up = new THREE.Vector3().crossVectors(u, rt).normalize();
        const tanH = Math.tan(this.cam.fov * Math.PI / 360);
        const asp = this.cam.aspect;
        let need = 0;
        const c0 = this._ctrT;
        for (let i = 0; i < 8; i++) {
          const p = new THREE.Vector3(
            i & 1 ? vb.max.x : vb.min.x,
            i & 2 ? vb.max.y : vb.min.y,
            i & 4 ? vb.max.z : vb.min.z).sub(c0);
          const a = p.dot(rt), b = p.dot(up), c = p.dot(u);
          need = Math.max(need, c + Math.abs(a) / (tanH * asp), c + Math.abs(b) / tanH);
        }
        this._distT = need * 1.1;
      }
      this._ctr.lerp(this._ctrT, snap ? 1 : 0.13);
      this._dist = lp(this._dist, this._distT);
      const d = this._dist * this._zoomMul;
      const { theta, phi } = this._sph;
      this.cam.position.set(
        this._ctr.x + d * Math.sin(phi) * Math.sin(theta),
        this._ctr.y + d * Math.cos(phi),
        this._ctr.z + d * Math.sin(phi) * Math.cos(theta));
      this.cam.lookAt(this._ctr);

      this.renderer.render(this.scene, this.cam);
      this._syncBadges(t, hot);
    }
  }

  // March from the badge toward the part's projected centre and return the first
  // world point whose FIRST scene intersection belongs to this group — guarantees
  // the leader lands on a visible pixel of its own part (handles hollow frames and occlusion).
  _pickSurface(g, bx, by, ax, ay, w, h, x0, x1, y0, y1) {
    const THREE = this.THREE;
    const solids = this.rot.children.filter(m => m.visible && m.userData.pickable !== false);
    const nd = new THREE.Vector2();
    const test = (px, py) => {
      if (px < 1 || py < 1 || px > w - 1 || py > h - 1) return null;
      nd.set((px / w) * 2 - 1, -((py / h) * 2 - 1));
      this._ray.setFromCamera(nd, this.cam);
      const hits = this._ray.intersectObjects(solids, false);
      return hits.length && g.meshes.indexOf(hits[0].object.name) >= 0 ? hits[0].point.clone() : null;
    };
    for (let i = 0; i <= 12; i++) {
      const k = 0.18 + (1.22 - 0.18) * (i / 12);
      const p = test(bx + (ax - bx) * k, by + (ay - by) * k);
      if (p) return p;
    }
    // fallback: scan the part's projected footprint, nearest the badge first
    const cand = [];
    for (let iy = 1; iy <= 7; iy++) for (let ix = 1; ix <= 7; ix++) {
      const px = x0 + (x1 - x0) * (ix / 8), py = y0 + (y1 - y0) * (iy / 8);
      cand.push([Math.hypot(px - bx, py - by), px, py]);
    }
    cand.sort((a, b) => a[0] - b[0]);
    for (const c of cand) { const p = test(c[1], c[2]); if (p) return p; }
    return null;
  }

  _syncBadges(t, hot) {
    const THREE = this.THREE;
    const w = this.clientWidth, h = this.clientHeight;
    this._pick = (this._fc = (this._fc || 0) + 1) % 5 === 1;
    for (const g of GROUPS) {
      const b = this._badges[g.id], ln = this._lines[g.id];
      if (!t[g.id].vis || this._state.mode === 'steps') { b.style.display = 'none'; ln.setAttribute('d', ''); continue; }
      const box = new THREE.Box3();
      for (const n of g.meshes) { const m = this.meshes[n]; if (m && m.visible) box.expandByObject(m); }
      if (box.isEmpty()) { b.style.display = 'none'; ln.setAttribute('d', ''); continue; }
      let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
      for (let i = 0; i < 8; i++) {
        const q = new THREE.Vector3(i & 1 ? box.max.x : box.min.x, i & 2 ? box.max.y : box.min.y,
          i & 4 ? box.max.z : box.min.z).project(this.cam);
        const px = (q.x * 0.5 + 0.5) * w, py = (-q.y * 0.5 + 0.5) * h;
        x0 = Math.min(x0, px); x1 = Math.max(x1, px); y0 = Math.min(y0, py); y1 = Math.max(y1, py);
      }
      const ax = (x0 + x1) / 2, ay = (y0 + y1) / 2;
      const hw = (x1 - x0) / 2, hh = (y1 - y0) / 2;
      const dvx = g.dir[0], dvy = g.dir[1], pad = 24;
      let tt = Math.max(
        Math.abs(dvx) > 0.01 ? (hw + pad) / Math.abs(dvx) : 0,
        Math.abs(dvy) > 0.01 ? (hh + pad) / Math.abs(dvy) : 0);
      tt = Math.min(tt, 200);
      const bxp = Math.max(22, Math.min(w - 22, ax + dvx * tt));
      const byp = Math.max(22, Math.min(h - 22, ay + dvy * tt));
      b.style.display = 'grid';
      b.style.left = bxp + 'px'; b.style.top = byp + 'px';
      b.classList.toggle('hot', hot === g.id);
      b.classList.toggle('dim', !!hot && hot !== g.id);
      if (this._pick) this._term[g.id] = this._pickSurface(g, bxp, byp, ax, ay, w, h, x0, x1, y0, y1);
      const tp = this._term[g.id];
      if (!tp) { ln.setAttribute('d', ''); continue; }
      const cp = tp.clone().project(this.cam);
      const ex = (cp.x * 0.5 + 0.5) * w, ey = (-cp.y * 0.5 + 0.5) * h;
      const dx = ex - bxp, dy = ey - byp, len = Math.hypot(dx, dy);
      if (len < 22) { ln.setAttribute('d', ''); continue; }
      const gap = 17;
      const sx = bxp + dx / len * gap, sy = byp + dy / len * gap;
      ln.setAttribute('d', `M${sx.toFixed(1)},${sy.toFixed(1)} L${ex.toFixed(1)},${ey.toFixed(1)}`);
      ln.setAttribute('stroke', hot === g.id ? this._state.accent : '#2f3237');
      ln.setAttribute('opacity', hot && hot !== g.id ? '0.25' : '0.65');
    }
  }

  disconnectedCallback() { cancelAnimationFrame(this._raf); }
}
if (!customElements.get('explode-stage')) customElements.define('explode-stage', ExplodeStage);
})();
