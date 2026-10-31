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
const { runSelfTest } = require("./lib/selftest.js");

const VERSION = "1.0.0";  // kept equal to manifest.json by build_panel.sh
// The data folder keeps its LookMatch name: references, looks and matches made before the rename stay.
const DATA_DIR = os.platform() === "win32"
  ? joinPath(process.env.APPDATA || "", "PRO PLUGS\\LookMatch")
  : joinPath(os.homedir(), "Library/Application Support/PRO PLUGS/LookMatch");
const LOG_FILE = joinPath(os.homedir(), "Library/Logs/LookMatch.log");
const NO_CLIPS = "No clips selected. Select a clip on the timeline, then click Match.";

const $ = (id) => document.getElementById(id);
const library = new Library(fs, DATA_DIR);
const bridge = new Bridge(ppro, fs, DATA_DIR);
let selectedRefs = [];   // the stills the look is read from: Cmd/Shift-click adds up to 5 of one look

// ---------- status & console ----------

function status(text, kind) {
  $("statusText").textContent = text || "";
  $("status").className = "status" + (kind ? " " + kind : "");
}

// The Tools console, mirrored to <data>/panel.log (kept under ~200 KB) for diagnostics.
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

// ---------- tabs, menu, folds, switches ----------

function showTab(name) {
  for (const t of document.querySelectorAll("[data-tab]")) t.classList.toggle("on", t.dataset.tab === name);
  for (const s of document.querySelectorAll("section[data-panel]")) s.hidden = s.dataset.panel !== name;
}
$("tabs").addEventListener("click", (ev) => {
  const tab = ev.target.closest ? ev.target.closest("[data-tab]") : ev.target;
  if (tab && tab.dataset.tab) showTab(tab.dataset.tab);
});
$("menuBtn").addEventListener("click", (ev) => { ev.stopPropagation(); $("menu").hidden = !$("menu").hidden; });
document.addEventListener("click", () => { $("menu").hidden = true; });
for (const item of document.querySelectorAll("[data-tool]")) {
  item.addEventListener("click", () => { showTab("tools"); $(item.dataset.tool).click(); });
}
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
  return unit === "%" ? `${Math.round(v)}%` : (Math.round(v * 100) / 100).toString();
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
    fill.style.width = `${pct}%`;
    thumb.style.left = `${pct}%`;
    val.textContent = fmt(value, unit);
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
  { param: P.matchContrast, title: "Match Contrast", icon: "contrast", def: 100,
    desc: "Brings blacks, mids and whites to the reference's levels. Skin is kept from crushing." },
  { param: P.finish, title: "Finish", icon: "finish", def: 100, desc: "Lands the match at the levels of a finished grade: open highlights, firm blacks." },
  { param: P.skinEvening, title: "Skin Evening", icon: "skin", def: 50, desc: "Evens patchy skin tone toward the skin around it. No smoothing." },
];
const updateAdvSub = () => {
  $("advSub").textContent = `Match ${Math.round(sliders.get(P.matchStrength).get())}% · Regions ${Math.round(sliders.get(P.regionMatching).get())}%`;
};
const ADVANCED = [
  { param: P.matchStrength, title: "Match strength", def: 100, onChange: updateAdvSub },
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
// Before / after: After (the grade), Split (original left of the line) or Before (the whole frame
// original: the split at 100%, where the effect draws no line). Uses the effect's split view.
const VIEWS = { after: { [P.split]: false }, split: { [P.split]: true, [P.splitPos]: 50, [P.splitAngle]: 0 },
                before: { [P.split]: true, [P.splitPos]: 100, [P.splitAngle]: 0 } };
function showView(view) {
  for (const b of document.querySelectorAll("[data-view]")) b.classList.toggle("on", b.dataset.view === view);
}
for (const b of document.querySelectorAll("[data-view]")) {
  b.addEventListener("click", () => {
    showView(b.dataset.view);
    for (const [k, v] of Object.entries(VIEWS[b.dataset.view])) live.set(k, v);
  });
}

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

async function thumbnailUrl(path) {
  try {
    const st = await fs.lstat(path);
    if (st.size > 12 * 1024 * 1024) return "";
    const buf = new Uint8Array(await fs.readFile(path));
    let bin = "";
    for (let i = 0; i < buf.length; i += 0x8000) bin += String.fromCharCode.apply(null, buf.subarray(i, i + 0x8000));
    const ext = path.toLowerCase().split(".").pop();
    const mime = ext === "png" ? "image/png" : ext === "bmp" ? "image/bmp" : "image/jpeg";
    return `data:${mime};base64,${btoa(bin)}`;
  } catch (e) {
    return "";
  }
}
const thumbCache = new Map();
const thumb = (path) => {
  if (!thumbCache.has(path)) thumbCache.set(path, thumbnailUrl(path));
  return thumbCache.get(path);
};

async function renderRefs() {
  const grid = $("refGrid");
  grid.innerHTML = "";
  // the add tile first, then the references, newest first
  const add = document.createElement("div");
  add.className = "ref add";
  add.title = "Add a still";
  add.textContent = "+";
  add.addEventListener("click", () => run("Add reference", async () => {
    const file = await uxp.storage.localFileSystem.getFileForOpening({ types: ["png", "jpg", "jpeg", "bmp", "tga"] });
    if (!file) return "Cancelled";
    return addReference(file.nativePath);
  }));
  grid.appendChild(add);
  for (const ref of library.refs) {
    const div = document.createElement("div");
    div.className = "ref" + (selectedRefs.some((r) => r.id === ref.id) ? " sel" : "");
    div.title = ref.path;
    div.innerHTML = `<img alt=""><div class="n"></div><div class="x" title="Remove">×</div>`;
    div.querySelector(".n").textContent = ref.name.replace(/\.(png|jpe?g|bmp|tga)$/i, "");
    div.addEventListener("click", (ev) => {
      if (ev.target.classList.contains("x")) {
        library.removeReference(ref.id);
        selectedRefs = selectedRefs.filter((r) => r.id !== ref.id);
        library.save().then(renderRefs);
        return;
      }
      selectedRefs = toggleReference(selectedRefs, ref, ev.metaKey || ev.shiftKey || ev.ctrlKey);
      renderRefs();
      if (selectedRefs.length) status(`Reference: ${referenceLabel(selectedRefs)}. Select clips and click Match.`);
    });
    grid.appendChild(div);
    thumb(ref.path).then((url) => { if (url) div.querySelector("img").src = url; });
  }
}

// Click picks one reference; Cmd/Shift/Ctrl-click adds or removes it from a set of up to 5 stills of
// one look (the match reads each and combines them).
function toggleReference(current, ref, add) {
  if (!add) return [ref];
  if (current.some((r) => r.id === ref.id)) return current.filter((r) => r.id !== ref.id);
  return current.concat([ref]).slice(-5);
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

// A new match puts every slider back to its default: the grade, source evening and advanced controls.
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
    showView(!params[P.split] || params[P.splitPos] <= 0 ? "after" : params[P.splitPos] >= 100 ? "before" : "split");
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
      p[P.finish] !== undefined ? `Finish ${Math.round(p[P.finish])}` : "", p[P.grain] ? `Grain ${Math.round(p[P.grain])}` : ""].filter(Boolean).join(" · ");
    if (look.reference) thumb(look.reference).then((url) => { if (url) row.querySelector("img").src = url; });
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

// ---------- tools ----------

$("showLog").addEventListener("click", () => run("Effect log", async () => {
  let text;
  try { text = await fs.readFile(LOG_FILE, { encoding: "utf-8" }); } catch (e) {
    return "No log. Start Premiere with LOOKMATCH_LOG=1 to enable it.";
  }
  log("---- effect log (last 40 lines) ----\n" + text.trim().split("\n").slice(-40).join("\n"));
  return "Log shown below";
}));

$("probe").addEventListener("click", () => run("Probe", async () => {
  const { items } = await bridge.selectedVideoItems();
  log(`${items.length} selected video clip(s)`);
  for (const item of items) {
    const { component } = await bridge.findLookMatch(item);
    log(`  ${await item.getName()}: ${component ? JSON.stringify(await bridge.readParams(component)) : "no Komodo"}`);
  }
  return `${items.length} clip(s)`;
}));

// In-Premiere self-test (lib/selftest.js): exercises the whole flow on the selected clip.
$("selfTest").addEventListener("click", () => run("Self-test", async () => {
  const { pass, lines } = await runSelfTest(bridge, { log, referencePath: selectedRefs.length ? selectedRefs[0].path : null });
  return pass ? `Self-test passed (${lines.filter((l) => l.startsWith("PASS")).length} checks)` : "Self-test failed: see Tools";
}));

// ---------- start ----------

(async () => {
  $("version").textContent = `v${VERSION} · PRO PLUGS`;
  await library.load();
  selectedRefs = library.refs.length ? [library.refs[0]] : [];
  await renderRefs();
  renderSaved();
  log(`Komodo panel ${VERSION} ready (data in ${DATA_DIR})`);
  setInterval(() => { readSelection().catch(() => {}); }, 1000);
  try {
    // dev builds only: lib/agent.js is left out of release packages
    require("./lib/agent.js").startAgent(fs, DATA_DIR, bridge, log, {
      // UI driving for tests: only these elements, only set value / click
      fields: ["lookName"],  // plus tab switching (uiTab)
      buttons: ["matchBtn", "saveLook", "selfTest", "probe", "showLog"],
      status: () => ({ text: $("statusText").textContent, kind: $("status").className }),
      savedLooks: () => library.looks.map((l) => l.name),
      element: (id) => document.getElementById(id),
      setSlider: (name, v) => { const s = sliders.get(name); if (!s) throw new Error(`no slider ${name}`); s.set(v); live.set(name, s.get()); return s.get(); },
    });
  } catch (e) { /* release build */ }
})();
