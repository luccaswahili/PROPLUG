# Komodo license server

A tiny Vercel app that generates **Ed25519-signed** Komodo license keys that expire monthly. The plugin
verifies keys **offline** with the matching public key baked into `src/premiere/License.h`; this server is
never contacted by the plugin, so it only has to mint keys.

## Endpoints
- `GET /` — password-protected admin page to create a key.
- `POST /api/generate` `{ password, email, months }` → `{ key, exp, expiresOn, months, email, id }`
- `GET /api/pubkey` → `{ publicKeyHex }` (public key is not secret; use it to confirm the plugin matches).

## Environment variables (set in Vercel, never in the repo)
- `ADMIN_PASSWORD` — gate for the generator page / API.
- `ED25519_PRIVATE_KEY` — the signing key, PKCS8 PEM **or** its base64. The matching public key must be the
  one in `src/premiere/License.h` (`kPublicKey`). Generate a pair with:
  ```
  node -e 'const c=require("crypto");const{publicKey,privateKey}=c.generateKeyPairSync("ed25519");\
  console.log(privateKey.export({type:"pkcs8",format:"pem"}));\
  console.log("PUBHEX",Buffer.from(publicKey.export({format:"jwk"}).x,"base64url").toString("hex"));'
  ```
  Put the PEM (or `base64 -w0` of it) in `ED25519_PRIVATE_KEY`, and the PUBHEX bytes in `License.h`.

## Deploy
```
cd license-server
vercel link            # create a NEW project (do not reuse other projects)
vercel env add ADMIN_PASSWORD production
vercel env add ED25519_PRIVATE_KEY production
vercel deploy --prod
```
