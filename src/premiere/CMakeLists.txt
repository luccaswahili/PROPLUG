# LookMatch effect for Premiere Pro / After Effects.
# Configure from the repo root with:
#   cmake -S . -B build -DLOOKMATCH_BUILD_PLUGIN=ON -DAE_SDK_DIR=/path/to/AfterEffectsSDK/Examples
set(AE_SDK_DIR "" CACHE PATH "Path to the After Effects SDK 'Examples' folder")
if(NOT EXISTS "${AE_SDK_DIR}/Headers/AE_Effect.h")
  message(FATAL_ERROR "Set AE_SDK_DIR to the After Effects SDK 'Examples' folder (it contains Headers/AE_Effect.h)")
endif()

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
    MACOSX_BUNDLE_BUNDLE_NAME "LookMatch")
  target_link_libraries(LookMatch PRIVATE "-framework Cocoa" "-framework UniformTypeIdentifiers")
else()
  message(FATAL_ERROR "The Premiere effect builds on Windows and macOS only")
endif()

target_include_directories(LookMatch PRIVATE ${AE_INCLUDES} "${CMAKE_SOURCE_DIR}/third_party" "${CMAKE_SOURCE_DIR}/src/core")
target_link_libraries(LookMatch PRIVATE lookmatch_core)
