// Everything that talks to Premiere's `premierepro` UXP module. The module, fs and the data
// folder are injected so the logic can be tested in node with a mock (test/mock-premiere.js).

const { P, POPUPS } = require("./params.js");
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
    // Premiere 26.5: param.getStartValue() -> Keyframe { value: { value: <number|bool> } }.
    // (getValueAtTime needs a TickTime; never call it with a number, it crashes Premiere.)
    const unwrap = (v) => {
      for (let i = 0; i < 3 && v && typeof v === "object" && "value" in v; i++) v = v.value;
      return v;
    };
    for (const [name, p] of await this.paramMap(component)) {
      try {
        const getter = typeof p.getStartValue === "function" ? "getStartValue" : "getValue";
        const value = unwrap(await p[getter]());
        if (typeof value === "number") out[name] = POPUPS.has(name) ? value + 1 : value;
        else if (typeof value === "boolean") out[name] = value;
      } catch (e) { /* hidden data has no value */ }
    }
    return out;
  }

  // Sets params on LookMatch on every item (adding the effect where missing) as ONE undo step.
  // Premiere invalidates script objects across model changes, so the param objects and their
  // keyframes/actions are created inside the same locked access as the transaction.
  // `params` is an object, or a function (itemIndex) -> object for per-clip values.
  async applyParams(scope, params) {
    const { project, items } = await this.items(scope);
    const paramsFor = typeof params === "function" ? params : () => params;
    if (!items.length) throw new Error(scope === "sequence" ? "The sequence has no video clips." : "Select a video clip on the timeline.");
    let added = 0;
    const targets = [];
    for (const item of items) {
      const { added: wasAdded } = await this.ensureLookMatch(project, item);
      if (wasAdded) added++;
    }
    // fresh component/param objects after any additions
    for (const [i, item] of items.entries()) {
      const { component } = await this.findLookMatch(item);
      const map = await this.paramMap(component);
      const entries = Object.entries(paramsFor(i));
      for (const [name] of entries)
        if (!map.has(name)) throw new Error(`LookMatch has no "${name}" control (update the effect).`);
      targets.push({ map, entries });
    }
    if (targets.some((t) => t.entries.length)) {
      let stage = "transaction";
      try {
        this.transaction(project, "LookMatch", (ca) => {
          for (const { map, entries } of targets) {
            for (const [name, value] of entries) {
              const p = map.get(name);
              const v = POPUPS.has(name) ? Math.max(0, Math.round(value) - 1) : value;
              stage = `keyframe for ${name}`;
              const kf = p.createKeyframe(v);
              stage = `set-value action for ${name}`;
              ca.addAction(p.createSetValueAction(kf, true));
            }
          }
          stage = "commit";
        });
      } catch (e) {
        throw new Error(`${e.message} (at ${stage})`);
      }
    }
    return { clips: items.length, added };
  }

  // The effect can't take a string param, so the path goes in refs/<slot>.txt and the slot in a param.
  async writeReferenceSlot(path) {
    try { await this.fs.mkdir(this.refsDir, { recursive: true }); } catch (e) { /* exists */ }
    const slot = 1 + Math.floor(Math.random() * 999998);
    await this.fs.writeFile(joinPath(this.refsDir, `${slot}.txt`), path, { encoding: "utf-8" });
    return slot;
  }

  // Each clip gets its own slot, so its match is built from its own frame (a shared slot would be
  // built once, from whichever clip Premiere renders first, and reused for all of them).
  // `extraParams` is an object, or a function (itemIndex) -> object for per-clip values.
  // A match never applies film print emulation: Film Strength goes to 0 (older projects may have it on).
  async applyReference(scope, path, extraParams) {
    const { items } = await this.items(scope);
    if (!items.length) throw new Error(scope === "sequence" ? "The sequence has no video clips." : "Select a video clip on the timeline.");
    const slots = [];
    for (let i = 0; i < items.length; i++) slots.push(await this.writeReferenceSlot(path));
    const extra = typeof extraParams === "function" ? extraParams : () => extraParams || {};
    const res = await this.applyParams(scope, (i) => Object.assign({ [P.referenceSlot]: slots[i], [P.filmStrength]: 0 }, extra(i)));
    const names = await Promise.all(items.map((it) => it.getName()));
    return Object.assign({ slot: slots[0], slots, names }, res);
  }

  // The effect's analysis of a slot's match (refs/<slot>.json: source, reference and result), written
  // when Premiere first draws the clip. Waits up to timeoutMs; null if it hasn't appeared.
  async readAnalysis(slot, { timeoutMs = 0, pollMs = 300, sleep } = {}) {
    const file = joinPath(this.refsDir, `${slot}.json`);
    const wait = sleep || ((ms) => new Promise((r) => setTimeout(r, ms)));
    for (let t = 0; ; t += pollMs) {
      try { return JSON.parse(String(await this.fs.readFile(file, { encoding: "utf-8" }))); } catch (e) { /* not yet */ }
      if (t >= timeoutMs) return null;
      await wait(pollMs);
    }
  }

  // What Premiere knows about each clip's colour (colour space, input LUT, media path, names), for
  // picking the Source Format from metadata. Every call is optional: builds differ in what they offer.
  async clipColourHints(scope) {
    const { items } = await this.items(scope);
    const out = [];
    for (const item of items) {
      const texts = [];
      try { texts.push(await item.getName()); } catch (e) { /* none */ }
      try {
        let pi = typeof item.getProjectItem === "function" ? await item.getProjectItem() : null;
        const C = this.ppro.ClipProjectItem;
        if (pi && C && typeof C.cast === "function") { try { pi = C.cast(pi); } catch (e) { /* keep */ } }
        for (const m of ["getColorSpace", "getOriginalColorSpace", "getInputLUTID", "getEmbeddedLUTID", "getMediaFilePath", "getName"]) {
          if (!pi || typeof pi[m] !== "function") continue;
          try {
            const v = await pi[m]();
            if (v) texts.push(typeof v === "string" ? v : JSON.stringify(v));
          } catch (e) { /* not offered */ }
        }
      } catch (e) { /* no project item */ }
      out.push(texts);
    }
    return out;
  }

  // Sets a "request slot" param on the first selected clip's LookMatch after writing the request
  // file, then waits for the effect's answer in refs/<kind>-<slot>.done (written on its next render).
  async effectRequest(kind, paramName, requestText, { timeoutMs = 15000, pollMs = 250, sleep } = {}) {
    const { project, items } = await this.selectedVideoItems();
    if (!items.length) throw new Error("Select a video clip on the timeline.");
    try { await this.fs.mkdir(this.refsDir, { recursive: true }); } catch (e) { /* exists */ }
    const slot = 1 + Math.floor(Math.random() * 999998);
    const base = joinPath(this.refsDir, `${kind}-${slot}`);
    if (requestText !== null) await this.fs.writeFile(`${base}.txt`, requestText, { encoding: "utf-8" });
    await this.ensureLookMatch(project, items[0]);
    const { component } = await this.findLookMatch(items[0]);
    const p = (await this.paramMap(component)).get(paramName);
    if (!p) throw new Error(`This LookMatch build has no "${paramName}" (update the effect).`);
    this.transaction(project, `LookMatch: ${kind}`, (ca) => ca.addAction(p.createSetValueAction(p.createKeyframe(slot), true)));
    const wait = sleep || ((ms) => new Promise((r) => setTimeout(r, ms)));
    for (let t = 0; t < timeoutMs; t += pollMs) {
      let result = null;
      try { result = String(await this.fs.readFile(`${base}.done`, { encoding: "utf-8" })).trim(); } catch (e) { /* not yet */ }
      if (result !== null) {
        if (result !== "ok") throw new Error(result);
        return { slot, base, clip: await items[0].getName() };
      }
      await wait(pollMs);
    }
    throw new Error("No answer from the effect. Put the playhead over the clip so Premiere renders it, then try again.");
  }

  // Asks the effect on the first selected clip to write its current look as a .cube.
  async exportCube(target, opts) {
    if (!/\.cube$/i.test(target)) throw new Error("The LUT path must end in .cube");
    const res = await this.effectRequest("export", P.exportSlot, target, opts);
    return { slot: res.slot, path: target };
  }

  // Saves the first selected clip's ungraded frame (at the playhead) as a PNG to use as a reference.
  async captureFrame(opts) {
    const res = await this.effectRequest("capture", P.captureSlot, null, opts);
    return { slot: res.slot, path: `${res.base}.png`, clip: res.clip };
  }

  async selectedLookMatchParams() {
    const { items } = await this.selectedVideoItems();
    for (const item of items) {
      const { component } = await this.findLookMatch(item);
      if (component) return { name: await item.getName(), params: await this.readParams(component) };
    }
    return null;
  }

  // Selects the index-th video clip of the sequence (for scripted tests). Premiere builds differ in
  // the selection API, so try the known shapes and report what worked.
  async selectClip(index) {
    const { project, sequence, items } = await this.allVideoItems();
    const item = items[index];
    if (!item) throw new Error(`no video clip #${index} (sequence has ${items.length})`);
    const T = this.ppro.TrackItemSelection;
    const tries = [];
    if (T && typeof T.createEmptySelection === "function") {
      tries.push(["TrackItemSelection.createEmptySelection + addItem + sequence.setSelection", async () => {
        const sel = await T.createEmptySelection();
        await sel.addItem(item, true);
        await sequence.setSelection(sel);
      }]);
    }
    if (typeof sequence.getSelection === "function") {
      tries.push(["sequence.getSelection(): remove others, addItem", async () => {
        const sel = await sequence.getSelection();
        if (typeof sel.clearSelection === "function") await sel.clearSelection();
        else if (typeof sel.removeItem === "function")
          for (const other of await sel.getTrackItems()) await sel.removeItem(other);
        await sel.addItem(item, true);
        if (sequence.setSelection) await sequence.setSelection(sel);
      }]);
    }
    const errors = [];
    for (const [name, fn] of tries) {
      try {
        await fn();
        const now = await (await sequence.getSelection()).getTrackItems();
        const names = await Promise.all(now.map((i) => i.getName()));
        if (now.length === 1) return { selected: await item.getName(), via: name };
        errors.push(`${name}: selection is [${names.join(", ")}]`);
      } catch (e) { errors.push(`${name}: ${e.message}`); }
    }
    throw new Error(`could not select: ${errors.join(" | ") || "no selection API"}`);
  }

  // Read-only debugging aid: what the param objects of LookMatch on the selection look like.
  async inspectParams(limit) {
    const { items } = await this.selectedVideoItems();
    if (!items.length) throw new Error("nothing selected");
    const { component } = await this.findLookMatch(items[0]);
    if (!component) throw new Error("no LookMatch on the selection");
    const describe = (v) => {
      if (v === null || typeof v !== "object") return { type: typeof v, value: v };
      const keys = [];
      for (const k in v) keys.push(k);
      let proto = [];
      try { proto = Object.getOwnPropertyNames(Object.getPrototypeOf(v)); } catch (e) { /* ignore */ }
      return { type: "object", ctor: v.constructor && v.constructor.name, keys, proto, value: v.value };
    };
    const out = [];
    const n = await component.getParamCount();
    for (let i = 0; i < Math.min(n, limit || 8); i++) {
      const p = await component.getParam(i);
      const entry = { i, name: p.displayName, param: describe(p) };
      // Only argument-less getters: getValueAtTime(<number>) crashes Premiere 26.5 (it needs a TickTime).
      for (const m of ["getValue", "getStartValue"]) {
        if (typeof p[m] !== "function") continue;
        try { entry[m] = describe(await p[m]()); } catch (e) { entry[m] = { error: e.message }; }
      }
      out.push(entry);
    }
    return out;
  }

  // Read-only: video clips with start/end (seconds) and the playhead position.
  async timeline() {
    const { sequence, items } = await this.allVideoItems();
    const secs = async (fn) => {
      try { const t = await fn(); return t && typeof t.seconds === "number" ? Math.round(t.seconds * 1000) / 1000 : null; }
      catch (e) { return null; }
    };
    const clips = [];
    for (const [i, item] of items.entries()) {
      clips.push({ i, name: await item.getName(),
                   start: await secs(() => item.getStartTime()), end: await secs(() => item.getEndTime()) });
    }
    const playhead = await secs(() => sequence.getPlayerPosition());
    return { playhead, clips };
  }

  // Moves the playhead (seconds). Needs a TickTime: Premiere crashes on raw numbers for time args.
  async setPlayhead(seconds) {
    const T = this.ppro.TickTime;
    if (!T || typeof T.createWithSeconds !== "function") throw new Error("TickTime.createWithSeconds unavailable");
    const { sequence } = await this.context();
    if (typeof sequence.setPlayerPosition !== "function") throw new Error("setPlayerPosition unavailable");
    await sequence.setPlayerPosition(T.createWithSeconds(Number(seconds)));
    const now = await sequence.getPlayerPosition();
    return { playhead: now && typeof now.seconds === "number" ? now.seconds : null };
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
