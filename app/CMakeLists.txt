cmake_minimum_required(VERSION 3.25)
# the one place the version lives: src/version.h (generated) feeds the exe resource and the settings window
project(FastMD VERSION 0.2.0 LANGUAGES C CXX RC)

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

configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

add_executable(FastMD WIN32
    src/view.cpp src/window.cpp src/loader.cpp src/parse.cpp src/html.cpp src/net.cpp src/svg.cpp
    src/layout.cpp src/canvas_gdi.cpp
    src/highlight.cpp src/theme.cpp src/util.cpp
    src/find.cpp src/toc.cpp src/home.cpp src/store.cpp src/shell.cpp src/settings_ui.cpp src/strings.cpp
    third_party/md4c/md4c.c
    res/fastmd.rc)

target_include_directories(FastMD PRIVATE ${CMAKE_BINARY_DIR}/generated)
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
    /DELAYLOAD:windowscodecs.dll /DELAYLOAD:comdlg32.dll /DELAYLOAD:winhttp.dll
    # /MANIFEST:EMBED with our own manifest: incremental linking would expect a separate .manifest file (Debug)
    /INCREMENTAL:NO
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF> ${FASTMD_SEC_L})
target_link_libraries(FastMD PRIVATE user32 gdi32 dwmapi dwrite ole32 shell32 imm32 windowscodecs comdlg32 advapi32
    winhttp delayimp)

# ---------------------------------------------------------------------------------------------- fastmd-svg.dll
# SVG (badges, diagrams) is drawn by lunasvg + plutovg. They live in a DLL, not in the exe: CreateProcess pays
# ~1.2-1.9 ms per megabyte of the exe image on every launch, while a DLL is mapped only when it is first needed -
# and SVG is never needed before the first frame.
file(GLOB LUNASVG_SOURCES third_party/lunasvg/source/*.cpp)
file(GLOB PLUTOVG_SOURCES third_party/plutovg/source/*.c)
add_library(fastmd_svg SHARED src/svg_dll.cpp ${LUNASVG_SOURCES} ${PLUTOVG_SOURCES})
set_target_properties(fastmd_svg PROPERTIES OUTPUT_NAME "fastmd-svg")
target_include_directories(fastmd_svg PRIVATE third_party/lunasvg/include third_party/plutovg/include
    third_party/plutovg/source)
target_compile_definitions(fastmd_svg PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    LUNASVG_BUILD_STATIC PLUTOVG_BUILD_STATIC)
target_compile_options(fastmd_svg PRIVATE /utf-8 /W0 /EHsc $<$<CONFIG:Release>:/O2 /Oi /Gy /DNDEBUG> ${FASTMD_SEC_C})
target_link_options(fastmd_svg PRIVATE /INCREMENTAL:NO $<$<CONFIG:Release>:/OPT:REF /OPT:ICF> ${FASTMD_SEC_L})
target_link_libraries(fastmd_svg PRIVATE user32 gdi32)
