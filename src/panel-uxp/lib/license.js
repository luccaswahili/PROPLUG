// Komodo licensing, panel side. A key is "<payloadB64url>.<sigB64url>"; the payload is JSON
// {v,id,email,iat,exp} with exp a Unix time in seconds. This module only DECODES the payload so the
// panel can show the status and the expiry date, and writes the key to disk. Enforcement is the
// plugin's job: it verifies the Ed25519 signature (src/premiere/License.h) and locks when there is no
// valid, unexpired key. So a forged or edited key may look "activated" here but the effect stays locked.

const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

function b64urlToBytes(s) {
  const lut = {};
  for (let i = 0; i < B64.length; i++) lut[B64[i]] = i;
  const out = [];
  let buf = 0, bits = 0;
  for (const ch of s) {
    if (ch === "=" || ch === "\n" || ch === "\r" || ch === " ") continue;
    const v = lut[ch];
    if (v === undefined) return null;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out.push((buf >> bits) & 0xff); }
  }
  return Uint8Array.from(out);
}

// Returns { present, formatOk, exp, email }. formatOk means it parses as a token with a 64-byte
// signature and a JSON payload carrying a numeric exp — NOT that the signature is valid.
function parseKey(token) {
  const t = (token || "").trim();
  if (!t) return { present: false, formatOk: false, exp: 0, email: "" };
  const dot = t.indexOf(".");
  if (dot < 0) return { present: true, formatOk: false, exp: 0, email: "" };
  const payloadB64 = t.slice(0, dot), sigB64 = t.slice(dot + 1);
  const sig = b64urlToBytes(sigB64);
  const payloadBytes = b64urlToBytes(payloadB64);
  if (!sig || sig.length !== 64 || !payloadBytes) return { present: true, formatOk: false, exp: 0, email: "" };
  let obj;
  try { obj = JSON.parse(new TextDecoder().decode(payloadBytes)); } catch (e) { return { present: true, formatOk: false, exp: 0, email: "" }; }
  const exp = Number(obj && obj.exp);
  if (!Number.isFinite(exp)) return { present: true, formatOk: false, exp: 0, email: String((obj && obj.email) || "") };
  return { present: true, formatOk: true, exp, email: String((obj && obj.email) || "") };
}

function keyStatus(token, nowSec) {
  const now = Number.isFinite(nowSec) ? nowSec : Math.floor(Date.now() / 1000);
  const p = parseKey(token);
  return {
    present: p.present,
    formatOk: p.formatOk,
    exp: p.exp,
    email: p.email,
    activated: p.formatOk && p.exp > now,
    expired: p.formatOk && p.exp <= now,
  };
}

function expiryDate(exp) {
  if (!exp) return "";
  try { return new Date(exp * 1000).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" }); }
  catch (e) { return ""; }
}

module.exports = { b64urlToBytes, parseKey, keyStatus, expiryDate };
