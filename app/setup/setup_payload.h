// What the installer carries inside itself. The ids match setup_payload.rc.in, which the build fills with the paths
// of the freshly built files.
#pragma once

struct PayloadItem {
    int id;
    const wchar_t* name;
};

static const PayloadItem kPayload[] = {
    {101, L"FastMD.exe"},
    {102, L"fastmd-svg.dll"},
    {103, L"fastmd-tex.dll"},
    {104, L"fastmd-mermaid.dll"},
    {105, L"LICENSE.txt"},
    {106, L"THIRD-PARTY-md4c.txt"},
    {107, L"THIRD-PARTY-lunasvg.txt"},
    {108, L"THIRD-PARTY-plutovg.txt"},
    {109, L"THIRD-PARTY-ratex.txt"},
    {110, L"THIRD-PARTY-katex-fonts.txt"},
    {111, L"THIRD-PARTY-mermaid-rs-renderer.txt"},
};
