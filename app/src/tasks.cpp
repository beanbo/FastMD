// Task lists: a click on the box of "- [ ]" ticks the item in the file itself, as GitHub, Obsidian and Typora do.
// A reader must never damage what it shows, so the file changes in exactly one place - the character between the
// brackets, written in place in the file's own encoding - and only while the file on disk is, byte for byte, the
// document on screen. The BOM, the line ends, the rest of the text and the file itself (its identity, links to it,
// its permissions) stay as they were. The watcher then sees our own write and does not reload (OnFileChanged).
//
// The tick is an edit like any other (EDIT-MODE.md §8.10, §10.2, T2): a splice of the mark in the source, the save of
// edit mode (only the changed bytes are rewritten, a recovery file stands by while they are), and the model swap that
// keeps the view, the layouts and the pictures of everything else.
#include "app.h"

bool ToggleTask(uint32_t block) {
    // a big document's full parse still reads g.src on its thread: its boxes are clickable once it has landed
    if (g.path.empty() || g.loadFailed || g.fullPending || block >= g.doc.blocks.size()) return false;
    auto it = std::lower_bound(g.doc.tasks.begin(), g.doc.tasks.end(), block,
                               [](const Task& t, uint32_t bi) { return t.block < bi; });
    if (it == g.doc.tasks.end() || it->block != block) return false;
    uint32_t src = it->src;
    if (src == 0 || src + 1 >= g.src.size() || g.src[src - 1] != L'[' || g.src[src + 1] != L']') return false;
    // Reading mode is never dirty (leaving edit mode saves or discards). Should it ever be, the tick is not written
    // over edits nobody saved, and nothing is reloaded under them either (D6).
    if (EditDirty()) {
        ShowToast(Tr(S_TASK_FAILED), 3000);
        return false;
    }
    SaveState st = SS_SAVED;
    switch (EditBaseline(&st)) {
    case BL_OK: break;
    case BL_CHANGED:  // someone else wrote it since it was loaded: the box on screen may not be this one
        ReloadDocument();
        ShowToast(Tr(S_TASK_CHANGED), 3000);
        return false;
    case BL_UNREADABLE:
        if (st == SS_BUSY) ShowToast(Tr(S_TASK_BUSY), 3000);
        else if (st == SS_DENIED) ShowToast(Tr(S_TASK_DENIED), 3000);
        else if (st == SS_MISSING) ShowToast(Tr(S_FILE_NOT_FOUND) + FileNameOf(g.path), 3000);
        else ShowToast(Tr(S_TASK_FAILED), 3000);
        return false;
    default: ShowToast(Tr(S_TASK_FAILED), 3000); return false;  // bytes that cannot be written back as they are
    }
    bool ticked = g.doc.blocks[block].marker == MK_TASK_DONE;
    std::wstring was(1, g.src[src]), mark(1, ticked ? L' ' : L'x');
    EditState before;
    before.focus = before.anchor = src;
    if (!EditSplice(src, 1, mark)) return false;
    st = EditSave(false);
    if (st != SS_SAVED) {
        EditSplice(src, 1, was);  // not written: the source is the file again, and the box stays as the file says
        switch (st) {
        case SS_CONFLICT:  // changed on disk behind the window's back: the file is read again, nothing is written
            ReloadDocument();
            ShowToast(Tr(S_TASK_CHANGED), 3000);
            break;
        case SS_BUSY: ShowToast(Tr(S_TASK_BUSY), 3000); break;
        case SS_DENIED: case SS_READONLY: ShowToast(Tr(S_TASK_DENIED), 3000); break;
        case SS_MISSING: ShowToast(Tr(S_FILE_NOT_FOUND) + FileNameOf(g.path), 3000); break;
        default: ShowToast(Tr(S_TASK_FAILED), 3000);
        }
        return false;
    }
    EditReparse(src, 1, 1);
    EditStep step;
    step.splices.push_back(Splice{src, was, mark});
    step.before = before;
    step.after = before;
    step.kind = EK_TASK;
    EditPushStep(std::move(step));
    return true;
}
