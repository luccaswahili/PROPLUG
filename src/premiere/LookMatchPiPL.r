// PiPL resource: tells Premiere / After Effects what this plug-in is before loading it.
// AE_Effect_Global_OutFlags must equal out_data->out_flags set in GlobalSetup (LookMatch.cpp):
//   PF_OutFlag_NON_PARAM_VARY (1<<2) | PF_OutFlag_DEEP_COLOR_AWARE (1<<25) = 0x02000004
#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { "LookMatch" },
		Category { "PRO PLUGS" },
#ifdef AE_OS_WIN
	#if defined(AE_PROC_INTELx64)
		CodeWin64X86 { "EffectMain" },
	#elif defined(AE_PROC_ARM64)
		CodeWinARM64 { "EffectMain" },
	#endif
#else
	#if defined(AE_OS_MAC)
		CodeMacARM64 { "EffectMain" },
		CodeMacIntel64 { "EffectMain" },
	#endif
#endif
		AE_PiPL_Version { 2, 0 },
		AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },
		AE_Effect_Version { 528385 },	/* PF_VERSION(1, 0, 2, PF_Stage_DEVELOP, 1) */
		AE_Effect_Info_Flags { 0 },
		AE_Effect_Global_OutFlags { 0x02000004 },
		AE_Effect_Global_OutFlags_2 { 0x00000000 },
		AE_Effect_Match_Name { "PROPLUGS LookMatch" },
		AE_Reserved_Info { 0 },
		AE_Effect_Support_URL { "https://github.com/luccaswahili" }
	}
};
