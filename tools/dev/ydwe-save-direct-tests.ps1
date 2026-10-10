# 原创 direct-loader 合同回归；只用临时文件及本次 PowerShell 自有 Win32 窗口。
param([string]$WorkDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = Join-Path $PSScriptRoot 'ydwe-save.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw "ydwe-save parser errors: $($parseErrors.Message -join ', ')" }
# 只取原脚本的 Add-Type 和纯只读函数；绝不执行其启动/保存入口。
$typeCommand = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Add-Type' }, $true)[0]
Add-Type -TypeDefinition $typeCommand.CommandElements[1].Value
$functionNames = @('Assert-PlainNativePath', 'Assert-NativeMember', 'Assert-NativeFileRecord',
    'Get-NativeDirectoryInventory', 'Assert-DirectNativeLoader', 'Get-DirectDialogState',
    'Test-NativeVersionWarning', 'Assert-DirectEditorReady', 'Assert-DirectLauncherRunning',
    'Get-EditorWindowObservation', 'Get-EditorLoadTimeoutMessage')
foreach ($function in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
    if ($function.Name -in $functionNames) { Invoke-Expression $function.Extent.Text }
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class YdweFixtureWin {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] private struct WindowClass {
        public uint Size, Style; public IntPtr Procedure; public int ClassExtra, WindowExtra;
        public IntPtr Instance, Icon, Cursor, Background; public string Menu, Name; public IntPtr SmallIcon;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Controls { public uint Size, Classes; }
    private delegate IntPtr WindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    private static readonly WindowProc Procedure = DefWindowProcW;
    private static readonly List<IntPtr> Owned = new List<IntPtr>();
    private static readonly List<string> Classes = new List<string>();
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] private static extern ushort RegisterClassExW(ref WindowClass value);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern bool UnregisterClassW(string value, IntPtr instance);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] private static extern IntPtr CreateWindowExW(uint ex, string cls, string text, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool DestroyWindow(IntPtr value);
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr value);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr value, out uint pid);
    [DllImport("user32.dll")] private static extern bool EnableWindow(IntPtr value, bool enabled);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] private static extern IntPtr GetModuleHandleW(string name);
    [DllImport("comctl32.dll")] private static extern bool InitCommonControlsEx(ref Controls value);
    public static void Initialize() {
        foreach (string name in new [] { "Warcraft III", "FORGE-P1-OtherFrame" }) {
            var value = new WindowClass { Size=(uint)Marshal.SizeOf(typeof(WindowClass)),
                Procedure=Marshal.GetFunctionPointerForDelegate(Procedure), Instance=GetModuleHandleW(null), Name=name };
            if (RegisterClassExW(ref value)==0) throw new Win32Exception(Marshal.GetLastWin32Error());
            Classes.Add(name);
        }
        var controls = new Controls { Size=8, Classes=0x20 };
        if (!InitCommonControlsEx(ref controls)) throw new InvalidOperationException("progress class initialization failed");
    }
    public static IntPtr Create(string cls, string text, IntPtr owner, bool visible, bool tool) {
        // 不激活、不使用 ShowWindow；可见夹具在桌面范围外创建。
        IntPtr value=CreateWindowExW(0x08000000u | (tool ? 0x80u : 0u), cls, text,
            0x80000000u | (visible ? 0x10000000u : 0u), -30000, -30000, 200, 100,
            owner, IntPtr.Zero, GetModuleHandleW(null), IntPtr.Zero);
        if (value==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        Owned.Add(value); return value;
    }
    public static IntPtr Child(IntPtr parent, string cls, string text, int id, bool visible) {
        AssertOwned(parent);
        IntPtr value=CreateWindowExW(0, cls, text, 0x40000000u | (visible ? 0x10000000u : 0u),
            0, 0, 50, 20, parent, new IntPtr(id), GetModuleHandleW(null), IntPtr.Zero);
        if (value==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        Owned.Add(value); return value;
    }
    private static void AssertOwned(IntPtr value) {
        uint pid; GetWindowThreadProcessId(value, out pid);
        if (!Owned.Contains(value) || pid!=(uint)Process.GetCurrentProcess().Id)
            throw new InvalidOperationException("fixture window is not owned by this test process");
    }
    public static void Enabled(IntPtr value, bool enabled) { AssertOwned(value); EnableWindow(value, enabled); }
    public static void Remove(IntPtr value) {
        if (!Owned.Contains(value)) throw new InvalidOperationException("unknown fixture handle");
        if (IsWindow(value)) { AssertOwned(value); if (!DestroyWindow(value)) throw new Win32Exception(Marshal.GetLastWin32Error()); }
        Owned.Remove(value);
        Owned.RemoveAll(item => !IsWindow(item));
    }
    public static void Cleanup() {
        foreach (IntPtr value in Owned.ToArray()) { if (IsWindow(value)) { AssertOwned(value); DestroyWindow(value); } }
        Owned.Clear();
        foreach (string name in Classes) UnregisterClassW(name, GetModuleHandleW(null));
        Classes.Clear();
    }
}
'@

$taskToken = [Guid]::NewGuid().ToString('N')
$autoCleanup = -not $WorkDirectory
if ($autoCleanup) { $WorkDirectory = Join-Path ([IO.Path]::GetTempPath()) ('forge-p1-direct-tests-' + $taskToken) }
$taskRoot = Assert-PlainNativePath $WorkDirectory
if (Test-Path -LiteralPath $taskRoot) { throw '测试目录必须不存在，不覆盖证据' }
[void][IO.Directory]::CreateDirectory($taskRoot)
$taskOwner = Join-Path $taskRoot 'owner.local.txt'
[IO.File]::WriteAllText($taskOwner, $taskToken)
$script:passed = 0
$script:failed = [Collections.Generic.List[object]]::new()
$script:fixtureNumber = 0
$taskEditorPid = [Diagnostics.Process]::GetCurrentProcess().Id
$versionText = '该地图是由一个版本更新的编辑器保存的.或者它包含了一些现有版本并不支持的数据.继续载入地图吗'
$foregroundBefore = [YdweFixtureWin]::GetForegroundWindow()
$started = [DateTime]::UtcNow

function Check([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
function Refuses([scriptblock]$Action, [string]$Pattern) {
    $rejected = $false
    try { & $Action | Out-Null } catch {
        $rejected = $true
        if ($Pattern -and $_.Exception.Message -notmatch $Pattern) { throw "错误拒绝原因: $($_.Exception.Message)" }
    }
    Check $rejected '应拒绝但通过了'
}
function Case([string]$Name, [scriptblock]$Action) {
    try { & $Action; $script:passed++; "PASS $Name" } catch {
        $script:failed.Add([pscustomobject]@{ Name=$Name; Error=$_.Exception.Message }); "FAIL $Name : $($_.Exception.Message)"
    }
}
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Write-New([string]$Path, [byte[]]$Bytes) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew)
    try { $stream.Write($Bytes, 0, $Bytes.Length) } finally { $stream.Dispose() }
}
function Save-Receipt($Fixture) {
    [IO.File]::WriteAllText($Fixture.ReceiptPath, ($Fixture.Receipt | ConvertTo-Json -Depth 12))
    $Fixture.Sha = Hash $Fixture.ReceiptPath
}
function Fixture {
    $script:fixtureNumber++
    $base = Join-Path $taskRoot ('fixture-' + $script:fixtureNumber)
    $game = Join-Path $base 'game'
    $hostRoot = Join-Path $game 'YDWE-p1'
    $original = Join-Path $base 'original/worldedit.exe'
    $loaderBytes = [Text.Encoding]::UTF8.GetBytes('original temporary loader bytes')
    Write-New $original $loaderBytes
    $configText = '<?xml version="1.0"?>' + "`n<configuration>`n`t<runtime>`n`t`t<loadFromRemoteSources enabled=`"true`" />`n`t</runtime>`n</configuration>`n"
    $names = @('war3.exe', 'game.dll', 'war3.mpq', 'war3patch.mpq', 'storm.dll', 'worldeditydwe.exe', 'worldeditydwe.exe.config')
    $hostNames = @('bin/worldedit.exe', 'bin/LuaEngine.dll', 'bin/ydbase.dll', 'script/ydwe/startup.lua',
        'ui/fixture/define.txt', 'logs/fixed.log', 'share/a.txt', 'share/b.txt', 'share/c.txt')
    $rows = [Collections.Generic.List[object]]::new()
    foreach ($scope in @('game', 'host')) {
        $root = if ($scope -ceq 'game') { $game } else { $hostRoot }
        $members = if ($scope -ceq 'game') { $names } else { $hostNames }
        foreach ($name in $members) {
            $path = Join-Path $root $name
            $bytes = if ($name -eq 'worldeditydwe.exe' -or $name -eq 'bin/worldedit.exe') { $loaderBytes }
                elseif ($name -eq 'worldeditydwe.exe.config') { [Text.Encoding]::UTF8.GetBytes($configText) }
                else { [Text.Encoding]::UTF8.GetBytes('fixture immutable member ' + $name) }
            Write-New $path $bytes
            $rows.Add([pscustomobject]@{root=$scope; relativePath=$name; bytes=$bytes.Length; sha256=(Hash $path)})
        }
    }
    $loaderRecord = $rows | Where-Object relativePath -CEQ 'worldeditydwe.exe'
    $configRecord = $rows | Where-Object relativePath -CEQ 'worldeditydwe.exe.config'
    $receipt = [pscustomobject]@{schema='p1-native-loader-v1'; status='prepared'; sourceUnchanged=$true;
        launched=$false; registryWritten=$false; nativeSaveValidated=$false; gameRoot=$game; hostRoot=$hostRoot;
        loader=$loaderRecord; config=$configRecord; dependencies=$rows.ToArray();
        directories=[pscustomobject]@{game=@();host=@('bin','script','script/ydwe','ui','ui/fixture','logs','share')};
        loaderReference=[pscustomobject]@{path=$original;bytes=$loaderBytes.Length;sha256=(Hash $original)}}
    $fixture = [pscustomobject]@{Base=$base;Game=$game;HostRoot=$hostRoot;Original=$original;
        ReceiptPath=(Join-Path $base 'prepared.local.json'); Receipt=$receipt;Sha=''}
    Save-Receipt $fixture
    return $fixture
}
function Validate($Fixture) { Assert-DirectNativeLoader $Fixture.Game $Fixture.HostRoot $Fixture.ReceiptPath $Fixture.Sha }
function NativeDialog([IntPtr]$Handle) {
    $found = @([YdweWin]::GetDialogs($taskEditorPid, $true) | Where-Object Handle -EQ $Handle)
    Check ($found.Count -eq 1) '本次 native 对话框应唯一'
    return $found[0]
}

try {
    [YdweFixtureWin]::Initialize()
    Case 'plain complete nested-host receipt passes' {
        $f=Fixture; Check ((Validate $f) -ceq (Join-Path $f.Game 'worldeditydwe.exe')) 'loader 返回值不符'
    }
    Case 'wrong receipt pin refuses' { $f=Fixture; $f.Sha='0'*64; Refuses { Validate $f } 'SHA' }
    Case 'fixed config rejects self-consistent replacement' {
        $f=Fixture; $path=Join-Path $f.Game 'worldeditydwe.exe.config'; [IO.File]::WriteAllText($path,'different config')
        $f.Receipt.config.bytes=(Get-Item $path).Length; $f.Receipt.config.sha256=Hash $path
        Save-Receipt $f; Refuses { Validate $f } '固定121'
    }
    Case 'dependency content drift refuses' { $f=Fixture; [IO.File]::WriteAllText((Join-Path $f.HostRoot 'script/ydwe/startup.lua'),'drift'); Refuses { Validate $f } '字节身份' }
    Case 'unbound executable refuses' { $f=Fixture; Write-New (Join-Path $f.Game 'new.dll') ([byte[]]@(1,2)); Refuses { Validate $f } '集合漂移' }
    Case 'new empty directory refuses' { $f=Fixture; [void][IO.Directory]::CreateDirectory((Join-Path $f.HostRoot 'empty-new')); Refuses { Validate $f } '集合漂移' }
    Case 'missing directory from receipt refuses' { $f=Fixture; $f.Receipt.directories.host=@('bin','script','script/ydwe','ui','ui/fixture','logs'); Save-Receipt $f; Refuses { Validate $f } '集合漂移' }
    Case 'duplicate directory and file entries refuse' {
        $f=Fixture; $f.Receipt.directories.host += 'BIN'; Save-Receipt $f; Refuses { Validate $f } '目录.*重复'
        $f=Fixture; $f.Receipt.dependencies += $f.Receipt.dependencies[0]; Save-Receipt $f; Refuses { Validate $f } '成员重复'
    }
    Case 'directory traversal and ADS refuse' {
        foreach ($name in @('../outside','bin/worldedit.exe:stream','ui/trailing.')) {
            $f=Fixture; $f.Receipt.directories.host=@($name); Save-Receipt $f; Refuses { Validate $f } '相对路径'
        }
    }
    Case 'real dependency hardlink refuses despite matching bytes' {
        $f=Fixture; $alias=Join-Path $f.Base 'alias.dll'; New-Item -ItemType HardLink -Path $alias -Target (Join-Path $f.HostRoot 'bin/LuaEngine.dll') | Out-Null
        try { Refuses { Validate $f } 'hardlink' } finally { Remove-Item -LiteralPath $alias -Force }
    }
    Case 'real reference hardlink refuses' {
        $f=Fixture; $alias=Join-Path $f.Base 'alias-reference.exe'; New-Item -ItemType HardLink -Path $alias -Target $f.Original | Out-Null
        try { Refuses { Validate $f } 'hardlink' } finally { Remove-Item -LiteralPath $alias -Force }
    }
    Case 'real receipt hardlink refuses before reading JSON' {
        $f=Fixture; $alias=Join-Path $f.Base 'alias-receipt.json'; New-Item -ItemType HardLink -Path $alias -Target $f.ReceiptPath | Out-Null
        try { Refuses { Validate $f } 'hardlink' } finally { Remove-Item -LiteralPath $alias -Force }
    }
    Case 'empty junction is refused before recursive enumeration' {
        $f=Fixture; $target=Join-Path $f.Base 'junction-target'; [void][IO.Directory]::CreateDirectory($target)
        $link=Join-Path $f.HostRoot 'empty-junction'; New-Item -ItemType Junction -Path $link -Target $target | Out-Null
        try { Refuses { Validate $f } 'reparse' } finally { Remove-Item -LiteralPath $link -Force }
    }
    Case 'boolean and numeric record types are strict' {
        $f=Fixture; $f.Receipt.sourceUnchanged='true'; Save-Receipt $f; Refuses { Validate $f } '身份不符'
        $f=Fixture; $f.Receipt.dependencies[0].bytes='42'; Save-Receipt $f; Refuses { Validate $f } '字节身份'
    }
    Case 'native SHA rejects same-length dependency content replacement' {
        $f=Fixture; $path=Join-Path $f.HostRoot 'bin/LuaEngine.dll'; $bytes=[IO.File]::ReadAllBytes($path)
        $bytes[0]=$bytes[0] -bxor 1; [IO.File]::WriteAllBytes($path,$bytes)
        Refuses { Validate $f } '字节身份'
    }
    Case 'native ancestor is rechecked after previous successful validation' {
        $f=Fixture; [void](Validate $f)
        $path=Join-Path $f.HostRoot 'script'; $moved=Join-Path $f.HostRoot 'script-previous'
        [IO.Directory]::Move($path,$moved)
        New-Item -ItemType Junction -Path $path -Target $moved | Out-Null
        try { Refuses { Validate $f } 'reparse' } finally { Remove-Item -LiteralPath $path -Force }
    }
    Case 'native SHA cannot report success while another handle permits writing' {
        $f=Fixture; [void](Validate $f); $path=Join-Path $f.HostRoot 'bin/LuaEngine.dll'
        $writer=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
        try { Refuses { Validate $f } '使用|used|sharing|访问|access' } finally { $writer.Dispose() }
        [void](Validate $f)
    }
    Case 'native long path retains exact bytes SHA and complete inventory' {
        $base=Join-Path $taskRoot 'longpath'; $path=$base
        for($n=0;$n -lt 8;$n++) { $path=Join-Path $path ('segment-' + $n + '-' + ('a'*32)) }
        $path=Join-Path $path 'fixture.bin'; $bytes=[Text.Encoding]::UTF8.GetBytes('original long path metadata fixture')
        Write-New $path $bytes; Check ($path.Length -gt 320) '夹具必须实际超过MAX_PATH'
        Assert-NativeFileRecord $path ([pscustomobject]@{bytes=$bytes.Length;sha256=(Hash $path)})
        $inventory=Get-NativeDirectoryInventory $base $null
        Check ($inventory.Files.Count -eq 1 -and $inventory.Files[0] -ceq $path -and $inventory.Directories.Count -eq 8) '长路径完整集合不符'
    }
    Case 'native receipt rejects over-budget before text parsing' {
        $f=Fixture; [IO.File]::WriteAllBytes($f.ReceiptPath,[byte[]]::new(8388609)); $f.Sha=Hash $f.ReceiptPath
        Refuses { Validate $f } '8MiB预算'
    }
    Case 'direct launcher accepts the still-running owned process and requires identity' {
        Assert-DirectLauncherRunning ([Diagnostics.Process]::GetCurrentProcess()) $true
        Refuses { Assert-DirectLauncherRunning $null $true } '启动进程身份'
    }
    Case 'actual Start-Process Hidden child inherits changed PATH name and explicit cwd' {
        $base=Join-Path $taskRoot 'launch-environment'
        # PowerShell 启动后会给 PATH 再加自身目录，故这里使用不会重写 PATH 的原创 Python 夹具。
        $helper=Join-Path $base 'original-env-fixture.py'; $receipt=Join-Path $base 'environment.local.json'
        $helperSource='import os, sys, json' + "`n" +
            'with open(sys.argv[1], "x", encoding="utf-8") as f:' + "`n" +
            '    json.dump({"Path": os.environ.get("PATH"), "Name": os.environ.get("ydwe-process-name"), "Cwd": os.getcwd()}, f)'
        Write-New $helper ([Text.Encoding]::UTF8.GetBytes($helperSource))
        $prefix=Join-Path $base 'bin'; $oldPath=$env:PATH
        $oldName=[Environment]::GetEnvironmentVariable('ydwe-process-name','Process')
        $child=$null
        try {
            $env:PATH=$prefix+[IO.Path]::PathSeparator+$oldPath
            [Environment]::SetEnvironmentVariable('ydwe-process-name','ydwe','Process')
            $child=Start-Process -FilePath (@(Get-Command python -CommandType Application)[0].Source) `
                -ArgumentList @('-B',('"'+$helper+'"'),('"'+$receipt+'"')) `
                -WorkingDirectory $base -WindowStyle Hidden -PassThru
        } finally {
            $env:PATH=$oldPath; [Environment]::SetEnvironmentVariable('ydwe-process-name',$oldName,'Process')
        }
        try {
            Check ($child.WaitForExit(10000) -and $child.ExitCode -eq 0) '原创环境夹具未正常退出'
            $row=[IO.File]::ReadAllText($receipt)|ConvertFrom-Json
            Check ($row.Path.StartsWith($prefix+[IO.Path]::PathSeparator,[StringComparison]::Ordinal) -and
                $row.Name -ceq 'ydwe' -and
                [string]::Equals($row.Cwd,[IO.Path]::GetFullPath($base),[StringComparison]::OrdinalIgnoreCase)) 'Start-Process实际环境/cwd不符'
        } finally { if($child){$child.Dispose()} }
    }
    foreach ($exitSpec in @(
        [pscustomobject]@{Code=37;Hex='0x00000025'},
        [pscustomobject]@{Code=-1073741515;Hex='0xC0000135'})) {
        Case "direct actual owned child exit $($exitSpec.Code) rejects with unsigned hex; default allows launcher exit" {
            $start=[Diagnostics.ProcessStartInfo]::new()
            $start.FileName=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
            $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.WindowStyle='Hidden'
            foreach($arg in @('-NoProfile','-NonInteractive','-Command',('exit '+$exitSpec.Code))) { $start.ArgumentList.Add($arg) }
            $child=[Diagnostics.Process]::Start($start)
            try {
                Check ($child.WaitForExit(10000)) '原创退出夹具未在10秒内完成'
                Check ($child.ExitCode -eq $exitSpec.Code) '实际退出码与原创夹具不符'
                Refuses { Assert-DirectLauncherRunning $child $true } ('actualExitCode='+$exitSpec.Code+' exitHex='+$exitSpec.Hex)
                Assert-DirectLauncherRunning $child $false
            } finally { $child.Dispose() }
        }
    }
    Case 'actual main-loop observation retains zero-frame zero-dialog arrays under StrictMode' {
        foreach ($mode in @($false,$true)) {
            $observation=Get-EditorWindowObservation $taskEditorPid $mode
            Check ($observation.Frames -is [array] -and $observation.Dialogs -is [array] -and
                $observation.Frames.Count -eq 0 -and $observation.Dialogs.Count -eq 0) '零项必须保持真正数组'
            Check (@(Get-EditorWindowObservation $taskEditorPid $mode).Count -eq 1) '返回必须为单一观察对象'
        }
        Refuses { Assert-DirectEditorReady $taskEditorPid ([IntPtr]::Zero) 'missing' } '唯一专用主窗口'
    }
    Case 'actual main-loop observation retains one hidden frame and dialog under StrictMode' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$false,$false)
        $dialog=[YdweFixtureWin]::Create('#32770','owned',$frame,$false,$false)
        try {
            $direct=Get-EditorWindowObservation $taskEditorPid $true
            Check ($direct.Frames -is [array] -and $direct.Frames.Count -eq 1 -and $direct.Frames[0].Handle -eq $frame) '单frame不能塌缩为标量'
            Check ($direct.Dialogs -is [array] -and $direct.Dialogs.Count -eq 1 -and $direct.Dialogs[0].Handle -eq $dialog) '单dialog不能塌缩为标量'
            $default=Get-EditorWindowObservation $taskEditorPid $false
            Check ($default.Frames.Count -eq 0 -and $default.Dialogs.Count -eq 0) '默认路径保持visible-only'
        } finally { [YdweFixtureWin]::Remove($dialog); [YdweFixtureWin]::Remove($frame) }
    }
    Case 'actual main-loop observation preserves multiple frames and refuses readiness under StrictMode' {
        $one=[YdweFixtureWin]::Create('Warcraft III','one',[IntPtr]::Zero,$false,$false)
        $two=[YdweFixtureWin]::Create('Warcraft III','two',[IntPtr]::Zero,$false,$false)
        try {
            $observation=Get-EditorWindowObservation $taskEditorPid $true
            Check ($observation.Frames -is [array] -and $observation.Frames.Count -eq 2) '多个frame必须完整保留'
            Refuses { Assert-DirectEditorReady $taskEditorPid $one 'one' } '唯一专用主窗口'
        } finally { [YdweFixtureWin]::Remove($two); [YdweFixtureWin]::Remove($one) }
    }
    Case 'load-timeout diagnosis retains absent editor identity under StrictMode' {
        $message=Get-EditorLoadTimeoutMessage $null $false
        Check ($message -match '地图未在期限内载入.*pid=not-observed') '缺editor不能覆盖原超时原因'
        $message=Get-EditorLoadTimeoutMessage ([Diagnostics.Process]::GetCurrentProcess()) $true
        Check ($message -match ('pid='+$taskEditorPid) -and $message -match 'versionWarningAccepted=True') '实际观察PID/版本状态不符'
    }
    Case 'native hidden frame found only by direct overload and ready state' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','YDWE [fixture] - [C:\owned\candidate.w3x]',[IntPtr]::Zero,$false,$false)
        try {
            Check (@([YdweWin]::GetEditorFrames($taskEditorPid)).Count -eq 0) '默认查询不能纳入隐藏 frame'
            $frames=@([YdweWin]::GetEditorFrames($taskEditorPid,$true)); Check ($frames.Count -eq 1) 'direct 唯一 frame'
            Check (-not $frames[0].IsVisible -and -not $frames[0].OwnVisibleStyle) 'native hidden style 不符'
            Assert-DirectEditorReady $taskEditorPid $frame $frames[0].Title
            Refuses { Assert-DirectEditorReady $taskEditorPid $frame 'different map title' } '主窗口'
            [YdweFixtureWin]::Enabled($frame,$false); Refuses { Assert-DirectEditorReady $taskEditorPid $frame $frames[0].Title } '可用状态'
        } finally { [YdweFixtureWin]::Remove($frame) }
    }
    Case 'native multiple frames wrong PID class and owner refuse selection' {
        $one=[YdweFixtureWin]::Create('Warcraft III','one',[IntPtr]::Zero,$false,$false)
        $two=[YdweFixtureWin]::Create('Warcraft III','two',[IntPtr]::Zero,$false,$false)
        $owned=[YdweFixtureWin]::Create('Warcraft III','owned',$one,$false,$false)
        $other=[YdweFixtureWin]::Create('FORGE-P1-OtherFrame','other',[IntPtr]::Zero,$false,$false)
        try {
            Check (@([YdweWin]::GetEditorFrames($taskEditorPid,$true)).Count -eq 2) 'owner/class 过滤失败'
            Check (@([YdweWin]::GetEditorFrames(($taskEditorPid+100000),$true)).Count -eq 0) '异 PID 过滤失败'
            Refuses { Assert-DirectEditorReady $taskEditorPid $one 'one' } '唯一'
            Check (-not [YdweWin]::IsEditorFrame($owned,$taskEditorPid)) '有 owner 的 frame 不允许'
        } finally { foreach ($handle in @($owned,$other,$two,$one)) { [YdweFixtureWin]::Remove($handle) } }
    }
    Case 'native default visible frame remains included' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','visible',[IntPtr]::Zero,$true,$false)
        try { Check (@([YdweWin]::GetEditorFrames($taskEditorPid)).Count -eq 1) '原可见路径应保持' }
        finally { [YdweFixtureWin]::Remove($frame) }
    }
    Case 'native hidden owned modal and Yes6 control require disabled owner' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','owned',[IntPtr]::Zero,$false,$false)
        $dialog=[YdweFixtureWin]::Create('#32770','警告',$frame,$false,$false)
        $text=[YdweFixtureWin]::Child($dialog,'Static',$versionText,10,$true)
        $button=[YdweFixtureWin]::Child($dialog,'Button','是(&Y)',6,$true)
        try {
            $d=NativeDialog $dialog; Check ($d.YesButtonEnabled -and -not $d.IsVisible) '隐藏祖先不应抹去按钮自身可见/可用证据'
            Check (-not (Test-NativeVersionWarning $d $taskEditorPid $versionText $true $frame)) 'enabled owner 不能批准 hidden warning'
            [YdweFixtureWin]::Enabled($frame,$false); $d=NativeDialog $dialog
            Check ((Get-DirectDialogState $d $frame $taskEditorPid) -ceq 'ActiveModal') 'disabled owner modal 应识别'
            Check (Test-NativeVersionWarning $d $taskEditorPid $versionText $true $frame) '原精确 warning 应接受'
            [YdweFixtureWin]::Enabled($button,$false); $d=NativeDialog $dialog
            Check (-not (Test-NativeVersionWarning $d $taskEditorPid $versionText $true $frame)) 'disabled Yes6 不能接受'
        } finally { [YdweFixtureWin]::Remove($dialog); [YdweFixtureWin]::Remove($frame) }
    }
    Case 'native hidden ownerless warning is unproven and never auto accepted' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$false,$false)
        $dialog=[YdweFixtureWin]::Create('#32770','警告',[IntPtr]::Zero,$false,$false)
        [void][YdweFixtureWin]::Child($dialog,'Static',$versionText,10,$true)
        [void][YdweFixtureWin]::Child($dialog,'Button','是(&Y)',6,$true)
        try {
            $d=NativeDialog $dialog; Check ((Get-DirectDialogState $d $frame $taskEditorPid) -ceq 'UnprovenHidden') 'ownerless hidden 必须报告证据不足'
            Check (-not (Test-NativeVersionWarning $d $taskEditorPid $versionText $true $frame)) 'ownerless hidden warning 不能点击'
            Check (-not [YdweWin]::IsOwnedDisabledModal($dialog,$frame,($taskEditorPid+100000))) '异 PID 不应通过'
            Refuses { Assert-DirectEditorReady $taskEditorPid $frame 'main' } '对话框'
        } finally { [YdweFixtureWin]::Remove($dialog); [YdweFixtureWin]::Remove($frame) }
    }
    Case 'native warning owner from another main is never approved' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$false,$false)
        $other=[YdweFixtureWin]::Create('Warcraft III','different main',[IntPtr]::Zero,$false,$false)
        $dialog=[YdweFixtureWin]::Create('#32770','警告',$other,$false,$false)
        [void][YdweFixtureWin]::Child($dialog,'Static',$versionText,10,$true)
        [void][YdweFixtureWin]::Child($dialog,'Button','是(&Y)',6,$true)
        try {
            [YdweFixtureWin]::Enabled($frame,$false); [YdweFixtureWin]::Enabled($other,$false)
            Check (-not (Test-NativeVersionWarning (NativeDialog $dialog) $taskEditorPid $versionText $true $frame)) '不同main owner链不得批准'
        } finally { foreach ($handle in @($dialog,$other,$frame)) { [YdweFixtureWin]::Remove($handle) } }
    }
    Case 'native default visible exact warning retains original match and refuses wrong text' {
        $dialog=[YdweFixtureWin]::Create('#32770','警告',[IntPtr]::Zero,$true,$false)
        [void][YdweFixtureWin]::Child($dialog,'Static',$versionText,10,$true)
        [void][YdweFixtureWin]::Child($dialog,'Button','是(&Y)',6,$true)
        try {
            $d=NativeDialog $dialog
            Check (Test-NativeVersionWarning $d $taskEditorPid $versionText $false ([IntPtr]::Zero)) '原可见警告匹配必须保留'
            Check (-not (Test-NativeVersionWarning $d $taskEditorPid ($versionText+'不同') $false ([IntPtr]::Zero))) '文本变体不能批准'
        } finally { [YdweFixtureWin]::Remove($dialog) }
    }
    Case 'native wrong Yes ID and hidden button reject exact-body warning' {
        foreach ($spec in @(@{Id=7;Visible=$true},@{Id=6;Visible=$false})) {
            $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$false,$false)
            $dialog=[YdweFixtureWin]::Create('#32770','警告',$frame,$false,$false)
            [void][YdweFixtureWin]::Child($dialog,'Static',$versionText,10,$true)
            [void][YdweFixtureWin]::Child($dialog,'Button','是(&Y)',$spec.Id,$spec.Visible)
            try { [YdweFixtureWin]::Enabled($frame,$false); Check (-not (Test-NativeVersionWarning (NativeDialog $dialog) $taskEditorPid $versionText $true $frame)) '错误按钮不得批准' }
            finally { [YdweFixtureWin]::Remove($dialog); [YdweFixtureWin]::Remove($frame) }
        }
    }
    Case 'native tool modal shape remains inactive' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$false,$false)
        $dialog=[YdweFixtureWin]::Create('#32770','tool',$frame,$false,$true)
        try { [YdweFixtureWin]::Enabled($frame,$false); Check ((Get-DirectDialogState (NativeDialog $dialog) $frame $taskEditorPid) -ceq 'Inactive') 'tool window 不能当 modal' }
        finally { [YdweFixtureWin]::Remove($dialog); [YdweFixtureWin]::Remove($frame) }
    }
    Case 'native hidden inactive progress does not block enabled frame forever' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$false,$false)
        $dialog=[YdweFixtureWin]::Create('#32770','进程',[IntPtr]::Zero,$false,$false)
        [void][YdweFixtureWin]::Child($dialog,'msctls_progress32','',1,$true)
        try {
            $d=NativeDialog $dialog; Check ((Get-DirectDialogState $d $frame $taskEditorPid) -ceq 'Inactive') 'hidden inactive progress 不应永久 active'
            [YdweFixtureWin]::Enabled($frame,$false); Check ((Get-DirectDialogState (NativeDialog $dialog) $frame $taskEditorPid) -ceq 'ActiveProgress') 'disabled main + progress shape 应继续等候'
            [YdweFixtureWin]::Enabled($frame,$true); Check ((Get-DirectDialogState (NativeDialog $dialog) $frame $taskEditorPid) -ceq 'Inactive') 'main恢复可用后隐藏进度壳解除'
        } finally { [YdweFixtureWin]::Remove($dialog); [YdweFixtureWin]::Remove($frame) }
    }
    Case 'native visible progress retains active progress shape' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$false,$false)
        $dialog=[YdweFixtureWin]::Create('#32770','进程',[IntPtr]::Zero,$true,$false)
        [void][YdweFixtureWin]::Child($dialog,'msctls_progress32','',1,$true)
        try {
            Check ((Get-DirectDialogState (NativeDialog $dialog) $frame $taskEditorPid) -ceq 'ActiveProgress') '原可见进度窗口必须继续等候'
            Refuses { Assert-DirectEditorReady $taskEditorPid $frame 'main' } '对话框'
        }
        finally { [YdweFixtureWin]::Remove($dialog); [YdweFixtureWin]::Remove($frame) }
    }
    Case 'native owner chain reaches only disabled same-process dedicated main' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$false,$false)
        $middle=[YdweFixtureWin]::Create('#32770','middle',$frame,$false,$false)
        $dialog=[YdweFixtureWin]::Create('#32770','nested',$middle,$false,$false)
        $other=[YdweFixtureWin]::Create('Warcraft III','other',[IntPtr]::Zero,$false,$false)
        try {
            [YdweFixtureWin]::Enabled($frame,$false); [YdweFixtureWin]::Enabled($middle,$false)
            Check ([YdweWin]::IsOwnedDisabledModal($dialog,$frame,$taskEditorPid)) '同PID链应到专用main'
            Check (-not [YdweWin]::IsOwnedDisabledModal($dialog,$other,$taskEditorPid)) '异main不能接受'
        } finally { foreach ($handle in @($dialog,$middle,$other,$frame)) { [YdweFixtureWin]::Remove($handle) } }
    }
} finally {
    [YdweFixtureWin]::Cleanup()
}
Case 'fixtures did not take foreground focus' { Check ([YdweFixtureWin]::GetForegroundWindow() -eq $foregroundBefore) '前台窗口发生变化' }
$summary = [pscustomobject]@{Schema='ydwe-save-direct-tests-v1';Passed=$script:passed;Failed=$script:failed.Count;
    Failures=$script:failed.ToArray();StartedUtc=$started.ToString('o');CompletedUtc=[DateTime]::UtcNow.ToString('o');
    ActualEditorOrGameLaunched=$false;AuthorWindowsOperated=$false;AuthorRegistryWritten=$false;SourceSha256=(Hash $source)}
[IO.File]::WriteAllText((Join-Path $taskRoot 'summary.local.json'),($summary|ConvertTo-Json -Depth 8))
$summary | ConvertTo-Json -Depth 8 -Compress
if ($autoCleanup) {
    $resolved = Assert-PlainNativePath $taskRoot
    if ($resolved -cne $taskRoot -or [IO.File]::ReadAllText($taskOwner) -cne $taskToken -or
        [IO.Path]::GetFileName($resolved) -cne ('forge-p1-direct-tests-' + $taskToken)) { throw '临时测试清理归属不符' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
if ($script:failed.Count -ne 0) { exit 1 }
