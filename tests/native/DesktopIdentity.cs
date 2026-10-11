// 仅供原创 Win32 合同夹具记录身份，不随产品发布。
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public sealed class NativeDesktopIdentity {
    public string Stage { get; set; }
    public uint ProcessId { get; set; }
    public uint ThreadId { get; set; }
    public ulong ProcessCreated { get; set; }
    public string ThreadDesktop { get; set; }
    public string InputDesktop { get; set; }
    public string JobName { get; set; }
    public bool InExpectedJob { get; set; }
    public long Window { get; set; }
    public uint WindowProcessId { get; set; }
    public uint WindowThreadId { get; set; }

    [StructLayout(LayoutKind.Sequential)] private struct FileTime { public uint Low, High; }
    [DllImport("kernel32.dll")] private static extern uint GetCurrentProcessId();
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool GetProcessTimes(IntPtr process,
        out FileTime created, out FileTime exited, out FileTime kernel, out FileTime user);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] private static extern IntPtr OpenJobObjectW(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool CloseHandle(IntPtr value);
    [DllImport("user32.dll", SetLastError=true)] private static extern IntPtr GetThreadDesktop(uint thread);
    [DllImport("user32.dll", SetLastError=true)] private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool CloseDesktop(IntPtr value);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] private static extern bool GetUserObjectInformationW(
        IntPtr value, int index, StringBuilder text, uint bytes, out uint needed);
    [DllImport("user32.dll", SetLastError=true)] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);

    private static void Check(bool result, string operation) {
        if (!result) throw new Win32Exception(Marshal.GetLastWin32Error(), operation);
    }
    private static string DesktopName(IntPtr desktop) {
        Check(desktop != IntPtr.Zero, "query Desktop handle");
        var text = new StringBuilder(256);
        uint needed;
        Check(GetUserObjectInformationW(desktop, 2, text, 512, out needed) && text.Length != 0, "query Desktop name");
        return text.ToString();
    }
    public static NativeDesktopIdentity Capture(string stage, string jobName, IntPtr window) {
        var row = new NativeDesktopIdentity {
            Stage=stage, ProcessId=GetCurrentProcessId(), ThreadId=GetCurrentThreadId(),
            JobName=jobName, Window=window.ToInt64()
        };
        FileTime created, exited, kernel, user;
        Check(GetProcessTimes(GetCurrentProcess(), out created, out exited, out kernel, out user), "query process creation time");
        row.ProcessCreated = ((ulong)created.High << 32) | created.Low;
        row.ThreadDesktop = DesktopName(GetThreadDesktop(row.ThreadId)); // 借用句柄，不关闭。
        IntPtr input = OpenInputDesktop(0, false, 1);
        Check(input != IntPtr.Zero, "open input Desktop for query");
        try { row.InputDesktop = DesktopName(input); }
        finally { Check(CloseDesktop(input), "close input Desktop query handle"); }
        IntPtr job = OpenJobObjectW(4, false, jobName);
        Check(job != IntPtr.Zero, "open expected Job for query");
        try {
            bool member;
            Check(IsProcessInJob(GetCurrentProcess(), job, out member), "query expected Job membership");
            row.InExpectedJob = member;
        } finally { Check(CloseHandle(job), "close Job query handle"); }
        if (window != IntPtr.Zero) {
            uint owner;
            row.WindowThreadId = GetWindowThreadProcessId(window, out owner);
            row.WindowProcessId = owner;
            Check(row.WindowThreadId != 0, "query created fixture window identity");
        }
        return row;
    }
    public void RequirePrivateDesktop(string expectedDesktop) {
        if (String.IsNullOrEmpty(expectedDesktop) || !InExpectedJob || ProcessCreated == 0 ||
            !String.Equals(ThreadDesktop, expectedDesktop, StringComparison.OrdinalIgnoreCase) ||
            String.Equals(ThreadDesktop, InputDesktop, StringComparison.OrdinalIgnoreCase) ||
            (Window != 0 && (WindowProcessId != ProcessId || WindowThreadId != ThreadId))) {
            throw new InvalidOperationException("test process/window Desktop or Job identity differs from isolated host");
        }
    }
}
