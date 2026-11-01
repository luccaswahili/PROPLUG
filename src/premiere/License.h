// Komodo licensing: offline verification of a monthly key issued by the Vercel key generator.
//
// A key is "<payloadB64url>.<sigB64url>". The payload is JSON {v,id,email,iat,exp} (exp is a Unix time
// in seconds). The generator signs the payload's base64url text with an Ed25519 private key that lives
// only on the server; this file holds the matching 32-byte PUBLIC key and verifies the signature with
// the vendored TweetNaCl (third_party/tweetnacl, public domain). No network call is ever made.
//
// No valid, unexpired key => the plugin is locked (Komodo is paid; there is no trial). The key file is
// written by the panel to the shared data folder and read here by both the CPU effect and the GPU filter.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include "tweetnacl.h"

namespace lmlic {

// Ed25519 public key (32 bytes). Private key is only in the Vercel generator's env (ED25519_PRIVATE_KEY).
// NOTE: paired with the key set on the deployed generator; regenerate both together if ever rotated.
static const unsigned char kPublicKey[32] = { 0x67,0x3d,0x3d,0x4b,0x2f,0x04,0x13,0x5f,0x9a,0x73,0xd5,0xae,0x4e,0x05,0x27,0x25,0x8f,0xb3,0x21,0xeb,0x7d,0xd2,0xa8,0x16,0xf0,0xfd,0x86,0x42,0x82,0x68,0xae,0x7d };

inline int b64urlDecode(const std::string& in, std::vector<unsigned char>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    out.clear();
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = val(c);
        if (v < 0) return -1;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
        }
    }
    return 0;
}

struct License {
    bool present = false;      // a key string was supplied
    bool signatureOk = false;  // signature verifies against kPublicKey
    long long exp = 0;         // Unix expiry (seconds)
    std::string email;
};

inline long long jsonInt(const std::string& js, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    auto p = js.find(k);
    if (p == std::string::npos) return 0;
    p += k.size();
    while (p < js.size() && js[p] == ' ') ++p;
    long long v = 0;
    bool any = false;
    while (p < js.size() && js[p] >= '0' && js[p] <= '9') {
        v = v * 10 + (js[p] - '0');
        ++p;
        any = true;
    }
    return any ? v : 0;
}

inline std::string jsonStr(const std::string& js, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    auto p = js.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    auto e = js.find('"', p);
    return e == std::string::npos ? "" : js.substr(p, e - p);
}

inline License parseTokenWithKey(const std::string& token, const unsigned char pub[32]) {
    License L;
    std::string t;
    for (char c : token)
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t') t += c;
    if (t.empty()) return L;
    L.present = true;
    auto dot = t.find('.');
    if (dot == std::string::npos) return L;
    std::string payloadB64 = t.substr(0, dot), sigB64 = t.substr(dot + 1);
    std::vector<unsigned char> sig, payload;
    if (b64urlDecode(sigB64, sig) || sig.size() != 64) return L;
    if (b64urlDecode(payloadB64, payload)) return L;
    // crypto_sign_open wants signed message = signature(64) || message; message = payloadB64 ASCII.
    std::vector<unsigned char> sm = sig;
    sm.insert(sm.end(), payloadB64.begin(), payloadB64.end());
    std::vector<unsigned char> m(sm.size());
    unsigned long long mlen = 0;
    if (crypto_sign_open(m.data(), &mlen, sm.data(), sm.size(), pub) != 0) return L;
    L.signatureOk = true;
    std::string js(reinterpret_cast<const char*>(payload.data()), payload.size());
    L.exp = jsonInt(js, "exp");
    L.email = jsonStr(js, "email");
    return L;
}

inline License parseToken(const std::string& token) { return parseTokenWithKey(token, kPublicKey); }

inline std::string dataDir() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    return base ? std::string(base) + "\\PRO PLUGS\\LookMatch\\" : "";
#else
    const char* base = std::getenv("HOME");
    return base ? std::string(base) + "/Library/Application Support/PRO PLUGS/LookMatch/" : "";
#endif
}

inline std::string readFileText(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    char buf[8192] = {0};
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    return std::string(buf, n);
}

// Max Unix time ever observed, persisted next to the key, so setting the clock back can't revive an
// expired key. Generous slack (2 days) absorbs timezone / DST moves and manual corrections.
inline long long antiRollbackNow() {
    const long long slack = 2 * 24 * 3600;
    long long now = static_cast<long long>(std::time(nullptr));
    const std::string path = dataDir() + "license.seen";
    long long seen = 0;
    std::string s = readFileText(path);
    if (!s.empty()) seen = std::atoll(s.c_str());
    if (seen > now + slack) now = seen;  // clock rolled back: keep using the last seen time
    if (now > seen) {
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "%lld", now);
            std::fclose(f);
        }
    }
    return now;
}

struct Status {
    bool licensed = false;  // valid signature AND not expired
    bool present = false;   // a key file exists (any contents)
    long long exp = 0;
};

// Reads and verifies the key file, cached ~4 s so Render doesn't touch disk every frame.
inline Status status() {
    static std::mutex mtx;
    static Status cached;
    static long long cachedAt = 0;
    std::lock_guard<std::mutex> lock(mtx);
    long long now = static_cast<long long>(std::time(nullptr));
    if (cachedAt != 0 && now - cachedAt < 4 && now >= cachedAt) return cached;
    std::string token = readFileText(dataDir() + "license.key");
    License L = parseToken(token);
    long long t = antiRollbackNow();
    Status st;
    st.present = L.present;
    st.exp = L.exp;
    st.licensed = L.signatureOk && L.exp > t;
    cached = st;
    cachedAt = now;
    return st;
}

}  // namespace lmlic
