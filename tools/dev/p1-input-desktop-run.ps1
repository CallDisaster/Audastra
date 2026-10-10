#requires -Version 7.2
# native08 唯一执行入口：同host/同来源、新副本、当前Default输入桌面。
# 只复用ydwe-save及registry guard公共实现，不实现另一保存器；失败活动OWN保留监管session。
param([Parameter(Mandatory)][string]$Manifest, [Parameter(Mandatory)][string]$ManifestSha256)
$ErrorActionPreference = 'Stop'
throw '原版WE Default启动已停止：native08出现可见窗口及前台错误；DISPLAY1定位不足以保证不激活，当前工具未实现pre-resume完整隔离，禁止新WE launch'
. (Join-Path $PSScriptRoot 'p1-registry-guard.ps1') -Mode Library
. (Join-Path $PSScriptRoot 'p1-input-desktop.ps1')

function Assert-Native08Pin([string]$Path, [string]$Sha256, [long]$Length = -1) {
    $path = Assert-P1PlainFilePath $Path
    if ($Sha256 -notmatch '^[0-9a-f]{64}$' -or -not [IO.File]::Exists($path) -or
        ($Length -ge 0 -and (Get-Item -LiteralPath $path).Length -ne $Length) -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Sha256) {
        throw "native08 文件冻结身份不符: $path"
    }
    return $path
}
function Get-Native08Inventory([string]$Output) {
    $inventoryOutput = @(& $run.Python.path -B (Join-Path $PSScriptRoot 'p1-input-desktop-inventory.py') --prepared $run.Prepared.path `
        --prepared-sha256 $run.Prepared.sha256 --native-loader-sha256 $nativeLoaderPin.sha256 --receipt $Output)
    if ($LASTEXITCODE -ne 0) { throw 'native08 完整只读清单失败' }
    foreach ($line in $inventoryOutput) { Write-Information $line -InformationAction Continue }
    return (Get-Content -LiteralPath $Output -Raw | ConvertFrom-Json)
}

$Manifest = Assert-Native08Pin $Manifest $ManifestSha256.ToLowerInvariant()
$run = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if ($run.schema -cne 'p1-input-desktop-native-save-v1' -or $run.desktop -cne 'WinSta0\Default' -or
    $run.saveCommand -ne 4 -or -not $run.unpackagedDescendants -or $run.runtimeValidated -ne $false -or
    $run.timeoutSeconds -lt 20 -or $run.timeoutSeconds -gt 600 -or $run.pjassCodePage -notin @(65001,936)) {
    throw 'native08 manifest模式不符'
}
$requiredScripts = @('p1-input-desktop-run.ps1','p1-input-desktop.ps1','p1-input-desktop-inventory.py',
    'ydwe-save.ps1','p1-native-loader.py','p1-registry-guard.ps1','p1-archive-compare.py','p1-support.py')
foreach ($name in $requiredScripts) {
    $pins = @($run.scripts | Where-Object { [IO.Path]::GetFileName($_.path) -ceq $name })
    if ($pins.Count -ne 1 -or -not [string]::Equals($pins[0].path,(Join-Path $PSScriptRoot $name),[StringComparison]::OrdinalIgnoreCase)) {
        throw "native08 必需公共实现未唯一冻结: $name"
    }
    [void](Assert-Native08Pin $pins[0].path $pins[0].sha256)
}
$nativeLoaderPin = @($run.scripts | Where-Object { [IO.Path]::GetFileName($_.path) -ceq 'p1-native-loader.py' })[0]
[void](Assert-Native08Pin $run.Python.path $run.Python.sha256)
[void](Assert-Native08Pin $run.Cli.path $run.Cli.sha256)
[void](Assert-Native08Pin $run.Prepared.path $run.Prepared.sha256)
[void](Assert-Native08Pin $run.baseline07.path $run.baseline07.sha256)
$baseline07=Get-Content -LiteralPath $run.baseline07.path -Raw | ConvertFrom-Json
if ($baseline07.sourceMap.path -cne $run.Source.path -or $baseline07.sourceMap.bytes -ne $run.Source.bytes -or
    $baseline07.sourceMap.sha256 -cne $run.Source.sha256 -or $baseline07.prepared.path -cne $run.Prepared.path -or
    $baseline07.prepared.sha256 -cne $run.Prepared.sha256 -or $baseline07.hostRoot -cne $run.hostRoot -or
    $baseline07.gameRoot -cne $run.gameRoot -or $baseline07.saveCommand -ne 4 -or -not $baseline07.unpackagedDescendants) {
    throw 'native08与冻结07同输入/host/direct保存合同不符'
}
$selectedEnvironment=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($property in $run.selectedEnvironment.PSObject.Properties) { $selectedEnvironment.Add($property.Name,[string]$property.Value) }
if ($selectedEnvironment.Count -ne 12 -or (Get-P1InputEnvironmentFingerprint $selectedEnvironment) -cne $run.environmentSha256) {
    throw 'native08选择的12键基础Windows环境冻结不符'
}
$baselineEnvironment=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($property in $baseline07.selectedEnvironment.PSObject.Properties) { $baselineEnvironment.Add($property.Name,[string]$property.Value) }
if ((Get-P1InputEnvironmentFingerprint $baselineEnvironment) -cne $run.environmentSha256) { throw 'native08环境值不再匹配07冻结来源' }
$source = Assert-Native08Pin $run.Source.path $run.Source.sha256 $run.Source.bytes
$root = Assert-P1PlainFilePath $run.runRoot
if ([IO.Directory]::Exists($root) -or [IO.File]::Exists($root) -or -not [IO.Directory]::Exists([IO.Path]::GetDirectoryName($root))) {
    throw 'native08 runRoot必须是已有父目录中的独占新路径'
}
foreach ($inputPath in @($source,$run.gameRoot,$run.hostRoot,$Manifest)) {
    $plain = Assert-P1PlainFilePath $inputPath
    if ($root.Equals($plain,[StringComparison]::OrdinalIgnoreCase) -or
        $root.StartsWith($plain.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or
        $plain.StartsWith($root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'native08 输出不能覆盖/包含输入或host/game' }
}
$prepared = Get-Content -LiteralPath $run.Prepared.path -Raw | ConvertFrom-Json
if (-not [string]::Equals($prepared.gameRoot,$run.gameRoot,[StringComparison]::OrdinalIgnoreCase) -or
    -not [string]::Equals($prepared.hostRoot,$run.hostRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'native08 prepared根不符' }
Test-P1NoEditorProcesses
$witness = [P1InputDesktopOwner]::Witness(); $witness.RequireActiveDefault()

# FileShare.None是本host的协作独占租约；不依赖仅记录PID的锁文件。
$hostKey = [Text.UTF8Encoding]::new($false).GetBytes([IO.Path]::GetFullPath($run.hostRoot).ToLowerInvariant())
$hostKey = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($hostKey)).ToLowerInvariant()
$leasePath = Join-Path ([IO.Path]::GetDirectoryName($run.Prepared.path)) ('p1-native08-host-lease-' + $hostKey + '.local.lock')
foreach ($installation in @($run.hostRoot,$run.gameRoot)) {
    if ($leasePath.StartsWith($installation.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'host租约必须位于game/host清单之外' }
}
$lease = [IO.File]::Open((Assert-P1PlainFilePath $leasePath),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
$native08Owner=$null; $context=$null; $restoration=$null; $saveSucceeded=$false; $compareSucceeded=$false
$terminal=$null; $errorText=$null; $after=$null; $continuedAfterActive=$false
try {
    [void][IO.Directory]::CreateDirectory($root)
    Write-P1InputDesktopNewJson (Join-Path $root 'execution-started.local.json') @{
        manifestPath=$Manifest; manifestSha256=$ManifestSha256; parentWitness=$witness;
        hostLeasePath=$leasePath; hostLeaseHeld=$true; source=$run.Source; environmentSha256=$run.environmentSha256;
        environmentKeys=@($selectedEnvironment.Keys | Sort-Object); ambientEnvironmentInherited=$false
    }
    $before = Get-Native08Inventory (Join-Path $root 'inventory-before.local.json')
    if (-not $before.matchesPrepared) { throw 'native08 host/game与prepared完整清单不符，拒绝启动' }
    $input = Join-Path $root 'owned-input.w3x'
    $incoming=[IO.File]::Open($source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try {
        $outgoing=[IO.File]::Open($input,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        try { $incoming.CopyTo($outgoing); $outgoing.Flush($true) } finally { $outgoing.Dispose() }
    } finally { $incoming.Dispose() }
    [void](Assert-Native08Pin $input $run.Source.sha256 $run.Source.bytes)
    [void](Assert-Native08Pin $source $run.Source.sha256 $run.Source.bytes)
    $context = Start-P1RegistryGuard -EvidenceDirectory (Join-Path $root 'registry') -EnableRestore
    try {
        & (Join-Path $PSScriptRoot 'ydwe-save.ps1') -YdweRoot $run.hostRoot -Map $input -SaveCommand 4 `
            -TimeoutSeconds $run.timeoutSeconds -DirectGameRoot $run.gameRoot -DirectLoaderReceipt $run.Prepared.path `
            -DirectLoaderReceiptSha256 $run.Prepared.sha256 -InputDesktopEvidence $root `
            -InputDesktopOwner ([ref]$native08Owner) -InputDesktopPjassCodePage $run.pjassCodePage `
            -InputDesktopEnvironment $selectedEnvironment *>&1 |
            Tee-Object -FilePath (Join-Path $root 'ydwe-save.local.log')
        $saveSucceeded=$true
    } catch { $errorText=$_.Exception.Message }
    if ($native08Owner) {
        $context.OwnedProcessIds=@($native08Owner.Id)
        $terminal=$native08Owner.Terminal()
        if (-not $terminal.EnvironmentQuiescent) {
            $continuedAfterActive=$true
            Write-P1InputDesktopNewJson (Join-Path $root 'active-owned-preserved.local.json') @{
                status='active-owned-session-preserved'; editorIdentity=$native08Owner.Identity;
                terminal=$terminal; saveError=$errorText; registryAfterCaptured=$false; registryRestored=$false;
                ownedFrames=@([YdweWin]::GetEditorFrames($native08Owner.Id,$true)); ownedDialogs=@([YdweWin]::GetDialogs($native08Owner.Id,$true));
                supervisorPid=$PID; hostLeaseHeld=$true; originalHandlesRetained=$true;
                readOnlyInspection="Get-Content -LiteralPath '$root\active-owned-preserved.local.json'; Get-CimInstance Win32_Process -Filter 'ProcessId = $($native08Owner.Id)'";
                instruction='监管session继续等待原句柄及关联项；不点击未知模态、不终止进程、不提前restore'
            }
            # 无kill-on-close/按名称终止/自动点击。原句柄和registry context在活动监管进程内保持。
            while (-not $terminal.EnvironmentQuiescent) {
                Start-Sleep -Seconds 2
                $terminal=$native08Owner.Terminal()
            }
        }
        Write-P1InputDesktopNewJson (Join-Path $root 'supervisor-terminal.local.json') $terminal
    }
    if ($saveSucceeded) {
        & $run.Python.path -B (Join-Path $PSScriptRoot 'p1-archive-compare.py') --cli $run.Cli.path --before $source `
            --after $input --report (Join-Path $root 'member-comparison.local.json')
        $compareSucceeded=$LASTEXITCODE -eq 0
        if (-not $compareSucceeded) { $errorText='原版保存后逐成员重新打开/比较未通过，须审差异' }
    }
} catch { $errorText=$_.Exception.Message }
finally {
    # 异常也先见证实际OWN终态；active OWN绝不进入after/restore。
    $safeToRestore = $null -eq $native08Owner
    if ($native08Owner) {
        try { $terminal=$native08Owner.Terminal(); $safeToRestore=$terminal.EnvironmentQuiescent }
        catch { $safeToRestore=$false; $errorText="$errorText; terminal: $($_.Exception.Message)" }
    }
    try {
    if ($context -and $safeToRestore) {
        try { $context=Complete-P1RegistryGuard $context; $restoration=Restore-P1RegistryGuard $context }
        catch { $errorText="$errorText; registry: $($_.Exception.Message)" }
    }
    if ($safeToRestore) {
        try { $after=Get-Native08Inventory (Join-Path $root 'inventory-after.local.json') }
        catch { $errorText="$errorText; inventory: $($_.Exception.Message)" }
    }
    $sourceUnchanged=$false
    try { [void](Assert-Native08Pin $source $run.Source.sha256 $run.Source.bytes); $sourceUnchanged=$true }
    catch { $errorText="$errorText; source: $($_.Exception.Message)" }
    $registryRestored=$restoration -and $restoration.Status -ceq 'restored' -and -not $restoration.Conflict -and @($restoration.RemainingDelta).Count -eq 0
    $hostRestored=$after -and $after.matchesPrepared
    $passed=$saveSucceeded -and $compareSucceeded -and $safeToRestore -and $terminal -and $terminal.StableTerminalState -and $terminal.ExitCode -eq 0 -and
        $registryRestored -and $hostRestored -and $sourceUnchanged
    $nativeSaveExecuted=$saveSucceeded -and $terminal -and $terminal.StableTerminalState -and $terminal.ExitCode -eq 0
    $roundtripValidated=$nativeSaveExecuted -and $compareSucceeded -and $sourceUnchanged
    if ([IO.Directory]::Exists($root)) {
        Write-P1InputDesktopNewJson (Join-Path $root 'run-outcome.local.json') @{
            schema='p1-input-desktop-native-save-outcome-v1'; passed=[bool]$passed; error=$errorText;
            saveSucceeded=$saveSucceeded; archiveComparisonSucceeded=$compareSucceeded; terminal=$terminal;
            nativeSaveExecuted=[bool]$nativeSaveExecuted; roundtripValidated=[bool]$roundtripValidated;
            comparisonReviewRequired=[bool]($saveSucceeded -and -not $compareSucceeded);
            registryRestored=[bool]$registryRestored; restoration=$restoration; hostRestored=[bool]$hostRestored;
            sourceUnchanged=$sourceUnchanged; continuedAfterActive=$continuedAfterActive; nativeSaveValidated=[bool]$passed; runtimeValidated=$false;
            hostInventoryReviewRequired=[bool](-not $hostRestored); activeGuardSessionRetained=[bool](-not $safeToRestore)
        }
    }
    } finally {
        # 留存是最外层finally的责任：即使证据磁盘满/写JSON失败，也不能越过活动OWN监管。
        if ($safeToRestore) { if ($native08Owner) { $native08Owner.Dispose() }; $lease.Dispose() }
        else {
            Write-Warning "native08 active OWN retained: supervisorPid=$PID editorPid=$($native08Owner.Id) root=$root; evidence write or terminal verification failed; no early registry restoration"
            $lateQuiet=$false
            while (-not $lateQuiet) {
                Start-Sleep -Seconds 2
                try { $terminal=$native08Owner.Terminal(); $lateQuiet=$terminal.EnvironmentQuiescent }
                catch { $lateQuiet=$false }
            }
            try {
                if ($context -and $null -eq $context.After) {
                    $context=Complete-P1RegistryGuard $context; $restoration=Restore-P1RegistryGuard $context
                }
                Write-P1InputDesktopNewJson (Join-Path $root 'late-terminal-restoration.local.json') @{
                    terminal=$terminal; restoration=$restoration; passed=$false; nativeSaveValidated=$false;
                    reason='此前证据/监管失败已保留；后续原OWN自然终态与restore单独记录，不改原outcome'
                }
            } finally { $native08Owner.Dispose(); $lease.Dispose() }
        }
    }
}
if (-not $passed) { exit 2 }
exit 0
