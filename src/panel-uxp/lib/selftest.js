// In-Premiere self-test: exercises the panel's whole flow on the selected clip, then restores the
// clip's settings. Used by the Tools tab button and (dev builds) the test agent.

const { P, FILM_LOOKS, lookParams } = require("./looks.js");
const { Library } = require("./library.js");

async function runSelfTest(bridge, { log, referencePath } = {}) {
  const lines = [];
  const say = (s) => { lines.push(s); if (log) log(s); };
  const results = [];
  const check = (name, ok, detail) => { results.push(!!ok); say(`${ok ? "PASS" : "FAIL"} ${name}${detail ? ` (${detail})` : ""}`); };
  say("---- self-test ----");
  let before = null;
  try {
    const { items } = await bridge.selectedVideoItems();
    check("a video clip is selected", items.length > 0, `${items.length}`);
    if (!items.length) throw new Error("select one video clip and run again");
    await bridge.applyParams("selection", {});
    before = await bridge.selectedLookMatchParams();
    check("LookMatch is on the clip", !!before);
    const missing = Object.values(P).filter((n) => !(n in before.params));
    check("all panel controls exist", missing.length === 0, missing.join(", ") || "all");

    await bridge.applyParams("selection", { [P.mix]: 37 });
    let now = await bridge.selectedLookMatchParams();
    check("setting a value works", Math.abs(now.params[P.mix] - 37) < 0.01, `Mix = ${now.params[P.mix]}`);

    const look = FILM_LOOKS.find((l) => l.id === "golden-hour");
    await bridge.applyParams("selection", lookParams(look));
    now = await bridge.selectedLookMatchParams();
    check("film look applies (popups too)", now.params[P.negative] === look.params[P.negative] &&
          now.params[P.print] === look.params[P.print] && now.params[P.grain] === look.params[P.grain],
          `negative ${now.params[P.negative]}, print ${now.params[P.print]}`);

    const half = lookParams(look, 0.5);
    check("look intensity scales", Math.abs(half[P.grain] - look.params[P.grain] / 2) < 0.01, `grain ${half[P.grain]}`);

    await bridge.applyParams("selection", { [P.split]: true, [P.splitPos]: 30 });
    now = await bridge.selectedLookMatchParams();
    check("split view toggles", now.params[P.split] === true && Math.abs(now.params[P.splitPos] - 30) < 0.01);

    const round = Library.lookFromFile(Library.lookToFile({ name: "t", params: lookParams(look) }));
    check(".lmlook round trip", round.params[P.grain] === look.params[P.grain]);

    if (referencePath) {
      const res = await bridge.applyReference("selection", referencePath, {});
      check("reference slot set", res.slot > 0, `slot ${res.slot}`);
    } else {
      say("SKIP reference match (no reference selected)");
    }
  } catch (e) {
    check("no errors", false, e.message);
  } finally {
    if (before) {
      const restore = {};
      for (const [k, v] of Object.entries(before.params)) if (k !== P.referenceSlot) restore[k] = v;
      await bridge.applyParams("selection", restore).catch((e) => say(`restore failed: ${e.message}`));
      say("restored the clip's settings");
    }
  }
  const pass = results.length > 0 && results.every(Boolean);
  say(`---- ${pass ? "ALL PASS" : "FAILURES"} (${results.filter(Boolean).length}/${results.length}) ----`);
  return { pass, lines };
}

module.exports = { runSelfTest };
