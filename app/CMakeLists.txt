cmake_minimum_required(VERSION 3.25)
project(FastMD LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: no VC runtime DLLs to load or ship
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(FASTMD_CFG "Control Flow Guard (+ /GS)" ON)
option(FASTMD_COMCTL6 "comctl32 v6 dependency in the process manifest (themed message boxes)" ON)
# dev builds stay in the build tree (build/<Config>/FastMD.exe); publish.ps1 copies a build to out/Release, the
# copy the .md association points to — so an open document never locks the file the next build writes
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(FASTMD_COMCTL6)
    set(FASTMD_MANIFEST ${CMAKE_SOURCE_DIR}/res/fastmd.manifest)
else()
    set(FASTMD_MANIFEST ${CMAKE_SOURCE_DIR}/res/fastmd-nocomctl.manifest)
endif()
if(FASTMD_CFG)
    set(FASTMD_SEC_C /GS /guard:cf)
    set(FASTMD_SEC_L /guard:cf)
else()
    set(FASTMD_SEC_C /GS-)
    set(FASTMD_SEC_L "")
endif()

add_executable(FastMD WIN32
    src/view.cpp src/window.cpp src/loader.cpp src/parse.cpp src/layout.cpp src/canvas_gdi.cpp
    src/highlight.cpp src/theme.cpp src/util.cpp
    third_party/md4c/md4c.c
    res/fastmd.rc)

target_compile_definitions(FastMD PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MD4C_USE_UTF16)
target_compile_options(FastMD PRIVATE /utf-8 /W3 /permissive- /Zc:__cplusplus /EHsc
    $<$<CONFIG:Release>:/O2 /Oi /GL /Gy /DNDEBUG> ${FASTMD_SEC_C})
set_source_files_properties(third_party/md4c/md4c.c PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4267")

# Everything not needed for the first frame is delay-loaded: dwrite is mapped on the doc thread (in parallel with
# window creation); COM/WIC/shell/dialogs only when used after the first frame.
target_link_options(FastMD PRIVATE
    /MANIFEST:EMBED /MANIFESTINPUT:${FASTMD_MANIFEST}
    /DELAYLOAD:dwrite.dll /DELAYLOAD:ole32.dll /DELAYLOAD:shell32.dll /DELAYLOAD:imm32.dll
    /DELAYLOAD:windowscodecs.dll /DELAYLOAD:comdlg32.dll
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO> ${FASTMD_SEC_L})
target_link_libraries(FastMD PRIVATE user32 gdi32 dwmapi dwrite ole32 shell32 imm32 windowscodecs comdlg32 advapi32
    delayimp)
