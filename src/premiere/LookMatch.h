// LookMatch effect for Premiere Pro (and After Effects), built on the Adobe AE effect API.
#pragma once

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "AEFX_SuiteHelper.h"
#include "Param_Utils.h"
#include "PrSDKAESupport.h"
#include "String_Utils.h"

#include <cstdint>

#define LM_NAME "LookMatch"
#define LM_DESCRIPTION "Match the look of any still, then finish it with film print emulation, halation, bloom and grain."
#define LM_MAJOR 1
#define LM_MINOR 0
#define LM_BUG 1
#define LM_BUILD 1

enum ParamId {
    P_INPUT_LAYER = 0,
    // look match
    P_MATCH_TOPIC,
    P_LOAD_REF,
    P_REMATCH,
    P_MATCH_STRENGTH,
    P_REGION_STRENGTH,
    P_SKIN_AUTO,
    P_SKIN_PROTECT,
    P_MATCH_TOPIC_END,
    // input
    P_INPUT_TOPIC,
    P_INPUT_SPACE,
    P_EXPOSURE,
    P_TEMPERATURE,
    P_TINT,
    P_INPUT_TOPIC_END,
    // film colour
    P_FILM_TOPIC,
    P_FILM_STRENGTH,
    P_NEGATIVE,
    P_PRINT,
    P_PRINTER_R,
    P_PRINTER_G,
    P_PRINTER_B,
    P_FILM_SAT,
    P_FADE,
    P_FILM_TOPIC_END,
    // film texture
    P_TEXTURE_TOPIC,
    P_HALATION,
    P_HALATION_RADIUS,
    P_HALATION_THRESHOLD,
    P_BLOOM,
    P_BLOOM_RADIUS,
    P_BLOOM_THRESHOLD,
    P_GRAIN,
    P_GRAIN_SIZE,
    P_GRAIN_COLOR,
    P_SOFTNESS,
    P_VIGNETTE,
    P_WEAVE,
    P_FLICKER,
    P_TEXTURE_TOPIC_END,
    // output
    P_MIX,
    P_EXPORT_LUT,
    P_MATCH_DATA,  // hidden: the computed match (see MatchData)
    // panel link (set by the LookMatch panel; see panelReferencePath)
    P_PANEL_TOPIC,
    P_REF_SLOT,     // match to the reference the panel wrote for this slot
    P_BAKE,         // button: store the panel's match in the project
    P_EXPORT_SLOT,  // write a .cube to the path the panel wrote for this slot
    P_CAPTURE_SLOT, // save this clip's frame as refs/capture-<slot>.png (hero clip -> reference)
    P_THUMBS_SLOT,  // render small previews of the looks listed in refs/thumbs-<slot>.txt
    P_PANEL_TOPIC_END,
    P_NUM_PARAMS
};

// Stable disk IDs: never renumber, only append (projects store parameters by these).
enum DiskId {
    D_MATCH_TOPIC = 1, D_LOAD_REF, D_REMATCH, D_MATCH_STRENGTH, D_REGION_STRENGTH, D_SKIN_AUTO, D_SKIN_PROTECT, D_MATCH_TOPIC_END,
    D_INPUT_TOPIC, D_INPUT_SPACE, D_EXPOSURE, D_TEMPERATURE, D_TINT, D_INPUT_TOPIC_END,
    D_FILM_TOPIC, D_FILM_STRENGTH, D_NEGATIVE, D_PRINT, D_PRINTER_R, D_PRINTER_G, D_PRINTER_B, D_FILM_SAT, D_FADE, D_FILM_TOPIC_END,
    D_TEXTURE_TOPIC, D_HALATION, D_HALATION_RADIUS, D_HALATION_THRESHOLD, D_BLOOM, D_BLOOM_RADIUS, D_BLOOM_THRESHOLD,
    D_GRAIN, D_GRAIN_SIZE, D_GRAIN_COLOR, D_SOFTNESS, D_VIGNETTE, D_WEAVE, D_FLICKER, D_TEXTURE_TOPIC_END,
    D_MIX, D_EXPORT_LUT, D_MATCH_DATA, D_REF_SLOT, D_PANEL_TOPIC, D_BAKE, D_EXPORT_SLOT, D_PANEL_TOPIC_END, D_CAPTURE_SLOT, D_THUMBS_SLOT
};

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

// platform file dialogs (Dialogs_mac.mm / Dialogs_win.cpp). Return false if cancelled.
bool LM_OpenImageDialog(char* pathOut, int capacity);
bool LM_SaveCubeDialog(char* pathOut, int capacity);

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                     const char* inHostVersion);

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra);
