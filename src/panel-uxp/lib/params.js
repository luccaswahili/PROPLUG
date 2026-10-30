// Effect parameter names (as shown in Effect Controls), source formats and texture defaults.
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
  exportSlot: "LUT Export Slot",
  captureSlot: "Capture Slot",
  split: "Split View",
  splitPos: "Split Position",
  splitAngle: "Split Angle",
  sourceFormat: "Source Format",
  normExposure: "Normalise Exposure",
  normWhite: "Normalise White Balance",
  normContrast: "Normalise Contrast",
  lookStrength: "Look Strength",
  matchContrast: "Match Contrast",
};

// Popup controls. The panel uses 1-based values like Effect Controls; Premiere's UXP API reads
// and writes them 0-based, so lib/bridge.js converts at the boundary.
const POPUPS = new Set([P.footageEncoding, P.sourceFormat]);

// Source Format popup: value 1 = Auto (detect from the picture), then lm::Transfer order (src/core/scene.h).
const SOURCE_FORMATS = ["Auto (detect)", "Rec.709", "sRGB", "Sony S-Log3 / S-Gamut3.Cine", "ARRI LogC3 / AWG3",
  "ARRI LogC4 / AWG4", "Canon Log 3 / Cinema Gamut", "Panasonic V-Log / V-Gamut", "Fujifilm F-Log / F-Gamut",
  "Fujifilm F-Log2 / F-Gamut", "Apple Log / Rec.2020", "Blackmagic Film Gen5 / Wide Gamut",
  "RED Log3G10 / REDWideGamutRGB", "DJI D-Log / D-Gamut", "DaVinci Intermediate / Wide Gamut"];

// Defaults of the texture controls (Effect Controls defaults), so Reset can put them back.
const TEXTURE_DEFAULTS = {
  [P.fade]: 0, [P.halation]: 0, [P.halationSize]: 1, [P.halationThreshold]: 0.6,
  [P.bloom]: 0, [P.bloomSize]: 1, [P.bloomThreshold]: 0.5, [P.grain]: 0, [P.grainSize]: 1, [P.grainColour]: 30,
  [P.softness]: 0, [P.vignette]: 0, [P.gateWeave]: 0, [P.flicker]: 0,
};

// Why guessSourceFormat chose what it did, for the panel's analysis line.
function sourceFormatReason(texts, format) {
  const s = [].concat(texts || []).map((t) => String(t || "")).join(" ").toLowerCase();
  if (format === 9 && s.includes("artlist") && s.includes("log") && !/f-?log/.test(s)) return "Artlist log → F-Log (best fit)";
  return format ? `${SOURCE_FORMATS[format - 1]} from metadata` : "detected from the picture";
}

// Source Format (popup value) named in clip metadata or a file name, or 0 if nothing says.
// Mirrors lm::transferFromName.
function guessSourceFormat(texts) {
  const s = [].concat(texts || []).map((t) => String(t || "")).join(" ").toLowerCase();
  const has = (...k) => k.some((x) => s.includes(x));
  if (has("s-log3", "slog3", "s-log 3", "s-gamut3")) return 4;
  // Artlist re-exports its "LOG" clips through Resolve, camera curve and tags lost; F-Log fits them best
  if (has("artlist") && has("log")) return 9;
  if (has("logc4", "log c4", "awg4")) return 6;
  if (has("logc", "log c", "awg3", "alexa")) return 5;
  if (has("canon log 3", "c-log3", "clog3", "canon log3")) return 7;
  if (has("v-log", "vlog", "v-gamut")) return 8;
  if (has("f-log2", "flog2")) return 10;
  if (has("f-log", "flog")) return 9;
  if (has("apple log", "applelog")) return 11;
  if (has("davinci intermediate", "davinci wide", "dwg")) return 15;
  if (has("blackmagic", "bmd film", "braw")) return 12;
  if (has("log3g10", "redwidegamut", "ipp2")) return 13;
  if (has("d-log", "dlog")) return 14;
  return 0;
}

module.exports = { P, POPUPS, SOURCE_FORMATS, TEXTURE_DEFAULTS, guessSourceFormat, sourceFormatReason };
