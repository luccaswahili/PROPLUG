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

#include "LookMatchParamIds.h"

#include "MatchData.h"

// platform file dialogs (Dialogs_mac.mm / Dialogs_win.cpp). Return false if cancelled.
bool LM_OpenImageDialog(char* pathOut, int capacity);
bool LM_SaveCubeDialog(char* pathOut, int capacity);

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                     const char* inHostVersion);

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra);
