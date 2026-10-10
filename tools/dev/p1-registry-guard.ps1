#requires -Version 7.2
# P1 独立宿主注册表保护。点加载后显式调用 API；直接执行默认 Library 不做操作。
[CmdletBinding()]
param(
    [ValidateSet('Library', 'Snapshot')][string]$Mode = 'Library',
    [string]$EvidenceDirectory,
    [string[]]$RootPaths = @('Software\Blizzard Entertainment\WorldEdit', 'Software\Blizzard Entertainment\Warcraft III'),
    [int]$MaxBytes = 16777216
)

Set-StrictMode -Version Latest

if (-not ('P1RegistryRawV1' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32;

public sealed class P1RegistryRawValueV1 {
    public string Name;
    public uint Type;
    public string RawBase64;
    public int ByteCount;
}
public static class P1RegistryRawV1 {
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)]
    static extern int RegQueryValueExW(IntPtr key, string name, IntPtr reserved, out uint type, byte[] data, ref uint size);
    [StructLayout(LayoutKind.Sequential)]
    struct UnicodeString { public ushort Length; public ushort MaximumLength; public IntPtr Buffer; }
    [DllImport("ntdll.dll")]
    static extern int NtSetValueKey(IntPtr key, ref UnicodeString name, uint titleIndex, uint type, byte[] data, uint size);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)]
    static extern int RegDeleteValueW(IntPtr key, string name);
    [DllImport("ntdll.dll")]
    static extern int NtQueryKey(IntPtr key, int informationClass, IntPtr information, uint length, out uint resultLength);

    public static string PhysicalName(RegistryKey key) {
        uint size;
        NtQueryKey(key.Handle.DangerousGetHandle(), 3, IntPtr.Zero, 0, out size);
        if (size < 4 || size > 65536) throw new InvalidOperationException("Native key name exceeds bound");
        IntPtr buffer = Marshal.AllocHGlobal((int)size);
        try {
            int status = NtQueryKey(key.Handle.DangerousGetHandle(), 3, buffer, size, out size);
            if (status != 0) throw new InvalidOperationException("NtQueryKey failed: " + status);
            int bytes = Marshal.ReadInt32(buffer);
            if (bytes < 0 || (bytes & 1) != 0 || bytes > size - 4) throw new InvalidOperationException("Invalid native key name");
            return Marshal.PtrToStringUni(IntPtr.Add(buffer, 4), bytes / 2);
        } finally { Marshal.FreeHGlobal(buffer); }
    }
    public static P1RegistryRawValueV1 Read(RegistryKey key, string name, int maximumBytes) {
        uint type, size = 0;
        int result = RegQueryValueExW(key.Handle.DangerousGetHandle(), name, IntPtr.Zero, out type, null, ref size);
        if (result == 2) return null;
        if (result != 0) throw new Win32Exception(result);
        if (size > maximumBytes) throw new InvalidOperationException("Registry raw bytes exceed bound");
        byte[] data = new byte[size];
        uint actual = size;
        result = RegQueryValueExW(key.Handle.DangerousGetHandle(), name, IntPtr.Zero, out type, data, ref actual);
        if (result != 0 || actual != size) throw new InvalidOperationException("Value changed during raw read: " + result);
        return new P1RegistryRawValueV1 { Name=name, Type=type, RawBase64=Convert.ToBase64String(data), ByteCount=data.Length };
    }
    public static void Set(RegistryKey key, string name, uint type, string rawBase64) {
        byte[] data = Convert.FromBase64String(rawBase64);
        int length = System.Text.Encoding.Unicode.GetByteCount(name);
        if (length > 65532) throw new InvalidOperationException("Native value name exceeds bound");
        var nativeName = new UnicodeString { Length=(ushort)length, MaximumLength=(ushort)(length+2), Buffer=Marshal.StringToHGlobalUni(name) };
        try {
            // 不让 Win32 字符串转换/补终止字节改变已记录的原始 payload。
            int status = NtSetValueKey(key.Handle.DangerousGetHandle(), ref nativeName, 0, type, data, (uint)data.Length);
            if (status != 0) throw new InvalidOperationException("NtSetValueKey failed: " + status.ToString("X8"));
        } finally { Marshal.FreeHGlobal(nativeName.Buffer); }
    }
    public static void Delete(RegistryKey key, string name) {
        int result = RegDeleteValueW(key.Handle.DangerousGetHandle(), name);
        if (result != 0) throw new Win32Exception(result);
    }
}
'@ | Out-Null
}

function Assert-P1PlainFilePath([string]$Path) {
    if (-not [IO.Path]::IsPathFullyQualified($Path) -or $Path.StartsWith('\\') -or $Path.Contains([char]0)) { throw 'Evidence 必须是本机绝对普通路径' }
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Substring([IO.Path]::GetPathRoot($full).Length).Contains(':')) { throw '拒绝 ADS' }
    $node = $full
    while ($node) {
        if ([IO.File]::Exists($node) -or [IO.Directory]::Exists($node)) {
            if (([IO.File]::GetAttributes($node) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "拒绝 reparse: $node" }
        }
        $parent = [IO.Directory]::GetParent($node)
        $node = if ($parent) { $parent.FullName } else { $null }
    }
    return $full
}

function Write-P1NewJson([string]$Path, $Value) {
    $target = Assert-P1PlainFilePath $Path
    $text = $Value | ConvertTo-Json -Depth 50
    $stream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($text + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
}

function Test-P1NoEditorProcesses([int[]]$OwnedProcessIds = @(), [switch]$RequireOwnedEnded) {
    $active = @(Get-Process | Where-Object { $_.ProcessName -match '^(worldedit|worldeditydwe|YDWE|war3|Warcraft III)$' })
    $foreign = @($active | Where-Object { $_.Id -notin $OwnedProcessIds })
    if ($foreign.Count -gt 0) { throw "非本次 WE/YDWE/game 正在运行，拒绝: $($foreign.Id -join ',')" }
    if ($RequireOwnedEnded -and $active.Count -gt 0) { throw '本次 WE 尚未结束，拒绝 after/restore' }
}

function Assert-P1RegistryRoots([string[]]$Paths) {
    if ($Paths.Count -lt 1 -or $Paths.Count -gt 8) { throw 'RootPaths 数量越界' }
    foreach ($path in $Paths) {
        if ($path -notmatch '^Software\\[^\\]' -or $path.Contains([char]0) -or $path.EndsWith('\') -or $path.Contains('\\') -or @($path.Split('\') | Where-Object { $_ -in @('.', '..') }).Count) { throw "拒绝不明确 HKCU 子树: $path" }
    }
    for ($left = 0; $left -lt $Paths.Count; $left++) {
        for ($right = $left + 1; $right -lt $Paths.Count; $right++) {
            if ($Paths[$left].Equals($Paths[$right], [StringComparison]::OrdinalIgnoreCase) -or $Paths[$left].StartsWith($Paths[$right] + '\', [StringComparison]::OrdinalIgnoreCase) -or $Paths[$right].StartsWith($Paths[$left] + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'RootPaths 不能重复或互为祖先' }
        }
    }
}

function Get-P1SnapshotFingerprint($Snapshot) {
    $canonical = [ordered]@{ scopes = @($Snapshot.Scopes); keys = @($Snapshot.Keys.Values | Sort-Object PhysicalName | ForEach-Object {
        [ordered]@{ physical = $_.PhysicalName; aliases = @($_.Aliases); values = @($_.Values.Values | Sort-Object Name) }
    }) }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($canonical | ConvertTo-Json -Depth 50 -Compress))
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
}

function Get-P1RegistrySnapshot {
    [CmdletBinding()]
    param([string[]]$RootPaths = @('Software\Blizzard Entertainment\WorldEdit', 'Software\Blizzard Entertainment\Warcraft III'),
          [int]$MaxBytes = 16777216, [int]$MaxKeys = 16384, [int]$MaxValues = 100000, [int]$MaxDepth = 256)
    Assert-P1RegistryRoots $RootPaths
    if ($MaxBytes -lt 1 -or $MaxBytes -gt 67108864 -or $MaxKeys -lt 1 -or $MaxValues -lt 1 -or $MaxDepth -lt 1) { throw '快照预算无效' }
    $snapshot = [pscustomobject]@{ Scopes = [Collections.Generic.List[object]]::new(); Keys = @{}; RawBytes = 0; ValueCount = 0; MaxBytes = $MaxBytes; MaxKeys = $MaxKeys; MaxValues = $MaxValues; MaxDepth = $MaxDepth; Fingerprint = $null }
    foreach ($view in @('Registry32', 'Registry64')) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::$view)
        try {
            foreach ($root in $RootPaths) {
                $rootKey = $base.OpenSubKey($root, $false)
                if (-not $rootKey) { $snapshot.Scopes.Add([pscustomobject]@{ Root=$root; View=$view; Exists=$false; PhysicalName=$null }); continue }
                $rootPhysical = [P1RegistryRawV1]::PhysicalName($rootKey)
                $rootKey.Dispose()
                $snapshot.Scopes.Add([pscustomobject]@{ Root=$root; View=$view; Exists=$true; PhysicalName=$rootPhysical })
                $pending = [Collections.Generic.Stack[object]]::new()
                $pending.Push([pscustomobject]@{ Path=$root; Relative=''; Depth=0 })
                while ($pending.Count) {
                    $node = $pending.Pop()
                    if ($node.Depth -gt $MaxDepth) { throw 'registry 深度超过预算' }
                    $key = $base.OpenSubKey($node.Path, $false)
                    if (-not $key) { throw '快照期间 registry 键消失' }
                    try {
                        $physical = [P1RegistryRawV1]::PhysicalName($key)
                        if (-not ($physical.Equals($rootPhysical, [StringComparison]::OrdinalIgnoreCase) -or $physical.StartsWith($rootPhysical + '\', [StringComparison]::OrdinalIgnoreCase))) { throw 'registry link 逃逸指定子树' }
                        $known = $snapshot.Keys.ContainsKey($physical)
                        $values = @{}
                        foreach ($name in @($key.GetValueNames() | Sort-Object)) {
                            $bound = if ($known) { $MaxBytes } else { $MaxBytes - $snapshot.RawBytes }
                            $raw = [P1RegistryRawV1]::Read($key, $name, $bound)
                            if (-not $raw) { throw '快照期间 registry 值消失' }
                            $values[$name] = [pscustomobject]@{ Name=$raw.Name; Type=$raw.Type; RawBase64=$raw.RawBase64; ByteCount=$raw.ByteCount }
                            if (-not $known) {
                                $snapshot.RawBytes += $raw.ByteCount + [Text.Encoding]::Unicode.GetByteCount($name)
                                $snapshot.ValueCount++
                                if ($snapshot.RawBytes -gt $MaxBytes -or $snapshot.ValueCount -gt $MaxValues) { throw 'registry value/bytes 超过预算' }
                            }
                        }
                        $alias = [pscustomobject]@{ Root=$root; View=$view; Path=$node.Path; Relative=$node.Relative }
                        if ($known) {
                            $existing = $snapshot.Keys[$physical]
                            if ($existing.Values.Count -ne $values.Count) { throw '同物理键的两视图读取不一致' }
                            foreach ($name in $values.Keys) { if (-not $existing.Values.ContainsKey($name) -or -not (Test-P1RawValueEqual $existing.Values[$name] $values[$name])) { throw '同物理键的两视图读取发生变化' } }
                            $existing.Aliases.Add($alias)
                        } else {
                            $snapshot.RawBytes += [Text.Encoding]::Unicode.GetByteCount($physical)
                            if ($snapshot.Keys.Count -ge $MaxKeys -or $snapshot.RawBytes -gt $MaxBytes) { throw 'registry key/bytes 超过预算' }
                            $aliases = [Collections.Generic.List[object]]::new(); $aliases.Add($alias)
                            $snapshot.Keys[$physical] = [pscustomobject]@{ PhysicalName=$physical; Aliases=$aliases; Values=$values }
                        }
                        foreach ($child in @($key.GetSubKeyNames() | Sort-Object -Descending)) {
                            $relative = if ($node.Relative) { $node.Relative + '\' + $child } else { $child }
                            $pending.Push([pscustomobject]@{ Path=$node.Path + '\' + $child; Relative=$relative; Depth=$node.Depth + 1 })
                        }
                    } finally { $key.Dispose() }
                }
            }
        } finally { $base.Dispose() }
    }
    $snapshot.Fingerprint = Get-P1SnapshotFingerprint $snapshot
    return $snapshot
}

function Test-P1RawValueEqual($Left, $Right) {
    if ($null -eq $Left -or $null -eq $Right) { return ($null -eq $Left -and $null -eq $Right) }
    return $Left.Type -eq $Right.Type -and $Left.RawBase64 -ceq $Right.RawBase64
}

function Get-P1RegistryDelta($Before, $After) {
    $creates = [Collections.Generic.List[object]]::new(); $values = [Collections.Generic.List[object]]::new(); $deletes = [Collections.Generic.List[object]]::new()
    foreach ($physical in @(@($Before.Keys.Keys) + @($After.Keys.Keys) | Sort-Object -Unique)) {
        $prior = $Before.Keys[$physical]; $later = $After.Keys[$physical]
        $alias = if ($prior) { $prior.Aliases[0] } else { $later.Aliases[0] }
        if ($prior -and -not $later) { $creates.Add([pscustomobject]@{ Kind='CreateKey'; PhysicalName=$physical; Alias=$alias }) }
        $priorValues = if ($prior) { $prior.Values } else { @{} }; $laterValues = if ($later) { $later.Values } else { @{} }
        foreach ($name in @(@($priorValues.Keys) + @($laterValues.Keys) | Sort-Object -Unique)) {
            $old = $priorValues[$name]; $new = $laterValues[$name]
            if (Test-P1RawValueEqual $old $new) { continue }
            $kind = if ($old) { 'SetValue' } else { 'DeleteValue' }
            $values.Add([pscustomobject]@{ Kind=$kind; PhysicalName=$physical; Alias=$alias; Name=$name; ExpectedAfter=$new; RestoreBefore=$old })
        }
        if ($later -and -not $prior) { $deletes.Add([pscustomobject]@{ Kind='DeleteNewKey'; PhysicalName=$physical; Alias=$alias }) }
    }
    return @(@($creates | Sort-Object { $_.PhysicalName.Length }) + @($values | Sort-Object PhysicalName, Name) + @($deletes | Sort-Object { $_.PhysicalName.Length } -Descending))
}

function Start-P1RegistryGuard {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$EvidenceDirectory,
          [string[]]$RootPaths = @('Software\Blizzard Entertainment\WorldEdit', 'Software\Blizzard Entertainment\Warcraft III'),
          [int]$MaxBytes = 16777216, [switch]$EnableRestore)
    Test-P1NoEditorProcesses
    $directory = Assert-P1PlainFilePath $EvidenceDirectory
    if ([IO.Directory]::Exists($directory) -or [IO.File]::Exists($directory)) { throw '证据目录必须是新路径' }
    $parent = [IO.Directory]::GetParent($directory).FullName
    if (-not [IO.Directory]::Exists($parent)) { throw '证据父目录必须存在' }
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $context = [pscustomobject]@{ Directory=$directory; Token=[Guid]::NewGuid().ToString('N'); RootPaths=$RootPaths; MaxBytes=$MaxBytes; RestoreEnabled=[bool]$EnableRestore; OwnedProcessIds=@(); Before=$null; After=$null; Delta=@() }
    Write-P1NewJson (Join-Path $directory 'session.local.json') ([ordered]@{ schema='p1-registry-guard-v1'; token=$context.Token; rootPaths=$RootPaths; views=@('Registry32','Registry64'); restoreEnabled=$context.RestoreEnabled; maxBytes=$MaxBytes; machineEnvironmentModified=$false })
    $context.Before = Get-P1RegistrySnapshot -RootPaths $RootPaths -MaxBytes $MaxBytes
    Write-P1NewJson (Join-Path $directory 'before.local.json') $context.Before
    return $context
}

function Complete-P1RegistryGuard($Context) {
    Test-P1NoEditorProcesses -OwnedProcessIds $Context.OwnedProcessIds -RequireOwnedEnded
    if ($null -ne $Context.After) { throw 'after 已存在，拒绝重复 complete' }
    $Context.After = Get-P1RegistrySnapshot -RootPaths $Context.RootPaths -MaxBytes $Context.MaxBytes
    $Context.Delta = @(Get-P1RegistryDelta $Context.Before $Context.After)
    Write-P1NewJson (Join-Path $Context.Directory 'after.local.json') $Context.After
    Write-P1NewJson (Join-Path $Context.Directory 'delta.local.json') @{ physicalOperationCount=$Context.Delta.Count; operations=@($Context.Delta) }
    return $Context
}

function Restore-P1RegistryGuard {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Context, [scriptblock]$BeforeOperation)
    if (-not $Context.RestoreEnabled -or $null -eq $Context.After) { throw '必须显式 EnableRestore 且已有 after' }
    $attempt = Join-Path $Context.Directory ('restore-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory((Assert-P1PlainFilePath $attempt)) | Out-Null
    $result = [pscustomobject]@{ Status='prepared'; AppliedOperations=[Collections.Generic.List[object]]::new(); Conflict=$null; RemainingDelta=@(); BeforeFingerprint=$Context.Before.Fingerprint; AfterFingerprint=$Context.After.Fingerprint; CurrentFingerprint=$null; EvidenceDirectory=$attempt }
    try {
        Test-P1NoEditorProcesses -OwnedProcessIds $Context.OwnedProcessIds -RequireOwnedEnded
        $current = Get-P1RegistrySnapshot -RootPaths $Context.RootPaths -MaxBytes $Context.MaxBytes
        $result.CurrentFingerprint = $current.Fingerprint
        Write-P1NewJson (Join-Path $attempt 'preflight.local.json') $current
        if ($current.Fingerprint -cne $Context.After.Fingerprint) { throw 'conflict: current != after，整次恢复拒绝写入' }
        foreach ($operation in $Context.Delta) {
            if ($BeforeOperation) { & $BeforeOperation $operation $result.AppliedOperations.Count | Out-Null }
            Test-P1NoEditorProcesses -OwnedProcessIds $Context.OwnedProcessIds -RequireOwnedEnded
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::$($operation.Alias.View))
            $key = $null
            try {
                $key = $base.OpenSubKey($operation.Alias.Path, $true)
                if ($operation.Kind -eq 'CreateKey') {
                    if ($key) { throw 'conflict: 被删除的键由外部重新创建' }
                    # 只创建这一层；不能让路径式 CreateSubKey 重建 scope 外消失的祖先。
                    $separator = $operation.Alias.Path.LastIndexOf('\')
                    $parentPath = $operation.Alias.Path.Substring(0, $separator)
                    $leaf = $operation.Alias.Path.Substring($separator + 1)
                    $expectedParent = $operation.PhysicalName.Substring(0, $operation.PhysicalName.LastIndexOf('\'))
                    $parentKey = $base.OpenSubKey($parentPath, $true)
                    if (-not $parentKey) { throw 'conflict: 父键消失，拒绝重建 scope 外祖先' }
                    try {
                        if (-not ([P1RegistryRawV1]::PhysicalName($parentKey)).Equals($expectedParent, [StringComparison]::OrdinalIgnoreCase)) { throw 'conflict: 父物理键变化' }
                        $key = $parentKey.CreateSubKey($leaf, $true)
                    } finally { $parentKey.Dispose() }
                } else {
                    if (-not $key) { throw 'conflict: 目标键已消失' }
                }
                if (-not ([P1RegistryRawV1]::PhysicalName($key)).Equals($operation.PhysicalName, [StringComparison]::OrdinalIgnoreCase)) { throw 'conflict: 目标物理键变化' }
                if ($operation.Kind -in @('SetValue', 'DeleteValue')) {
                    $raw = [P1RegistryRawV1]::Read($key, $operation.Name, $Context.MaxBytes)
                    if (-not (Test-P1RawValueEqual $raw $operation.ExpectedAfter)) { throw 'conflict: 值在恢复步骤前发生变化' }
                    if ($operation.Kind -eq 'SetValue') { [P1RegistryRawV1]::Set($key, $operation.Name, $operation.RestoreBefore.Type, $operation.RestoreBefore.RawBase64) }
                    else { [P1RegistryRawV1]::Delete($key, $operation.Name) }
                } elseif ($operation.Kind -eq 'DeleteNewKey') {
                    if ($key.GetValueNames().Length -ne 0 -or $key.GetSubKeyNames().Length -ne 0) { throw 'conflict: 本次新增键非空，保留外部值/子键' }
                    $key.Dispose(); $key = $null
                    $base.DeleteSubKey($operation.Alias.Path, $true)
                }
                $result.AppliedOperations.Add($operation)
                Write-P1NewJson (Join-Path $attempt ('operation-' + $result.AppliedOperations.Count.ToString('D6') + '.local.json')) $operation
            } finally { if ($key) { $key.Dispose() }; $base.Dispose() }
        }
        $final = Get-P1RegistrySnapshot -RootPaths $Context.RootPaths -MaxBytes $Context.MaxBytes
        Write-P1NewJson (Join-Path $attempt 'final.local.json') $final
        if ($final.Fingerprint -cne $Context.Before.Fingerprint) { throw 'conflict: 最终树与 before 不一致' }
        $result.Status = 'restored'
    } catch {
        $result.Status = 'conflict'; $result.Conflict = $_.Exception.Message
        try {
            $remaining = Get-P1RegistrySnapshot -RootPaths $Context.RootPaths -MaxBytes $Context.MaxBytes
            $result.RemainingDelta = @(Get-P1RegistryDelta $Context.Before $remaining)
            Write-P1NewJson (Join-Path $attempt 'remaining-current.local.json') $remaining
        } catch { $result.RemainingDelta = @($Context.Delta); $result.Conflict += '; remaining snapshot failed: ' + $_.Exception.Message }
    }
    Write-P1NewJson (Join-Path $attempt 'result.local.json') $result
    return $result
}

function Invoke-P1RegistryGuard {
    [CmdletBinding(DefaultParameterSetName='ScriptBlock')]
    param([Parameter(Mandatory)][string]$EvidenceDirectory,
          [Parameter(Mandatory, ParameterSetName='ScriptBlock')][scriptblock]$Action,
          [Parameter(Mandatory, ParameterSetName='Argv')][string[]]$CommandArgv,
          [string[]]$RootPaths = @('Software\Blizzard Entertainment\WorldEdit', 'Software\Blizzard Entertainment\Warcraft III'),
          [int]$MaxBytes = 16777216, [switch]$EnableRestore)
    $context = Start-P1RegistryGuard -EvidenceDirectory $EvidenceDirectory -RootPaths $RootPaths -MaxBytes $MaxBytes -EnableRestore:$EnableRestore
    $commandError = $null; $commandOutput = @(); $restoration = $null
    try {
        if ($PSCmdlet.ParameterSetName -eq 'ScriptBlock') { $commandOutput = @(& $Action) }
        else {
            if ($CommandArgv.Count -lt 1) { throw 'CommandArgv 不能为空' }
            $command = Assert-P1PlainFilePath $CommandArgv[0]
            if (-not [IO.File]::Exists($command)) { throw '自有命令不存在' }
            $start = [Diagnostics.ProcessStartInfo]::new($command)
            $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
            foreach ($argument in @($CommandArgv | Select-Object -Skip 1)) { $start.ArgumentList.Add($argument) }
            $process = [Diagnostics.Process]::Start($start)
            $context.OwnedProcessIds = @($process.Id)
            try {
                $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
                $process.WaitForExit()
                $commandOutput = @($stdout.GetAwaiter().GetResult())
                Write-P1NewJson (Join-Path $context.Directory 'command.local.json') @{ argv=$CommandArgv; ownedPid=$process.Id; exitCode=$process.ExitCode; stdout=$commandOutput; stderr=$stderr.GetAwaiter().GetResult() }
                if ($process.ExitCode -ne 0) { throw "自有命令失败 exit=$($process.ExitCode)" }
            } finally { $process.Dispose() }
        }
    } catch { $commandError = $_.Exception.Message }
    finally {
        try { $context = Complete-P1RegistryGuard $context; if ($EnableRestore) { $restoration = Restore-P1RegistryGuard $context } }
        catch { $restoration = [pscustomobject]@{ Status='conflict'; Conflict=$_.Exception.Message; RemainingDelta=@($context.Delta) } }
    }
    $outcome = [pscustomobject]@{ CommandError=$commandError; CommandOutput=$commandOutput; Restoration=$restoration; EvidenceDirectory=$context.Directory }
    Write-P1NewJson (Join-Path $context.Directory 'wrapper-result.local.json') $outcome
    return $outcome
}

if ($Mode -eq 'Snapshot') {
    if (-not $EvidenceDirectory) { throw 'Snapshot 必须指定新 EvidenceDirectory' }
    $snapshotContext = Start-P1RegistryGuard -EvidenceDirectory $EvidenceDirectory -RootPaths $RootPaths -MaxBytes $MaxBytes
    [pscustomobject]@{ status='snapshot'; fingerprint=$snapshotContext.Before.Fingerprint; physicalKeys=$snapshotContext.Before.Keys.Count; values=$snapshotContext.Before.ValueCount; rawBytes=$snapshotContext.Before.RawBytes; evidenceDirectory=$snapshotContext.Directory } | ConvertTo-Json
}
