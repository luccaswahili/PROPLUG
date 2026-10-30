// node src/panel-uxp/test/run.js : unit tests for the panel logic (no Premiere needed).
const assert = require("assert");
const os = require("os");
const path = require("path");
const fsp = require("fs").promises;
const { P, SOURCE_FORMATS, TEXTURE_DEFAULTS, guessSourceFormat, sourceFormatReason, abNext } = require("../lib/params.js");
const { Library } = require("../lib/library.js");
const { Bridge, isLookMatch } = require("../lib/bridge.js");
const { COMMANDS } = require("../lib/agent.js");
const { makePremiere, makeItem } = require("./mock-premiere.js");

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test("params: source format from metadata and names", () => {
  assert.strictEqual(SOURCE_FORMATS.length, 15, "Auto + the 14 transfers of lm::Transfer");
  assert.strictEqual(guessSourceFormat("A001_C002_SLOG3.mov"), 4);
  assert.strictEqual(guessSourceFormat(["clip", "ARRI LogC4"]), 6);
  assert.strictEqual(guessSourceFormat("ALEXA_LogC_clip.mxf"), 5);
  assert.strictEqual(guessSourceFormat("GH5 V-Log 0003.mp4"), 8);
  assert.strictEqual(guessSourceFormat("X-H2S F-Log2"), 10);
  assert.strictEqual(guessSourceFormat("iPhone Apple Log"), 11);
  assert.strictEqual(guessSourceFormat("Sample Media Clip 2.mp4"), 0);
  assert.strictEqual(guessSourceFormat("DaVinci Intermediate DWG export"), 15);
  const artlist = "6274378_Beach Workout Talking Women_By_Andrii_Nekrasov_Artlist_LOG_4K.mov";
  assert.strictEqual(guessSourceFormat(artlist), 9);
  assert.strictEqual(sourceFormatReason([artlist], 9), "Artlist log → F-Log (best fit)");
  assert.strictEqual(sourceFormatReason(["x"], 0), "detected from the picture");
  assert.strictEqual(guessSourceFormat(undefined), 0);
  for (const k of Object.keys(TEXTURE_DEFAULTS)) assert(Object.values(P).includes(k), `unknown texture param ${k}`);
});

test("params: A/B compare goes off -> A (original) -> B (matched) -> A", () => {
  let st = abNext({ [P.split]: false, [P.splitPos]: 50 });
  assert.strictEqual(st.label, "A: original");
  assert.deepStrictEqual(st.params, { [P.split]: true, [P.splitPos]: 100, [P.splitAngle]: 0 });
  st = abNext(st.params);
  assert.strictEqual(st.label, "B: matched");
  assert.strictEqual(st.params[P.splitPos], 0);
  assert.strictEqual(abNext(st.params).label, "A: original");
  assert.strictEqual(abNext({ [P.split]: true, [P.splitPos]: 40 }).label, "A: original");
});

test("library: references, looks, persistence and .lmlook files", async () => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), "lm-"));
  const lib = new Library(fsp, path.join(dir, "data"));
  await lib.load();
  assert.strictEqual(lib.refs.length, 0);
  const r = lib.addReference("/x/ref one.jpg");
  assert.strictEqual(lib.addReference("/x/ref one.jpg"), r, "duplicate path returns the same entry");
  assert.throws(() => lib.addReference("/x/notes.txt"));
  lib.saveLook("Warm", { [P.grain]: 20 }, "/x/ref one.jpg");
  lib.saveLook("Warm", { [P.grain]: 40 }, null);
  assert.strictEqual(lib.looks.length, 1, "same name replaces");
  await lib.save();
  const lib2 = await new Library(fsp, path.join(dir, "data")).load();
  assert.strictEqual(lib2.refs[0].name, "ref one.jpg");
  assert.strictEqual(lib2.looks[0].params[P.grain], 40);
  const text = Library.lookToFile(lib2.looks[0]);
  assert.strictEqual(Library.lookFromFile(text).params[P.grain], 40);
  assert.throws(() => Library.lookFromFile('{"lookmatch":1,"params":{"Grain":"rm -rf"}}'));
  assert.throws(() => Library.lookFromFile('{"name":"x"}'));
});

test("bridge: match name with the AE. prefix", () => {
  assert(isLookMatch("AE.PROPLUGS LookMatch"));
  assert(isLookMatch("PROPLUGS LookMatch"));
  assert(!isLookMatch("AE.ADBE Motion"));
});

test("bridge: applyParams adds LookMatch, skips audio, one undo step", async () => {
  const v1 = makeItem("V1", true, false), a1 = makeItem("A1", false, false), v2 = makeItem("V2", true, true);
  const ppro = makePremiere({ selected: [v1, a1, v2] });
  const b = new Bridge(ppro, fsp, os.tmpdir());
  const res = await b.applyParams("selection", { [P.grain]: 55, [P.mix]: 80 });
  assert.deepStrictEqual(res, { clips: 2, added: 1 });
  assert.strictEqual(a1.chain.comps.length, 1, "audio item untouched");
  const set = ppro.log.filter((l) => l.label === "LookMatch");
  assert.strictEqual(set.length, 1, "all values in a single transaction");
  assert.strictEqual(set[0].count, 4);
  for (const it of [v1, v2]) {
    const { component } = await b.findLookMatch(it);
    const params = await b.readParams(component);
    assert.strictEqual(params[P.grain], 55);
    assert.strictEqual(params[P.mix], 80);
  }
});

test("bridge: popups are 1-based in the panel, 0-based in Premiere", async () => {
  const v1 = makeItem("V1", true, true);
  const b = new Bridge(makePremiere({ selected: [v1] }), fsp, os.tmpdir());
  const { component } = await b.findLookMatch(v1);
  assert.strictEqual((await b.readParams(component))[P.sourceFormat], 1, "default reads back 1-based");
  await b.applyParams("selection", { [P.sourceFormat]: 5, [P.footageEncoding]: 3 });
  const raw = Object.fromEntries(component.list.map((p) => [p.displayName, p.value]));
  assert.strictEqual(raw[P.sourceFormat], 4);
  assert.strictEqual(raw[P.footageEncoding], 2);
  const back = await b.readParams(component);
  assert.strictEqual(back[P.sourceFormat], 5);
  assert.strictEqual(back[P.footageEncoding], 3);
});

test("bridge: unknown control is reported, nothing is half-applied", async () => {
  const v1 = makeItem("V1", true, true);
  const ppro = makePremiere({ selected: [v1] });
  const b = new Bridge(ppro, fsp, os.tmpdir());
  await assert.rejects(b.applyParams("selection", { "Nope": 1, [P.mix]: 5 }), /no "Nope" control/);
  const { component } = await b.findLookMatch(v1);
  assert.strictEqual((await b.readParams(component))[P.mix], 100);
});

test("bridge: applyReference writes refs/<slot>.txt and sets the slot", async () => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), "lm-"));
  const v1 = makeItem("V1", true, true);
  const b = new Bridge(makePremiere({ selected: [v1] }), fsp, dir);
  const { component } = await b.findLookMatch(v1);
  await b.applyParams("selection", { [P.filmStrength]: 80 });
  const res = await b.applyReference("selection", "/refs/look.png", { [P.matchStrength]: 70 });
  assert(res.slot >= 1 && res.slot <= 999999);
  assert.strictEqual(await fsp.readFile(path.join(dir, "refs", `${res.slot}.txt`), "utf-8"), "/refs/look.png");
  const params = await b.readParams(component);
  assert.strictEqual(params[P.referenceSlot], res.slot);
  assert.strictEqual(params[P.matchStrength], 70);
  assert.strictEqual(params[P.filmStrength], 0, "a match never applies a film print");
  assert.deepStrictEqual(res.names, ["V1"]);
});

test("bridge: a look from several stills is one path per line (at most 5)", async () => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), "lm-"));
  const v1 = makeItem("V1", true, true);
  const b = new Bridge(makePremiere({ selected: [v1] }), fsp, dir);
  const stills = ["/a.png", "/b.png", "/c.png", "/d.png", "/e.png", "/f.png"];
  const res = await b.applyReference("selection", stills, {});
  assert.strictEqual(await fsp.readFile(path.join(dir, "refs", `${res.slot}.txt`), "utf-8"), stills.slice(0, 5).join("\n"));
});

test("bridge: scope \"others\" is the sequence without the selected hero clip", async () => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), "lm-"));
  const clips = [makeItem("hero", true, true), makeItem("b", true, false), makeItem("c", true, true)];
  const b = new Bridge(makePremiere({ selected: [clips[0]], tracks: [clips] }), fsp, dir);
  const { items } = await b.items("others");
  assert.deepStrictEqual(await Promise.all(items.map((i) => i.getName())), ["b", "c"]);
  const res = await b.applyReference("others", "/hero.png", {});
  assert.deepStrictEqual(res.names, ["b", "c"]);
});

test("bridge: every clip gets its own reference slot", async () => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), "lm-"));
  const clips = [makeItem("a", true, true), makeItem("b", true, false), makeItem("c", true, true)];
  const ppro = makePremiere({ selected: clips });
  const b = new Bridge(ppro, fsp, dir);
  const res = await b.applyReference("selection", "/r.png", { [P.matchStrength]: 50 });
  assert.strictEqual(new Set(res.slots).size, 3, "distinct slots");
  for (const [i, c] of clips.entries()) {
    const { component } = await b.findLookMatch(c);
    const p = await b.readParams(component);
    assert.strictEqual(p[P.referenceSlot], res.slots[i]);
    assert.strictEqual(p[P.matchStrength], 50);
    assert.strictEqual(await fsp.readFile(path.join(dir, "refs", `${res.slots[i]}.txt`), "utf-8"), "/r.png");
  }
  assert.strictEqual(ppro.log.filter((l) => l.label === "LookMatch").length, 1, "still one undo step");
});

test("bridge: whole-sequence scope and empty selection", async () => {
  const tracks = [[makeItem("a", true, false), makeItem("b", true, false)], [makeItem("c", true, true)]];
  const b = new Bridge(makePremiere({ tracks }), fsp, os.tmpdir());
  assert.deepStrictEqual(await b.applyParams("sequence", { [P.fade]: 10 }), { clips: 3, added: 2 });
  await assert.rejects(b.applyParams("selection", { [P.fade]: 10 }), /Select a video clip/);
});

test("bridge: exportCube writes the request, sets the slot and waits for the effect", async () => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), "lm-"));
  const v1 = makeItem("V1", true, true);
  const b = new Bridge(makePremiere({ selected: [v1] }), fsp, dir);
  let polls = 0;
  const sleep = async () => {
    // play the effect: on the 3rd poll, answer like Render does
    if (++polls === 3) {
      const { component } = await b.findLookMatch(v1);
      const slot = (await b.readParams(component))[P.exportSlot];
      const req = await fsp.readFile(path.join(dir, "refs", `export-${slot}.txt`), "utf-8");
      assert.strictEqual(req, "/out/My Look.cube");
      await fsp.writeFile(path.join(dir, "refs", `export-${slot}.done`), "ok");
    }
  };
  const res = await b.exportCube("/out/My Look.cube", { sleep, pollMs: 1, timeoutMs: 100 });
  assert.strictEqual(res.path, "/out/My Look.cube");
  await assert.rejects(b.exportCube("/out/x.png", { sleep, pollMs: 1 }), /\.cube/);
  await assert.rejects(b.exportCube("/out/y.cube", { sleep: async () => {}, pollMs: 1, timeoutMs: 5 }), /playhead/);
});

test("bridge: captureFrame sets the capture slot and returns the PNG the effect wrote", async () => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), "lm-"));
  const v1 = makeItem("Hero clip", true, true);
  const b = new Bridge(makePremiere({ selected: [v1] }), fsp, dir);
  const sleep = async () => {
    const { component } = await b.findLookMatch(v1);
    const slot = (await b.readParams(component))[P.captureSlot];
    await fsp.writeFile(path.join(dir, "refs", `capture-${slot}.done`), "ok");
  };
  const res = await b.captureFrame({ sleep, pollMs: 1, timeoutMs: 50 });
  assert.strictEqual(res.clip, "Hero clip");
  assert.strictEqual(res.path, path.join(dir, "refs", `capture-${res.slot}.png`));
  const failing = async () => {
    const { component } = await b.findLookMatch(v1);
    const slot = (await b.readParams(component))[P.captureSlot];
    await fsp.writeFile(path.join(dir, "refs", `capture-${slot}.done`), "could not read the frame");
  };
  await assert.rejects(b.captureFrame({ sleep: failing, pollMs: 1, timeoutMs: 50 }), /could not read/);
  // graded: the request file asks the effect for the frame as it renders it
  const g = await b.captureFrame({ sleep, pollMs: 1, timeoutMs: 50, graded: true });
  assert.strictEqual(await fsp.readFile(path.join(dir, "refs", `capture-${g.slot}.txt`), "utf-8"), "graded");
});

test("bridge: per-clip match params, analysis file, colour hints", async () => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), "lm-"));
  const clips = [makeItem("A001_SLOG3", true, true), makeItem("Sample 2", true, true)];
  const b = new Bridge(makePremiere({ selected: clips }), fsp, dir);
  const hints = await b.clipColourHints("selection");
  assert.deepStrictEqual(hints.map(guessSourceFormat), [4, 0]);
  const res = await b.applyReference("selection", "/r.png", (i) => ({ [P.sourceFormat]: i ? 1 : 4 }));
  for (const [i, c] of clips.entries()) {
    const { component } = await b.findLookMatch(c);
    assert.strictEqual((await b.readParams(component))[P.sourceFormat], i ? 1 : 4);
  }
  assert.strictEqual(await b.readAnalysis(res.slots[0]), null, "nothing until the effect writes it");
  const a = { source: { transfer: "S-Log3", log: true, exposure: -1.2, cct: 4300, tint: 2, contrast: 6 } };
  setTimeout(() => fsp.writeFile(path.join(dir, "refs", `${res.slots[0]}.json`), JSON.stringify(a)), 20);
  const got = await b.readAnalysis(res.slots[0], { timeoutMs: 2000, pollMs: 10 });
  assert.strictEqual(got.source.transfer, "S-Log3");
});

test("self-test passes against the mock and restores the clip", async () => {
  const { runSelfTest } = require("../lib/selftest.js");
  const v1 = makeItem("V1", true, true);
  const b = new Bridge(makePremiere({ selected: [v1] }), fsp, await fsp.mkdtemp(path.join(os.tmpdir(), "lm-")));
  const { pass, lines } = await runSelfTest(b, { referencePath: "/r.png" });
  assert(pass, lines.join("\n"));
  const { component } = await b.findLookMatch(v1);
  const p = await b.readParams(component);
  assert.strictEqual(p[P.mix], 100, "restored");
  assert.strictEqual(p[P.split], false, "restored");
});

test("agent: whitelist only, values sanitised", async () => {
  assert.deepStrictEqual(Object.keys(COMMANDS).sort(),
    ["applyParams", "applyReference", "captureFrame", "clipNames", "exportCube", "inspectParams", "ping", "readAnalysis", "readSelected", "saveProject", "selectClip", "selfTest", "setPlayhead", "timeline"]);
  const v1 = makeItem("V1", true, true);
  const ppro = makePremiere({ selected: [v1] });
  const b = new Bridge(ppro, fsp, os.tmpdir());
  await COMMANDS.applyParams(b, { scope: "selection", params: { [P.grain]: 12, [P.mix]: "evil()" } });
  const { component } = await b.findLookMatch(v1);
  const params = await b.readParams(component);
  assert.strictEqual(params[P.grain], 12);
  assert.strictEqual(params[P.mix], 100, "non-numeric value dropped");
  await COMMANDS.saveProject(b);
  assert.strictEqual(ppro.project.saved, 1);
});

(async () => {
  let failed = 0;
  for (const t of tests) {
    try { await t.fn(); console.log(`PASS ${t.name}`); } catch (e) { failed++; console.log(`FAIL ${t.name}\n     ${e.message}`); }
  }
  console.log(failed ? `${failed} test(s) failed` : `all ${tests.length} panel tests passed`);
  process.exit(failed ? 1 : 0);
})();
