// Edit mode's file half (docs/EDIT-MODE.md §10): what the editor believes is on disk, the byte-exact encoding rules,
// the write itself and the recovery file that stands around it.
//
// No `g` and no window here - only the Win32 file API - so fastmd-edit-tests drives every path of it, faults included,
// on temporary files. The glue (edit.cpp) decides when to call it and what to show.
#pragma once
#include "doc.h"

// Save states, in the numbering of Q_EDIT_SAVE_STATE (§13.2). SaveSource answers with the error classes of §10.4.
enum SaveState : uint8_t { SS_SAVED, SS_PENDING, SS_SAVING, SS_BUSY, SS_DENIED, SS_READONLY, SS_MISSING, SS_CONFLICT,
                           SS_UNENCODABLE, SS_FAILED, SS_UNKNOWN, SS_OFF };

// The baseline (§10.1, §10.2): the text, bytes, encoding and identity believed to be on disk. It is set only from bytes
// just read and verified, or just written - never from a load that failed.
struct DiskState {
    bool valid = false;
    std::wstring text;
    UINT cp = CP_UTF8;              // 65001, 1200 (UTF-16 LE) or an ANSI code page's number - never CP_ACP
    std::string header;             // the byte-order mark as the file has it (0, 2, 3 or 5 bytes)
    uint64_t length = 0, hash = 0;  // byte length and FNV-1a-64 of all the bytes, header included
    uint32_t volume = 0;            // identity: volume serial number and file index
    uint64_t index = 0;
    FILETIME mtime{};
    uint64_t size = 0;
    DWORD attributes = 0;
    bool remote = false, cloud = false;
    uint32_t crlf = 0, lf = 0, cr = 0;  // the line ends of `text` (§7.2)
};

// the line end the editor writes where a line has none of its own (§7.2): the most frequent, ties CRLF then LF, LF
// for a file without any
const wchar_t* DiskEol(const DiskState& d);
void CountEols(const std::wstring& text, DiskState& d);
uint64_t Fnv64(const void* p, size_t n, uint64_t h = 14695981039346656037ull);
bool StatefulCodePage(UINT cp);  // ISO-2022, ISCII, UTF-7: their bytes cannot be rewritten in place

// §10.1: encodes text in cp, byte for byte what the file would hold. false = a character has no bytes of its own in
// cp (*bad = its offset, *reason = "UNENCODABLE" or "LONE_SURROGATE"), or the encoder failed ("ENCODER_ERROR").
bool EncodeText(UINT cp, const wchar_t* p, size_t n, std::string& out, size_t* bad, const char** reason);

// Reads the whole file in 1 MB chunks, the stamp and identity from the same handle (`d` gets volume, index, mtime,
// size, attributes, remote, cloud). SS_SAVED = read; SS_UNKNOWN = a short read or a size that changed meanwhile, never
// "empty" or "changed" (D5); otherwise the error class of the open.
SaveState ReadDisk(const wchar_t* path, std::string& bytes, DiskState* d, DWORD* err);

// The entry checks that need only the bytes (§10.1), in order: no NUL byte unless the file is BOM-detected UTF-16, not
// UTF-16 BE, UTF-16 of even length, not a stateful code page, and the byte-exact round trip header + Encode(text) ==
// bytes. `out` gets the text, code page, header, length, hash and line ends whatever the answer, so a caller can
// compare the text first. acp: the code page bytes that are not UTF-8 are read in (AnsiCodePage()).
enum DiskRefusal : uint8_t { DR_OK, DR_BINARY, DR_UTF16BE, DR_UTF16ODD, DR_STATEFUL, DR_LOSSY };
DiskRefusal DecodeDisk(const std::string& bytes, UINT acp, DiskState& out);

// ---- the write (§10.3)
struct SaveRequest {
    const wchar_t* path = nullptr;
    const std::wstring* text = nullptr;  // the snapshot to write
    const DiskState* disk = nullptr;     // the baseline it was edited from (valid)
    std::wstring recoveryDir;            // ...\recovery\ ; the recovery file is written there around the write
    bool flushPoint = false;             // leave, close, session end, Ctrl+S: a local volume is flushed too
    bool fullProof = false;              // FASTMD_EDIT_SELFCHECK: the whole output is decoded back whatever its size
};
struct SaveResult {
    SaveState state = SS_FAILED;
    const char* reason = "";             // UNENCODABLE, LONE_SURROGATE, BOM_LOOKALIKE, ENCODER_ERROR, or what failed
    DWORD error = 0;
    uint32_t bad = UINT32_MAX;           // the first character that cannot be saved (offset in the snapshot)
    bool adopted = false;                // the file was re-encoded outside with the same text: `disk` is the new baseline
    DiskState disk;                      // SS_SAVED (or `adopted`): the baseline from now on
    std::wstring recoveryKept;           // the write failed and so did the rollback: this file holds the old bytes
    double flushMs = -1;                 // the target's flush took this long (-1: not flushed)
};
SaveResult SaveSource(const SaveRequest& rq);

// ---- recovery files (§10.5): <volume:8 hex>-<file index:16 hex>-<pid>.rec, a header and the old bytes from pb on
struct RecoveryInfo {
    std::wstring file;                   // the recovery file
    std::wstring path;                   // the document it belongs to
    uint64_t preSize = 0, mtime = 0, pb = 0;
    UINT cp = 0;
    std::string header;
    DWORD pid = 0;
    uint64_t tailOff = 0, tailLen = 0;   // where the saved bytes start in `file`, and how many
    uint64_t written = 0;                // when the recovery file was written (FILETIME ticks)
};
std::wstring RecoveryName(uint32_t volume, uint64_t index, DWORD pid);
bool ReadRecovery(const std::wstring& file, RecoveryInfo& out);
// Leftovers for a file identity: files of processes that are gone (a live FastMD is still saving, or will delete its
// own), oldest first.
std::vector<RecoveryInfo> FindRecovery(const std::wstring& dir, uint32_t volume, uint64_t index);
// [Восстановить]: the saved bytes go back at pb and the old length is restored, flushed.
bool RecoveryRestore(const RecoveryInfo& r, const wchar_t* target, DWORD* err);
// [Открыть копию]: the previous file rebuilt beside it as `out` - the current bytes up to pb, then the saved ones.
bool RecoveryRebuild(const RecoveryInfo& r, const wchar_t* current, const wchar_t* out);

// FASTMD_TEST_FAIL_WRITE=<kind>[:<n>][,always][,norollback] (§13.5), read once. Tests of the core set it directly.
void SetFailWriteForTests(const wchar_t* spec);
