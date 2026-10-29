// Minimal in-memory stand-in for Premiere's `premierepro` UXP module, shaped like the calls the
// panel makes (async getters, component chains, keyframe/set-value actions, transactions).

function makeParam(name, value) {
  return {
    displayName: name,
    value,
    // like Premiere 26.5: a Keyframe whose value is { value }
    async getStartValue() {
      if (this.value === undefined) throw new Error("no value");
      return { value: { value: this.value }, position: 0 };
    },
    createKeyframe(v) { return { v }; },
    createSetValueAction(kf) { return { apply: () => { this.value = kf.v; } }; },
  };
}

function makeComponent(matchName, params) {
  const list = Object.entries(params).map(([n, v]) => makeParam(n, v));
  return {
    matchName,
    list,
    async getMatchName() { return matchName; },
    async getDisplayName() { return matchName; },
    async getParamCount() { return list.length; },
    async getParam(i) { return list[i]; },
  };
}

function lookMatchComponent() {
  const { FILM_DEFAULTS, P } = require("../lib/looks.js");
  // popups are 0-based in Premiere's UXP API (FILM_DEFAULTS are 1-based like Effect Controls)
  const zeroBased = {};
  for (const [k, v] of Object.entries(FILM_DEFAULTS)) zeroBased[k] = [P.negative, P.print].includes(k) ? v - 1 : v;
  return makeComponent("AE.PROPLUGS LookMatch", Object.assign({
    "Look Match": undefined, Reference: undefined, [P.matchStrength]: 100, [P.regionMatching]: 100,
    [P.autoSkin]: true, [P.skinProtect]: 30, [P.footageEncoding]: 0, [P.exposure]: 0, [P.temperature]: 0, [P.tint]: 0,
    [P.mix]: 100, [P.referenceSlot]: 0, [P.exportSlot]: 0, [P.captureSlot]: 0,
  }, zeroBased));
}

function makeItem(name, video, withLookMatch) {
  const comps = video ? [makeComponent("AE.ADBE Opacity", {}), makeComponent("AE.ADBE Motion", {})]
                      : [makeComponent("Internal Volume Stereo", {})];
  if (withLookMatch) comps.push(lookMatchComponent());
  const chain = {
    comps,
    async getComponentCount() { return comps.length; },
    async getComponentAtIndex(i) { return comps[i]; },
    createAppendComponentAction(c) { return { apply: () => comps.push(c) }; },
  };
  return { name, chain, async getName() { return name; }, async getComponentChain() { return chain; } };
}

function makePremiere({ selected = [], tracks = [[]] } = {}) {
  const log = [];
  const project = {
    saved: 0,
    lockedAccess(fn) { fn(); },
    executeTransaction(fill, label) {
      const actions = [];
      fill({ addAction: (a) => actions.push(a) });
      actions.forEach((a) => a.apply());
      log.push({ label, count: actions.length });
      return true;
    },
    async getActiveSequence() { return sequence; },
    async save() { this.saved++; },
  };
  const sequence = {
    async getSelection() { return { async getTrackItems() { return selected; } }; },
    async getVideoTrackCount() { return tracks.length; },
    async getVideoTrack(i) { return { async getTrackItems() { return tracks[i]; } }; },
  };
  return {
    log,
    project,
    Project: { async getActiveProject() { return project; } },
    Constants: { TrackItemType: { CLIP: 1 } },
    VideoFilterFactory: { async createComponent(name) {
      if (name !== "AE.PROPLUGS LookMatch") throw new Error("Invalid parameter.");
      return lookMatchComponent();
    } },
  };
}

module.exports = { makePremiere, makeItem };
