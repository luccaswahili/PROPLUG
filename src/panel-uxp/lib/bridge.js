// Everything that talks to Premiere's `premierepro` UXP module. The module, fs and the data
// folder are injected so the logic can be tested in node with a mock (test/mock-premiere.js).

const { P } = require("./looks.js");
const { joinPath } = require("./library.js");

const LOOKMATCH = "PROPLUGS LookMatch";
// Premiere lists After Effects-API effects as "AE.<match name>" in a component chain.
const isLookMatch = (matchName) => !!matchName && matchName.endsWith(LOOKMATCH);

class Bridge {
  constructor(ppro, fs, dataDir) {
    this.ppro = ppro;
    this.fs = fs;
    this.refsDir = joinPath(dataDir, "refs");
  }

  async context() {
    const project = await this.ppro.Project.getActiveProject();
    if (!project) throw new Error("Open a project first.");
    const sequence = await project.getActiveSequence();
    if (!sequence) throw new Error("Open a sequence first.");
    return { project, sequence };
  }

  // Video clips only: a selected clip brings its audio half along, which can't take a video effect.
  async isVideoItem(item) {
    const chain = await item.getComponentChain();
    const n = await chain.getComponentCount();
    for (let i = 0; i < n; i++) {
      const name = await (await chain.getComponentAtIndex(i)).getMatchName();
      if (name === "AE.ADBE Motion" || name === "AE.ADBE Opacity") return true;
    }
    return false;
  }

  async selectedVideoItems() {
    const { project, sequence } = await this.context();
    const selection = await sequence.getSelection();
    const items = [];
    for (const item of await selection.getTrackItems()) if (await this.isVideoItem(item)) items.push(item);
    return { project, sequence, items };
  }

  async allVideoItems() {
    const { project, sequence } = await this.context();
    const items = [];
    const tracks = await sequence.getVideoTrackCount();
    const clipType = this.ppro.Constants && this.ppro.Constants.TrackItemType ? this.ppro.Constants.TrackItemType.CLIP : 1;
    for (let t = 0; t < tracks; t++) {
      const track = await sequence.getVideoTrack(t);
      for (const item of await track.getTrackItems(clipType, false)) items.push(item);
    }
    return { project, sequence, items };
  }

  async items(scope) {
    return scope === "sequence" ? this.allVideoItems() : this.selectedVideoItems();
  }

  async findLookMatch(item) {
    const chain = await item.getComponentChain();
    const n = await chain.getComponentCount();
    for (let i = 0; i < n; i++) {
      const c = await chain.getComponentAtIndex(i);
      if (isLookMatch(await c.getMatchName())) return { chain, component: c };
    }
    return { chain, component: null };
  }

  transaction(project, label, fill) {
    let ok = false;
    project.lockedAccess(() => {
      ok = project.executeTransaction((compound) => fill(compound), label);
    });
    if (ok === false) throw new Error(`Premiere refused: ${label}`);
  }

  async ensureLookMatch(project, item) {
    let { chain, component } = await this.findLookMatch(item);
    if (component) return { component, added: false };
    const created = await this.ppro.VideoFilterFactory.createComponent("AE." + LOOKMATCH);
    this.transaction(project, "Add LookMatch", (ca) => ca.addAction(chain.createAppendComponentAction(created)));
    ({ component } = await this.findLookMatch(item));
    if (!component) throw new Error("LookMatch could not be added (is the effect installed?)");
    return { component, added: true };
  }

  async paramMap(component) {
    const map = new Map();
    const n = await component.getParamCount();
    for (let i = 0; i < n; i++) {
      const p = await component.getParam(i);
      if (p && p.displayName) map.set(p.displayName, p);
    }
    return map;
  }

  async readParams(component) {
    const out = {};
    for (const [name, p] of await this.paramMap(component)) {
      try {
        const v = await p.getValue();
        const value = v && typeof v === "object" && "value" in v ? v.value : v;
        if (typeof value === "number" || typeof value === "boolean") out[name] = value;
      } catch (e) { /* buttons, topics and hidden data have no value */ }
    }
    return out;
  }

  // Sets params on LookMatch on every item (adding the effect where missing) as ONE undo step.
  async applyParams(scope, params) {
    const { project, items } = await this.items(scope);
    if (!items.length) throw new Error(scope === "sequence" ? "The sequence has no video clips." : "Select a video clip on the timeline.");
    const actions = [];
    let added = 0;
    for (const item of items) {
      const { component, added: wasAdded } = await this.ensureLookMatch(project, item);
      if (wasAdded) added++;
      const map = await this.paramMap(component);
      for (const [name, value] of Object.entries(params)) {
        const p = map.get(name);
        if (!p) throw new Error(`LookMatch has no "${name}" control (update the effect).`);
        actions.push(p.createSetValueAction(p.createKeyframe(value), true));
      }
    }
    if (actions.length) this.transaction(project, "LookMatch", (ca) => actions.forEach((a) => ca.addAction(a)));
    return { clips: items.length, added };
  }

  // The effect can't take a string param, so the path goes in refs/<slot>.txt and the slot in a param.
  async writeReferenceSlot(path) {
    try { await this.fs.mkdir(this.refsDir, { recursive: true }); } catch (e) { /* exists */ }
    const slot = 1 + Math.floor(Math.random() * 999998);
    await this.fs.writeFile(joinPath(this.refsDir, `${slot}.txt`), path, { encoding: "utf-8" });
    return slot;
  }

  async applyReference(scope, path, extraParams) {
    const slot = await this.writeReferenceSlot(path);
    const res = await this.applyParams(scope, Object.assign({ [P.referenceSlot]: slot }, extraParams || {}));
    return Object.assign({ slot }, res);
  }

  // Asks the effect on the first selected clip to write its current look as a .cube. The effect
  // does it on its next render of that clip and reports in refs/export-<slot>.done.
  async exportCube(target, { timeoutMs = 15000, pollMs = 250, sleep } = {}) {
    if (!/\.cube$/i.test(target)) throw new Error("The LUT path must end in .cube");
    const { project, items } = await this.selectedVideoItems();
    if (!items.length) throw new Error("Select the clip whose look you want to export.");
    try { await this.fs.mkdir(this.refsDir, { recursive: true }); } catch (e) { /* exists */ }
    const slot = 1 + Math.floor(Math.random() * 999998);
    const base = joinPath(this.refsDir, `export-${slot}`);
    await this.fs.writeFile(`${base}.txt`, target, { encoding: "utf-8" });
    const { component } = await this.ensureLookMatch(project, items[0]);
    const p = (await this.paramMap(component)).get(P.exportSlot);
    if (!p) throw new Error("This LookMatch build can't export from the panel (update the effect).");
    this.transaction(project, "LookMatch: export LUT", (ca) => ca.addAction(p.createSetValueAction(p.createKeyframe(slot), true)));
    const wait = sleep || ((ms) => new Promise((r) => setTimeout(r, ms)));
    for (let t = 0; t < timeoutMs; t += pollMs) {
      try {
        const result = String(await this.fs.readFile(`${base}.done`, { encoding: "utf-8" })).trim();
        if (result !== "ok") throw new Error(result);
        return { slot, path: target };
      } catch (e) {
        if (e.code !== "ENOENT" && !/no such file|not found|ENOENT|cannot find/i.test(e.message)) throw e;
      }
      await wait(pollMs);
    }
    throw new Error("No answer from the effect. Put the playhead over the clip so Premiere renders it, then try again.");
  }

  async selectedLookMatchParams() {
    const { items } = await this.selectedVideoItems();
    for (const item of items) {
      const { component } = await this.findLookMatch(item);
      if (component) return { name: await item.getName(), params: await this.readParams(component) };
    }
    return null;
  }

  async clipNames(scope) {
    const { items } = await this.items(scope);
    return Promise.all(items.map((i) => i.getName()));
  }

  async saveProject() {
    const { project } = await this.context();
    if (typeof project.save !== "function") throw new Error("this Premiere version can't save from a panel");
    await project.save();
  }
}

module.exports = { Bridge, isLookMatch, LOOKMATCH };
