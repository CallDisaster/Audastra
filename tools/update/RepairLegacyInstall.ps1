#requires -Version 5.1
<#
.SYNOPSIS
使用官方外置修复包，将精确匹配的 Audastra 0.1.1 / 0.1.5 / 0.1.6 安装升级到 0.1.7。
.DESCRIPTION
先保存并正常关闭旧编辑器。脚本须位于完整修复包根目录；不接受外部清单或公钥。
-PlanOnly 仅运行验签和只读计划。安装、运行身份检查、备份及恢复均交给原 updater。
#>
[CmdletBinding()]
param(
    [string]$InstallDirectory = '',
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([string]::IsNullOrWhiteSpace($InstallDirectory)) {
    Write-Host '用法：.\RepairLegacyInstall.ps1 -InstallDirectory "C:\Apps\Audastra" [-PlanOnly]'
    Write-Host '请先保存并正常关闭旧编辑器，再从完整官方修复包运行。详见 LEGACY_UPDATE_REPAIR.md。'
    return
}

$targetVersion = '0.1.7'
$entryPoint = 'bin/war3-map-editor.exe'
$runDirectory = $null
$previousConsoleEncoding = [Console]::OutputEncoding
$utf8 = [Text.UTF8Encoding]::new($false, $true)

function Test-Within([string]$Child, [string]$Parent) {
    return [StringComparer]::OrdinalIgnoreCase.Equals($Child, $Parent) -or
        $Child.StartsWith($Parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Assert-PlainPath([string]$Path, [bool]$Directory) {
    $item = Get-Item -LiteralPath $Path -Force
    if (($item -is [IO.DirectoryInfo]) -ne $Directory) { throw "文件/目录类型不符：$Path" }
    $cursor = $item
    for ($depth = 0; $null -ne $cursor; ++$depth) {
        if ($depth -gt 65 -or ($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "路径过深或含链接/reparse，停止：$Path"
        }
        $cursor = if ($cursor -is [IO.DirectoryInfo]) { $cursor.Parent } else { $cursor.Directory }
    }
    return $item
}

function Read-BoundedJson([string]$Path, [long]$Limit = 1MB) {
    $item = Assert-PlainPath $Path $false
    if ($item.Length -le 0 -or $item.Length -gt $Limit) { throw "JSON 回执为空或超过容量：$Path" }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Invoke-BundleJson([string]$Executable, [string[]]$Arguments, [string]$Label, [switch]$AllowFailure) {
    $outputPath = Join-Path $runDirectory ($Label + '.json')
    $errorPath = Join-Path $runDirectory ($Label + '.stderr.log')
    if ((Test-Path -LiteralPath $outputPath) -or (Test-Path -LiteralPath $errorPath)) { throw '拒绝覆盖已有回执。' }
    $savedPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 将 native stderr 表示为错误记录；候选计划的拒绝属于预期结果。
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments 1> $outputPath 2> $errorPath
        $nativeExitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    $data = $null
    if ((Get-Item -LiteralPath $outputPath).Length -gt 0) {
        try { $data = Read-BoundedJson $outputPath 64MB }
        catch { if ($nativeExitCode -eq 0) { throw } }
    }
    if (!$AllowFailure -and ($nativeExitCode -ne 0 -or $null -eq $data)) {
        throw "$Label 校验失败（exit $nativeExitCode）。原始回执：$outputPath；诊断：$errorPath"
    }
    return [pscustomobject]@{ ExitCode = $nativeExitCode; Data = $data }
}

function Read-VerifiedManifest([string]$Path, $Check) {
    $item = Assert-PlainPath $Path $false
    if ($item.Length -le 0 -or $item.Length -gt 1MB) { throw '签名清单容量无效。' }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($algorithm.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
    if ($Check.signature_verified -ne $true -or $digest -cne $Check.manifest_sha256) {
        throw '实际读取的清单字节与 CLI 验签回执不符。'
    }
    return $utf8.GetString($bytes).TrimStart([char]0xfeff) | ConvertFrom-Json
}

function Get-EntryMap($Entries) {
    $map = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $Entries) { $map.Add([string]$entry.path, $entry) }
    return ,$map
}

function Assert-Plan($Plan, $Baseline, [string]$Version, [string]$BaselineHash) {
    if ($null -eq $Plan -or $Plan.dry_run -ne $true -or $Plan.installed -ne $false -or
        $Plan.signature_verified -ne $true -or $Plan.package_verified -ne $true -or
        $Plan.update_available -ne $true -or $Plan.directly_upgradeable -ne $true -or
        $Plan.current_version -cne $Version -or $Plan.target_version -cne $targetVersion -or
        $Plan.manifest_sha256 -cne $targetCheck.manifest_sha256 -or
        $Plan.package_sha256 -cne $targetCheck.package_sha256 -or
        $Plan.baseline_manifest_sha256 -cne $BaselineHash -or $Plan.inventory_sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw '只读计划的签名、版本或文件身份不符。'
    }
    $current = Get-EntryMap $Plan.files
    # 正式 plan 只要求所有待覆盖程序匹配；选择旧版时额外证明旧清单全部文件（含废弃文件）。
    foreach ($old in $Baseline.files) {
        if (!$current.ContainsKey($old.path) -or $current[$old.path].current_bytes -ne $old.bytes -or
            $current[$old.path].current_sha256 -cne $old.sha256) { throw '安装树没有逐项匹配此原签名旧版本。' }
    }
}

try {
    [Console]::OutputEncoding = $utf8
    $PSNativeCommandUseErrorActionPreference = $false
    if (($env:QT_QPA_PLATFORM -and $env:QT_QPA_PLATFORM -cne 'windows') -or
        @(Get-ChildItem Env: | Where-Object { $_.Name -like 'AUDASTRA_TEST_*' -or $_.Name -like 'FORGE_WORLD_PROBE_*' }).Count -gt 0) {
        throw '当前终端包含离屏/测试注入环境；请从普通桌面的正常 PowerShell 环境运行，不授予测试健康。'
    }
    if ($InstallDirectory -notmatch '^[A-Za-z]:[\\/]') { throw '请明确传入现有安装目录的本机绝对路径。' }
    $installRoot = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\', '/')
    if ($installRoot.Length -le 3) { throw '不能把磁盘根作为安装目录。' }
    $null = Assert-PlainPath $installRoot $true
    $bundleRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
    $null = Assert-PlainPath $bundleRoot $true
    if ((Test-Within $bundleRoot $installRoot) -or (Test-Within $installRoot $bundleRoot)) {
        throw '请把完整修复包解压到安装树之外的独立目录，两者不能相互包含。'
    }
    $helperRoot = Join-Path $bundleRoot 'helper'
    $cli = Join-Path $helperRoot 'bin/war3-map-cli.exe'
    $updater = Join-Path $helperRoot 'bin/audastra-updater.exe'
    $manifestPath = Join-Path $bundleRoot 'target/update-manifest.json'
    $signaturePath = Join-Path $bundleRoot 'target/update-manifest.json.sig'
    $packagePath = Join-Path $bundleRoot 'target/Audastra-0.1.7-windows-x64-update.zip'
    foreach ($path in @($cli, $updater, $manifestPath, $signaturePath, $packagePath)) { $null = Assert-PlainPath $path $false }
    $receiptRoot = Join-Path $bundleRoot 'receipts'
    if (!(Test-Path -LiteralPath $receiptRoot)) { $null = [IO.Directory]::CreateDirectory($receiptRoot) }
    $null = Assert-PlainPath $receiptRoot $true
    $runDirectory = Join-Path $receiptRoot ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [Guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $runDirectory) { throw '本次回执目录已存在。' }
    $null = [IO.Directory]::CreateDirectory($runDirectory)
    Push-Location -LiteralPath $bundleRoot
    try {
        $identity = (Invoke-BundleJson $cli @('--version-json') 'helper-identity').Data
        $targetCheck = (Invoke-BundleJson $cli @('update-check', '--manifest', $manifestPath, '--signature', $signaturePath,
            '--package', $packagePath, '--current-version', '0.1.1', '--channel', 'stable') 'target-check').Data
        $target = Read-VerifiedManifest $manifestPath $targetCheck
        if ($targetCheck.target_version -cne $targetVersion -or $targetCheck.package_verified -ne $true -or
            $targetCheck.update_available -ne $true -or $targetCheck.directly_upgradeable -ne $true -or
            $target.version -cne $targetVersion -or $target.zip.filename -cne [IO.Path]::GetFileName($packagePath) -or
            $target.zip.sha256 -cne $targetCheck.package_sha256 -or $target.source_commit -cnotmatch '^[0-9a-f]{40}$' -or
            $identity.product -cne 'Audastra' -or $identity.version -cne $targetVersion -or
            $identity.commit -cne $target.source_commit -or $identity.update_public_key_hex -cnotmatch '^[0-9a-fA-F]{64}$') {
            throw '修复 helper 与 0.1.7 签名目标的版本、commit 或包身份不一致。'
        }
        $signedFiles = Get-EntryMap $target.files
        if (!$signedFiles.ContainsKey($entryPoint)) { throw '签名目标缺少正式编辑器启动入口。' }
        # helper 只能是目标签名程序文件的外置子集；发布方负责完整 app-local 依赖验收。
        $queue = [Collections.Generic.Queue[string]]::new()
        $queue.Enqueue($helperRoot)
        $helperItems = 0
        while ($queue.Count -gt 0) {
            $directory = $queue.Dequeue()
            foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
                if (++$helperItems -gt 8192 -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'helper 目录超限或含链接。' }
                if ($item.PSIsContainer) { $queue.Enqueue($item.FullName); continue }
                $relative = $item.FullName.Substring($helperRoot.Length + 1).Replace('\', '/')
                if (!$signedFiles.ContainsKey($relative) -or $item.Length -ne $signedFiles[$relative].bytes -or
                    (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $signedFiles[$relative].sha256) {
                    throw "外置 helper 文件与签名目标不符：$relative"
                }
            }
        }
        $matches = [Collections.Generic.List[object]]::new()
        foreach ($version in @('0.1.1', '0.1.5', '0.1.6')) {
            $oldManifest = Join-Path $bundleRoot ('baseline/' + $version + '/update-manifest.json')
            $oldSignature = $oldManifest + '.sig'
            $null = Assert-PlainPath $oldManifest $false
            $null = Assert-PlainPath $oldSignature $false
            $oldCheck = (Invoke-BundleJson $cli @('update-check', '--manifest', $oldManifest, '--signature', $oldSignature,
                '--current-version', '0.0.0') ('baseline-' + $version)).Data
            $old = Read-VerifiedManifest $oldManifest $oldCheck
            if ($oldCheck.target_version -cne $version -or $old.version -cne $version) { throw '原签名 baseline 版本与固定目录不符。' }
            $planArguments = @('update-apply', '--dry-run', '--install-dir', $installRoot, '--package', $packagePath,
                '--manifest', $manifestPath, '--signature', $signaturePath, '--current-version', $version,
                '--baseline-manifest', $oldManifest, '--baseline-signature', $oldSignature, '--exe', $entryPoint)
            $attempt = Invoke-BundleJson $cli $planArguments ('plan-' + $version) -AllowFailure
            if ($attempt.ExitCode -ne 0) { continue }
            try { Assert-Plan $attempt.Data $old $version $oldCheck.manifest_sha256 }
            catch { continue }
            $matches.Add([pscustomobject]@{ Version = $version; Manifest = $oldManifest; Signature = $oldSignature;
                Baseline = $old; Hash = $oldCheck.manifest_sha256; Plan = $attempt.Data; Arguments = $planArguments })
        }
        if ($matches.Count -ne 1) { throw "旧版本完整签名树匹配数为 $($matches.Count)，需要恰好一个。程序修改、缺文件、未知 ADS 或版本不符均应停止；请保留回执。" }
        $selected = $matches[0]
        $finalPlan = (Invoke-BundleJson $cli $selected.Arguments 'selected-plan-final').Data
        Assert-Plan $finalPlan $selected.Baseline $selected.Version $selected.Hash
        if ($finalPlan.inventory_sha256 -cne $selected.Plan.inventory_sha256) { throw '选择旧版本期间安装树发生变化，停止；请先关闭编辑器并保留回执。' }
        Write-Host "已核：$($selected.Version) → $targetVersion，计划 $(@($finalPlan.files).Count) 项。"
        Write-Host "本地回执：$runDirectory"
        if ($PlanOnly) { Write-Host '只读计划通过，尚未安装。'; return }
        Write-Host '即将交给正式 updater；旧编辑器必须已保存并正常关闭，仍在运行时 updater 会拒绝。'
        $installArguments = @('--install-dir', $installRoot, '--package', $packagePath, '--manifest', $manifestPath,
            '--signature', $signaturePath, '--current-version', $selected.Version, '--baseline-manifest', $selected.Manifest,
            '--baseline-signature', $selected.Signature, '--exe', $entryPoint, '--health-timeout-ms', '60000')
        $installed = Invoke-BundleJson $updater $installArguments 'install-result' -AllowFailure
        $result = $installed.Data
        if ($installed.ExitCode -ne 0) {
            Write-Host "更新未完成（updater exit $($installed.ExitCode)）。"
            if ($null -ne $result -and $result.PSObject.Properties['reason_code']) {
                Write-Host "$($result.reason_code) $($result.reason)"
            } elseif ($null -ne $result -and $result.PSObject.Properties['diagnostics']) {
                foreach ($diagnostic in @($result.diagnostics)) { Write-Host "$($diagnostic.code) $($diagnostic.message)" }
            }
            Write-Host "请保留全部事务与回执，不自动重试或删除备份：$runDirectory"
            exit $(if ($installed.ExitCode -eq 2) { 2 } else { 1 })
        }
        if ($null -eq $result -or $result.installed -ne $true -or $result.outcome -cne 'committed' -or
            $result.health_verified -ne $true -or $result.user_files_preserved -ne $true -or
            ![StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($result.install_directory).TrimEnd('\'), $installRoot) -or
            $result.transaction -cnotmatch '^[0-9a-f]{32}$' -or $null -eq $result.child -or $result.child.pid -le 0 -or $result.child.creation_time_100ns -le 0) {
            throw 'updater 已返回，但成功、健康或目录身份回执不完整；不要自动重试。'
        }
        $transactionRoot = Join-Path ([IO.Path]::GetDirectoryName($installRoot)) ('.audastra-update-' + $result.transaction + '.transaction')
        $journalPath = Join-Path $transactionRoot 'journal.json'
        if (![StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($result.transaction_directory), $transactionRoot) -or
            ![StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($result.receipt_path), $journalPath)) { throw '返回的事务回执位置不符。' }
        $journal = Read-BoundedJson $journalPath
        if ($journal.phase -cne 'committed' -or $journal.transaction -cne $result.transaction -or $journal.current_version -cne $selected.Version -or
            $journal.target_version -cne $targetVersion -or $journal.source_commit -cne $target.source_commit -or $journal.executable -cne $entryPoint -or
            $journal.manifest_sha256 -cne $targetCheck.manifest_sha256 -or $journal.baseline_sha256 -cne $selected.Hash -or
            $journal.child.pid -ne $result.child.pid -or $journal.child.creation_time_100ns -ne $result.child.creation_time_100ns) {
            throw 'updater 报告已提交，但最终 journal 的版本、签名或健康进程身份未对应；请保留现场，不要重试。'
        }
        Write-Host "升级已提交，0.1.7 编辑器已通过正式健康门。事务回执：$journalPath"
    } finally { Pop-Location }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    if ($runDirectory) { [Console]::Error.WriteLine("请保留回执及原目录；若 updater 已启动，先按回执确认状态，不要重跑：$runDirectory") }
    exit 1
} finally { [Console]::OutputEncoding = $previousConsoleEncoding }
