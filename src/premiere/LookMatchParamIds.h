// LookMatch parameter indices and disk IDs. No host SDK dependencies, so the CPU effect
// (LookMatch.cpp) and the Premiere GPU filter (LookMatchGPU.mm) share them.
#pragma once

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
    // compare: before/after split wipe
    P_COMPARE_TOPIC,
    P_SPLIT,        // checkbox
    P_SPLIT_POS,    // % across the frame
    P_SPLIT_ANGLE,  // degrees, 0 = vertical line
    P_COMPARE_TOPIC_END,
    // source-aware match: how the clip is read and normalised before the reference's look
    P_SOURCE_TOPIC,
    P_SOURCE_FORMAT,    // popup: Auto, then lm::Transfer order
    P_NORM_EXPOSURE,    // %
    P_NORM_WB,          // %
    P_NORM_CONTRAST,    // %
    P_LOOK_STRENGTH,    // %
    P_SOURCE_TOPIC_END,
    // after the source group (appended, so projects saved before it keep their parameter layout)
    P_MATCH_CONTRAST,   // %: final tone match of the graded result to the reference's levels
    P_FIX_BANDING,      // checkbox: deband the source (skies), off by default
    P_SKIN_EVENING,     // %: pull skin's colour toward the skin around it
    P_FINISH,           // %: lift the match to where a finished grade lands ("Finish Band")
    // tone and colour adjustments after the match (src/core/adjust.h)
    P_ADJUST_TOPIC,
    P_ADJ_EXPOSURE,     // stops
    P_ADJ_CONTRAST,     // -100 .. 100
    P_ADJ_SHADOWS,      // -100 .. 100
    P_ADJ_HIGHLIGHTS,   // -100 .. 100
    P_ADJ_WHITES,       // -100 .. 100
    P_ADJ_SATURATION,   // -100 .. 100
    P_ADJ_TEMPERATURE,  // -100 .. 100
    P_ADJUST_TOPIC_END,
    P_NUM_PARAMS
};

// Stable disk IDs: never renumber, only append (projects store parameters by these).
enum DiskId {
    D_MATCH_TOPIC = 1, D_LOAD_REF, D_REMATCH, D_MATCH_STRENGTH, D_REGION_STRENGTH, D_SKIN_AUTO, D_SKIN_PROTECT, D_MATCH_TOPIC_END,
    D_INPUT_TOPIC, D_INPUT_SPACE, D_EXPOSURE, D_TEMPERATURE, D_TINT, D_INPUT_TOPIC_END,
    D_FILM_TOPIC, D_FILM_STRENGTH, D_NEGATIVE, D_PRINT, D_PRINTER_R, D_PRINTER_G, D_PRINTER_B, D_FILM_SAT, D_FADE, D_FILM_TOPIC_END,
    D_TEXTURE_TOPIC, D_HALATION, D_HALATION_RADIUS, D_HALATION_THRESHOLD, D_BLOOM, D_BLOOM_RADIUS, D_BLOOM_THRESHOLD,
    D_GRAIN, D_GRAIN_SIZE, D_GRAIN_COLOR, D_SOFTNESS, D_VIGNETTE, D_WEAVE, D_FLICKER, D_TEXTURE_TOPIC_END,
    D_MIX, D_EXPORT_LUT, D_MATCH_DATA, D_REF_SLOT, D_PANEL_TOPIC, D_BAKE, D_EXPORT_SLOT, D_PANEL_TOPIC_END, D_CAPTURE_SLOT, D_THUMBS_SLOT,
    D_COMPARE_TOPIC, D_SPLIT, D_SPLIT_POS, D_SPLIT_ANGLE, D_COMPARE_TOPIC_END,
    D_SOURCE_TOPIC, D_SOURCE_FORMAT, D_NORM_EXPOSURE, D_NORM_WB, D_NORM_CONTRAST, D_LOOK_STRENGTH, D_SOURCE_TOPIC_END,
    D_MATCH_CONTRAST,
    D_FIX_BANDING,
    D_SKIN_EVENING,
    D_FINISH,
    D_ADJUST_TOPIC, D_ADJ_EXPOSURE, D_ADJ_CONTRAST, D_ADJ_SHADOWS, D_ADJ_HIGHLIGHTS, D_ADJ_WHITES, D_ADJ_SATURATION,
    D_ADJ_TEMPERATURE, D_ADJUST_TOPIC_END
};
