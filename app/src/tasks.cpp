// Task lists: a click on the box of "- [ ]" ticks the item in the file itself, as GitHub, Obsidian and Typora do.
// A reader must never damage what it shows, so the file changes in exactly one place - the character between the
// brackets, overwritten in place in the file's own encoding - and only while the file on disk is, byte for byte, the
// document on screen. The BOM, the line ends, the rest of the text and the file itself (its identity, links to it,
// its permissions) stay as they were. The watcher then sees our own write and does not reload (OnFileChanged).
#include "app.h"

namespace {
enum TaskResult { TR_OK, TR_BUSY, TR_DENIED, TR_MISSING, TR_CHANGED, TR_FAILED };

// the file's code unit at a byte offset: one byte, or two for UTF-16 LE
uint32_t UnitAt(const std::vector<char>& bytes, size_t off, uint32_t unit) {
    if (off >= bytes.size() || bytes.size() - off < unit) return 0xFFFFFFFF;
    uint32_t v = (uint8_t)bytes[off];
    if (unit == 2) v |= (uint32_t)(uint8_t)bytes[off + 1] << 8;
    return v;
}

// Writes the mark into the file. `src` is the mark's offset in g.src, `mark` the character to put there; `after` gets
// the file's stamp as the write left it, taken through the same handle - nobody else can have written in between.
TaskResult WriteMark(uint32_t src, wchar_t mark, BY_HANDLE_FILE_INFORMATION* after) {
    // no one else may write while the file is checked and changed; reading it stays allowed
    HANDLE f = CreateFileW(g.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) return TR_BUSY;
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return TR_MISSING;
        if (e == ERROR_ACCESS_DENIED || e == ERROR_WRITE_PROTECT) return TR_DENIED;
        return TR_FAILED;
    }
    TaskResult res = TR_FAILED;
    LARGE_INTEGER sz{};
    std::vector<char> bytes;
    bool read = GetFileSizeEx(f, &sz) && sz.QuadPart <= (1ll << 30);  // the loader's limit
    if (read) {
        bytes.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        read = bytes.empty() || (ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr) && got == bytes.size());
    }
    std::wstring now;
    TextEncoding enc;
    if (read) DecodeText(bytes.data(), (int)bytes.size(), now, &enc);
    if (!read) {
        res = TR_FAILED;
    } else if (now != g.src) {
        res = TR_CHANGED;  // someone else wrote it since it was loaded: the box on screen may not be this one
    } else {
        // where the mark is: UTF-16 has two bytes per character; UTF-8 and the ANSI code page take the text before it
        uint32_t unit = enc.cp == 1200 ? 2 : 1;
        size_t off = enc.header;
        if (unit == 2) off += 2 * (size_t)src;
        else off += (size_t)WideCharToMultiByte(enc.cp, 0, g.src.data(), (int)src, nullptr, 0, nullptr, nullptr);
        if (UnitAt(bytes, off - unit, unit) == L'[' && UnitAt(bytes, off, unit) == g.src[src] &&
            UnitAt(bytes, off + unit, unit) == L']') {
            // Proof before writing: the changed file must read back as exactly the text on screen with the new mark.
            // For UTF-8 and UTF-16 it always does; an ANSI code page that does not map back one to one fails here.
            bytes[off] = (char)mark;
            std::wstring text;
            DecodeText(bytes.data(), (int)bytes.size(), text, nullptr);
            std::wstring want = g.src;
            want[src] = mark;
            LARGE_INTEGER at{};
            at.QuadPart = (LONGLONG)off;
            DWORD put = 0;
            if (text == want && SetFilePointerEx(f, at, nullptr, FILE_BEGIN) &&
                WriteFile(f, bytes.data() + off, unit, &put, nullptr) && put == unit) {
                res = TR_OK;
                // NTFS moves the write time at WriteFile, not at CloseHandle (measured): this is the final stamp
                if (!GetFileInformationByHandle(f, after)) *after = BY_HANDLE_FILE_INFORMATION{};
            }
        }
    }
    CloseHandle(f);
    return res;
}
}  // namespace

bool ToggleTask(uint32_t block) {
    // a big document's full parse still reads g.src on its thread: its boxes are clickable once it has landed
    if (g.path.empty() || g.loadFailed || g.fullPending || block >= g.doc.blocks.size()) return false;
    auto it = std::lower_bound(g.doc.tasks.begin(), g.doc.tasks.end(), block,
                               [](const Task& t, uint32_t bi) { return t.block < bi; });
    if (it == g.doc.tasks.end() || it->block != block) return false;
    uint32_t src = it->src;
    Block& b = g.doc.blocks[block];
    if (src == 0 || src + 1 >= g.src.size() || g.src[src - 1] != L'[' || g.src[src + 1] != L']') return false;
    bool ticked = b.marker == MK_TASK_DONE;
    wchar_t mark = ticked ? L' ' : L'x';
    BY_HANDLE_FILE_INFORMATION after{};
    switch (WriteMark(src, mark, &after)) {
    case TR_OK: break;
    case TR_BUSY: ShowToast(Tr(S_TASK_BUSY), 3000); return false;
    case TR_DENIED: ShowToast(Tr(S_TASK_DENIED), 3000); return false;
    case TR_MISSING: ShowToast(Tr(S_FILE_NOT_FOUND) + FileNameOf(g.path), 3000); return false;
    case TR_CHANGED:
        ReloadDocument();
        ShowToast(Tr(S_TASK_CHANGED), 3000);
        return false;
    default: ShowToast(Tr(S_TASK_FAILED), 3000); return false;
    }
    g.src[src] = mark;  // "copy as Markdown" gives the new mark, and the next click compares the file with this
    b.marker = ticked ? MK_TASK_OPEN : MK_TASK_DONE;
    // our own write: the watcher must not reload for it (a file system that moves the time at close costs one reload)
    if (after.ftLastWriteTime.dwLowDateTime || after.ftLastWriteTime.dwHighDateTime) {
        g.fileTime = after.ftLastWriteTime;
        g.fileSize = ((uint64_t)after.nFileSizeHigh << 32) | after.nFileSizeLow;
    } else {
        GetFileStamp(g.path.c_str(), &g.fileTime, &g.fileSize);
    }
    g.pixelSerial++;  // the same layout draws differently: a scrolled frame must not reuse the old pixels
    Invalidate();
    return true;
}
