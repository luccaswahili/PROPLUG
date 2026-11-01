// Komodo panel for Premiere Pro (UXP). Drives the Komodo effect (match name "PROPLUGS LookMatch", kept so
// projects made with LookMatch still open): reference matching, live grade and texture sliders, and a
// library of saved looks. All Premiere access goes through lib/bridge.js.

const ppro = require("premierepro");
const uxp = require("uxp");
const fs = require("fs");
const os = require("os");
const { P, SOURCE_FORMATS, TEXTURE_DEFAULTS, MATCH_DEFAULTS, guessSourceFormat, sourceFormatReason } = require("./lib/params.js");
const { Library, joinPath, baseName } = require("./lib/library.js");
const { Bridge } = require("./lib/bridge.js");
const { Live } = require("./lib/live.js");
const { keyStatus, expiryDate } = require("./lib/license.js");

const VERSION = "1.0.0";  // kept equal to manifest.json by build_panel.sh
// The data folder keeps its LookMatch name: references, looks and matches made before the rename stay.
const DATA_DIR = os.platform() === "win32"
  ? joinPath(process.env.APPDATA || "", "PRO PLUGS\\LookMatch")
  : joinPath(os.homedir(), "Library/Application Support/PRO PLUGS/LookMatch");
const NO_CLIPS = "No clips selected. Select a clip on the timeline, then click Match.";
const LICENSE_FILE = joinPath(DATA_DIR, "license.key");

const $ = (id) => document.getElementById(id);
const library = new Library(fs, DATA_DIR);
const bridge = new Bridge(ppro, fs, DATA_DIR);
let selectedRefs = [];   // the one still the look is read from (a click selects it; no multi-select)

// ---------- status & console ----------

function status(text, kind) {
  $("statusText").textContent = text || "";
  $("status").className = "status" + (kind ? " " + kind : "");
}

// The Probe box under Advanced, mirrored to <data>/panel.log (kept under ~200 KB) for diagnostics.
const PANEL_LOG = joinPath(DATA_DIR, "panel.log");
let logQueue = Promise.resolve();
function log(...a) {
  const line = `${new Date().toLocaleTimeString()} ${a.join(" ")}`;
  const el = $("console");
  el.textContent = (line + "\n" + el.textContent).slice(0, 20000);
  console.log(line);
  logQueue = logQueue.then(async () => {
    let old = "";
    try { old = await fs.readFile(PANEL_LOG, { encoding: "utf-8" }); } catch (e) { /* new file */ }
    if (old.length > 200000) old = old.slice(-100000);
    await fs.writeFile(PANEL_LOG, old + line + "\n", { encoding: "utf-8" });
  }).catch(() => {});
}

// Runs a panel action with a busy/ok/error status line; errors never escape to Premiere.
async function run(label, fn) {
  status(`${label}…`, "busy");
  try {
    const msg = await fn();
    status(msg || `${label}: done`);
    log(`${label}: ${msg || "done"}`);
  } catch (e) {
    status(e.message, "err");
    log(`${label} failed: ${e.message}`);
  }
}

// ---------- tabs, folds, switches ----------

// ---------- licensing ----------
// The panel decodes the key to show status and writes it to disk; the plugin verifies the signature and
// enforces the lock. When there's no valid key the gate covers the panel and the effect defaces the frame.

async function readLicenseKey() {
  try { return String(await fs.readFile(LICENSE_FILE, { encoding: "utf-8" })); } catch (e) { return ""; }
}

async function refreshLicense() {
  const st = keyStatus(await readLicenseKey());
  const chip = $("licChip");
  if (st.activated) {
    chip.textContent = `Licensed · ${expiryDate(st.exp)}`;
    chip.className = "licchip ok";
    $("licGate").hidden = true;
  } else if (st.present && !st.formatOk) {
    // A key file exists but the panel couldn't read it. The plugin, not the panel, enforces the licence,
    // so never block a paying user over a panel-side decode issue — trust the effect and just note it.
    log(`license: key present but panel couldn't parse it (${st.error}); trusting the plugin's own check`);
    chip.textContent = "License present";
    chip.className = "licchip";
    $("licGate").hidden = true;
  } else {
    chip.textContent = st.expired ? "License expired" : "Not activated";
    chip.className = "licchip " + (st.expired ? "warn" : "err");
    openLicenseGate(st);
  }
  return st;
}

function openLicenseGate(st) {
  const gate = $("licGate"), msg = $("licMsg");
  if (st && st.expired) {
    msg.textContent = `Your license expired on ${expiryDate(st.exp)}. Enter a new key to keep using Komodo.`;
  } else if (st && st.present && !st.formatOk) {
    msg.textContent = "That key wasn't recognised. Paste the full key from your purchase email.";
  } else {
    msg.textContent = "Enter the license key from your purchase email to use Komodo.";
  }
  msg.className = "licmsg";
  gate.hidden = false;
  try { $("licInput").focus(); } catch (e) { /* ignore */ }
}

async function activateLicense() {
  const key = String($("licInput").value || "").trim();
  const msg = $("licMsg");
  if (!key) { msg.textContent = "Paste your license key first."; msg.className = "licmsg err"; return; }
  const st = keyStatus(key);
  if (!st.formatOk) {
    log(`license: pasted key rejected (${st.error}); ${key.length} chars`);
    msg.textContent = "That doesn't look like a valid key. Copy the whole key from your email."; msg.className = "licmsg err"; return;
  }
  if (st.expired) { msg.textContent = `That key expired on ${expiryDate(st.exp)}. Please use a current key.`; msg.className = "licmsg err"; return; }
  try {
    await fs.mkdir(DATA_DIR, { recursive: true });
  } catch (e) { /* exists */ }
  await fs.writeFile(LICENSE_FILE, key, { encoding: "utf-8" });
  log(`License activated (until ${expiryDate(st.exp)})`);
  $("licInput").value = "";
  await refreshLicense();
  status(`Komodo activated — licensed until ${expiryDate(st.exp)}.`);
}

$("licActivate").addEventListener("click", () => { activateLicense().catch((e) => log(`activate failed: ${e.message}`)); });
$("licChip").addEventListener("click", () => refreshLicense().then((st) => { if (!st.activated) openLicenseGate(st); }));

function showTab(name) {
  for (const t of document.querySelectorAll("[data-tab]")) t.classList.toggle("on", t.dataset.tab === name);
  for (const s of document.querySelectorAll("section[data-panel]")) s.hidden = s.dataset.panel !== name;
}
$("tabs").addEventListener("click", (ev) => {
  const tab = ev.target.closest ? ev.target.closest("[data-tab]") : ev.target;
  if (tab && tab.dataset.tab) showTab(tab.dataset.tab);
});
for (const f of document.querySelectorAll(".fold")) {
  f.querySelector(".fold-head").addEventListener("click", () => {
    f.classList.toggle("open");
    f.querySelector(".chev").textContent = f.classList.contains("open") ? "⌄" : "›";
  });
}

for (const sw of document.querySelectorAll(".sw")) sw.innerHTML = '<div class="k"></div>';
const isOn = (id) => $(id).classList.contains("on");
const setOn = (id, on) => $(id).classList.toggle("on", !!on);

// ---------- sliders ----------
// Every slider is live: moving it writes the value to Komodo on the selected clips (coalesced, see
// lib/live.js). Double-click puts it back to its default.

const live = new Live((params) => bridge.applyParams("selection", params), {
  interval: 300,
  onError: (e) => status(/Select a video clip/.test(e.message) ? "No clips selected. Select a clip on the timeline to adjust it." : e.message, "err"),
});
const sliders = new Map();  // param name -> { set(v), get(), def }

function fmt(v, unit) {
  if (unit === "%") return `${Math.round(v)}%`;
  if (unit === "±") return v > 0 ? `+${Math.round(v)}` : `${Math.round(v)}`;
  if (unit === "ev") return (v > 0 ? "+" : "") + (Math.round(v * 100) / 100).toFixed(2);
  return (Math.round(v * 100) / 100).toString();
}

// A control row: icon, bold title, an ⓘ button (shows the explanation), slider, value. `mini` rows (inside
// the folds) have no icon. The explanation is also the row's hover text.
function makeSlider({ param, title, desc, icon, min = 0, max = 100, step = 1, unit = "%", def, mini, onChange }) {
  const el = document.createElement("div");
  el.className = mini ? "ctl mini" : "ctl";
  el.innerHTML = `<div class="ctl-line">` + (mini ? "" : `<img class="ico" src="icons/${icon}.svg" alt="">`) +
    `<span class="ctl-title"></span>` + (desc ? `<span class="info" title="What does this do?">i</span>` : "") +
    `<div class="slider"><div class="track"></div><div class="fill"></div><div class="thumb"></div></div>` +
    `<span class="ctl-val"></span></div>` + (desc ? `<div class="ctl-desc"></div>` : "");
  el.querySelector(".ctl-title").textContent = title;
  if (desc) {
    el.title = desc;
    el.querySelector(".ctl-desc").textContent = desc;
    el.querySelector(".info").addEventListener("click", (ev) => { ev.stopPropagation(); el.classList.toggle("show-info"); });
  }
  const bar = el.querySelector(".slider"), fill = el.querySelector(".fill"), thumb = el.querySelector(".thumb");
  const val = el.querySelector(".ctl-val");
  let value = def;
  const draw = () => {
    const pct = ((value - min) / (max - min)) * 100;
    // a slider that goes both ways (-100 .. +100) fills from its centre
    const zero = min < 0 && max > 0 ? ((0 - min) / (max - min)) * 100 : 0;
    fill.style.left = `${Math.min(pct, zero)}%`;
    fill.style.width = `${Math.abs(pct - zero)}%`;
    thumb.style.left = `${pct}%`;
    val.textContent = fmt(value, unit);
    // grey at its default, blue once moved
    el.classList.toggle("moved", Math.abs(value - def) > 1e-9);
  };
  const set = (v, send) => {
    v = Math.min(max, Math.max(min, Math.round(Number(v) / step) * step));
    if (!Number.isFinite(v)) return;
    const changed = v !== value;
    value = v;
    draw();
    if (send && changed) {
      live.set(param, v);
      if (onChange) onChange(v);
    }
  };
  const fromX = (x) => {
    const r = bar.getBoundingClientRect();
    set(min + ((x - r.left) / Math.max(1, r.width)) * (max - min), true);
  };
  let dragging = false;
  bar.addEventListener("mousedown", (ev) => { dragging = true; fromX(ev.clientX); ev.preventDefault(); });
  document.addEventListener("mousemove", (ev) => { if (dragging) fromX(ev.clientX); });
  document.addEventListener("mouseup", () => { dragging = false; });
  bar.addEventListener("dblclick", () => set(def, true));
  draw();
  sliders.set(param, { set: (v) => set(v, false), get: () => value, def });
  return el;
}

const GRADE = [
  { param: P.lookStrength, title: "Look Strength", icon: "look", def: 100, desc: "How much of the reference's look goes on." },
  { param: P.finish, title: "Finish Band", icon: "finish", def: 100, desc: "Lands the match at the levels of a finished grade: open highlights, firm blacks." },
  { param: P.skinEvening, title: "Skin Evening", icon: "skin", def: 50, desc: "Evens patchy skin tone toward the skin around it. No smoothing." },
];
// Tone and colour adjustments after the match: instant (no rebuild), 0 = untouched.
const ADJUST = [
  { param: P.adjExposure, title: "Exposure", min: -4, max: 4, step: 0.01, unit: "ev", def: 0,
    desc: "Brightens or darkens the whole picture, in stops. Highlights roll into white instead of clipping." },
  { param: P.adjContrast, title: "Contrast", min: -100, max: 100, unit: "±", def: 0,
    desc: "Spreads or flattens the tones around mid grey. Black and white stay put; colours keep their hue." },
  { param: P.adjShadows, title: "Shadows", min: -100, max: 100, unit: "±", def: 0,
    desc: "Lifts or deepens the dark half of the picture without moving black." },
  { param: P.adjHighlights, title: "Highlights", min: -100, max: 100, unit: "±", def: 0,
    desc: "Brings back or brightens the bright half of the picture without moving white." },
  { param: P.adjWhites, title: "Whites", min: -100, max: 100, unit: "±", def: 0,
    desc: "Moves the white point: up for brighter whites, down for softer, creamier whites." },
  { param: P.adjSaturation, title: "Saturation", min: -100, max: 100, unit: "±", def: 0,
    desc: "More or less colour everywhere. -50 and below is black and white; +100 is triple the colour." },
  { param: P.adjTemperature, title: "Temperature", min: -100, max: 100, unit: "±", def: 0,
    desc: "Warmer (right) or cooler (left), keeping brightness the same." },
];
const updateAdvSub = () => {
  $("advSub").textContent = `Regions ${Math.round(sliders.get(P.regionMatching).get())}%`;
};
const ADVANCED = [
  { param: P.regionMatching, title: "Region matching", def: 100, onChange: updateAdvSub },
];
const TEXTURE = [
  { param: P.grain, title: "Grain", icon: "grain", desc: "Film grain over the picture, finer in the highlights.", max: 200, def: TEXTURE_DEFAULTS[P.grain] },
  { param: P.halation, title: "Halation", icon: "halation", desc: "Red-orange glow around bright highlights, as light scatters in film.", max: 200, def: TEXTURE_DEFAULTS[P.halation] },
  { param: P.bloom, title: "Bloom", icon: "bloom", desc: "Soft white glow that spreads from the brightest areas.", max: 200, def: TEXTURE_DEFAULTS[P.bloom] },
  { param: P.vignette, title: "Vignette", icon: "vignette", desc: "Darkens the corners of the frame.", def: TEXTURE_DEFAULTS[P.vignette] },
];
const TEXTURE_MORE = [
  { param: P.fade, title: "Fade" },
  { param: P.softness, title: "Softness" },
  { param: P.gateWeave, title: "Gate weave", max: 4, step: 0.05, unit: "" },
  { param: P.flicker, title: "Flicker" },
  { param: P.grainSize, title: "Grain size", min: 0.5, max: 4, step: 0.05, unit: "" },
  { param: P.grainColour, title: "Grain colour" },
  { param: P.halationSize, title: "Halation size", min: 0.25, max: 3, step: 0.05, unit: "" },
  { param: P.halationThreshold, title: "Halation threshold", min: 0.1, max: 2, step: 0.05, unit: "" },
  { param: P.bloomSize, title: "Bloom size", min: 0.25, max: 3, step: 0.05, unit: "" },
  { param: P.bloomThreshold, title: "Bloom threshold", min: 0.1, max: 2, step: 0.05, unit: "" },
].map((c) => Object.assign({ def: TEXTURE_DEFAULTS[c.param] }, c));

for (const c of GRADE) $("gradeControls").appendChild(makeSlider(c));
for (const c of ADJUST) $("adjustControls").appendChild(makeSlider(Object.assign({ mini: true }, c)));
for (const c of ADVANCED) $("advancedControls").appendChild(makeSlider(Object.assign({ mini: true }, c)));
for (const c of TEXTURE) $("textureControls").appendChild(makeSlider(c));
for (const c of TEXTURE_MORE) $("textureMore").appendChild(makeSlider(Object.assign({ mini: true }, c)));
updateAdvSub();

// switches that write a checkbox live
function liveSwitch(id, param, after) {
  $(id).addEventListener("click", (ev) => {
    ev.stopPropagation();
    setOn(id, !isOn(id));
    live.set(param, isOn(id));
    if (after) after(isOn(id));
  });
}
liveSwitch("autoSkinSw", P.autoSkin);
liveSwitch("bandSw", P.fixBanding);
// Before / after: one button for the effect's split view (original left of the line); blue while on.
$("beforeAfter").addEventListener("click", async () => {
  const { items } = await bridge.selectedVideoItems().catch(() => ({ items: [] }));
  if (!items.length) { status("No clips selected. Select a clip on the timeline, then click Before / after.", "err"); return; }
  const on = !$("beforeAfter").classList.contains("on");
  $("beforeAfter").classList.toggle("on", on);
  live.set(P.split, on);
  if (on) { live.set(P.splitPos, 50); live.set(P.splitAngle, 0); }
});

// Source Format picker: 1 = Auto, then the effect's popup order (applies live too)
SOURCE_FORMATS.forEach((name, i) => {
  const item = document.createElement("sp-menu-item");
  item.textContent = name;
  item.value = String(i + 1);
  $("sourceMenu").appendChild(item);
});
$("sourceFormat").selectedIndex = 0;
const sourceFormat = () => Math.max(1, ($("sourceFormat").selectedIndex | 0) + 1);
$("sourceFormat").addEventListener("change", () => live.set(P.sourceFormat, sourceFormat()));

// ---------- reference strip ----------

// Pixel size from the file header (PNG, JPEG, BMP), so the tile can crop the still itself: Premiere's
// panel engine ignores object-fit.
function imageSize(b) {
  if (b[0] === 0x89 && b[1] === 0x50) return { w: (b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19]) >>> 0, h: (b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23]) >>> 0 };
  if (b[0] === 0x42 && b[1] === 0x4d) return { w: Math.abs(b[18] | b[19] << 8 | b[20] << 16 | b[21] << 24), h: Math.abs(b[22] | b[23] << 8 | b[24] << 16 | b[25] << 24) };
  if (b[0] === 0xff && b[1] === 0xd8) {
    for (let i = 2; i + 9 < b.length;) {
      if (b[i] !== 0xff) { i++; continue; }
      const m = b[i + 1];
      if (m >= 0xc0 && m <= 0xcf && m !== 0xc4 && m !== 0xc8 && m !== 0xcc) return { w: b[i + 7] << 8 | b[i + 8], h: b[i + 5] << 8 | b[i + 6] };
      if (m === 0xd8 || m === 0x01 || (m >= 0xd0 && m <= 0xd7)) { i += 2; continue; }
      i += 2 + (b[i + 2] << 8 | b[i + 3]);
    }
  }
  return null;
}

async function thumbnailUrl(path) {
  try {
    const st = await fs.lstat(path);
    if (st.size > 12 * 1024 * 1024) return null;
    const buf = new Uint8Array(await fs.readFile(path));
    let bin = "";
    for (let i = 0; i < buf.length; i += 0x8000) bin += String.fromCharCode.apply(null, buf.subarray(i, i + 0x8000));
    const ext = path.toLowerCase().split(".").pop();
    const mime = ext === "png" ? "image/png" : ext === "bmp" ? "image/bmp" : "image/jpeg";
    return { url: `data:${mime};base64,${btoa(bin)}`, size: imageSize(buf) };
  } catch (e) {
    return null;
  }
}

// Fill the tile like object-fit: cover: scale the still so it covers the tile, centred, cropped by the tile.
function coverImage(img, size, TILE_W = 74, TILE_H = 48) {
  if (!size || !size.w || !size.h) { img.style.width = `${TILE_W}px`; img.style.height = `${TILE_H}px`; return; }
  const s = Math.max(TILE_W / size.w, TILE_H / size.h);
  const w = Math.ceil(size.w * s), h = Math.ceil(size.h * s);
  img.style.width = `${w}px`;
  img.style.height = `${h}px`;
  img.style.left = `${Math.round((TILE_W - w) / 2)}px`;
  img.style.top = `${Math.round((TILE_H - h) / 2)}px`;
}

// A short tile label: macOS screenshot names all start "Screenshot <date> at", so show the time instead.
function refLabel(name) {
  const base = name.replace(/\.(png|jpe?g|bmp|tga)$/i, "");
  const shot = base.match(/^Screen ?[Ss]hot .* at (\d{1,2}[.:]\d{2}[.:]\d{2})/);
  return shot ? shot[1] : base;
}

const thumbCache = new Map();
const thumb = (path) => {
  if (!thumbCache.has(path)) thumbCache.set(path, thumbnailUrl(path));
  return thumbCache.get(path);
};

// A plain mouse wheel over the reference row scrolls it sideways (trackpads already can).
$("refGrid").addEventListener("wheel", (ev) => {
  const g = $("refGrid");
  if (Math.abs(ev.deltaY) > Math.abs(ev.deltaX) && g.scrollWidth > g.clientWidth) {
    g.scrollLeft += ev.deltaY;
    ev.preventDefault();
  }
});

async function renderRefs() {
  const grid = $("refGrid");
  const keepScroll = grid.scrollLeft;
  grid.innerHTML = "";
  // the add tile stays put on the left; the references, newest first, scroll beside it
  $("refAdd").innerHTML = "";
  const add = document.createElement("div");
  add.className = "ref add";
  add.title = "Add a still";
  add.textContent = "+";
  add.addEventListener("click", () => run("Add reference", async () => {
    const file = await uxp.storage.localFileSystem.getFileForOpening({ types: ["png", "jpg", "jpeg", "bmp", "tga"] });
    if (!file) return "Cancelled";
    return addReference(file.nativePath);
  }));
  $("refAdd").appendChild(add);
  for (const ref of library.refs) {
    const div = document.createElement("div");
    div.className = "ref" + (selectedRefs.some((r) => r.id === ref.id) ? " sel" : "");
    div.title = ref.path;
    div.innerHTML = `<img alt=""><div class="n"></div><div class="x" title="Remove">×</div>`;
    div.querySelector(".n").textContent = refLabel(ref.name);
    div.addEventListener("click", (ev) => {
      if (ev.target.classList.contains("x")) {
        library.removeReference(ref.id);
        selectedRefs = selectedRefs.filter((r) => r.id !== ref.id);
        library.save().then(renderRefs);
        return;
      }
      selectedRefs = [ref];
      renderRefs();
      if (selectedRefs.length) status(`Reference: ${referenceLabel(selectedRefs)}. Select clips and click Match.`);
    });
    grid.appendChild(div);
    grid.scrollLeft = keepScroll;
    thumb(ref.path).then((t) => {
      if (!t) return;
      const img = div.querySelector("img");
      coverImage(img, t.size);
      img.src = t.url;
    });
  }
}

function referenceLabel(refs) {
  return refs.length > 1 ? `${refs.length} stills (${refs.map((r) => r.name).join(", ")})` : `“${refs[0].name}”`;
}

async function addReference(path) {
  path = String(path || "").trim().replace(/^file:\/\//, "");
  if (!path) throw new Error("Drop an image to add it.");
  try { await fs.lstat(path); } catch (e) { throw new Error(`No file at ${path}`); }
  const ref = library.addReference(path);
  selectedRefs = [ref];
  await library.save();
  await renderRefs();
  return `Added ${baseName(path)}`;
}

// Drop a still anywhere on the panel (Finder / Explorer)
document.body.addEventListener("dragover", (ev) => { ev.preventDefault(); $("drop").classList.add("over"); });
document.body.addEventListener("dragleave", () => $("drop").classList.remove("over"));
document.body.addEventListener("drop", (ev) => {
  ev.preventDefault();
  $("drop").classList.remove("over");
  const dt = ev.dataTransfer;
  const paths = [];
  if (dt && dt.files) for (const f of dt.files) paths.push(f.nativePath || f.path || f.name);
  if (!paths.length && dt) paths.push(dt.getData("text/uri-list") || dt.getData("text/plain"));
  showTab("match");
  run("Add reference", async () => {
    let msg = "";
    for (const p of paths.filter(Boolean)) msg = await addReference(p);
    return msg || "Nothing to add";
  });
});

// ---------- matching ----------

// A new match puts every slider back to its default: the grade, adjustments, source evening and advanced controls.
function resetMatchSliders() {
  live.cancel();
  for (const [name, v] of Object.entries(MATCH_DEFAULTS)) {
    if (sliders.has(name)) sliders.get(name).set(v);
  }
  setOn("autoSkinSw", MATCH_DEFAULTS[P.autoSkin]);
  updateAdvSub();
}

// After a match: Auto's detection goes in the format picker's hover text; a clip that couldn't be
// matched is reported in the status line. (The full analysis is kept in the Tools log.)
function showClipAnalysis(entries) {
  const failed = entries.find((e) => e.analysis && e.analysis.error);
  if (failed) status(`${failed.name}: ${failed.analysis.error}`, "err");
  const first = entries.find((e) => e.analysis && e.analysis.source);
  if (first && sourceFormat() === 1) {
    $("sourceFormat").title = `Auto detected ${first.analysis.source.guess || first.analysis.source.transfer}` +
      (first.how ? ` (${first.how})` : "");
  }
}

// Polls each clip's analysis until all arrived (or ~60 s), redrawing as they come in.
async function followAnalysis(entries) {
  showClipAnalysis(entries);
  for (let t = 0; t < 200 && entries.some((e) => !e.analysis); t++) {
    await new Promise((r) => setTimeout(r, 300));
    let changed = false;
    for (const e of entries) {
      if (e.analysis) continue;
      e.analysis = await bridge.readAnalysis(e.slot);
      if (e.analysis) {
        changed = true;
        if (e.analysis.source) log(`analysis ${e.name}: ${JSON.stringify(e.analysis.source)} -> ${JSON.stringify(e.analysis.result)}`);
      }
    }
    if (changed) showClipAnalysis(entries);
  }
}

// Match: the selected clips only. Nothing selected -> say so and do nothing.
async function matchSelected() {
  const { items } = await bridge.selectedVideoItems();
  if (!items.length) throw new Error(NO_CLIPS);
  if (!selectedRefs.length) throw new Error("Pick a reference first: drop a still on the panel and click it.");
  await live.idle();
  resetMatchSliders();
  const params = Object.assign({}, MATCH_DEFAULTS);
  // The match decodes the footage itself, so the Input group passes code values straight through.
  params[P.footageEncoding] = 1;
  const chosen = sourceFormat();
  let formats = null, how = [];
  if (chosen === 1) {
    // Auto: clip metadata (colour space, input LUT, file name) first; the effect detects the rest from the picture
    const hints = await bridge.clipColourHints("selection");
    formats = hints.map((h) => guessSourceFormat(h));
    how = formats.map((f, i) => sourceFormatReason(hints[i], f));
  }
  const res = await bridge.applyReference("selection", selectedRefs.map((r) => r.path), (i) => Object.assign({}, params,
    { [P.sourceFormat]: formats ? (formats[i] || 1) : chosen }));
  const label = referenceLabel(selectedRefs);
  log(`matched ${res.clips} clip(s) to ${label} (slot ${res.slot}${res.added ? `, added Komodo to ${res.added}` : ""})`);
  const entries = res.slots.map((slot, i) => ({ slot, name: res.names[i], how: chosen === 1 ? how[i] : SOURCE_FORMATS[chosen - 1], analysis: null }));
  followAnalysis(entries).catch((e) => log(`analysis: ${e.message}`));
  return `${res.clips} clip${res.clips === 1 ? "" : "s"} matched to ${label}`;
}

$("matchBtn").addEventListener("click", async () => {
  if ($("matchBtn").classList.contains("busy")) return;
  $("matchBtn").classList.add("busy");
  await run("Matching", matchSelected);
  $("matchBtn").classList.remove("busy");
});

// ---------- follow the selection ----------
// The sliders show the first selected clip's values; the status line counts the selection.

let selectionKey = null;
async function readSelection() {
  if (live.timer || live.running || document.querySelector(".matchbtn.busy")) return;
  let keys = [];
  try {
    const { items } = await bridge.selectedVideoItems();
    keys = await Promise.all(items.map((it) => bridge.itemKey(it)));
  } catch (e) { return; }
  const key = keys.join("|");
  if (key === selectionKey) return;
  selectionKey = key;
  if (!/ busy/.test($("status").className)) {
    status(keys.length ? `${keys.length} clip${keys.length === 1 ? "" : "s"} selected` : "Select a clip on the timeline");
  }
  if (!keys.length) return;
  const cur = await bridge.selectedLookMatchParams().catch(() => null);
  if (cur) showParams(cur.params);
}

function showParams(params) {
  for (const [name, s] of sliders) if (typeof params[name] === "number") s.set(params[name]);
  if (params[P.autoSkin] !== undefined) setOn("autoSkinSw", params[P.autoSkin]);
  if (params[P.fixBanding] !== undefined) setOn("bandSw", params[P.fixBanding]);
  if (params[P.split] !== undefined)
    $("beforeAfter").classList.toggle("on", !!params[P.split] && params[P.splitPos] > 0 && params[P.splitPos] < 100);
  if (params[P.sourceFormat] !== undefined) {
    $("sourceFormat").selectedIndex = Math.max(0, params[P.sourceFormat] - 1);
  }
  updateAdvSub();
}

// ---------- library ----------

function currentLookParams() {
  const out = { [P.sourceFormat]: sourceFormat(), [P.autoSkin]: isOn("autoSkinSw"), [P.fixBanding]: isOn("bandSw") };
  for (const [name, s] of sliders) out[name] = s.get();
  return out;
}

function renderSaved() {
  const list = $("savedList");
  list.innerHTML = "";
  if (!library.looks.length) {
    list.innerHTML = '<div class="hint">No saved looks yet. Grade a clip, name the look and save it.</div>';
    return;
  }
  for (const look of library.looks) {
    const row = document.createElement("div");
    row.className = "item";
    row.innerHTML = `<div class="sw8"><img alt=""></div><div class="txt"><div class="nm"></div><div class="meta"></div></div>
      <div class="btn-o" data-a="apply">Apply</div><div class="del" data-a="delete" title="Delete">×</div>`;
    row.querySelector(".nm").textContent = look.name;
    const p = look.params || {};
    row.querySelector(".meta").textContent = [look.reference ? baseName(look.reference) : "no reference",
      p[P.finish] !== undefined ? `Finish Band ${Math.round(p[P.finish])}` : "", p[P.grain] ? `Grain ${Math.round(p[P.grain])}` : ""].filter(Boolean).join(" · ");
    if (look.reference) thumb(look.reference).then((t) => {
      if (!t) return;
      const img = row.querySelector("img");
      coverImage(img, t.size, 34, 34);
      img.src = t.url;
    });
    row.addEventListener("click", (ev) => {
      const a = ev.target.dataset && ev.target.dataset.a;
      if (a === "apply") applySavedLook(look);
      if (a === "delete") { library.removeLook(look.id); library.save().then(renderSaved); }
    });
    list.appendChild(row);
  }
}

// film print settings from looks saved by early builds: matching no longer applies a print
const OLD_FILM = ["Film Strength", "Negative Stock", "Print Stock", "Printer Light R", "Printer Light G", "Printer Light B", "Film Saturation"];

function applySavedLook(look) {
  return run(`Apply ${look.name}`, async () => {
    const { items } = await bridge.selectedVideoItems();
    if (!items.length) throw new Error("No clips selected. Select a clip on the timeline, then click Apply.");
    await live.idle();
    const params = Object.assign({}, look.params);
    for (const k of OLD_FILM) delete params[k];
    showParams(params);
    if (look.reference) {
      const res = await bridge.applyReference("selection", look.reference, params);
      return `${look.name} on ${res.clips} clip(s)`;
    }
    const res = await bridge.applyParams("selection", params);
    return `${look.name} on ${res.clips} clip(s)`;
  });
}

$("saveLook").addEventListener("click", () => run("Save look", async () => {
  const look = library.saveLook($("lookName").value, currentLookParams(), selectedRefs.length ? selectedRefs[0].path : null);
  await library.save();
  renderSaved();
  return `Saved “${look.name}”`;
}));

// ---------- probe ----------

// Probe (under Advanced): what Komodo sees on the selected clips, written into the box below it.
$("probe").addEventListener("click", () => run("Probe", async () => {
  $("console").hidden = false;
  const { items } = await bridge.selectedVideoItems();
  log(`${items.length} selected video clip(s)`);
  for (const item of items) {
    const { component } = await bridge.findLookMatch(item);
    log(`  ${await item.getName()}: ${component ? JSON.stringify(await bridge.readParams(component)) : "no Komodo"}`);
  }
  return `${items.length} clip(s)`;
}));

// ---------- start ----------

(async () => {
  $("version").textContent = `v${VERSION} · PRO PLUGS`;
  await library.load();
  selectedRefs = library.refs.length ? [library.refs[0]] : [];
  await renderRefs();
  renderSaved();
  log(`Komodo panel ${VERSION} ready (data in ${DATA_DIR})`);
  await refreshLicense().catch((e) => log(`license check failed: ${e.message}`));
  setInterval(() => { readSelection().catch(() => {}); }, 1000);
  setInterval(() => { refreshLicense().catch(() => {}); }, 60000);  // catch expiry while the panel is open
  try {
    // dev builds only: lib/agent.js is left out of release packages
    require("./lib/agent.js").startAgent(fs, DATA_DIR, bridge, log, {
      // UI driving for tests: only these elements, only set value / click
      fields: ["lookName"],  // plus tab switching (uiTab)
      buttons: ["matchBtn", "beforeAfter", "saveLook", "probe"],
      status: () => ({ text: $("statusText").textContent, kind: $("status").className }),
      savedLooks: () => library.looks.map((l) => l.name),
      element: (id) => document.getElementById(id),
      setSlider: (name, v) => { const s = sliders.get(name); if (!s) throw new Error(`no slider ${name}`); s.set(v); live.set(name, s.get()); return s.get(); },
    });
  } catch (e) { /* release build */ }
})();
