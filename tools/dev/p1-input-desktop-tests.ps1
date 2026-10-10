#requires -Version 7.2
# 原创native08监管回归；只启动本次Python静态窗口夹具，不运行WE/游戏，不写注册表。
param([string]$Python = 'python', [string]$WorkDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'p1-input-desktop.ps1')
$testWitness=[P1InputDesktopOwner]::Witness()
try { $testWitness.RequireActiveDefault() }
catch { "SKIP native08 original fixtures unavailable: $($_.Exception.Message); session=$($testWitness.SessionId) state=$($testWitness.ConnectionStateName) station=$($testWitness.WindowStation) thread=$($testWitness.ThreadDesktop) input=$($testWitness.InputDesktop)"; exit 77 }
if (-not [IO.Path]::IsPathFullyQualified($Python)) { $Python=(Get-Command $Python -ErrorAction Stop).Source }
$Python=[IO.Path]::GetFullPath($Python)
$automaticCleanup=-not $WorkDirectory
if (-not $WorkDirectory) { $WorkDirectory=Join-Path ([IO.Path]::GetTempPath()) ('forge-native08-fixture-'+[Guid]::NewGuid().ToString('N')) }
$root=[IO.Path]::GetFullPath($WorkDirectory)
if (Test-Path -LiteralPath $root) { throw '原创回归输出目录必须不存在' }
[void][IO.Directory]::CreateDirectory($root)
$script:passed=0; $script:failed=[Collections.Generic.List[object]]::new()
function Check([bool]$Value,[string]$Message) { if (-not $Value) { throw $Message } }
function Case([string]$Name,[scriptblock]$Action) {
    try { & $Action; $script:passed++; "PASS $Name" }
    catch { $script:failed.Add(@{name=$Name;error=$_.Exception.Message}); "FAIL $Name : $($_.Exception.Message)" }
}
function Refuses([scriptblock]$Action,[string]$Pattern) {
    $refused=$false
    try { & $Action | Out-Null }
    catch { $refused=$true; if ($_.Exception.Message -notmatch $Pattern) { throw "拒绝原因不符: $($_.Exception.Message)" } }
    Check $refused '应拒绝但通过'
}
$fixture=Join-Path $root 'original-fixture.py'
$fixtureSource=@'
import ctypes as C
from ctypes import wintypes as W
import json, os, pathlib, sys, time
k=C.WinDLL('kernel32', use_last_error=True)
u=C.WinDLL('user32', use_last_error=True)
u.CreateWindowExW.argtypes=[W.DWORD,W.LPCWSTR,W.LPCWSTR,W.DWORD,C.c_int,C.c_int,C.c_int,C.c_int,W.HWND,W.HMENU,W.HINSTANCE,C.c_void_p]
u.CreateWindowExW.restype=W.HWND
u.DestroyWindow.argtypes=[W.HWND]
k.GetCurrentThreadId.restype=W.DWORD
u.GetThreadDesktop.argtypes=[W.DWORD]
u.GetThreadDesktop.restype=C.c_void_p
u.GetUserObjectInformationW.argtypes=[C.c_void_p,C.c_int,C.c_void_p,W.DWORD,C.POINTER(W.DWORD)]
u.GetUserObjectInformationW.restype=W.BOOL
hwnd=u.CreateWindowExW(0x08000000,'STATIC','original native08 fixture',0x80000000,-30000,-30000,40,40,None,None,None,None)
if not hwnd: raise C.WinError(C.get_last_error())
tid=k.GetCurrentThreadId()
name=C.create_unicode_buffer(256);needed=W.DWORD()
if not u.GetUserObjectInformationW(u.GetThreadDesktop(tid),2,name,C.sizeof(name),C.byref(needed)):
    raise C.WinError(C.get_last_error())
with open(sys.argv[1],'x',encoding='utf-8') as f:
    json.dump({'pid':os.getpid(),'tid':tid,'hwnd':int(hwnd),'threadDesktop':name.value,'environmentKeys':sorted(os.environ)},f)
try:
    deadline=time.monotonic()+90
    while not pathlib.Path(sys.argv[2]).exists() and time.monotonic()<deadline:
        time.sleep(0.1)
finally:
    u.DestroyWindow(hwnd)
'@
[IO.File]::WriteAllText($fixture,$fixtureSource,[Text.UTF8Encoding]::new($false))
$owned=$null; $other=$null
$release=Join-Path $root 'release.local.txt'; $otherRelease=Join-Path $root 'other-release.local.txt'
try {
    Case 'real parent Active WinSta0 Default witness' {
        $w=[P1InputDesktopOwner]::Witness(); $w.RequireActiveDefault()
        Check ($w.ConnectionStateName -ceq 'WTSActive') 'WTS连接状态名称不符'
        Write-P1InputDesktopNewJson (Join-Path $root 'parent-witness.local.json') $w
    }
    foreach($field in @('WindowStation','ThreadDesktop','InputDesktop','ConnectionState','SessionId','InteractiveWindowStation')) {
        $fieldCopy=$field
        Case ('synthetic non-active witness refuses '+$fieldCopy) {
            $w=[P1InputDesktopWitness]::new(); $w.WindowStation='WinSta0'; $w.ThreadDesktop='Default'
            $w.InputDesktop='Default'; $w.ConnectionState=0; $w.SessionId=1; $w.InteractiveWindowStation=$true
            if ($fieldCopy -in @('ConnectionState','SessionId')) { $w.$fieldCopy=if($fieldCopy -eq 'SessionId'){0}else{4} }
            elseif($fieldCopy -eq 'InteractiveWindowStation'){$w.$fieldCopy=$false}else{$w.$fieldCopy='Other'}
            Refuses { $w.RequireActiveDefault() } 'Active'
        }
    }
    Case 'nonexistent launch refuses before creating process' {
        Refuses { [P1InputDesktopOwner]::Start((Join-Path $root 'missing.exe'),'missing',$root) } '路径'
    }
    Case 'arbitrary environment key refuses before process creation' {
        $envFixture=[P1InputDesktopOwner]::SelectedWindowsEnvironment(); $envFixture.Add('P1_NATIVE08_PRIVATE','fixture value')
        Refuses { [P1InputDesktopOwner]::Start($Python,'fixture',$root,$envFixture) } '环境'
    }
    Case 'WE Default launch refuses even with existing own fixture executable path' {
        $blocked=Join-Path $root 'worldeditydwe.exe'; [IO.File]::WriteAllText($blocked,'original non-executable fixture')
        Refuses { [P1InputDesktopOwner]::Start($blocked,'fixture',$root) } 'Default启动已阻止'
    }
    $childReceipt=Join-Path $root 'child.local.json'
    $command='"'+$Python+'" "'+$fixture+'" "'+$childReceipt+'" "'+$release+'"'
    $owned=[P1InputDesktopOwner]::Start($Python,$command,$root)
    if ($owned.StartupFailure) { throw $owned.StartupFailure }
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    while (-not [IO.File]::Exists($childReceipt) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    $child=Get-Content -LiteralPath $childReceipt -Raw | ConvertFrom-Json
    Case 'CreateProcess original process and thread handles retained' {
        Check ($owned.Identity.RetainedOriginalProcessHandle -and $owned.Identity.RetainedOriginalThreadHandle) '原句柄记录缺失'
        Check ($owned.Id -eq $child.pid -and $owned.PrimaryThreadId -eq $child.tid) '原创子进程PID/原主线程不符'
        Check ($owned.Identity.Image -ceq $Python -and $owned.Identity.CreationFileTime -gt 0) '原句柄image/时间缺失'
    }
    Case 'real child actual GUI Desktop and parent session verified' {
        $owned.AssertWindow([IntPtr][long]$child.hwnd,$true)
        Check ($child.threadDesktop -ceq 'Default' -and $owned.Identity.PrimaryThreadDesktop -ceq 'Default') '真实GUI Desktop未成立'
        Check ($owned.GuiThreadId -eq $child.tid -and $owned.Identity.SessionId -eq $owned.ParentBefore.SessionId) 'GUI线程/活动会话绑定缺失'
    }
    Case 'unpackaged descendants policy no inherit no kill-on-close' {
        Check ($owned.Identity.UnpackagedProcessTreePolicy -and -not $owned.Identity.InheritHandles -and -not $owned.Identity.KillOnJobClose) 'policy/句柄/Job规则不符'
        Check ($child.environmentKeys.Count -eq 12 -and $child.environmentKeys -notcontains 'P1_NATIVE08_PRIVATE') '子进程必须只接收选择的12个Windows环境键'
    }
    Case 'active original handle cannot become terminal or Dispose' {
        $terminal=$owned.Terminal()
        Check (-not $terminal.OriginalProcessSignaled -and -not $terminal.StableTerminalState -and $null -eq $terminal.ExitCode) '活动OWN被伪签终态'
        Refuses { $owned.Dispose() } '监管session'
        $owned.AssertRunningDefault()
    }
    Case 'zero or stale window refuses' {
        Refuses { $owned.AssertWindow([IntPtr]::Zero,$false) } 'HWND'
    }
    $otherReceipt=Join-Path $root 'other-child.local.json'
    $otherCommand='"'+$Python+'" "'+$fixture+'" "'+$otherReceipt+'" "'+$otherRelease+'"'
    $other=[P1InputDesktopOwner]::Start($Python,$otherCommand,$root)
    if ($other.StartupFailure) { throw $other.StartupFailure }
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    while (-not [IO.File]::Exists($otherReceipt) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    $otherChild=Get-Content -LiteralPath $otherReceipt -Raw | ConvertFrom-Json
    Case 'different owned fixture GUI cannot receive this owners command' {
        Refuses { $owned.AssertWindow([IntPtr][long]$otherChild.hwnd,$false) } 'HWND'
        $other.AssertWindow([IntPtr][long]$otherChild.hwnd,$true)
    }
    Case 'new receipt refuses overwrite' {
        $path=Join-Path $root 'new-only.local.json'
        Write-P1InputDesktopNewJson $path @{fixture=$true}
        Refuses { Write-P1InputDesktopNewJson $path @{fixture=$false} } 'exist|存在'
    }
    [IO.File]::WriteAllText($release,'fixture natural exit'); [IO.File]::WriteAllText($otherRelease,'fixture natural exit')
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    while (-not $owned.Terminal().EnvironmentQuiescent -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    while (-not $other.Terminal().EnvironmentQuiescent -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    Case 'natural fixture exit signs same original process thread handles' {
        $terminal=$owned.Terminal(); Check ($terminal.StableTerminalState -and $terminal.ExitCode -eq 0) '原句柄终态/退出码缺失'
        Check ($terminal.OriginalThreadSignaled -and $terminal.JobEmpty) '原线程/OWN Job终态缺失'
        Write-P1InputDesktopNewJson (Join-Path $root 'original-terminal.local.json') $terminal
    }
    Case 'terminated original identity refuses later window commands' {
        Refuses { $owned.AssertWindow([IntPtr][long]$child.hwnd,$true) } '退出'
    }
    Case 'Dispose only after proven quiescence' { $owned.Dispose(); $owned.Dispose(); $owned=$null; $other.Dispose(); $other=$null }
    Case 'environment receipt emits stable digest only' {
        $sha=Get-P1InputEnvironmentFingerprint; Check ($sha -match '^[0-9a-f]{64}$' -and $sha -ceq (Get-P1InputEnvironmentFingerprint)) 'environment digest不稳'
    }
} finally {
    foreach($marker in @($release,$otherRelease)) { if (-not [IO.File]::Exists($marker)) { [IO.File]::WriteAllText($marker,'fixture cleanup natural exit') } }
    foreach($item in @($owned,$other)) {
        if ($item -and -not $item.IsDisposed) {
            $deadline=[DateTime]::UtcNow.AddSeconds(15)
            while (-not $item.Terminal().EnvironmentQuiescent -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
            if ($item.Terminal().EnvironmentQuiescent) { $item.Dispose() }
        }
    }
    Write-P1InputDesktopNewJson (Join-Path $root 'summary.local.json') @{
        schema='p1-input-desktop-original-fixture-tests-v1'; passed=$script:passed; failed=@($script:failed);
        worldEditorStarted=$false; gameStarted=$false; registryWritten=$false; nativeSaveValidated=$false; runtimeValidated=$false
    }
    if ($automaticCleanup -and $script:failed.Count -eq 0) {
        # 只删除本次随机原创夹具目录；验证绝对目标位于TEMP下且未替换为reparse。
        $temporaryRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
        if (-not $root.StartsWith($temporaryRoot,[StringComparison]::OrdinalIgnoreCase) -or
            ([IO.File]::GetAttributes($root) -band [IO.FileAttributes]::ReparsePoint)) { throw '回归清理目标越界' }
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
"native08 original fixtures: passed=$script:passed failed=$($script:failed.Count)"
if ($script:failed.Count) { exit 1 }
