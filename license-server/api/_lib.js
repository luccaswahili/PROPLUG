// Shared helpers for the Komodo license generator. The Ed25519 private key lives only in the Vercel
// environment variable ED25519_PRIVATE_KEY (PKCS8 PEM, or its base64). The matching PUBLIC key is baked
// into the plugin (src/premiere/License.h), which verifies keys offline — the server is never contacted
// by the plugin.
const crypto = require("crypto");

function loadPrivateKey() {
  const v = process.env.ED25519_PRIVATE_KEY;
  if (!v) throw new Error("ED25519_PRIVATE_KEY is not set");
  const pem = v.includes("BEGIN") ? v : Buffer.from(v, "base64").toString("utf8");
  return crypto.createPrivateKey(pem);
}

function publicKeyHex() {
  const pub = crypto.createPublicKey(loadPrivateKey());
  return Buffer.from(pub.export({ format: "jwk" }).x, "base64url").toString("hex");
}

function constantTimeEqual(a, b) {
  const ba = Buffer.from(String(a)), bb = Buffer.from(String(b));
  if (ba.length !== bb.length) return false;
  return crypto.timingSafeEqual(ba, bb);
}

async function readBody(req) {
  if (req.body && typeof req.body === "object") return req.body;
  if (typeof req.body === "string") { try { return JSON.parse(req.body); } catch (e) { return {}; } }
  return await new Promise((resolve) => {
    let data = "";
    req.on("data", (c) => (data += c));
    req.on("end", () => { try { resolve(JSON.parse(data || "{}")); } catch (e) { resolve({}); } });
    req.on("error", () => resolve({}));
  });
}

// Signs a license token: "<payloadB64url>.<sigB64url>", signature over the payload's base64url text.
function signLicense({ email, months }) {
  const m = Math.max(1, Math.min(60, parseInt(months, 10) || 1));
  const now = Math.floor(Date.now() / 1000);
  const exp = now + m * 30 * 24 * 3600;             // "monthly" = 30-day periods
  const payload = { v: 1, id: crypto.randomUUID(), email: String(email || "").slice(0, 120), iat: now, exp };
  const payloadB64 = Buffer.from(JSON.stringify(payload)).toString("base64url");
  const sig = crypto.sign(null, Buffer.from(payloadB64, "ascii"), loadPrivateKey()).toString("base64url");
  return { key: payloadB64 + "." + sig, exp, months: m, email: payload.email, id: payload.id };
}

module.exports = { loadPrivateKey, publicKeyHex, constantTimeEqual, readBody, signLicense };
