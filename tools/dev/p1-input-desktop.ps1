#requires -Version 7.2
# native08 的启动/原句柄监管库；点加载本文件不启动 WE、不写注册表、不切换 Desktop。
if (-not ('P1InputDesktopOwner' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public sealed class P1InputDesktopWitness {
    public int ProcessId, ThreadId, SessionId, ConnectionState;
    public string WindowStation, ThreadDesktop, InputDesktop;
    public bool InteractiveWindowStation;
    public string ConnectionStateName { get { return ConnectionState == 0 ? "WTSActive" : "not-active"; } }
    public void RequireActiveDefault() {
        if (WindowStation != "WinSta0" || !InteractiveWindowStation || ThreadDesktop != "Default" ||
            InputDesktop != "Default" || ConnectionState != 0 || SessionId == 0)
            throw new InvalidOperationException("native08 要求同会话 Active WinSta0\\Default 输入桌面");
    }
}
public sealed class P1InputProcessIdentity {
    public int Pid, PrimaryThreadId, SessionId;
    public ulong CreationFileTime;
    public string Image, RequestedDesktop, PrimaryThreadDesktop, OriginalProcessHandle, OriginalThreadHandle;
    public bool RetainedOriginalProcessHandle = true, RetainedOriginalThreadHandle = true;
    public bool InheritHandles = false, UnpackagedProcessTreePolicy = true, KillOnJobClose = false;
}
public sealed class P1InputTerminal {
    public bool OriginalProcessSignaled, OriginalThreadSignaled, JobEmpty, AssociatedQueryHandlesSignaled;
    public bool AssociatedQueryEvidenceComplete;
    public int? ExitCode;
    public long[] AssociatedPids;
    public string[] UnresolvedQueryHandles;
    public bool StableTerminalState {
        get { return EnvironmentQuiescent && AssociatedQueryEvidenceComplete; }
    }
    public bool EnvironmentQuiescent {
        get { return OriginalProcessSignaled && OriginalThreadSignaled && JobEmpty && AssociatedQueryHandlesSignaled; }
    }
    // 后代使用关系验证后取得的查询句柄，绝不标为创建原句柄。
    public bool DescendantCreationHandlesRetained = false;
}
public sealed class P1InputDesktopOwner : IDisposable {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct Startup {
        public int Size; public string Reserved, Desktop, Title;
        public uint X, Y, XSize, YSize, XChars, YChars, Fill, Flags;
        public ushort Show, ReservedSize;
        public IntPtr ReservedBytes, StdIn, StdOut, StdErr;
    }
    [StructLayout(LayoutKind.Sequential)] struct StartupEx { public Startup Info; public IntPtr Attributes; }
    [StructLayout(LayoutKind.Sequential)] struct ProcessInfo { public IntPtr Process, Thread; public int Pid, Tid; }
    [StructLayout(LayoutKind.Sequential)] struct ObjectFlags { public int Inherit, Reserved; public uint Flags; }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool CreateProcessW(string app, StringBuilder command, IntPtr processSecurity, IntPtr threadSecurity,
        bool inherit, uint flags, IntPtr environment, string cwd, ref StartupEx startup, out ProcessInfo info);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, uint flags, ref IntPtr size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
    [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateJobObjectW(IntPtr security, string name);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job, int kind, IntPtr data, uint bytes, out uint returned);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool member);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr handle, out int code);
    [DllImport("kernel32.dll", SetLastError=true)] static extern int GetProcessId(IntPtr process);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool ProcessIdToSessionId(int pid, out int session);
    [DllImport("kernel32.dll")] static extern int GetCurrentProcessId();
    [DllImport("kernel32.dll")] static extern int GetCurrentThreadId();
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetProcessTimes(IntPtr process, out ulong creation, out ulong exit, out ulong kernel, out ulong user);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, StringBuilder path, ref int length);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr GetProcessWindowStation();
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr GetThreadDesktop(int tid);
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError=true)] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool GetUserObjectInformationW(IntPtr obj, int kind, IntPtr data, uint bytes, out uint needed);
    [DllImport("user32.dll", SetLastError=true)] static extern int GetWindowThreadProcessId(IntPtr hwnd, out int pid);
    [DllImport("wtsapi32.dll", SetLastError=true)] static extern bool WTSQuerySessionInformationW(IntPtr server, int session, int kind, out IntPtr data, out int bytes);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr data);

    IntPtr process, thread, job;
    readonly Dictionary<string, IntPtr> associated = new Dictionary<string, IntPtr>();
    readonly List<string> unresolved = new List<string>();
    bool disposed;
    int guiThread;
    public int Id { get; private set; }
    public int PrimaryThreadId { get; private set; }
    public string StartupFailure { get; private set; }
    public P1InputProcessIdentity Identity { get; private set; }
    public P1InputDesktopWitness ParentBefore { get; private set; }
    public int GuiThreadId { get { return guiThread; } }
    public bool IsDisposed { get { return disposed; } }
    public bool HasExited { get { return Signaled(process); } }
    public int ExitCode {
        get {
            if (!HasExited) throw new InvalidOperationException("原 hProcess 仍活动");
            int code; Check(GetExitCodeProcess(process, out code)); return code;
        }
    }
    public DateTime StartTime { get { return DateTime.FromFileTimeUtc((long)Identity.CreationFileTime).ToLocalTime(); } }
    static void Check(bool value) { if (!value) throw new Win32Exception(Marshal.GetLastWin32Error()); }
    static bool Signaled(IntPtr handle) {
        if (handle == IntPtr.Zero) throw new InvalidOperationException("缺少原句柄");
        uint state = WaitForSingleObject(handle, 0);
        if (state == 0xffffffffu) throw new Win32Exception(Marshal.GetLastWin32Error());
        if (state != 0 && state != 258) throw new InvalidOperationException("未知等待结果");
        return state == 0;
    }
    static string ObjectName(IntPtr handle) {
        if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        uint bytes; GetUserObjectInformationW(handle, 2, IntPtr.Zero, 0, out bytes);
        if (bytes < 2 || bytes > 65536) throw new InvalidOperationException("Desktop 名称预算无效");
        IntPtr buffer = Marshal.AllocHGlobal((int)bytes);
        try { Check(GetUserObjectInformationW(handle, 2, buffer, bytes, out bytes)); return Marshal.PtrToStringUni(buffer); }
        finally { Marshal.FreeHGlobal(buffer); }
    }
    public static string ThreadDesktopName(int tid) { return ObjectName(GetThreadDesktop(tid)); }
    public static P1InputDesktopWitness Witness() {
        int pid = GetCurrentProcessId(), session;
        Check(ProcessIdToSessionId(pid, out session));
        IntPtr state; int bytes;
        Check(WTSQuerySessionInformationW(IntPtr.Zero, session, 8, out state, out bytes));
        int connection;
        try { if (bytes != 4) throw new InvalidOperationException("WTSConnectState 长度无效"); connection = Marshal.ReadInt32(state); }
        finally { WTSFreeMemory(state); }
        IntPtr station = GetProcessWindowStation();
        var flags = new ObjectFlags();
        IntPtr storage = Marshal.AllocHGlobal(Marshal.SizeOf<ObjectFlags>());
        uint needed;
        try {
            Check(GetUserObjectInformationW(station, 1, storage, (uint)Marshal.SizeOf<ObjectFlags>(), out needed));
            flags = Marshal.PtrToStructure<ObjectFlags>(storage);
        } finally { Marshal.FreeHGlobal(storage); }
        // 不申请 SWITCHDESKTOP，不借此更改输入桌面。
        IntPtr input = OpenInputDesktop(0, false, 1);
        if (input == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        string inputName;
        try { inputName = ObjectName(input); } finally { Check(CloseDesktop(input)); }
        return new P1InputDesktopWitness { ProcessId=pid, ThreadId=GetCurrentThreadId(), SessionId=session,
            ConnectionState=connection, WindowStation=ObjectName(station),
            InteractiveWindowStation=(flags.Flags & 1) != 0,
            ThreadDesktop=ThreadDesktopName(GetCurrentThreadId()), InputDesktop=inputName };
    }
    void ValidateIdentity() {
        if (disposed || GetProcessId(process) != Id) throw new InvalidOperationException("原 hProcess 身份无效");
        ulong creation, exit, kernel, user; Check(GetProcessTimes(process, out creation, out exit, out kernel, out user));
        var image = new StringBuilder(32768); int length = image.Capacity;
        Check(QueryFullProcessImageNameW(process, 0, image, ref length));
        int session; Check(ProcessIdToSessionId(Id, out session));
        if (creation != Identity.CreationFileTime || !String.Equals(image.ToString(), Identity.Image, StringComparison.OrdinalIgnoreCase) ||
            session != Identity.SessionId || session != ParentBefore.SessionId)
            throw new InvalidOperationException("原 hProcess image/创建时间/会话发生身份冲突");
    }
    public static Dictionary<string,string> SelectedWindowsEnvironment() {
        var selected=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        foreach(string key in new [] { "SystemRoot","WINDIR","PATH","TEMP","TMP","SystemDrive","ComSpec",
            "PATHEXT","USERPROFILE","LOCALAPPDATA","APPDATA","ProgramData" }) {
            string value=Environment.GetEnvironmentVariable(key);
            if (String.IsNullOrEmpty(value)) throw new InvalidOperationException("缺少native08基础Windows环境键: "+key);
            selected.Add(key,value);
        }
        return selected;
    }
    public static P1InputDesktopOwner Start(string image, string command, string cwd) {
        return Start(image,command,cwd,SelectedWindowsEnvironment());
    }
    public static P1InputDesktopOwner Start(string image, string command, string cwd, IDictionary<string,string> environment) {
        var owner = new P1InputDesktopOwner { ParentBefore=Witness() };
        owner.ParentBefore.RequireActiveDefault();
        image = Path.GetFullPath(image); cwd = Path.GetFullPath(cwd);
        if (command.IndexOf('\0') >= 0 || !File.Exists(image) || !Directory.Exists(cwd))
            throw new InvalidOperationException("启动合同路径/命令无效");
        string executable=Path.GetFileName(image).ToLowerInvariant();
        if (executable == "worldedit.exe" || executable == "worldeditydwe.exe" || executable == "ydwe.exe" ||
            executable == "war3.exe" || executable == "warcraft iii.exe")
            throw new InvalidOperationException("专用WE/game Default启动已阻止：DISPLAY1定位与SW_HIDE均不保证禁止激活，尚无pre-resume完整隔离合同");
        var allowed=new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "SystemRoot","WINDIR","PATH","TEMP","TMP",
            "SystemDrive","ComSpec","PATHEXT","USERPROFILE","LOCALAPPDATA","APPDATA","ProgramData","ydwe-process-name" };
        var names=new List<string>(environment.Keys); names.Sort(StringComparer.OrdinalIgnoreCase);
        if (names.Count != 12 && names.Count != 13) throw new InvalidOperationException("native08环境键数量不符");
        var block=new StringBuilder();
        foreach(string key in names) {
            string value=environment[key];
            if (!allowed.Remove(key) || key.IndexOfAny(new [] {'\0','='}) >= 0 || String.IsNullOrEmpty(value) || value.IndexOf('\0') >= 0)
                throw new InvalidOperationException("native08环境含未获准/重复/无效键值");
            if (String.Equals(key,"ydwe-process-name",StringComparison.OrdinalIgnoreCase) && value != "ydwe")
                throw new InvalidOperationException("native08 direct-loader进程名环境不符");
            block.Append(key).Append('=').Append(value).Append('\0');
        }
        if (allowed.Count > 1 || (allowed.Count == 1 && !allowed.Contains("ydwe-process-name")))
            throw new InvalidOperationException("native08缺少必要基础Windows环境");
        block.Append('\0');
        owner.job = CreateJobObjectW(IntPtr.Zero, null);
        Check(owner.job != IntPtr.Zero);
        IntPtr size = IntPtr.Zero, attributes = IntPtr.Zero, policy = IntPtr.Zero, environmentBlock=IntPtr.Zero;
        bool initialized = false, created = false;
        try {
            bool sized = InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
            if (sized || Marshal.GetLastWin32Error() != 122 || size.ToInt64() < 1 || size.ToInt64() > 1048576)
                throw new InvalidOperationException("desktop-app attribute 容量查询失败");
            attributes = Marshal.AllocHGlobal(size); policy = Marshal.AllocHGlobal(4); Marshal.WriteInt32(policy, 1);
            Check(InitializeProcThreadAttributeList(attributes, 1, 0, ref size)); initialized = true;
            // ProcThreadAttributeDesktopAppPolicy (18 | INPUT), ENABLE_PROCESS_TREE=1；无 breakaway。
            Check(UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x20012), policy, new IntPtr(4), IntPtr.Zero, IntPtr.Zero));
            var startup = new StartupEx { Info=new Startup { Size=Marshal.SizeOf<StartupEx>(),
                Desktop=@"WinSta0\Default", Flags=1, Show=0 }, Attributes=attributes };
            if (IntPtr.Size != 8 || Marshal.SizeOf<StartupEx>() != 112)
                throw new InvalidOperationException("native08 仅支持 AMD64 监管器");
            ProcessInfo info;
            environmentBlock=Marshal.StringToHGlobalUni(block.ToString());
            Check(CreateProcessW(image, new StringBuilder(command), IntPtr.Zero, IntPtr.Zero, false,
                4u | 0x80000u | 0x400u, environmentBlock, cwd, ref startup, out info));
            created = true;
            owner.process=info.Process; owner.thread=info.Thread; owner.Id=info.Pid; owner.PrimaryThreadId=info.Tid;
            ulong creation, exit, kernel, user; Check(GetProcessTimes(owner.process, out creation, out exit, out kernel, out user));
            int session; Check(ProcessIdToSessionId(info.Pid, out session));
            owner.Identity = new P1InputProcessIdentity { Pid=info.Pid, PrimaryThreadId=info.Tid,
                SessionId=session, CreationFileTime=creation, Image=image, RequestedDesktop=@"WinSta0\Default",
                OriginalProcessHandle="0x"+info.Process.ToInt64().ToString("X"),
                OriginalThreadHandle="0x"+info.Thread.ToInt64().ToString("X") };
            owner.ValidateIdentity();
            Check(AssignProcessToJobObject(owner.job, owner.process));
            if (ResumeThread(owner.thread) != 1) throw new InvalidOperationException("原主线程恢复失败");
        } catch (Exception error) {
            if (!created) { CloseHandle(owner.job); owner.job=IntPtr.Zero; throw; }
            // 已创建的原句柄全部留给调用者；不终止、不假签终态。
            owner.StartupFailure=error.Message;
        } finally {
            if (initialized) DeleteProcThreadAttributeList(attributes);
            if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
            if (policy != IntPtr.Zero) Marshal.FreeHGlobal(policy);
            if (environmentBlock != IntPtr.Zero) Marshal.FreeHGlobal(environmentBlock);
        }
        return owner;
    }
    public void AssertRunningDefault() {
        if (HasExited) throw new InvalidOperationException("本次原 hProcess 已退出: "+ExitCode);
        ValidateIdentity();
        var witness = Witness(); witness.RequireActiveDefault();
        if (witness.SessionId != Identity.SessionId) throw new InvalidOperationException("父/OWN 活动会话不符");
        string actual = ThreadDesktopName(PrimaryThreadId);
        if (actual != "Default") throw new InvalidOperationException("OWN 原主线程实际 Desktop 不符");
        Identity.PrimaryThreadDesktop=actual;
        ObserveAssociated();
    }
    public void AssertWindow(IntPtr hwnd, bool bindGuiThread) {
        AssertRunningDefault();
        int pid; int tid=GetWindowThreadProcessId(hwnd, out pid);
        if (hwnd == IntPtr.Zero || pid != Id || tid == 0 || ThreadDesktopName(tid) != "Default")
            throw new InvalidOperationException("命令 HWND 的 PID/GUI线程/Desktop 不属于原 OWN 句柄");
        if (guiThread != 0 && guiThread != tid) throw new InvalidOperationException("命令 HWND 不属于已绑定唯一 GUI线程");
        if (bindGuiThread) guiThread=tid;
    }
    long[] JobPids() {
        IntPtr buffer=Marshal.AllocHGlobal(32768);
        try {
            uint returned; Check(QueryInformationJobObject(job, 3, buffer, 32768, out returned));
            int assigned=Marshal.ReadInt32(buffer), listed=Marshal.ReadInt32(buffer, 4);
            if (assigned != listed || listed < 0 || listed > 4095 || returned < 8 + listed * 8)
                throw new InvalidOperationException("OWN Job 枚举不完整");
            var pids=new long[listed];
            for (int i=0;i<listed;i++) pids[i]=Marshal.ReadInt64(buffer, 8+i*8);
            return pids;
        } finally { Marshal.FreeHGlobal(buffer); }
    }
    public void ObserveAssociated() {
        foreach (long pidValue in JobPids()) {
            if (pidValue == Id) continue;
            if (pidValue < 1 || pidValue > Int32.MaxValue) throw new InvalidOperationException("OWN 关联 PID 越界");
            int pid=(int)pidValue;
            IntPtr handle=OpenProcess(0x1000u | 0x100000u, false, pid);
            if (handle == IntPtr.Zero) { unresolved.Add("query-unavailable:"+pid); continue; }
            bool kept=false;
            try {
                bool member; Check(IsProcessInJob(handle, job, out member));
                if (!member) { unresolved.Add("query-not-member:"+pid); continue; }
                ulong creation, exit, kernel, user; Check(GetProcessTimes(handle, out creation, out exit, out kernel, out user));
                string key=pid+":"+creation;
                if (!associated.ContainsKey(key)) { associated.Add(key,handle); kept=true; }
            } finally { if (!kept) CloseHandle(handle); }
        }
    }
    public P1InputTerminal Terminal() {
        ObserveAssociated();
        var pending=new List<string>(unresolved);
        bool queriesEnded=true;
        foreach (var row in associated) if (!Signaled(row.Value)) { pending.Add(row.Key); queriesEnded=false; }
        bool ended=HasExited;
        long[] pids=JobPids();
        return new P1InputTerminal { OriginalProcessSignaled=ended, OriginalThreadSignaled=Signaled(thread),
            ExitCode=ended ? (int?)ExitCode : null, AssociatedPids=pids,
            JobEmpty=pids.Length == 0, AssociatedQueryHandlesSignaled=queriesEnded,
            AssociatedQueryEvidenceComplete=unresolved.Count == 0,
            UnresolvedQueryHandles=pending.ToArray() };
    }
    public void Dispose() {
        if (disposed) return;
        if (!Terminal().EnvironmentQuiescent) throw new InvalidOperationException("OWN 未到原句柄终态；监管session/句柄必须保留");
        foreach (var handle in associated.Values) Check(CloseHandle(handle));
        Check(CloseHandle(thread)); Check(CloseHandle(process)); Check(CloseHandle(job)); disposed=true;
    }
}
'@
}

function Get-P1InputEnvironmentFingerprint([object]$Variables) {
    $environment = [ordered]@{}
    if ($null -eq $Variables) { $Variables=[P1InputDesktopOwner]::SelectedWindowsEnvironment() }
    foreach ($key in @($variables.Keys | Sort-Object)) { $environment[$key.ToLowerInvariant()] = $variables[$key] }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($environment | ConvertTo-Json -Compress))
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
}

function Write-P1InputDesktopNewJson([string]$Path, $Value) {
    $text = $Value | ConvertTo-Json -Depth 35
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($text + "`n")
        $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true)
    } finally { $stream.Dispose() }
}
