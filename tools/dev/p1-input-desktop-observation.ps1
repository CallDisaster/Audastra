#requires -Version 7.2
# native09 Default专用窗口观察；旧非输入Desktop的Get-DirectDialogState合同保持。
if (-not ('P1InputDialogControls' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public sealed class P1InputDialogControl {
    public IntPtr Handle, Parent;
    public int Id, Pid, ThreadId;
    public string Class, Text;
    public bool Visible, OwnVisibleStyle, Enabled;
}
public static class P1InputDialogControls {
    private delegate bool EnumProc(IntPtr hwnd, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr hwnd, out int pid);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] static extern IntPtr GetWindowLongPtr(IntPtr hwnd,int index);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowTextLengthW(IntPtr hwnd);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd,StringBuilder text,int length);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd,StringBuilder text,int length);
    public static P1InputDialogControl[] Read(IntPtr dialog,int expectedPid) {
        int pid; int tid=GetWindowThreadProcessId(dialog,out pid);
        if (dialog == IntPtr.Zero || pid != expectedPid || tid == 0) throw new InvalidOperationException("welcome控件的dialog归属不符");
        var result=new List<P1InputDialogControl>();
        Exception failure=null;
        EnumChildWindows(dialog,(hwnd,data)=> {
          try {
            int childPid; int childTid=GetWindowThreadProcessId(hwnd,out childPid);
            if (childPid != expectedPid || childTid != tid || result.Count >= 256)
                throw new InvalidOperationException("welcome控件的GUI线程/数量不符");
            int length=GetWindowTextLengthW(hwnd);
            if (length < 0 || length > 16384) throw new InvalidOperationException("welcome控件文本预算不符");
            var text=new StringBuilder(length+1); GetWindowTextW(hwnd,text,text.Capacity);
            var cls=new StringBuilder(256); GetClassNameW(hwnd,cls,cls.Capacity);
            result.Add(new P1InputDialogControl {Handle=hwnd,Parent=GetParent(hwnd),Id=GetDlgCtrlID(hwnd),
                Pid=childPid,ThreadId=childTid,Class=cls.ToString(),Text=text.ToString(),
                Visible=IsWindowVisible(hwnd),Enabled=IsWindowEnabled(hwnd),
                OwnVisibleStyle=(GetWindowLongPtr(hwnd,-16).ToInt64() & 0x10000000) != 0});
            return true;
          } catch(Exception error) { failure=error; return false; }
        },IntPtr.Zero);
        if (failure != null) throw failure;
        return result.ToArray();
    }
    public static bool VisibleOwnFrame(IntPtr frame,int expectedPid) {
        int pid; int tid=GetWindowThreadProcessId(frame,out pid);
        return frame != IntPtr.Zero && pid == expectedPid && tid != 0 && IsWindowVisible(frame) &&
            (GetWindowLongPtr(frame,-16).ToInt64() & 0x10000000) != 0;
    }
}
'@
}

function Get-InputDesktopDialogState([YdweDialog]$Dialog,[IntPtr]$MainFrame,[int]$EditorPid,[bool]$InputDesktopProven) {
    if ($InputDesktopProven -and [YdweWin]::IsEditorFrame($MainFrame,$EditorPid) -and
        [P1InputDialogControls]::VisibleOwnFrame($MainFrame,$EditorPid) -and
        -not $Dialog.IsVisible -and -not $Dialog.OwnVisibleStyle -and $Dialog.Title -ceq '工具面板' -and
        $Dialog.Owner -eq $MainFrame -and $Dialog.OwnerPid -eq $EditorPid -and
        -not $Dialog.HasProgressControl -and $Dialog.YesButton -eq [IntPtr]::Zero) {
        # 当前Default可见主窗下，WS_VISIBLE也为false的OWN工具壳没有显示。
        # 其他模态把主窗禁用，不能把这个未显示的工具壳再算成第二个活动模态。
        return 'Inactive'
    }
    return Get-DirectDialogState $Dialog $MainFrame $EditorPid
}

function Get-InputDesktopWelcomeBody {
    return @(
        '欢迎使用YDWE',
        '这个编辑器是个威力强大的工具，可以让使用者调整魔兽争霸Ⅲ游戏中几乎各方面的功能，为了熟悉编辑器中的诸项功能，我们强烈的建议你加载预先制作的地图，做为参考。',
        '请注意: 虽然我们不会对编辑器提供任何直接的帮助，但是你可以前往魔兽地图编辑器贴吧，那里的人们会回答关于编辑器功能的问题或者帮助你解决在使用编辑器时遇到的困难。',
        '在使用这个程序时，你同意受到最终用户协议中的各项条款约束，这份合约可以在魔兽争霸Ⅲ安装目录中的「license.txt」文件档中找到。',
        '欢迎访问www.ydwe.net'
    ) -join "`n"
}
