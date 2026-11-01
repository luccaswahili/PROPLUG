// Unit tests for Komodo license verification (src/premiere/License.h). Uses a throwaway test keypair,
// independent of the production public key, so the vectors below never depend on the shipping key.
#include <cstdio>
#include <string>
#include "License.h"

static const unsigned char kTestPub[32] = { 0xac,0xa9,0x64,0x4f,0xf0,0xe4,0xe9,0x9d,0x60,0xb5,0xc7,0xcc,0x12,0xa5,0xcb,0x39,0xed,0x48,0xdd,0x20,0x1a,0x2e,0xf1,0x57,0xe0,0x44,0x0c,0xa6,0x75,0xca,0xd8,0x41 };
// Signed with the matching test private key by tools/gen (Node crypto, Ed25519).
static const char* kFuture = "eyJ2IjoxLCJpZCI6IlVOSVQiLCJlbWFpbCI6InVuaXRAdGVzdCIsImlhdCI6MSwiZXhwIjo0MTAyNDQ0ODAwfQ.O7_mrXuWlQpvCu59gb8riCpBPmlrdQy_VPFOmRmx4a8TuAloEnAyviOhx3xCNvk4Y1yuJQ8VeyDq_9wim3RKCA";  // exp = 4102444800 (year 2100)
static const char* kPast   = "eyJ2IjoxLCJpZCI6IlVOSVQiLCJlbWFpbCI6InVuaXRAdGVzdCIsImlhdCI6MSwiZXhwIjoxMDAwfQ.j2b-bBeiNwkq2B1A5Vo0SAMwKcb0Ve3bUUtIbb2hCsdAVb5O7LIC4T5Ulu7Tb5vzBknBwp4yS4lW01L8qqK8Cw";  // exp = 1000 (long expired)

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", #c, __LINE__); ++failures; } } while (0)

int main() {
    // valid signature, future expiry
    lmlic::License f = lmlic::parseTokenWithKey(kFuture, kTestPub);
    CHECK(f.present);
    CHECK(f.signatureOk);
    CHECK(f.exp == 4102444800LL);
    CHECK(f.email == "unit@test");

    // valid signature, past expiry (signatureOk true, but caller treats as unlicensed via exp)
    lmlic::License p = lmlic::parseTokenWithKey(kPast, kTestPub);
    CHECK(p.signatureOk);
    CHECK(p.exp == 1000LL);

    // tampered signature fails
    std::string bad = kFuture;
    bad[bad.size() - 3] = (bad[bad.size() - 3] == 'A') ? 'B' : 'A';
    lmlic::License b = lmlic::parseTokenWithKey(bad, kTestPub);
    CHECK(b.present);
    CHECK(!b.signatureOk);

    // wrong key fails
    lmlic::License w = lmlic::parseTokenWithKey(kFuture, lmlic::kPublicKey);
    CHECK(!w.signatureOk);

    // garbage / empty
    CHECK(!lmlic::parseTokenWithKey("", kTestPub).present);
    CHECK(!lmlic::parseTokenWithKey("not-a-token", kTestPub).signatureOk);
    CHECK(!lmlic::parseTokenWithKey("a.b", kTestPub).signatureOk);

    if (failures == 0) std::printf("license_tests: all passed\n");
    return failures ? 1 : 0;
}

// TweetNaCl references randombytes from its key-generation functions, which these tests never call.
extern "C" void randombytes(unsigned char*, unsigned long long) { }
