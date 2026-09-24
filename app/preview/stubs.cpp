// The preview pane uses the reader's engine, but not the reader's window: no find bar, no outline drawer, no start
// screen, no settings window, no background threads, no network. These are the few functions the engine calls into
// those modules - here they do nothing, which is exactly right for a pane that only shows a document.
#include "app.h"

// ---------------------------------------------------------------------------------------------- window.cpp
void Invalidate() {
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}

void Relayout() {
    ClearLayoutCache();
    UpdateColumns();
    InitGeometry();
    RecomputeY();
}

void Command(UINT, UINT) {}
void ApplySettings(uint32_t, bool) {}
void ApplyTheme() {}

// ---------------------------------------------------------------------------------------------- find.cpp
void DrawFindMarks(float, float) {}
void DrawFindBar() {}
bool FindInputFocused() { return false; }
void FindRelayoutInput() {}
HWND FindEditHwnd() { return nullptr; }
void FindUpdate(bool) {}
void FindClose() {}

// ---------------------------------------------------------------------------------------------- toc.cpp
void DrawToc() {}
void TocSync() {}
bool TocDocked() { return false; }
bool TocOverlayOpen() { return false; }
bool TocAvailable() { return false; }
float TocPanelW() { return 0.f; }
bool TocButtonRect(float*, float*, float*, float*) { return false; }
void TocSetOpen(bool) {}
int TocCurrent() { return -1; }
float TocItemY(int) { return 0.f; }

// ---------------------------------------------------------------------------------------------- home.cpp
void DrawHome() {}
void HomeRebuild() {}
int HomeItemAt(float, float) { return -1; }

// ---------------------------------------------------------------------------------------------- editbar.cpp
void DrawEditChrome() {}  // the pane never edits: no toolbar, no strips

// ---------------------------------------------------------------------------------------------- settings_ui.cpp
void DrawSettingsButton() {}
bool SettingsButtonRect(float*, float*, float*, float*) { return false; }
HWND SettingsHwnd() { return nullptr; }
void SettingsRefresh() {}

// ---------------------------------------------------------------------------------------------- scrolling
// The reader animates a scroll over several frames; a preview pane just goes there.
void ScrollTo(float y, bool) {
    g.scrollY = g.targetY = std::clamp(y, 0.f, MaxScroll());
    Invalidate();
}

// ---------------------------------------------------------------------------------------------- loader.cpp
// Pictures: the ones on this disk are read straight away when a block needs them (no worker threads in a preview
// pane); the ones from the network are not fetched at all, which keeps the pane offline.
bool RemoteImagesAllowed() { return false; }
void LoadRemoteImages() {}
bool DocHasRemoteImages() { return false; }
void ScheduleImageScaling() {}
void StartMeasure() {}
void StartBackgroundWork() {}
HANDLE Spawn(LPTHREAD_START_ROUTINE, void*, int, SIZE_T, WorkerKind) { return nullptr; }
void OpenDocument(const std::wstring&, bool, float, bool) {}
void ReloadDocument() {}
