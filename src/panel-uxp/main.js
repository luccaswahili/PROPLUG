// LookMatch panel for Premiere Pro (UXP). Drives the LookMatch effect: source-aware reference
// matching, texture controls and saved looks. All Premiere access goes through lib/bridge.js.

const ppro = require("premierepro");
const uxp = require("uxp");
const fs = require("fs");
const os = require("os");
const { P, SOURCE_FORMATS, TEXTURE_DEFAULTS, guessSourceFormat, sourceFormatReason, abNext } = require("./lib/params.js");
const { Library, joinPath, baseName } = require("./lib/library.js");
const { Bridge } = require("./lib/bridge.js");
const { runSelfTest } = require("./lib/selftest.js");

const VERSION = "0.7.0";  // kept equal to manifest.json by build_panel.sh
const DATA_DIR = os.platform() === "win32"
  ? joinPath(process.env.APPDATA || "", "PRO PLUGS\\LookMatch")
  : joinPath(os.homedir(), "Library/Application Support/PRO PLUGS/LookMatch");
const LOG_FILE = joinPath(os.homedir(), "Library/Logs/LookMatch.log");

const $ = (id) => document.getElementById(id);
const library = new Library(fs, DATA_DIR);
const bridge = new Bridge(ppro, fs, DATA_DIR);
let selectedRef = null;  // the reference clicked last (also the first of selectedRefs)
let selectedRefs = [];   // the stills the look is read from: Cmd/Shift-click adds up to 5 of one look

// ---------- status & console ----------

function status(text, kind) {
  const el = $("status");
  el.textContent = text || "";
  el.className = "status" + (kind ? " " + kind : "");
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
    status(msg || `${label}: done`, "ok");
    log(`${label}: ${msg || "done"}`);
  } catch (e) {
    status(`${label}: ${e.message}`, "err");
    log(`${label} failed: ${e.message}`);
  }
}

// ---------- tabs ----------

$("tabs").addEventListener("click", (ev) => {
  const tab = ev.target.closest ? ev.target.closest("[data-tab]") : ev.target;
  if (!tab || !tab.dataset.tab) return;
  for (const b of document.querySelectorAll("[data-tab]")) b.selected = b === tab;
  for (const s of document.querySelectorAll("section[data-panel]")) s.hidden = s.dataset.panel !== tab.dataset.tab;
});

// ---------- reference library ----------

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

async function renderRefs() {
  const grid = $("refGrid");
  grid.innerHTML = "";
  if (!library.refs.length) {
    grid.innerHTML = '<sp-body size="XS" class="hint">No references yet.</sp-body>';
    return;
  }
  for (const ref of library.refs) {
    const div = document.createElement("div");
    div.className = "thumb" + (selectedRefs.some((r) => r.id === ref.id) ? " sel" : "");
    div.title = ref.path;
    div.innerHTML = `<img alt=""><div class="name"><span class="x" title="Remove">×</span></div>`;
    div.querySelector(".name").appendChild(document.createTextNode(ref.name));
    div.addEventListener("click", (ev) => {
      if (ev.target.classList.contains("x")) {
        library.removeReference(ref.id);
        selectedRefs = selectedRefs.filter((r) => r.id !== ref.id);
        selectedRef = selectedRefs[0] || null;
        library.save().then(renderRefs);
        return;
      }
      selectedRefs = toggleReference(selectedRefs, ref, ev.metaKey || ev.shiftKey || ev.ctrlKey);
      selectedRef = selectedRefs[0] || null;
      renderRefs();
    });
    grid.appendChild(div);
    thumbnailUrl(ref.path).then((url) => { if (url) div.querySelector("img").src = url; });
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
  return refs.length > 1 ? `${refs.length} stills (${refs.map((r) => r.name).join(", ")})` : refs[0].name;
}

async function addReference(path) {
  path = String(path || "").trim().replace(/^file:\/\//, "");
  if (!path) throw new Error("Enter or drop an image path.");
  try { await fs.lstat(path); } catch (e) { throw new Error(`No file at ${path}`); }
  selectedRef = library.addReference(path);
  selectedRefs = [selectedRef];
  await library.save();
  await renderRefs();
  return `Added ${baseName(path)}`;
}

$("addRef").addEventListener("click", () => run("Add reference", () => addReference($("refPath").value)));
$("browseRef").addEventListener("click", () => run("Add reference", async () => {
  const file = await uxp.storage.localFileSystem.getFileForOpening({ types: ["png", "jpg", "jpeg", "bmp", "tga"] });
  if (!file) return "Cancelled";
  return addReference(file.nativePath);
}));

// Drag and drop from Finder / Explorer
const drop = $("drop");
drop.addEventListener("dragover", (ev) => { ev.preventDefault(); drop.classList.add("over"); });
drop.addEventListener("dragleave", () => drop.classList.remove("over"));
drop.addEventListener("drop", (ev) => {
  ev.preventDefault();
  drop.classList.remove("over");
  const dt = ev.dataTransfer;
  const paths = [];
  if (dt && dt.files) for (const f of dt.files) paths.push(f.nativePath || f.path || f.name);
  if (!paths.length && dt) paths.push(dt.getData("text/uri-list") || dt.getData("text/plain"));
  run("Add reference", async () => {
    let msg = "";
    for (const p of paths.filter(Boolean)) msg = await addReference(p);
    return msg || "Nothing to add";
  });
});

// ---------- matching ----------

function bindSlider(id, labelId) {
  const s = $(id);
  const update = () => { $(labelId).textContent = `${Math.round(s.value)}%`; };
  s.addEventListener("input", update);
  update();
}
bindSlider("strength", "strengthV");
bindSlider("region", "regionV");

bindSlider("normExposure", "normExposureV");
bindSlider("normWhite", "normWhiteV");
bindSlider("normContrast", "normContrastV");
bindSlider("lookStrength", "lookStrengthV");
bindSlider("matchContrast", "matchContrastV");

// Source Format picker: 1 = Auto, then the effect's popup order
const sourceMenu = $("sourceMenu");
SOURCE_FORMATS.forEach((name, i) => {
  const item = document.createElement("sp-menu-item");
  item.textContent = name;
  item.value = String(i + 1);
  sourceMenu.appendChild(item);
});
$("sourceFormat").selectedIndex = 0;
const sourceFormat = () => Math.max(1, ($("sourceFormat").selectedIndex | 0) + 1);

function matchSettings() {
  return {
    [P.matchStrength]: Number($("strength").value),
    [P.regionMatching]: Number($("region").value),
    [P.autoSkin]: !!$("autoSkin").checked,
    [P.normExposure]: Number($("normExposure").value),
    [P.normWhite]: Number($("normWhite").value),
    [P.normContrast]: Number($("normContrast").value),
    [P.lookStrength]: Number($("lookStrength").value),
    [P.matchContrast]: Number($("matchContrast").value),
  };
}

// ---------- source analysis (inline, per clip) ----------

const fmtStops = (v) => (v >= 0 ? "+" : "") + v.toFixed(2);
const fmtTint = (v) => (v >= 0 ? "+" : "") + v.toFixed(1);
function analysisTable(a) {
  const col = (x) => x ? [x.transfer + (x.log ? " (log)" : ""), fmtStops(x.exposure), `${Math.round(x.cct)} K`, fmtTint(x.tint),
                          `${x.contrast.toFixed(1)} st`] : ["", "", "", "", ""];
  const rows = ["Format", "Exposure", "Temperature", "Tint", "Contrast"];
  const s = col(a.source), r = col(a.result), f = col(a.reference);
  let html = "<table><tr><th></th><th>Before</th><th>After</th><th>Reference</th></tr>";
  rows.forEach((name, i) => { html += `<tr><td>${name}</td><td>${s[i]}</td><td>${r[i]}</td><td>${i ? f[i] : ""}</td></tr>`; });
  return html + "</table>";
}

const analysisBox = $("analysis");
function showClipAnalysis(entries) {
  analysisBox.innerHTML = "";
  for (const e of entries) {
    const div = document.createElement("div");
    div.className = "clip";
    const title = document.createElement("div");
    title.className = "title";
    title.textContent = e.name + (e.how ? `  ·  ${e.how}` : "");
    div.appendChild(title);
    const body = document.createElement("div");
    if (!e.analysis) body.innerHTML = '<span class="pending">Measuring… (Premiere builds the match the first time it draws the clip: park the playhead on it)</span>';
    else if (e.analysis.error) { body.className = "err"; body.textContent = e.analysis.error; }
    else body.innerHTML = analysisTable(e.analysis);
    div.appendChild(body);
    analysisBox.appendChild(div);
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

async function matchClips(scope) {
  if (!selectedRefs.length && selectedRef) selectedRefs = [selectedRef];
  if (!selectedRefs.length) throw new Error("Pick a reference first (add one above and click it).");
  const params = matchSettings();
  // The match decodes the footage itself, so the Input group passes code values straight through.
  params[P.footageEncoding] = 1;
  const chosen = sourceFormat();
  let formats = null, how = [];
  if (chosen === 1) {
    // Auto: clip metadata (colour space, input LUT, file name) first; the effect detects the rest from the picture
    const hints = await bridge.clipColourHints(scope);
    formats = hints.map((h) => guessSourceFormat(h));
    how = formats.map((f, i) => sourceFormatReason(hints[i], f));
  }
  const refLabel = referenceLabel(selectedRefs);
  const res = await bridge.applyReference(scope, selectedRefs.map((r) => r.path), (i) => Object.assign({}, params,
    { [P.sourceFormat]: formats ? (formats[i] || 1) : chosen }));
  log(`matched ${res.clips} clip(s) to ${refLabel} (slot ${res.slot}${res.added ? `, added LookMatch to ${res.added}` : ""})`);
  log("tip: to keep this match inside the project (for other Macs), click LookMatch > Panel Link > Save into Project in Effect Controls");
  const entries = res.slots.map((slot, i) => ({ slot, name: res.names[i], how: chosen === 1 ? how[i] : SOURCE_FORMATS[chosen - 1], analysis: null }));
  followAnalysis(entries).catch((e) => log(`analysis: ${e.message}`));
  return `Matched ${res.clips} clip(s) to ${refLabel}`;
}

// Shot matching: grab the selected (hero) clip's frame through the effect, add it to the library.
// With "Match to its graded look", the frame as LookMatch renders it (its match included), so the other
// clips take the hero's finished look; otherwise the clip as shot.
async function captureHero() {
  const graded = !!$("heroGraded").checked;
  const res = await bridge.captureFrame({ graded });
  selectedRef = library.addReference(res.path);
  selectedRef.name = `Hero${graded ? " (graded)" : ""}: ${res.clip}`;
  selectedRefs = [selectedRef];
  await library.save();
  await renderRefs();
  log(`captured hero frame of ${res.clip} -> ${res.path}`);
  return res;
}

$("heroRef").addEventListener("click", () => run("Capture hero", async () => {
  const res = await captureHero();
  return `${res.clip} is now the reference: select other clips and click Match`;
}));
$("heroMatchSeq").addEventListener("click", () => run("Match sequence to hero", async () => {
  const res = await captureHero();
  // every clip but the hero: the hero keeps its own grade
  const msg = await matchClips("others");
  return `${msg} (hero: ${res.clip})`;
}));

$("matchSel").addEventListener("click", () => run("Match", () => matchClips("selection")));
$("matchSeq").addEventListener("click", () => run("Match sequence", () => matchClips("sequence")));
$("applyStrength").addEventListener("click", () => run("Apply strength", async () => {
  const res = await bridge.applyParams("selection", matchSettings());
  return `Updated ${res.clips} clip(s)`;
}));
// Before / after: the effect's Split View wipe (original on the left of the line).
$("beforeAfter").addEventListener("click", () => run("Before / after", async () => {
  const cur = await bridge.selectedLookMatchParams();
  if (!cur) throw new Error("Select a clip with LookMatch.");
  // after A/B (split at 0% / 100%) this turns the compare off
  const ab = cur.params[P.split] && (Number(cur.params[P.splitPos]) <= 0 || Number(cur.params[P.splitPos]) >= 100);
  const on = !cur.params[P.split] && !ab;
  await bridge.applyParams("selection", { [P.split]: on, [P.splitPos]: Number($("splitPos").value) });
  return on ? "Split view on: original on the left" : "Split view off";
}));
$("abCompare").addEventListener("click", () => run("A / B", async () => {
  const cur = await bridge.selectedLookMatchParams();
  if (!cur) throw new Error("Select a clip with LookMatch.");
  const next = abNext(cur.params);
  await bridge.applyParams("selection", next.params);
  return `${next.label} (${cur.name}). Click again to switch, Before / after to turn it off.`;
}));
bindSlider("splitPos", "splitPosV");
$("splitPos").addEventListener("change", () => run("Split position", async () => {
  await bridge.applyParams("selection", { [P.splitPos]: Number($("splitPos").value) });
  return `Split at ${Math.round($("splitPos").value)}%`;
}));

// ---------- texture controls ----------

const FILM_CONTROLS = [
  [P.fade, 0, 100, 1, "%"],
  [P.halation, 0, 200, 1, "%"], [P.halationSize, 0.25, 3, 0.05, ""], [P.halationThreshold, 0.1, 2, 0.05, ""],
  [P.bloom, 0, 200, 1, "%"], [P.bloomSize, 0.25, 3, 0.05, ""], [P.bloomThreshold, 0.1, 2, 0.05, ""],
  [P.grain, 0, 200, 1, "%"], [P.grainSize, 0.5, 4, 0.05, ""], [P.grainColour, 0, 100, 1, "%"],
  [P.softness, 0, 100, 1, "%"], [P.vignette, 0, 100, 1, "%"], [P.gateWeave, 0, 4, 0.05, ""], [P.flicker, 0, 100, 1, "%"],
];
const filmInputs = new Map();

function fmt(v, unit) {
  return unit === "%" ? `${Math.round(v)}%` : (Math.round(v * 100) / 100).toString();
}

function renderFilmControls() {
  const box = $("filmControls");
  for (const [name, a, b, step, unit] of FILM_CONTROLS) {
    const row = document.createElement("div");
    row.className = "slider-row";
    const label = document.createElement("sp-detail");
    label.textContent = name.toUpperCase();
    row.appendChild(label);
    {
      const s = document.createElement("sp-slider");
      s.setAttribute("min", a); s.setAttribute("max", b); s.setAttribute("step", step);
      const val = document.createElement("span");
      s.addEventListener("input", () => { val.textContent = fmt(Number(s.value), unit); });
      row.appendChild(s);
      row.appendChild(val);
      filmInputs.set(name, { get: () => Number(s.value), set: (v) => { s.value = v; val.textContent = fmt(v, unit); } });
    }
    box.appendChild(row);
  }
  filmInputs.set(P.fixBanding, { get: () => !!$("fixBanding").checked, set: (v) => { $("fixBanding").checked = !!v; } });
  setFilmControls(TEXTURE_DEFAULTS);
}

function setFilmControls(params) {
  for (const [name, input] of filmInputs) if (params[name] !== undefined) input.set(params[name]);
}

function filmParams() {
  const out = {};
  for (const [name, input] of filmInputs) out[name] = input.get();
  return out;
}

$("readFilm").addEventListener("click", () => run("Read clip", async () => {
  const cur = await bridge.selectedLookMatchParams();
  if (!cur) throw new Error("Select a clip with LookMatch.");
  setFilmControls(cur.params);
  if (cur.params[P.matchStrength] !== undefined) $("strength").value = cur.params[P.matchStrength];
  for (const [id, name] of [["normExposure", P.normExposure], ["normWhite", P.normWhite], ["normContrast", P.normContrast], ["lookStrength", P.lookStrength], ["matchContrast", P.matchContrast]])
    if (cur.params[name] !== undefined) { $(id).value = cur.params[name]; $(`${id}V`).textContent = `${Math.round(cur.params[name])}%`; }
  if (cur.params[P.sourceFormat] !== undefined) $("sourceFormat").selectedIndex = Math.max(0, cur.params[P.sourceFormat] - 1);
  return `Read ${cur.name}`;
}));
$("resetFilm").addEventListener("click", () => setFilmControls(TEXTURE_DEFAULTS));
$("applyFilm").addEventListener("click", () => run("Apply texture", async () => {
  const res = await bridge.applyParams("selection", filmParams());
  return `Texture on ${res.clips} clip(s)`;
}));
$("applyFilmSeq").addEventListener("click", () => run("Apply texture to sequence", async () => {
  const res = await bridge.applyParams("sequence", filmParams());
  return `Texture on ${res.clips} clip(s)`;
}));

// ---------- saved looks ----------

function renderSaved() {
  const list = $("savedList");
  list.innerHTML = "";
  if (!library.looks.length) {
    list.innerHTML = '<sp-body size="XS" class="hint">No saved looks yet.</sp-body>';
    return;
  }
  for (const look of library.looks) {
    const row = document.createElement("div");
    row.className = "item";
    row.innerHTML = `<span class="name"></span>
      <sp-action-button size="s" data-a="apply">Apply</sp-action-button>
      <sp-action-button size="s" data-a="export">Export</sp-action-button>
      <sp-action-button size="s" data-a="delete">Delete</sp-action-button>`;
    row.querySelector(".name").textContent = look.name + (look.reference ? `  ·  ${baseName(look.reference)}` : "");
    row.addEventListener("click", (ev) => {
      const a = ev.target.dataset && ev.target.dataset.a;
      if (a === "apply") applySavedLook(look);
      if (a === "export") exportLook(look);
      if (a === "delete") { library.removeLook(look.id); library.save().then(renderSaved); }
    });
    list.appendChild(row);
  }
}

function applySavedLook(look) {
  return run(`Apply ${look.name}`, async () => {
    setFilmControls(look.params);
    if (look.reference) {
      const params = Object.assign({}, look.params);
      // looks saved by 0.5 kept film print settings: matching no longer applies a print
      for (const k of ["Film Strength", "Negative Stock", "Print Stock", "Printer Light R", "Printer Light G", "Printer Light B", "Film Saturation"]) delete params[k];
      const res = await bridge.applyReference("selection", look.reference, params);
      return `${look.name} (matched to ${baseName(look.reference)}) on ${res.clips} clip(s)`;
    }
    const params = Object.assign({}, look.params);
    for (const k of ["Negative Stock", "Print Stock", "Printer Light R", "Printer Light G", "Printer Light B", "Film Saturation"]) delete params[k];
    const res = await bridge.applyParams("selection", params);
    return `${look.name} on ${res.clips} clip(s)`;
  });
}

function exportLook(look) {
  return run("Export look", async () => {
    let target = $("lookFile").value.trim();
    if (!target) target = joinPath(DATA_DIR, "looks");
    if (!/\.lmlook$/i.test(target)) {
      try { await fs.mkdir(target, { recursive: true }); } catch (e) { /* exists */ }
      target = joinPath(target, `${look.name.replace(/[\\/:*?"<>|]+/g, "_")}.lmlook`);
    }
    await fs.writeFile(target, Library.lookToFile(look), { encoding: "utf-8" });
    log(`exported ${look.name} to ${target}`);
    return `Exported to ${target}`;
  });
}

$("saveLook").addEventListener("click", () => run("Save look", async () => {
  const params = Object.assign({}, filmParams(), matchSettings(), { [P.sourceFormat]: sourceFormat() });
  const look = library.saveLook($("lookName").value, params, selectedRef ? selectedRef.path : null);
  await library.save();
  renderSaved();
  return `Saved "${look.name}"`;
}));

$("exportCube").addEventListener("click", () => run("Export LUT", async () => {
  let target = $("cubePath").value.trim();
  if (!target) target = joinPath(DATA_DIR, "luts/LookMatch.cube");
  if (!/\.cube$/i.test(target)) target = joinPath(target, "LookMatch.cube");
  try { await fs.mkdir(target.replace(/[\\/][^\\/]*$/, ""), { recursive: true }); } catch (e) { /* exists */ }
  const res = await bridge.exportCube(target);
  log(`exported LUT to ${res.path}`);
  return `LUT written to ${res.path}`;
}));

$("importLook").addEventListener("click", () => run("Import look", async () => {
  const path = $("lookFile").value.trim();
  if (!path) throw new Error("Enter the path of a .lmlook file.");
  const data = Library.lookFromFile(await fs.readFile(path, { encoding: "utf-8" }));
  const look = library.saveLook(data.name, data.params, data.reference);
  await library.save();
  renderSaved();
  return `Imported "${look.name}"`;
}));

// ---------- tools ----------

$("showLog").addEventListener("click", () => run("Effect log", async () => {
  let text;
  try { text = await fs.readFile(LOG_FILE, { encoding: "utf-8" }); } catch (e) {
    return "No log. Start Premiere with LOOKMATCH_LOG=1 to enable it.";
  }
  log("---- LookMatch.log (last 40 lines) ----\n" + text.trim().split("\n").slice(-40).join("\n"));
  return "Log shown below";
}));

$("probe").addEventListener("click", () => run("Probe", async () => {
  const { items } = await bridge.selectedVideoItems();
  log(`${items.length} selected video clip(s)`);
  for (const item of items) {
    const { component } = await bridge.findLookMatch(item);
    log(`  ${await item.getName()}: ${component ? JSON.stringify(await bridge.readParams(component)) : "no LookMatch"}`);
  }
  return `${items.length} clip(s)`;
}));

// In-Premiere self-test (lib/selftest.js): exercises the whole flow on the selected clip.
$("selfTest").addEventListener("click", () => run("Self-test", async () => {
  const { pass, lines } = await runSelfTest(bridge, { log, referencePath: selectedRef ? selectedRef.path : null });
  return pass ? `Self-test passed (${lines.filter((l) => l.startsWith("PASS")).length} checks)` : "Self-test failed: see Tools";
}));

// ---------- start ----------

(async () => {
  renderFilmControls();
  await library.load();
  selectedRef = library.refs[0] || null;
  selectedRefs = selectedRef ? [selectedRef] : [];
  await renderRefs();
  renderSaved();
  log(`LookMatch panel ${VERSION} ready (data in ${DATA_DIR})`);
  try {
    // dev builds only: lib/agent.js is left out of release packages
    require("./lib/agent.js").startAgent(fs, DATA_DIR, bridge, log, {
      // UI driving for tests: only these elements, only set value / click
      fields: ["refPath", "lookName", "lookFile", "cubePath", "normExposure", "normWhite", "normContrast", "lookStrength", "matchContrast"],  // plus tab switching (uiTab)
      buttons: ["addRef", "matchSel", "matchSeq", "applyStrength", "beforeAfter", "abCompare", "heroRef", "heroMatchSeq", "readFilm", "resetFilm",
                "applyFilm", "saveLook", "importLook", "exportCube", "selfTest", "probe", "showLog"],
      status: () => ({ text: $("status").textContent, kind: $("status").className }),
      savedLooks: () => library.looks.map((l) => l.name),
      element: (id) => document.getElementById(id),
      analysis: () => analysisBox.textContent,
    });
  } catch (e) { /* release build */ }
})();
