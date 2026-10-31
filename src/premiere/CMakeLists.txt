# LookMatch effect for Premiere Pro / After Effects.
# Configure from the repo root with:
#   cmake -S . -B build -DLOOKMATCH_BUILD_PLUGIN=ON -DAE_SDK_DIR=/path/to/AfterEffectsSDK/Examples
set(AE_SDK_DIR "" CACHE PATH "Path to the After Effects SDK 'Examples' folder")
if(NOT EXISTS "${AE_SDK_DIR}/Headers/AE_Effect.h")
  message(FATAL_ERROR "Set AE_SDK_DIR to the After Effects SDK 'Examples' folder (it contains Headers/AE_Effect.h)")
endif()

# The match cache must miss when the engine changes: hash its sources into the key.
file(GLOB LM_ENGINE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/core/*.cpp" "${CMAKE_SOURCE_DIR}/src/core/*.h")
list(SORT LM_ENGINE_FILES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LM_ENGINE_FILES})
set(LM_ENGINE_TEXT "")
foreach(f ${LM_ENGINE_FILES})
  file(SHA256 "${f}" h)
  string(APPEND LM_ENGINE_TEXT "${h}")
endforeach()
string(SHA256 LM_ENGINE_HASH "${LM_ENGINE_TEXT}")
add_compile_definitions(LM_ENGINE_HASH="${LM_ENGINE_HASH}")

file(GLOB AE_SUITE_HELPER "${AE_SDK_DIR}/Util/AEFX_SuiteHelper.c*")
set(AE_INCLUDES
  "${AE_SDK_DIR}/Headers"
  "${AE_SDK_DIR}/Headers/SP"
  "${AE_SDK_DIR}/Util"
  "${AE_SDK_DIR}/Resources")

set(SOURCES LookMatch.cpp ${AE_SUITE_HELPER})

if(WIN32)
  # PiPL: preprocess .r -> PiPLTool -> .rc, as in the SDK's Visual Studio projects
  set(PIPL_RR  "${CMAKE_CURRENT_BINARY_DIR}/LookMatchPiPL.rr")
  set(PIPL_RRC "${CMAKE_CURRENT_BINARY_DIR}/LookMatchPiPL.rrc")
  set(PIPL_RC  "${CMAKE_CURRENT_BINARY_DIR}/LookMatchPiPL.rc")
  set(PIPL_INC "")
  foreach(d ${AE_INCLUDES})
    list(APPEND PIPL_INC "/I${d}")
  endforeach()
  add_custom_command(
    OUTPUT "${PIPL_RC}"
    COMMAND cl ${PIPL_INC} /EP "${CMAKE_CURRENT_SOURCE_DIR}/LookMatchPiPL.r" > "${PIPL_RR}"
    COMMAND "${AE_SDK_DIR}/Resources/PiPLTool.exe" "${PIPL_RR}" "${PIPL_RRC}"
    COMMAND cl /D "MSWindows" /EP "${PIPL_RRC}" > "${PIPL_RC}"
    DEPENDS LookMatchPiPL.r
    COMMENT "Compiling PiPL resource")
  list(APPEND SOURCES Dialogs_win.cpp "${PIPL_RC}")
  add_library(LookMatch MODULE ${SOURCES})
  set_target_properties(LookMatch PROPERTIES SUFFIX ".aex" PREFIX "")
  target_compile_definitions(LookMatch PRIVATE MSWindows WIN32 _WINDOWS UNICODE _UNICODE NOMINMAX)
  target_link_libraries(LookMatch PRIVATE comdlg32 user32)
elseif(APPLE)
  enable_language(OBJCXX)
  list(APPEND SOURCES Dialogs_mac.mm)
  # PiPL is compiled with Rez into the bundle's resource file
  set(PIPL_RSRC "${CMAKE_CURRENT_BINARY_DIR}/LookMatch.rsrc")
  set(REZ_INC "")
  foreach(d ${AE_INCLUDES})
    list(APPEND REZ_INC -i "${d}")
  endforeach()
  add_custom_command(
    OUTPUT "${PIPL_RSRC}"
    COMMAND xcrun Rez -o "${PIPL_RSRC}" -useDF -script Roman -d AE_OS_MAC -d AE_OS_MAC_ARM64
            -arch arm64 -arch x86_64 ${REZ_INC} "${CMAKE_CURRENT_SOURCE_DIR}/LookMatchPiPL.r"
    DEPENDS LookMatchPiPL.r
    COMMENT "Compiling PiPL resource")
  set_source_files_properties("${PIPL_RSRC}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  add_library(LookMatch MODULE ${SOURCES} "${PIPL_RSRC}")
  set_target_properties(LookMatch PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "plugin"
    OSX_ARCHITECTURES "arm64;x86_64"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.proplugs.lookmatch"
    MACOSX_BUNDLE_BUNDLE_NAME "LookMatch"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/LookMatch-Info.plist.in")
  # PkgInfo (type + creator), as the SDK samples generate with GENERATE_PKGINFO_FILE
  add_custom_command(TARGET LookMatch POST_BUILD
    COMMAND /bin/sh -c "printf eFKTFXTC > \"$<TARGET_BUNDLE_CONTENT_DIR:LookMatch>/PkgInfo\""
    VERBATIM)
  target_link_libraries(LookMatch PRIVATE "-framework Cocoa" "-weak_framework UniformTypeIdentifiers")  # weak: absent before macOS 11

  # Premiere GPU render (Metal). Needs the Premiere Pro SDK's Examples folder (any version from CC 2017;
  # the GPU filter interface is unchanged since). Without it the effect renders on the CPU only.
  set(LOOKMATCH_PR_SDK_DIR "" CACHE PATH "Premiere Pro SDK 'Examples' folder (enables the GPU render)")
  if(LOOKMATCH_PR_SDK_DIR)
    if(NOT EXISTS "${LOOKMATCH_PR_SDK_DIR}/Headers/PrSDKGPUFilter.h")
      message(FATAL_ERROR "LOOKMATCH_PR_SDK_DIR must be the Premiere Pro SDK 'Examples' folder (Headers/PrSDKGPUFilter.h)")
    endif()
    target_sources(LookMatch PRIVATE LookMatchGPU.mm)
    # Premiere and After Effects SDK headers share names: scope the Premiere ones to this file only
    set_source_files_properties(LookMatchGPU.mm PROPERTIES
      INCLUDE_DIRECTORIES "${LOOKMATCH_PR_SDK_DIR}/Headers"
      COMPILE_OPTIONS "-fno-objc-arc")
    target_link_libraries(LookMatch PRIVATE "-framework Metal" "-framework Foundation")
    target_compile_definitions(LookMatch PRIVATE LOOKMATCH_GPU=1)
    message(STATUS "LookMatch: Premiere GPU render enabled (${LOOKMATCH_PR_SDK_DIR})")
  endif()
  # the universal plugin links the engine, so the engine must be universal too
  set_target_properties(lookmatch_core PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
else()
  message(FATAL_ERROR "The Premiere effect builds on Windows and macOS only")
endif()

target_include_directories(LookMatch PRIVATE ${AE_INCLUDES} "${CMAKE_SOURCE_DIR}/third_party" "${CMAKE_SOURCE_DIR}/src/core")
target_link_libraries(LookMatch PRIVATE lookmatch_core)
