// Spike: can a UXP panel make the LookMatch effect match to a reference?
// The panel writes the reference path to <app support>/PRO PLUGS/LookMatch/refs/<slot>.txt and
// sets the effect's "Reference Slot" param; the effect reacts in PF_Cmd_USER_CHANGED_PARAM.
const ppro = require("premierepro");
const uxp = require("uxp");
const fs = require("fs");
const os = require("os");

const MATCH_NAME = "PROPLUGS LookMatch";
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
    if ((await c.getMatchName()) === MATCH_NAME) return { chain, component: c };
  }
  return { chain, component: null };
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
      const names = [];
      for (let i = 0; i < n; i++) names.push(await (await chain.getComponentAtIndex(i)).getMatchName());
      log(`  ${await item.getName()}: ${names.join(", ")}`);
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
  fs.mkdirSync(refsDir(), { recursive: true });
  fs.writeFileSync(`${refsDir()}/${slot}.txt`, refPath);
  log(`reference ${refPath} -> slot ${slot}`);

  const { project, items } = await selectedTrackItems();
  if (!items.length) { log("select a clip on the timeline first"); return; }
  for (const item of items) {
    let { chain, component } = await findLookMatch(item);
    if (!component) {
      component = await ppro.VideoFilterFactory.createComponent(MATCH_NAME);
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
    if (!fs.existsSync(path)) { log(`no file at ${path}`); return; }
    await matchSelectedTo(path);
  } catch (e) { log("match failed:", e.message); }
});

log("ready");
