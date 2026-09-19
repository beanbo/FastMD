// FastMD startup lab launcher: t0 = GetSystemTimePreciseAsFileTime() right before the launch call,
// the child writes its own marks into the shared block (labmark.h). One JSON line per run on stdout.
//
// launch.exe [options] -- <exe-or-document> [args...]
//   --n N            runs (default 1)             --gap MS      pause between runs (default 300)
//   --mode M         cp | cpjob | shell | explorer | none       (default cp)
//                    cp       = CreateProcessW
//                    cpjob    = CreateProcessW(SUSPENDED) + AssignProcessToJobObject + ResumeThread (like bench.py)
//                    shell    = ShellExecuteExW(verb open) in this process
//                    explorer = IShellDispatch2::ShellExecute executed INSIDE explorer.exe (the double-click path)
//                    none     = do not launch anything (someone else triggers the child), just time the event
//                    susp     = CreateProcessW(CREATE_SUSPENDED) then TerminateProcess: the child never runs a single
//                               instruction -> isolates the kernel/AV/parent-side cost for ANY exe (signed, huge, ...)
//   --defcpu         report the CPU time MsMpEng.exe (Defender engine) consumed during the sample
//   --console C      inherit | new | detached | nowindow (cp modes; default inherit)
//   --fresh MS       copy the exe to a brand-new unique file (+random bytes) before each run, wait MS, launch the copy
//   --timeout MS     wait for the "done" event / exit (default 10000)
//   --label L        label in the output
//   --nowait-exit    do not wait for the process to exit (resident / hand-off tests)
//   --msonly         child gets PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON
//                    (blocks injected third-party DLLs such as windhawk.dll; cp modes only)
//   --self-msonly    re-run this launcher itself under that policy first (so its CreateProcessW is not hooked)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shldisp.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <intrin.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <set>
#include "labmark.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

static double g_tsc_hz = 0;

static void calibrate_tsc() {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    unsigned long long c0 = __rdtsc();
    Sleep(60);
    QueryPerformanceCounter(&b);
    unsigned long long c1 = __rdtsc();
    g_tsc_hz = (double)(c1 - c0) / ((double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart);
}

static std::wstring quote(const std::wstring &s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring r = L"\"";
    for (wchar_t c : s) { if (c == L'"') r += L'\\'; r += c; }
    return r + L"\"";
}

static std::string esc(const char *s) {
    std::string r;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
        else if (c == '\n') r += "\\n";
        else if (c < 0x20) r += ' ';
        else r += (char)c;
    }
    return r;
}

static std::string w2u(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::set<DWORD> snapshot_pids() {
    std::set<DWORD> r;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (Process32FirstW(h, &pe)) do { r.insert(pe.th32ProcessID); } while (Process32NextW(h, &pe));
    CloseHandle(h);
    return r;
}

static std::string new_process_names(const std::set<DWORD> &before) {
    std::string out;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (Process32FirstW(h, &pe)) do {
        if (!before.count(pe.th32ProcessID)) { out += w2u(pe.szExeFile); out += ","; }
    } while (Process32NextW(h, &pe));
    CloseHandle(h);
    return out;
}

static HANDLE g_msmpeng;
static double msmpeng_cpu_ms() {
    if (!g_msmpeng) {
        HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = {sizeof(pe)};
        if (Process32FirstW(h, &pe)) do {
            if (!_wcsicmp(pe.szExeFile, L"MsMpEng.exe")) { g_msmpeng = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID); break; }
        } while (Process32NextW(h, &pe));
        CloseHandle(h);
        if (!g_msmpeng) return -1;
    }
    FILETIME c, e, k, u;
    if (!GetProcessTimes(g_msmpeng, &c, &e, &k, &u)) return -1;
    return ((((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) +
            (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e4;
}

// ---- IShellDispatch2 living in explorer.exe (Raymond Chen, "How can I launch an unelevated process from my
// elevated process", devblogs.microsoft.com/oldnewthing/20131118-00/?p=2643) -----------------------------------
static ComPtr<IShellDispatch2> g_explorer_shell;
static HRESULT get_explorer_shell() {
    ComPtr<IShellWindows> sw;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sw));
    if (FAILED(hr)) return hr;
    VARIANT loc; VariantInit(&loc); loc.vt = VT_I4; loc.lVal = CSIDL_DESKTOP;
    VARIANT empty; VariantInit(&empty);
    long hwnd = 0;
    ComPtr<IDispatch> disp;
    hr = sw->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp);
    if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
    ComPtr<IServiceProvider> sp; hr = disp.As(&sp); if (FAILED(hr)) return hr;
    ComPtr<IShellBrowser> sb; hr = sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb)); if (FAILED(hr)) return hr;
    ComPtr<IShellView> sv; hr = sb->QueryActiveShellView(&sv); if (FAILED(hr)) return hr;
    ComPtr<IDispatch> bg; hr = sv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bg)); if (FAILED(hr)) return hr;
    ComPtr<IShellFolderViewDual> fvd; hr = bg.As(&fvd); if (FAILED(hr)) return hr;
    ComPtr<IDispatch> app; hr = fvd->get_Application(&app); if (FAILED(hr)) return hr;
    return app.As(&g_explorer_shell);
}

int wmain(int argc, wchar_t **argv) {
    int n = 1, gap = 300, timeout = 10000, fresh = -1;
    bool wait_exit = true, msonly = false, self_msonly = false, defcpu = false;
    std::wstring mode = L"cp", console = L"inherit", label = L"run";
    int i = 1;
    for (; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--") { i++; break; }
        else if (a == L"--n") n = _wtoi(argv[++i]);
        else if (a == L"--gap") gap = _wtoi(argv[++i]);
        else if (a == L"--mode") mode = argv[++i];
        else if (a == L"--console") console = argv[++i];
        else if (a == L"--fresh") fresh = _wtoi(argv[++i]);
        else if (a == L"--timeout") timeout = _wtoi(argv[++i]);
        else if (a == L"--label") label = argv[++i];
        else if (a == L"--nowait-exit") wait_exit = false;
        else if (a == L"--msonly") msonly = true;
        else if (a == L"--self-msonly") self_msonly = true;
        else if (a == L"--defcpu") defcpu = true;
        else { fwprintf(stderr, L"unknown option %s\n", a.c_str()); return 2; }
    }
    if (i >= argc && mode != L"none") { fwprintf(stderr, L"no target\n"); return 2; }
    std::wstring target = i < argc ? argv[i] : L"";
    std::wstring rest;
    for (int k = i + 1; k < argc; k++) { rest += L" "; rest += quote(argv[k]); }

    if (self_msonly && !GetEnvironmentVariableW(L"LAB_RELAUNCHED", nullptr, 0)) {
        // re-exec ourselves (same command line minus nothing: the child sees LAB_RELAUNCHED and skips this block)
        SetEnvironmentVariableW(L"LAB_RELAUNCHED", L"1");
        SIZE_T sz = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
        std::vector<char> attr(sz);
        auto al = (LPPROC_THREAD_ATTRIBUTE_LIST)attr.data();
        InitializeProcThreadAttributeList(al, 1, 0, &sz);
        DWORD64 pol = PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON;
        UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &pol, sizeof(pol), nullptr, nullptr);
        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        HANDLE hs[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
        for (HANDLE h : hs) if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.StartupInfo.hStdInput = hs[0]; si.StartupInfo.hStdOutput = hs[1]; si.StartupInfo.hStdError = hs[2];
        PROCESS_INFORMATION pi = {};
        std::wstring cl = GetCommandLineW();
        std::vector<wchar_t> b(cl.begin(), cl.end()); b.push_back(0);
        if (!CreateProcessW(nullptr, b.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                            &si.StartupInfo, &pi)) { fwprintf(stderr, L"relaunch failed %lu\n", GetLastError()); return 5; }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        return (int)code;
    }
    calibrate_tsc();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (mode == L"explorer") {
        HRESULT hr = get_explorer_shell();
        if (FAILED(hr)) { fwprintf(stderr, L"explorer shell dispatch failed 0x%08x\n", hr); return 3; }
    }

    // SDDL-free default security; Local\ namespace = this session.
    HANDLE hmap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(lab_shm_t), LAB_SHM_NAME);
    lab_shm_t *shm = (lab_shm_t *)MapViewOfFile(hmap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(lab_shm_t));
    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, LAB_EVT_NAME);
    if (!shm || !evt) { fwprintf(stderr, L"shm/event failed %lu\n", GetLastError()); return 3; }

    for (int run = 0; run < n; run++) {
        if (run) Sleep(gap);
        ZeroMemory(shm, sizeof(lab_shm_t));
        ResetEvent(evt);
        std::wstring exe = target;
        if (fresh >= 0) {
            wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH); PathRemoveFileSpecW(dir);
            wchar_t name[MAX_PATH];
            unsigned long long r = lab_now() ^ ((unsigned long long)GetCurrentProcessId() << 40);
            swprintf_s(name, L"%s\\..\\tmp\\fresh_%llx.exe", dir, r);
            if (!CopyFileW(target.c_str(), name, FALSE)) { fwprintf(stderr, L"copy failed %lu\n", GetLastError()); return 4; }
            HANDLE f = CreateFileW(name, FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            unsigned long long junk[4] = {r, r * 2654435761ULL, ~r, r ^ 0x5bd1e995ULL};
            DWORD wr; WriteFile(f, junk, sizeof(junk), &wr, nullptr); CloseHandle(f);  // unique hash, PE overlay
            exe = name;
            if (fresh > 0) Sleep(fresh);
        }
        std::set<DWORD> before;
        if (mode == L"shell" || mode == L"explorer" || mode == L"none") before = snapshot_pids();

        HANDLE hproc = nullptr;
        unsigned long long t0 = 0, t_ret = 0;
        double def0 = defcpu ? msmpeng_cpu_ms() : 0;
        BOOL ok = TRUE;
        DWORD err = 0;
        if (mode == L"susp") {
            std::wstring cmd = quote(exe) + rest;
            std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
            STARTUPINFOEXW six = {};
            six.StartupInfo.cb = sizeof(six.StartupInfo);
            PROCESS_INFORMATION pi = {};
            DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
            std::vector<char> attr;
            if (msonly) {
                SIZE_T sz = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
                attr.resize(sz);
                six.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr.data();
                InitializeProcThreadAttributeList(six.lpAttributeList, 1, 0, &sz);
                static DWORD64 pol = PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON;
                UpdateProcThreadAttribute(six.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &pol,
                                          sizeof(pol), nullptr, nullptr);
                flags |= EXTENDED_STARTUPINFO_PRESENT;
                six.StartupInfo.cb = sizeof(six);
            }
            ULONG64 cyc0 = 0, cyc1 = 0;
            FILETIME tc_, te_, tk0, tu0, tk1, tu1;
            GetThreadTimes(GetCurrentThread(), &tc_, &te_, &tk0, &tu0);
            QueryThreadCycleTime(GetCurrentThread(), &cyc0);
            t0 = lab_now();
            ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &six.StartupInfo, &pi);
            err = GetLastError();
            t_ret = lab_now();
            QueryThreadCycleTime(GetCurrentThread(), &cyc1);
            GetThreadTimes(GetCurrentThread(), &tc_, &te_, &tk1, &tu1);
            double parent_cpu = (cyc1 - cyc0) / g_tsc_hz * 1e3;
            auto ft = [](FILETIME f) { return (double)(((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e4; };
            double parent_kernel = ft(tk1) - ft(tk0), parent_user = ft(tu1) - ft(tu0);
            if (ok) {
                FILETIME c, e, k, u;
                double ct = -1;
                if (GetProcessTimes(pi.hProcess, &c, &e, &k, &u))
                    ct = ((double)(((unsigned long long)c.dwHighDateTime << 32 | c.dwLowDateTime)) - (double)t0) / 1e4;
                TerminateProcess(pi.hProcess, 0);
                WaitForSingleObject(pi.hProcess, 5000);
                unsigned long long t_exit = lab_now();
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                Sleep(defcpu ? 150 : 0);
                double def1 = defcpu ? msmpeng_cpu_ms() : 0;
                printf("{\"label\":\"%s\",\"run\":%d,\"ok\":true,\"call_ms\":%.3f,\"create_time_ms\":%.3f,\"done_ms\":-1,"
                       "\"exit_ms\":%.3f,\"cpu_ms\":%.3f,\"parent_kernel_ms\":%.1f,\"parent_user_ms\":%.1f,\"defender_cpu_ms\":%.1f,\"pid\":0,\"new_procs\":\"\",\"marks\":[],\"note\":\"\"}\n",
                       esc(w2u(label).c_str()).c_str(), run, (t_ret - t0) / 1e4, ct, (t_exit - t0) / 1e4,
                       parent_cpu, parent_kernel, parent_user, defcpu ? def1 - def0 : -1.0);
            } else {
                printf("{\"label\":\"%s\",\"run\":%d,\"ok\":false,\"err\":%lu}\n", esc(w2u(label).c_str()).c_str(), run, err);
            }
            fflush(stdout);
            if (fresh >= 0) { Sleep(50); DeleteFileW(exe.c_str()); }
            continue;
        } else if (mode == L"cp" || mode == L"cpjob") {
            std::wstring cmd = quote(exe) + rest;
            std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
            STARTUPINFOEXW six = {};
            six.StartupInfo.cb = sizeof(six);
            STARTUPINFOW &si = six.StartupInfo;
            PROCESS_INFORMATION pi = {};
            DWORD flags = CREATE_UNICODE_ENVIRONMENT;
            std::vector<char> attr;
            if (msonly) {
                SIZE_T sz = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
                attr.resize(sz);
                six.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr.data();
                InitializeProcThreadAttributeList(six.lpAttributeList, 1, 0, &sz);
                static DWORD64 pol = PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON;
                UpdateProcThreadAttribute(six.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &pol,
                                          sizeof(pol), nullptr, nullptr);
                flags |= EXTENDED_STARTUPINFO_PRESENT;
                si.cb = sizeof(six);
            }
            if (console == L"new") flags |= CREATE_NEW_CONSOLE;
            else if (console == L"detached") flags |= DETACHED_PROCESS;
            else if (console == L"nowindow") flags |= CREATE_NO_WINDOW;
            HANDLE job = nullptr;
            if (mode == L"cpjob") {
                flags |= CREATE_SUSPENDED;
                job = CreateJobObjectW(nullptr, nullptr);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
                li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
            }
            t0 = lab_now();
            ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
            err = GetLastError();
            if (ok && job) { AssignProcessToJobObject(job, pi.hProcess); ResumeThread(pi.hThread); }
            t_ret = lab_now();
            if (ok) { CloseHandle(pi.hThread); hproc = pi.hProcess; }
            // job handle intentionally leaked until the process exits (KILL_ON_JOB_CLOSE)
        } else if (mode == L"shell") {
            SHELLEXECUTEINFOW sei = {sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = L"open";
            sei.lpFile = exe.c_str();
            sei.lpParameters = rest.empty() ? nullptr : rest.c_str() + 1;
            sei.nShow = SW_SHOWNORMAL;
            t0 = lab_now();
            ok = ShellExecuteExW(&sei);
            err = GetLastError();
            t_ret = lab_now();
            hproc = ok ? sei.hProcess : nullptr;  // NULL for DelegateExecute / DDE
        } else if (mode == L"explorer") {
            BSTR file = SysAllocString(exe.c_str());
            VARIANT args, dir, verb, show;
            VariantInit(&args); VariantInit(&dir); VariantInit(&verb); VariantInit(&show);
            if (!rest.empty()) { args.vt = VT_BSTR; args.bstrVal = SysAllocString(rest.c_str() + 1); }
            verb.vt = VT_BSTR; verb.bstrVal = SysAllocString(L"open");
            show.vt = VT_I4; show.lVal = SW_SHOWNORMAL;
            t0 = lab_now();
            HRESULT hr = g_explorer_shell->ShellExecute(file, args, dir, verb, show);
            t_ret = lab_now();
            ok = SUCCEEDED(hr); err = (DWORD)hr;
            SysFreeString(file);
        } else {  // none
            t0 = lab_now(); t_ret = t0;
        }
        if (!ok) {
            printf("{\"label\":\"%s\",\"run\":%d,\"ok\":false,\"err\":%lu}\n", esc(w2u(label).c_str()).c_str(), run, err);
            fflush(stdout);
            continue;
        }
        DWORD w = WaitForSingleObject(evt, timeout);
        unsigned long long t_done = lab_now();
        std::string newp = before.empty() ? "" : new_process_names(before);
        unsigned long long t_exit = 0, cycles = 0;
        double ct_ms = -1;
        if (hproc) {
            if (wait_exit && WaitForSingleObject(hproc, timeout) == WAIT_OBJECT_0) t_exit = lab_now();
            FILETIME c, e, k, u;
            if (GetProcessTimes(hproc, &c, &e, &k, &u))
                ct_ms = ((double)(((unsigned long long)c.dwHighDateTime << 32 | c.dwLowDateTime)) - (double)t0) / 1e4;
            QueryProcessCycleTime(hproc, &cycles);
            if (!wait_exit) { /* leave it running */ }
            CloseHandle(hproc);
        }
        printf("{\"label\":\"%s\",\"run\":%d,\"ok\":%s,\"call_ms\":%.3f,\"create_time_ms\":%.3f,\"done_ms\":%.3f,"
               "\"exit_ms\":%.3f,\"cpu_ms\":%.3f,\"pid\":%lu,\"new_procs\":\"%s\",\"marks\":[",
               esc(w2u(label).c_str()).c_str(), run, w == WAIT_OBJECT_0 ? "true" : "false", (t_ret - t0) / 1e4, ct_ms,
               w == WAIT_OBJECT_0 ? (t_done - t0) / 1e4 : -1.0, t_exit ? (t_exit - t0) / 1e4 : -1.0,
               cycles ? cycles / g_tsc_hz * 1e3 : -1.0, shm->pid, esc(newp.c_str()).c_str());
        LONG cnt = shm->count < LAB_MAX ? shm->count : LAB_MAX;
        for (LONG m = 0; m < cnt; m++)
            printf("%s[\"%s\",%.3f]", m ? "," : "", esc(shm->m[m].name).c_str(), ((double)shm->m[m].t - (double)t0) / 1e4);
        printf("],\"note\":\"%s\"}\n", esc(shm->note).c_str());
        fflush(stdout);
        if (fresh >= 0) { Sleep(50); DeleteFileW(exe.c_str()); }
    }
    return 0;
}
