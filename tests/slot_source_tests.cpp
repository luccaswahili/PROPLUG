// Live grade sliders: the slot's frame survives a round trip, and the settings key changes exactly when a
// match setting does (src/premiere/SlotSource.h).
#include <cstdlib>
#include <cstdio>
#include <string>
#include "../src/premiere/SlotSource.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

int main() {
    lmpanel::SlotSource s;
    s.src.width = 3; s.src.height = 2;
    for (int i = 0; i < 6; ++i) s.src.px.push_back({i * 0.1f, 0.5f, 1.0f - i * 0.1f});
    s.detected = 7; s.logGuess = "F-Log (0.82)";
    const std::string file = "slot_source_test.src";
    CHECK(lmpanel::writeSlotSource(file, s));
    auto r = lmpanel::readSlotSource(file);
    CHECK(r && r->src.width == 3 && r->src.height == 2 && r->detected == 7 && r->logGuess == "F-Log (0.82)");
    for (int i = 0; i < 6; ++i) CHECK(r->src.px[i].r == s.src.px[i].r && r->src.px[i].b == s.src.px[i].b);
    std::remove(file.c_str());
    CHECK(!lmpanel::readSlotSource(file));

    lmpanel::MatchSettings a, b;
    CHECK(lmpanel::settingsKey(a) == lmpanel::settingsKey(b));
    b.finish = 100.0001f;  // float noise between the CPU and GPU readings
    CHECK(lmpanel::settingsKey(a) == lmpanel::settingsKey(b));
    b.finish = 99;  // a 1% slide is a change
    CHECK(lmpanel::settingsKey(a) != lmpanel::settingsKey(b));
    b = a; b.lookStrength = 50; CHECK(lmpanel::settingsKey(a) != lmpanel::settingsKey(b));
    b = a; b.matchContrast = 0; CHECK(lmpanel::settingsKey(a) != lmpanel::settingsKey(b));
    b = a; b.skinAuto = 0; CHECK(lmpanel::settingsKey(a) != lmpanel::settingsKey(b));
    CHECK(lmpanel::keyHex(lmpanel::settingsKey(a)).size() == 16);
    std::puts("slot_source_tests: ok");
    return 0;
}
