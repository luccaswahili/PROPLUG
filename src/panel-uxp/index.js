// Spike: can a UXP panel make the LookMatch effect match to a reference?
// The panel writes the reference path to <app support>/PRO PLUGS/LookMatch/refs/<slot>.txt and
// sets the effect's "Reference Slot" param; the effect reacts in PF_Cmd_USER_CHANGED_PARAM.
const ppro = require("premierepro");
const uxp = require("uxp");
const fs = require("fs");
const os = require("os");

const MATCH_NAME = "PROPLUGS LookMatch";
// Premiere prefixes After Effects-API effects with "AE." in the component chain
const isLookMatch = (matchName) => !!matchName && matchName.endsWith(MATCH_NAME);
const logEl = document.getElementById("log");
const log = (...a) => { logEl.textContent += a.join(" ") + "\n"; console.log(...a); };

function refsDir() {
  return os.platform() === "win32"
    ? `${process.env.APPDATA}\\PRO PLUGS\\LookMatch\\refs`
    : `${os.homedir()}/Library/Application Support/PRO PLUGS/LookMatch/refs`;
}

async function selectedTrackItems() {
  const project = await ppro.Project.getActiveProject();
  if (!project) throw new Error("no project open");
  const sequence = await project.getActiveSequence();
  if (!sequence) throw new Error("no active sequence");
  const selection = await sequence.getSelection();
  const items = await selection.getTrackItems();
  return { project, sequence, items };
}

async function findLookMatch(item) {
  const chain = await item.getComponentChain();
  const n = await chain.getComponentCount();
  for (let i = 0; i < n; i++) {
    const c = await chain.getComponentAtIndex(i);
    if (isLookMatch(await c.getMatchName())) return { chain, component: c };
  }
  return { chain, component: null };
}

async function isVideoItem(item) {
  const chain = await item.getComponentChain();
  const n = await chain.getComponentCount();
  for (let i = 0; i < n; i++) {
    const name = await (await chain.getComponentAtIndex(i)).getMatchName();
    if (name === "AE.ADBE Motion" || name === "AE.ADBE Opacity") return true;
  }
  return false;
}

async function findParam(component, name) {
  const n = await component.getParamCount();
  for (let i = 0; i < n; i++) {
    const p = await component.getParam(i);
    if (p.displayName === name) return p;
  }
  return null;
}

document.getElementById("probe").addEventListener("click", async () => {
  try {
    const { items } = await selectedTrackItems();
    log(`selected track items: ${items.length}`);
    for (const item of items) {
      const { chain, component } = await findLookMatch(item);
      const n = await chain.getComponentCount();
      log(`  ${await item.getName()} (${item.constructor && item.constructor.name}): ${n} components`);
      for (let i = 0; i < n + 4; i++) {
        try {
          const c = await chain.getComponentAtIndex(i);
          if (!c) { log(`    [${i}] (none)`); continue; }
          log(`    [${i}] ${await c.getMatchName()} / ${await c.getDisplayName()}`);
        } catch (e) { log(`    [${i}] error: ${e.message}`); }
      }
      if (component) {
        const pc = await component.getParamCount();
        for (let i = 0; i < pc; i++) {
          const p = await component.getParam(i);
          let v; try { v = await p.getValue(); } catch (e) { v = `(${e.message})`; }
          log(`    [${i}] ${p.displayName} = ${JSON.stringify(v)}`);
        }
      }
    }
  } catch (e) { log("probe failed:", e.message); }
});

async function matchSelectedTo(refPath) {
  const slot = 1 + Math.floor(Math.random() * 999998);
  try { await fs.mkdir(refsDir(), { recursive: true }); } catch (e) { /* already there */ }
  await fs.writeFile(`${refsDir()}/${slot}.txt`, refPath, { encoding: "utf-8" });
  log(`reference ${refPath} -> slot ${slot}`);

  const { project, items } = await selectedTrackItems();
  if (!items.length) { log("select a clip on the timeline first"); return; }
  for (const item of items) {
    // a selected clip brings its audio half along; effects only go on the video part
    if (!(await isVideoItem(item))) continue;
    let { chain, component } = await findLookMatch(item);
    if (!component) {
      component = await ppro.VideoFilterFactory.createComponent("AE." + MATCH_NAME);
      project.lockedAccess(() => project.executeTransaction((ca) => {
        ca.addAction(chain.createAppendComponentAction(component));
      }, "Add LookMatch"));
      ({ component } = await findLookMatch(item));
      log("  added LookMatch");
    }
    const param = await findParam(component, "Reference Slot");
    if (!param) { log("  no Reference Slot param (old LookMatch build?)"); continue; }
    const kf = param.createKeyframe(slot);
    project.lockedAccess(() => project.executeTransaction((ca) => {
      ca.addAction(param.createSetValueAction(kf, true));
    }, "LookMatch: match to reference"));
    log(`  set Reference Slot = ${slot} on ${await item.getName()}`);
  }
}

document.getElementById("match").addEventListener("click", async () => {
  try {
    const file = await uxp.storage.localFileSystem.getFileForOpening({ types: ["png", "jpg", "jpeg", "bmp", "tga"] });
    if (file) await matchSelectedTo(file.nativePath);
  } catch (e) { log("match failed:", e.message); }
});

document.getElementById("usePath").addEventListener("click", async () => {
  try {
    const path = document.getElementById("path").value.trim();
    try { await fs.lstat(path); } catch (e) { log(`no file at ${path}`); return; }
    await matchSelectedTo(path);
  } catch (e) { log("match failed:", e.message); }
});

log("ready");
