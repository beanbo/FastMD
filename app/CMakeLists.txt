cmake_minimum_required(VERSION 3.25)
# the one place the version lives: src/version.h (generated) feeds the exe resource and the settings window
project(FastMD VERSION 1.3.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Every target names its optimisation level itself (edit mode's sources build with /O1, the rest with /O2): CMake's
# default /O2 in the Release flags would clash with that
string(REPLACE "/O2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
string(REPLACE "/O2" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
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
    src/view.cpp src/window.cpp src/loader.cpp src/parse.cpp src/html.cpp
    src/layout.cpp src/canvas_gdi.cpp
    src/highlight.cpp src/theme.cpp src/util.cpp
    src/find.cpp src/toc.cpp src/home.cpp src/strings.cpp
    third_party/md4c/md4c.c
    res/fastmd.rc)

target_include_directories(FastMD PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(FastMD PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MD4C_USE_UTF16)
target_compile_options(FastMD PRIVATE /utf-8 /W3 /permissive- /Zc:__cplusplus /EHsc
    $<$<CONFIG:Release>:/O2 /Oi /GL /Gy /DNDEBUG> ${FASTMD_SEC_C})
set_source_files_properties(third_party/md4c/md4c.c PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4267")

# Edit mode's sources favour size (docs/EDIT-MODE.md §1 principle 3): none of them is on a hot path except the map
# queries, which are measured, and the exe pays ~1.5 ms per MB on every launch whether edit mode is used or not. They
# are compiled on their own with /O1 (the rest of the program keeps /O2) and linked into the exe with everything else.
# The screen-reader provider joins them: it runs only while a screen reader asks, one call at a time.
add_library(fastmd_edit OBJECT src/editcore.cpp src/editops.cpp src/editfile.cpp src/edit.cpp src/editbar.cpp src/editpop.cpp
    src/uia.cpp)
# The cold half of reading mode goes the same way (Phase 3a): the updater, the settings window, the shell's dialogs,
# links and clipboard, printing and its canvas, the crash handler, dragging out and copying run on a click or after the
# first frame - never on the way to it, never while scrolling (the gear is one icon a frame); so do the downloads, the
# glue of the SVG, TeX and Mermaid libraries (the work is in the DLLs, on workers) and the reading-mode task tick; and
# (Phase 3b) the settings and positions store - a handful of registry reads on the way to the first frame, the rest
# after it. The parser, layout, view, canvas, window, loader, start screen, highlighter, theme, strings, find and outline
# keep /O2.
add_library(fastmd_cold OBJECT src/update.cpp src/settings_ui.cpp src/shell.cpp src/print.cpp src/canvas_print.cpp
    src/crash.cpp src/drag.cpp src/copy.cpp src/net.cpp src/svg.cpp src/formulas.cpp src/tasks.cpp src/store.cpp)
foreach(lib fastmd_edit fastmd_cold)
    target_include_directories(${lib} PRIVATE ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(${lib} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
        MD4C_USE_UTF16)
    target_compile_options(${lib} PRIVATE /utf-8 /W3 /permissive- /Zc:__cplusplus /EHsc
        $<$<CONFIG:Release>:/O1 /Oi /GL /Gy /DNDEBUG> ${FASTMD_SEC_C})
    target_sources(FastMD PRIVATE $<TARGET_OBJECTS:${lib}>)
endforeach()
# the version resource is generated from version.h, and the resource compiler's dependencies are not scanned:
# without this a version bump left the old number inside the exe
set_source_files_properties(res/fastmd.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/generated/version.h)

# Everything not needed for the first frame is delay-loaded: dwrite is mapped on the doc thread (in parallel with
# window creation); COM/WIC/shell/dialogs only when used after the first frame.
target_link_options(FastMD PRIVATE
    /MANIFEST:EMBED /MANIFESTINPUT:${FASTMD_MANIFEST}
    /DELAYLOAD:dwrite.dll /DELAYLOAD:ole32.dll /DELAYLOAD:shell32.dll /DELAYLOAD:imm32.dll
    /DELAYLOAD:windowscodecs.dll /DELAYLOAD:comdlg32.dll /DELAYLOAD:winhttp.dll
    /DELAYLOAD:winspool.drv /DELAYLOAD:uiautomationcore.dll /DELAYLOAD:uxtheme.dll
    # /MANIFEST:EMBED with our own manifest: incremental linking would expect a separate .manifest file (Debug)
    /INCREMENTAL:NO
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF> ${FASTMD_SEC_L})
target_link_libraries(FastMD PRIVATE user32 gdi32 dwmapi dwrite ole32 shell32 imm32 windowscodecs comdlg32 advapi32
    winhttp winspool uiautomationcore uxtheme delayimp)

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

# ------------------------------------------------------------------------------- fastmd-tex.dll, fastmd-mermaid.dll
# Formulas (RaTeX) and Mermaid diagrams are written in Rust and live in their own DLLs: they are mapped only when a
# document actually holds a formula or a diagram, so neither the exe nor the start-up path pays for them.
# Set FASTMD_RUST=OFF to build without them - the viewer then shows formulas and diagrams as their own source.
option(FASTMD_RUST "Build the Rust libraries for formulas and Mermaid diagrams" ON)
if(FASTMD_RUST)
    find_program(CARGO_EXE cargo)
    if(CARGO_EXE)
        set(RUST_OUT ${CMAKE_BINARY_DIR}/rust/release)
        # Every source of both crates, not just the two lib.rs: the flowchart engine is a folder of its own.
        file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/rust/fastmd-tex/src/*.rs ${CMAKE_SOURCE_DIR}/rust/fastmd-mermaid/src/*.rs)
        add_custom_command(
            OUTPUT ${RUST_OUT}/fastmd_tex.dll ${RUST_OUT}/fastmd_mermaid.dll
            COMMAND ${CARGO_EXE} build --release --manifest-path ${CMAKE_SOURCE_DIR}/rust/Cargo.toml
                    --target-dir ${CMAKE_BINARY_DIR}/rust
            DEPENDS ${RUST_SOURCES} ${CMAKE_SOURCE_DIR}/rust/Cargo.toml
                    ${CMAKE_SOURCE_DIR}/rust/fastmd-tex/Cargo.toml ${CMAKE_SOURCE_DIR}/rust/fastmd-mermaid/Cargo.toml
            COMMENT "cargo build --release (formulas, diagrams)"
            VERBATIM)
        add_custom_target(fastmd_rust ALL
            DEPENDS ${RUST_OUT}/fastmd_tex.dll ${RUST_OUT}/fastmd_mermaid.dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RUST_OUT}/fastmd_tex.dll
                    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fastmd-tex.dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RUST_OUT}/fastmd_mermaid.dll
                    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fastmd-mermaid.dll
            VERBATIM)
        add_dependencies(FastMD fastmd_rust)
    else()
        message(WARNING "cargo not found: building without formulas and Mermaid diagrams")
    endif()
endif()

# ------------------------------------------------------------------------------------------ FastMD-Setup.exe
# The per-user installer (plan 5.4) carries the program inside itself as resources: no MSI, no MSIX, no admin.
# It is built last, because what it carries is what the other targets have just produced.
option(FASTMD_SETUP "Build the per-user installer" ON)
if(FASTMD_SETUP)
    set(PAYLOAD_EXE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/FastMD.exe)
    set(PAYLOAD_SVG ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fastmd-svg.dll)
    set(PAYLOAD_TEX ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fastmd-tex.dll)
    set(PAYLOAD_MERMAID ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fastmd-mermaid.dll)
    set(PAYLOAD_PREVIEW ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fastmd-preview.dll)
    set(PAYLOAD_LICENSE ${CMAKE_SOURCE_DIR}/../LICENSE)
    set(PAYLOAD_MD4C ${CMAKE_SOURCE_DIR}/third_party/md4c/LICENSE.md)
    set(PAYLOAD_LUNASVG ${CMAKE_SOURCE_DIR}/third_party/lunasvg/LICENSE)
    set(PAYLOAD_PLUTOVG ${CMAKE_SOURCE_DIR}/third_party/plutovg/LICENSE)
    set(PAYLOAD_RATEX ${CMAKE_SOURCE_DIR}/third_party/ratex/LICENSE-ratex.txt)
    set(PAYLOAD_KATEX ${CMAKE_SOURCE_DIR}/third_party/ratex/OFL-katex-fonts.txt)
    set(PAYLOAD_MERMAID_LICENSE ${CMAKE_SOURCE_DIR}/third_party/ratex/LICENSE-mermaid-rs-renderer.txt)
    set(FASTMD_ICON ${CMAKE_SOURCE_DIR}/res/fastmd.ico)
    set(SETUP_RC ${CMAKE_BINARY_DIR}/generated/setup_payload.rc)
    configure_file(setup/setup_payload.rc.in ${SETUP_RC} @ONLY)
    # the resource script embeds files the other targets write, so it must be recompiled when they change
    set_source_files_properties(${SETUP_RC} PROPERTIES OBJECT_DEPENDS
        "${PAYLOAD_EXE};${PAYLOAD_SVG};${PAYLOAD_TEX};${PAYLOAD_MERMAID};${PAYLOAD_PREVIEW};${CMAKE_BINARY_DIR}/generated/version.h")
    add_executable(FastMD_Setup WIN32 setup/setup.cpp ${SETUP_RC})
    set_target_properties(FastMD_Setup PROPERTIES OUTPUT_NAME "FastMD-Setup")
    target_include_directories(FastMD_Setup PRIVATE ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/setup)
    target_compile_definitions(FastMD_Setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _CRT_SECURE_NO_WARNINGS)
    target_compile_options(FastMD_Setup PRIVATE /utf-8 /W3 /permissive- /EHsc
        $<$<CONFIG:Release>:/O2 /Oi /Gy /DNDEBUG> ${FASTMD_SEC_C})
    target_link_options(FastMD_Setup PRIVATE /MANIFEST:EMBED /MANIFESTINPUT:${FASTMD_MANIFEST} /INCREMENTAL:NO
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF> ${FASTMD_SEC_L})
    target_link_libraries(FastMD_Setup PRIVATE user32 shell32 shlwapi ole32 advapi32 comctl32)
    add_dependencies(FastMD_Setup FastMD fastmd_svg)
    if(TARGET fastmd_preview)
        add_dependencies(FastMD_Setup fastmd_preview)
    endif()
    if(TARGET fastmd_rust)
        add_dependencies(FastMD_Setup fastmd_rust)
    endif()
endif()

# ---------------------------------------------------------------------------------------------- fuzz harness
# Feeds mutated documents to the parser (plan 6.5). Built on request, usually with AddressSanitizer:
#   pwsh -File tests/fuzz/run.ps1
option(FASTMD_FUZZ "Build the fuzz harness" OFF)
option(FASTMD_ASAN "Build it with AddressSanitizer" OFF)
if(FASTMD_FUZZ)
    # editcore.cpp: every input is also parsed with edit mode's map and checked by MapSelfCheck
    add_executable(fastmd-fuzz tests/fuzz/fuzz.cpp src/parse.cpp src/html.cpp src/highlight.cpp src/util.cpp
        src/theme.cpp src/editcore.cpp src/editops.cpp third_party/md4c/md4c.c)
    target_include_directories(fastmd-fuzz PRIVATE ${CMAKE_BINARY_DIR}/generated src)
    target_compile_definitions(fastmd-fuzz PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _CRT_SECURE_NO_WARNINGS MD4C_USE_UTF16 FASTMD_NO_APP)
    target_compile_options(fastmd-fuzz PRIVATE /utf-8 /W3 /permissive- /Zc:__cplusplus /EHsc /Zi
        $<$<BOOL:${FASTMD_ASAN}>:/fsanitize=address>)
    target_link_options(fastmd-fuzz PRIVATE /DEBUG /INCREMENTAL:NO)
    target_link_libraries(fastmd-fuzz PRIVATE user32 advapi32 shlwapi)
endif()

# --------------------------------------------------------------------------------------------- fastmd-edit-tests
# Edit mode's window-free core (docs/EDIT-MODE.md §14.1): golden cases from tests/edit/cases and property sweeps over
# the corpus, on the same parser the app uses but with no window and no `g`. The exit code is the number of failures.
#   pwsh -File tests/edit/run.ps1
option(FASTMD_TESTS "Build the edit-mode unit tests" ON)
if(FASTMD_TESTS)
    add_executable(fastmd-edit-tests tests/edit/edit_tests.cpp src/editcore.cpp src/editops.cpp src/editfile.cpp src/parse.cpp src/html.cpp
        src/highlight.cpp src/util.cpp src/theme.cpp third_party/md4c/md4c.c)
    target_include_directories(fastmd-edit-tests PRIVATE ${CMAKE_BINARY_DIR}/generated src)
    target_compile_definitions(fastmd-edit-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _CRT_SECURE_NO_WARNINGS MD4C_USE_UTF16 FASTMD_NO_APP FASTMD_APP_DIR="${CMAKE_SOURCE_DIR}")
    target_compile_options(fastmd-edit-tests PRIVATE /utf-8 /W3 /permissive- /Zc:__cplusplus /EHsc
        $<$<CONFIG:Release>:/O2 /Oi /DNDEBUG>)
    target_link_options(fastmd-edit-tests PRIVATE /INCREMENTAL:NO)
    target_link_libraries(fastmd-edit-tests PRIVATE user32 advapi32 shlwapi)
endif()

# ------------------------------------------------------------------------------------------- fastmd-preview.dll
# Explorer's preview pane on the same engine (plan 5.2). It is a COM in-proc server: the reader registers it per
# user, Explorer loads it into prevhost.exe. Only the drawing half of the program is compiled in - no window, no
# find bar, no outline, no settings, no threads, no network (preview/stubs.cpp stands in for all of that).
option(FASTMD_PREVIEW "Build the Explorer preview handler" ON)
if(FASTMD_PREVIEW)
    add_library(fastmd_preview SHARED
        preview/preview.cpp preview/stubs.cpp
        src/view.cpp src/layout.cpp src/canvas_gdi.cpp src/parse.cpp src/html.cpp src/highlight.cpp src/theme.cpp
        src/util.cpp src/strings.cpp src/svg.cpp src/formulas.cpp
        third_party/md4c/md4c.c
        preview/preview.def)
    set_target_properties(fastmd_preview PROPERTIES OUTPUT_NAME "fastmd-preview")
    target_include_directories(fastmd_preview PRIVATE ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(fastmd_preview PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _CRT_SECURE_NO_WARNINGS MD4C_USE_UTF16 FASTMD_PREVIEW_DLL)
    target_compile_options(fastmd_preview PRIVATE /utf-8 /W3 /permissive- /Zc:__cplusplus /EHsc
        $<$<CONFIG:Release>:/O2 /Oi /Gy /DNDEBUG> ${FASTMD_SEC_C})
    target_link_options(fastmd_preview PRIVATE /INCREMENTAL:NO $<$<CONFIG:Release>:/OPT:REF /OPT:ICF> ${FASTMD_SEC_L})
    target_link_libraries(fastmd_preview PRIVATE user32 gdi32 dwrite ole32 shell32 shlwapi windowscodecs advapi32
        dwmapi)
    set_source_files_properties(third_party/md4c/md4c.c PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4267")
endif()
