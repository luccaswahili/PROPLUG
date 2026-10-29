// The match stored in LookMatch's hidden "Match Data" parameter. No host SDK dependencies, so the
// CPU effect and the Premiere GPU filter can both use it.
#pragma once
#include <cstdint>

// The computed match lives in a hidden arbitrary-data parameter, not in sequence data:
// Premiere renders on separate effect instances with their own copies of sequence data, so a
// match computed when the user clicks a button never reached the renderer. Parameters are
// passed to every render, saved in the project and undoable. Plain old data, no pointers.
constexpr int kMatchLutSize = 33;
constexpr uint32_t kMatchMagic = 0x4C4D4D44;  // 'LMMD'
constexpr uint32_t kMatchVersion = 1;

struct MatchData {
    uint32_t magic;
    uint32_t version;
    uint64_t matchId;  // changes every time a new match is computed (engine cache key)
    int32_t hasMatch;
    int32_t panelSlot;  // Reference Slot value this match was made at (a newer panel slot wins)
    char referencePath[1024];  // UTF-8, used by Match to This Frame
    uint16_t lut[kMatchLutSize * kMatchLutSize * kMatchLutSize * 3];  // display RGB 0..1 as 0..65535
};
