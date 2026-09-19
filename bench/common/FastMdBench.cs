// FastMD bench protocol v1 helper for C# prototypes (NativeAOT / trimming safe).
// Usage:
//   FastMdBench.Init();                        // first line of Main (records "main" mark)
//   FastMdBench.Mark("parsed");                // optional breakdown marks
//   FastMdBench.WindowShown();                 // optional
//   if (FastMdBench.ContentPresented()) Exit;  // after the first rendered frame with the document (see PROTOCOL.md §3)
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

internal static partial class FastMdBench
{
    private static string? _out;
    private static bool _exitAfter = true;
    private static bool _done;
    private static long _tWindow;
    private static readonly List<(string, long)> _marks = new();

    [DllImport("dwmapi.dll")]
    private static extern int DwmFlush();

    public static bool IsBench => _out != null;
    public static long Now() => DateTime.UtcNow.ToFileTimeUtc();

    public static void Init()
    {
        _out = Environment.GetEnvironmentVariable("FASTMD_BENCH_OUT");
        if (string.IsNullOrEmpty(_out)) { _out = null; return; }
        _exitAfter = Environment.GetEnvironmentVariable("FASTMD_BENCH_EXIT") != "0";
        Mark("main");
    }

    public static void Mark(string name) { if (_out != null) _marks.Add((name, Now())); }

    public static void WindowShown() { if (_out != null && _tWindow == 0) _tWindow = Now(); }

    /// <summary>Returns true if the app should exit now (bench mode with exit requested). Idempotent.</summary>
    public static bool ContentPresented(string notes = "")
    {
        if (_out == null || _done) return false;
        DwmFlush();
        long tContent = Now();
        _done = true;
        var sb = new StringBuilder();
        sb.Append("{\"protocol\":1,\"t_content\":").Append(tContent);
        sb.Append(",\"t_window\":").Append(_tWindow == 0 ? "null" : _tWindow.ToString());
        sb.Append(",\"ws_bytes\":").Append(Environment.WorkingSet);
        sb.Append(",\"marks\":{");
        for (int i = 0; i < _marks.Count; i++)
            sb.Append(i == 0 ? "" : ",").Append('"').Append(_marks[i].Item1).Append("\":").Append(_marks[i].Item2);
        sb.Append("},\"notes\":\"").Append(notes.Replace("\\", "\\\\").Replace("\"", "'")).Append("\"}");
        string tmp = _out + ".tmp";
        File.WriteAllText(tmp, sb.ToString());
        File.Move(tmp, _out, overwrite: true);
        return _exitAfter;
    }
}
