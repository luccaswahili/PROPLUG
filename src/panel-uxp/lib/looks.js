// Effect parameter names (as shown in Effect Controls) and film looks.
// Values are in the units Effect Controls shows: percentages 0-100, popups 1-based.

const P = {
  matchStrength: "Match Strength",
  regionMatching: "Region Matching",
  autoSkin: "Auto Skin Protect",
  skinProtect: "Skin Protect",
  footageEncoding: "Footage Encoding",
  exposure: "Exposure",
  temperature: "Temperature",
  tint: "Tint",
  filmStrength: "Film Strength",
  negative: "Negative Stock",
  print: "Print Stock",
  printerR: "Printer Light R",
  printerG: "Printer Light G",
  printerB: "Printer Light B",
  filmSaturation: "Film Saturation",
  fade: "Fade",
  halation: "Halation",
  halationSize: "Halation Size",
  halationThreshold: "Halation Threshold",
  bloom: "Bloom",
  bloomSize: "Bloom Size",
  bloomThreshold: "Bloom Threshold",
  grain: "Grain",
  grainSize: "Grain Size",
  grainColour: "Grain Colour",
  softness: "Softness",
  vignette: "Vignette",
  gateWeave: "Gate Weave",
  flicker: "Flicker",
  mix: "Mix",
  referenceSlot: "Reference Slot",
};

// Order matches the effect's popups (src/core/film.cpp); index 0 here is popup value 1.
const NEGATIVES = ["Tungsten 500T", "Daylight 250D", "Fine Grain 50D", "Portrait 400", "Low-Con Neutral",
                   "Night 800T", "Black & White 400"];
const PRINTS = ["None (scanned negative)", "Cine Print Warm", "Cine Print Cool", "Low-Con Print", "High-Con Print"];
const ENCODINGS = ["Rec.709 / display", "ARRI LogC3", "Sony S-Log3", "Panasonic V-Log"];

// Defaults of every film/texture control (Effect Controls defaults), so a look can reset what it doesn't set.
const FILM_DEFAULTS = {
  [P.filmStrength]: 0, [P.negative]: 2, [P.print]: 2, [P.printerR]: 0, [P.printerG]: 0, [P.printerB]: 0,
  [P.filmSaturation]: 100, [P.fade]: 0, [P.halation]: 0, [P.halationSize]: 1, [P.halationThreshold]: 0.6,
  [P.bloom]: 0, [P.bloomSize]: 1, [P.bloomThreshold]: 0.5, [P.grain]: 0, [P.grainSize]: 1, [P.grainColour]: 30,
  [P.softness]: 0, [P.vignette]: 0, [P.gateWeave]: 0, [P.flicker]: 0,
};

// Original look names (no trademarks). Each sets a full film/texture state.
const FILM_LOOKS = [
  { id: "clean", name: "Clean (film off)", params: {} },
  { id: "golden-hour", name: "Golden Hour Print", params: {
      [P.filmStrength]: 80, [P.negative]: 2, [P.print]: 2, [P.printerR]: 2, [P.printerB]: -2,
      [P.halation]: 40, [P.grain]: 25, [P.vignette]: 15 } },
  { id: "teal-night", name: "Teal Night", params: {
      [P.filmStrength]: 85, [P.negative]: 6, [P.print]: 3, [P.printerB]: 3, [P.printerR]: -1,
      [P.halation]: 70, [P.bloom]: 25, [P.grain]: 35 } },
  { id: "soft-portrait", name: "Soft Portrait", params: {
      [P.filmStrength]: 70, [P.negative]: 4, [P.print]: 4, [P.filmSaturation]: 90, [P.fade]: 10,
      [P.halation]: 20, [P.softness]: 15, [P.grain]: 15 } },
  { id: "bleach-skip", name: "Silver Retain", params: {
      [P.filmStrength]: 90, [P.negative]: 2, [P.print]: 5, [P.filmSaturation]: 55, [P.grain]: 30,
      [P.vignette]: 20 } },
  { id: "grainy-16", name: "Grainy 16", params: {
      [P.filmStrength]: 85, [P.negative]: 1, [P.print]: 2, [P.grain]: 90, [P.grainSize]: 2.2, [P.grainColour]: 45,
      [P.halation]: 45, [P.softness]: 20, [P.vignette]: 30, [P.gateWeave]: 1.2, [P.flicker]: 15 } },
  { id: "faded-memory", name: "Faded Memory", params: {
      [P.filmStrength]: 75, [P.negative]: 5, [P.print]: 4, [P.fade]: 35, [P.filmSaturation]: 80,
      [P.grain]: 40, [P.vignette]: 25 } },
  { id: "mono-400", name: "Mono 400", params: {
      [P.filmStrength]: 100, [P.negative]: 7, [P.print]: 5, [P.grain]: 55, [P.vignette]: 20 } },
];

// Full parameter set for a look: defaults overlaid with the look's values.
function lookParams(look) {
  return Object.assign({}, FILM_DEFAULTS, look.params);
}

// Every negative x print combination as a look card.
function stockCombos() {
  const out = [];
  NEGATIVES.forEach((neg, n) => PRINTS.forEach((pr, p) => out.push({
    id: `stock-${n + 1}-${p + 1}`, name: `${neg} / ${pr}`,
    params: { [P.filmStrength]: 100, [P.negative]: n + 1, [P.print]: p + 1 },
  })));
  return out;
}

// Guess Footage Encoding (popup value) from a clip or file name.
function guessEncoding(name) {
  const s = String(name || "").toLowerCase();
  if (/s-?log ?3|slog3/.test(s)) return 3;
  if (/log ?c|logc|alexa|arri/.test(s)) return 2;
  if (/v-?log|vlog/.test(s)) return 4;
  return 1;
}

module.exports = { P, NEGATIVES, PRINTS, ENCODINGS, FILM_DEFAULTS, FILM_LOOKS, lookParams, stockCombos, guessEncoding };
