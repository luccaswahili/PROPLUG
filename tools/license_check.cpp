// Standalone end-to-end check of the licence path with the PRODUCTION public key: reads the installed
// license.key through the exact code the plugin uses (src/premiere/License.h + TweetNaCl) and prints the
// status. Build on macOS:
//   cc -c third_party/tweetnacl/tweetnacl.c -Ithird_party/tweetnacl -o /tmp/tn.o
//   c++ -std=c++17 -Isrc/premiere -Ithird_party/tweetnacl tools/license_check.cpp /tmp/tn.o -o /tmp/lickcheck
//   /tmp/lickcheck
#include <cstdio>
#include <ctime>
#include "License.h"

extern "C" void randombytes(unsigned char*, unsigned long long) {}  // TweetNaCl keygen path, unused here

int main() {
    std::string path = lmlic::dataDir() + "license.key";
    std::string tok = lmlic::readFileText(path);
    lmlic::License L = lmlic::parseToken(tok);
    lmlic::Status st = lmlic::status();
    long long now = (long long)std::time(nullptr);
    std::printf("file:        %s\n", path.c_str());
    std::printf("present:     %s\n", L.present ? "yes" : "no");
    std::printf("signatureOk: %s  (verified against the embedded production public key)\n", L.signatureOk ? "yes" : "NO");
    std::printf("email:       %s\n", L.email.c_str());
    std::printf("exp:         %lld  (%.2f days from now)\n", L.exp, (L.exp - now) / 86400.0);
    std::printf("LICENSED:    %s\n", st.licensed ? "YES — plugin unlocks" : "no — plugin stays locked");
    return st.licensed ? 0 : 1;
}
