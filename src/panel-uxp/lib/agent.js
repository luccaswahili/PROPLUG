// DEV ONLY - not included in release packages (see build_panel.sh --release).
// Lets the developer drive the panel's own operations from outside Premiere for automated tests:
// while <data>/agent/enabled exists, the panel polls <data>/agent/cmd.json and runs ONE command
// from the fixed whitelist below, then writes <data>/agent/result.json. No eval, no scripts.

const { joinPath } = require("./library.js");
const { FILM_LOOKS, lookParams } = require("./looks.js");
const { runSelfTest } = require("./selftest.js");

const COMMANDS = {
  ping: async () => "pong",
  clipNames: async (b, a) => b.clipNames(a.scope),
  readSelected: async (b) => b.selectedLookMatchParams(),
  applyReference: async (b, a) => b.applyReference(a.scope, String(a.path)),
  applyParams: async (b, a) => {
    const params = {};
    for (const [k, v] of Object.entries(a.params || {}))
      if (typeof v === "number" || typeof v === "boolean") params[String(k)] = v;
    return b.applyParams(a.scope, params);
  },
  saveProject: async (b) => { await b.saveProject(); return "saved"; },
  exportCube: async (b, a) => b.exportCube(String(a.path)),
  captureFrame: async (b) => b.captureFrame(),
  selectClip: async (b, a) => b.selectClip(Number(a.index) || 0),
  inspectParams: async (b, a) => b.inspectParams(Number(a.limit) || 8),
  timeline: async (b) => b.timeline(),
  setPlayhead: async (b, a) => b.setPlayhead(Number(a.seconds) || 0),
  selfTest: async (b, a) => runSelfTest(b, { referencePath: a.referencePath ? String(a.referencePath) : null }),
  previewLooks: async (b) => b.lookThumbnails(FILM_LOOKS.map((l) => ({ id: l.id, params: lookParams(l) })), { timeoutMs: 30000 }),
};

function startAgent(fs, dataDir, bridge, log) {
  const dir = joinPath(dataDir, "agent");
  let lastId = null;
  let busy = false;
  const tick = async () => {
    if (busy) return;
    busy = true;
    try {
      try { await fs.lstat(joinPath(dir, "enabled")); } catch (e) { return; }
      let cmd;
      try { cmd = JSON.parse(await fs.readFile(joinPath(dir, "cmd.json"), { encoding: "utf-8" })); } catch (e) { return; }
      if (!cmd || cmd.id === undefined || cmd.id === lastId) return;
      lastId = cmd.id;
      const fn = Object.prototype.hasOwnProperty.call(COMMANDS, cmd.name) ? COMMANDS[cmd.name] : null;
      let out;
      try {
        if (!fn) throw new Error(`unknown command ${cmd.name}`);
        out = { id: cmd.id, ok: true, result: await fn(bridge, cmd.args || {}) };
      } catch (e) {
        out = { id: cmd.id, ok: false, error: e.message };
      }
      out.at = new Date().toISOString();
      await fs.writeFile(joinPath(dir, "result.json"), JSON.stringify(out, null, 2), { encoding: "utf-8" });
      log(`agent: ${cmd.name} -> ${out.ok ? "ok" : out.error}`);
    } finally {
      busy = false;
    }
  };
  return setInterval(tick, 1000);
}

module.exports = { startAgent, COMMANDS };
