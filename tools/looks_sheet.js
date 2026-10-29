#!/usr/bin/env node
// Render every panel film look (src/panel-uxp/lib/looks.js) through the CLI engine and make a
// labelled contact sheet.  node tools/looks_sheet.js FRAME.png OUT.png [--ref REFERENCE] [--combos]
const { execFileSync } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { P, FILM_LOOKS, stockCombos, lookParams } = require("../src/panel-uxp/lib/looks.js");

const root = path.join(__dirname, "..");
const cli = path.join(root, "build-installer/cmake/lookmatch-render");
const args = process.argv.slice(2);
const [frame, out] = args;
const refIdx = args.indexOf("--ref");
const ref = refIdx > 0 ? args[refIdx + 1] : null;
const looks = args.includes("--combos") ? stockCombos() : FILM_LOOKS;

// panel params (Effect Controls units) -> CLI flags
function cliArgs(p) {
  const pct = (k) => String((p[k] || 0) / 100);
  return [
    "--film", pct(P.filmStrength), "--negative", String(p[P.negative] - 1), "--print", String(p[P.print] - 1),
    "--printer", `${p[P.printerR]},${p[P.printerG]},${p[P.printerB]}`, "--film-sat", pct(P.filmSaturation),
    "--fade", pct(P.fade), "--halation", pct(P.halation), "--halation-radius", String(p[P.halationSize]),
    "--halation-threshold", String(p[P.halationThreshold]), "--bloom", pct(P.bloom),
    "--bloom-radius", String(p[P.bloomSize]), "--bloom-threshold", String(p[P.bloomThreshold]),
    "--grain", pct(P.grain), "--grain-size", String(p[P.grainSize]), "--grain-color", pct(P.grainColour),
    "--softness", pct(P.softness), "--vignette", pct(P.vignette), "--weave", String(p[P.gateWeave]),
    "--flicker", pct(P.flicker),
  ];
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "lm-looks-"));
const tiles = [["--in-only", ref ? "Matched, film off" : "Original"]];
const baseArgs = ["--in", frame].concat(ref ? ["--ref", ref] : []);
const first = path.join(tmp, "base.png");
execFileSync(cli, baseArgs.concat(["--out", first]), { stdio: "ignore" });
tiles[0][0] = first;
for (const look of looks) {
  const o = path.join(tmp, `${look.id}.png`);
  execFileSync(cli, baseArgs.concat(["--out", o], cliArgs(lookParams(look))), { stdio: "ignore" });
  tiles.push([o, look.name]);
}
const sheet = path.join(root, "build-installer/contact_sheet");
if (!fs.existsSync(sheet) || fs.statSync(sheet).mtimeMs < fs.statSync(path.join(__dirname, "contact_sheet.swift")).mtimeMs)
  execFileSync("xcrun", ["swiftc", "-O", path.join(__dirname, "contact_sheet.swift"), "-o", sheet]);
execFileSync(sheet, [out, "3"].concat(...tiles));
console.log(`wrote ${out} (${tiles.length} tiles)`);
