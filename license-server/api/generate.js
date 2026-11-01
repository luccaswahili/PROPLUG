// POST /api/generate  { password, email, months }  ->  { key, exp, expiresOn, months, email, id }
// The admin password (ADMIN_PASSWORD) gates key creation. Never expose the private key.
const { constantTimeEqual, readBody, signLicense } = require("./_lib.js");

module.exports = async (req, res) => {
  if (req.method !== "POST") { res.status(405).json({ error: "POST only" }); return; }
  const admin = process.env.ADMIN_PASSWORD;
  if (!admin || !process.env.ED25519_PRIVATE_KEY) {
    res.status(500).json({ error: "Server not configured: set ADMIN_PASSWORD and ED25519_PRIVATE_KEY." });
    return;
  }
  const body = await readBody(req);
  if (!body.password || !constantTimeEqual(body.password, admin)) { res.status(401).json({ error: "Wrong password." }); return; }
  try {
    const r = signLicense({ email: body.email, months: body.months });
    res.status(200).json({ ...r, expiresOn: new Date(r.exp * 1000).toISOString() });
  } catch (e) {
    res.status(500).json({ error: String(e.message || e) });
  }
};
