// LookMatch panel for Premiere Pro (UXP). Drives the LookMatch effect: reference matching,
// film looks, film controls and saved looks. All Premiere access goes through lib/bridge.js.

const ppro = require("premierepro");
const uxp = require("uxp");
const fs = require("fs");
const os = require("os");
const { P, NEGATIVES, PRINTS, FILM_DEFAULTS, FILM_LOOKS, lookParams, stockCombos, guessEncoding } = require("./lib/looks.js");
const { Library, joinPath, baseName } = require("./lib/library.js");
const { Bridge } = require("./lib/bridge.js");
const { runSelfTest } = require("./lib/selftest.js");

const VERSION = "0.5.0";  // kept equal to manifest.json by build_panel.sh
const DATA_DIR = os.platform() === "win32"
  ? joinPath(process.env.APPDATA || "", "PRO PLUGS\\LookMatch")
  : joinPath(os.homedir(), "Library/Application Support/PRO PLUGS/LookMatch");
const LOG_FILE = joinPath(os.homedir(), "Library/Logs/LookMatch.log");

const $ = (id) => document.getElementById(id);
const library = new Library(fs, DATA_DIR);
const bridge = new Bridge(ppro, fs, DATA_DIR);
let selectedRef = null;

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
    div.className = "thumb" + (selectedRef && selectedRef.id === ref.id ? " sel" : "");
    div.title = ref.path;
    div.innerHTML = `<img alt=""><div class="name"><span class="x" title="Remove">×</span></div>`;
    div.querySelector(".name").appendChild(document.createTextNode(ref.name));
    div.addEventListener("click", (ev) => {
      if (ev.target.classList.contains("x")) {
        library.removeReference(ref.id);
        if (selectedRef && selectedRef.id === ref.id) selectedRef = null;
        library.save().then(renderRefs);
        return;
      }
      selectedRef = ref;
      renderRefs();
    });
    grid.appendChild(div);
    thumbnailUrl(ref.path).then((url) => { if (url) div.querySelector("img").src = url; });
  }
}

async function addReference(path) {
  path = String(path || "").trim().replace(/^file:\/\//, "");
  if (!path) throw new Error("Enter or drop an image path.");
  try { await fs.lstat(path); } catch (e) { throw new Error(`No file at ${path}`); }
  selectedRef = library.addReference(path);
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

function matchSettings() {
  return {
    [P.matchStrength]: Number($("strength").value),
    [P.regionMatching]: Number($("region").value),
    [P.autoSkin]: !!$("autoSkin").checked,
  };
}

async function matchClips(scope) {
  if (!selectedRef) throw new Error("Pick a reference first (add one above and click it).");
  const params = matchSettings();
  if ($("detectLog").checked) {
    // one encoding for all clips in the operation: take it from the first clip's name
    const names = await bridge.clipNames(scope);
    const enc = guessEncoding(names[0]);
    if (enc !== 1) { params[P.footageEncoding] = enc; log(`detected log footage in "${names[0]}" (encoding ${enc})`); }
  }
  const res = await bridge.applyReference(scope, selectedRef.path, params);
  log(`matched ${res.clips} clip(s) to ${selectedRef.name} (slot ${res.slot}${res.added ? `, added LookMatch to ${res.added}` : ""})`);
  log("tip: to keep this match inside the project (for other Macs), click LookMatch > Panel Link > Save into Project in Effect Controls");
  return `Matched ${res.clips} clip(s) to ${selectedRef.name}`;
}

// Shot matching: grab the selected (hero) clip's frame through the effect, add it to the library.
async function captureHero() {
  const res = await bridge.captureFrame();
  selectedRef = library.addReference(res.path);
  selectedRef.name = `Hero: ${res.clip}`;
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
  const msg = await matchClips("sequence");
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
  const on = !cur.params[P.split];
  await bridge.applyParams("selection", { [P.split]: on, [P.splitPos]: Number($("splitPos").value) });
  return on ? "Split view on: original on the left" : "Split view off";
}));
bindSlider("splitPos", "splitPosV");
$("splitPos").addEventListener("change", () => run("Split position", async () => {
  await bridge.applyParams("selection", { [P.splitPos]: Number($("splitPos").value) });
  return `Split at ${Math.round($("splitPos").value)}%`;
}));

// ---------- looks ----------

// A rough swatch per look so cards are recognisable without rendering thumbnails.
function swatchFor(params) {
  const neg = params[P.negative] || 2, print = params[P.print] || 2;
  const warm = [0, 18, 6, -6, 10, 0, -14, 0][neg] + [0, 0, 14, -16, 4, 8][print];
  const sat = neg === 7 ? 0 : 0.45 + (params[P.filmSaturation] === undefined ? 0.2 : (params[P.filmSaturation] - 80) / 300);
  const fade = (params[P.fade] || 0) / 100;
  const hue = 200 - warm * 4;
  const l1 = 18 + fade * 20, l2 = 62 - fade * 10;
  return `linear-gradient(90deg, hsl(${hue + 160},${sat * 100}%,${l1}%), hsl(${hue + 120},${sat * 70}%,${(l1 + l2) / 2}%), hsl(${hue},${sat * 90}%,${l2}%))`;
}

function lookCard(look, onClick) {
  const card = document.createElement("div");
  card.className = "card";
  card.dataset.look = look.id;
  card.innerHTML = `<div class="swatch"></div><div class="name"></div>`;
  card.querySelector(".swatch").style.background = swatchFor(look.params);
  card.querySelector(".name").textContent = look.name;
  card.title = `Apply "${look.name}" to the selected clips`;
  card.addEventListener("click", onClick);
  return card;
}

const intensity = () => Number($("intensity").value) / 100;
bindSlider("intensity", "intensityV");

function applyLook(look) {
  return run(`Apply ${look.name}`, async () => {
    const params = lookParams(look, intensity());
    setFilmControls(params);
    const res = await bridge.applyParams("selection", params);
    return `${look.name} on ${res.clips} clip(s)`;
  });
}

function renderLooks() {
  const grid = $("lookGrid");
  for (const look of FILM_LOOKS) grid.appendChild(lookCard(look, () => applyLook(look)));
  const combos = $("comboGrid");
  for (const look of stockCombos()) combos.appendChild(lookCard(look, () => applyLook(look)));
}

// Real previews: the effect renders the selected clip's frame through every look.
$("previewLooks").addEventListener("click", () => run("Preview looks", async () => {
  const looks = FILM_LOOKS.concat(stockCombos()).map((l) => ({ id: l.id, params: lookParams(l, intensity()) }));
  const paths = await bridge.lookThumbnails(looks, { timeoutMs: 30000 });
  let shown = 0;
  for (const card of document.querySelectorAll(".card[data-look]")) {
    const url = await thumbnailUrl(paths[card.dataset.look]);
    if (!url) continue;
    const sw = card.querySelector(".swatch");
    let img = sw.querySelector("img");
    if (!img) { img = document.createElement("img"); sw.appendChild(img); }
    img.src = url;
    sw.classList.add("preview");
    shown++;
  }
  return `Previewed ${shown} looks on the selected clip`;
}));

// ---------- film controls ----------

const FILM_CONTROLS = [
  [P.filmStrength, 0, 100, 1, "%"], [P.negative, NEGATIVES], [P.print, PRINTS],
  [P.printerR, -12, 12, 0.5, ""], [P.printerG, -12, 12, 0.5, ""], [P.printerB, -12, 12, 0.5, ""],
  [P.filmSaturation, 0, 200, 1, "%"], [P.fade, 0, 100, 1, "%"],
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
    if (Array.isArray(a)) {
      const picker = document.createElement("sp-picker");
      picker.setAttribute("size", "s");
      const menu = document.createElement("sp-menu");
      menu.setAttribute("slot", "options");
      a.forEach((opt, i) => {
        const item = document.createElement("sp-menu-item");
        item.textContent = opt;
        item.value = String(i + 1);
        menu.appendChild(item);
      });
      picker.appendChild(menu);
      row.appendChild(picker);
      filmInputs.set(name, { get: () => (picker.selectedIndex >= 0 ? picker.selectedIndex + 1 : FILM_DEFAULTS[name]),
                             set: (v) => { picker.selectedIndex = Math.max(0, Math.round(v) - 1); } });
    } else {
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
  setFilmControls(FILM_DEFAULTS);
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
  return `Read ${cur.name}`;
}));
$("resetFilm").addEventListener("click", () => setFilmControls(FILM_DEFAULTS));
$("applyFilm").addEventListener("click", () => run("Apply film", async () => {
  const res = await bridge.applyParams("selection", filmParams());
  return `Film settings on ${res.clips} clip(s)`;
}));
$("applyFilmSeq").addEventListener("click", () => run("Apply film to sequence", async () => {
  const res = await bridge.applyParams("sequence", filmParams());
  return `Film settings on ${res.clips} clip(s)`;
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
      const res = await bridge.applyReference("selection", look.reference, look.params);
      return `${look.name} (matched to ${baseName(look.reference)}) on ${res.clips} clip(s)`;
    }
    const res = await bridge.applyParams("selection", look.params);
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
  const params = Object.assign({}, filmParams(), matchSettings());
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
  renderLooks();
  renderFilmControls();
  await library.load();
  selectedRef = library.refs[0] || null;
  await renderRefs();
  renderSaved();
  log(`LookMatch panel ${VERSION} ready (data in ${DATA_DIR})`);
  try {
    // dev builds only: lib/agent.js is left out of release packages
    require("./lib/agent.js").startAgent(fs, DATA_DIR, bridge, log, {
      // UI driving for tests: only these elements, only set value / click
      fields: ["refPath", "lookName", "lookFile", "cubePath"],
      buttons: ["addRef", "matchSel", "matchSeq", "applyStrength", "beforeAfter", "heroRef", "readFilm", "resetFilm",
                "applyFilm", "saveLook", "importLook", "exportCube", "selfTest", "previewLooks", "probe", "showLog"],
      status: () => ({ text: $("status").textContent, kind: $("status").className }),
      savedLooks: () => library.looks.map((l) => l.name),
      element: (id) => document.getElementById(id),
    });
  } catch (e) { /* release build */ }
})();
