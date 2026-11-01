// GET /api/pubkey -> { publicKeyHex }  (public key is not secret; handy for verifying the plugin matches)
const { publicKeyHex } = require("./_lib.js");
module.exports = async (req, res) => {
  try { res.status(200).json({ publicKeyHex: publicKeyHex() }); }
  catch (e) { res.status(500).json({ error: String(e.message || e) }); }
};
