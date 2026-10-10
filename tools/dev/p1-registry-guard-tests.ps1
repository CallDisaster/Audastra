#requires -Version 7.2
# 原创临时 HKCU namespace 回归；不启动 WE/game，不写作者 Blizzard 键。
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceParent)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$library = Join-Path $PSScriptRoot 'p1-registry-guard.ps1'
. $library -Mode Library
Test-P1NoEditorProcesses
$testParent = Assert-P1PlainFilePath $EvidenceParent
if (Test-Path -LiteralPath $testParent) { throw 'test EvidenceParent 必须是新目录' }
if (-not (Test-Path -LiteralPath ([IO.Directory]::GetParent($testParent).FullName))) { throw 'test父目录必须存在' }
[IO.Directory]::CreateDirectory($testParent) | Out-Null
$script:caseResults = [Collections.Generic.List[object]]::new()
$script:testNamespacePrefix = 'Software\FORGE-P1-RegistryTests\'

function Assert-Test($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

function Assert-OwnedRoot([string]$Path) {
    Assert-Test ($Path -ceq ($script:testNamespacePrefix + $script:testOwnerToken)) 'exactroot 与随机 token 不符'
    Assert-Test ($script:testOwnerToken -cmatch '^[a-f0-9]{32}$') 'token格式不符'
    Assert-Test ($Path -cmatch '^Software\\FORGE-P1-RegistryTests\\[a-f0-9]{32}$') '拒绝越出 PlainPrefix'
}

function Get-TestKey([string]$Relative = '', [switch]$Create, [string]$View = 'Registry64') {
    Assert-OwnedRoot $script:testOwnedRoot
    Assert-Test ($Relative -match '^[A-Za-z0-9_\\-]*$' -and -not $Relative.StartsWith('\') -and -not $Relative.EndsWith('\')) 'fixture 相对键越界'
    $path = if ($Relative) { $script:testOwnedRoot + '\' + $Relative } else { $script:testOwnedRoot }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::$View)
    try { if ($Create) { return $base.CreateSubKey($path, $true) }; return $base.OpenSubKey($path, $true) }
    finally { $base.Dispose() }
}

function Set-TestRaw([string]$Name, [uint32]$Type, [byte[]]$Bytes, [string]$Relative = '') {
    $key = Get-TestKey -Relative $Relative -Create
    try { [P1RegistryRawV1]::Set($key, $Name, $Type, [Convert]::ToBase64String($Bytes)) }
    finally { $key.Dispose() }
}

function Read-TestRaw([string]$Name, [string]$Relative = '') {
    $key = Get-TestKey -Relative $Relative
    if (-not $key) { return $null }
    try { return [P1RegistryRawV1]::Read($key, $Name, 16777216) }
    finally { $key.Dispose() }
}

function Remove-TestChild([string]$Relative) {
    Assert-OwnedRoot $script:testOwnedRoot
    Assert-Test ($Relative -match '^[A-Za-z0-9_\\-]+$' -and -not $Relative.StartsWith('\') -and -not $Relative.EndsWith('\')) 'fixture删除范围无效'
    $parent = Get-TestKey
    try { $parent.DeleteSubKeyTree($Relative, $false) } finally { $parent.Dispose() }
}

function Case([string]$Name, [scriptblock]$Body) {
    $script:testOwnerToken = [Guid]::NewGuid().ToString('N')
    $script:testOwnedRoot = $script:testNamespacePrefix + $script:testOwnerToken
    Assert-OwnedRoot $script:testOwnedRoot
    $fixtureBase = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
    $prior = $fixtureBase.OpenSubKey($script:testOwnedRoot, $false)
    Assert-Test ($null -eq $prior) '随机namespace已存在，拒绝接管'
    $fixtureBase.Dispose()
    $owned = Get-TestKey -Create
    try { [P1RegistryRawV1]::Set($owned, 'P1OwnerToken', 1, [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script:testOwnerToken + [char]0))) }
    finally { $owned.Dispose() }
    $script:caseDirectory = Join-Path $testParent $Name
    $record = [ordered]@{ name=$Name; status='running'; ownedRoot=$script:testOwnedRoot; randomToken=$script:testOwnerToken; authorRegistryWritten=$false; weOrGameStarted=$false; cleanupVerified=$false }
    try { & $Body; $record.status='passed' }
    catch { $record.status='failed'; $record.error=$_.Exception.ToString() }
    finally {
        # 递归清理仅此 exactroot：复核 namespace/token/native 路径及所有权值后操作。
        Assert-OwnedRoot $script:testOwnedRoot
        $key = Get-TestKey
        Assert-Test ($null -ne $key) 'fixture exactroot消失，不能证明清理身份'
        try {
            $marker = [P1RegistryRawV1]::Read($key, 'P1OwnerToken', 1024)
            $expected = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script:testOwnerToken + [char]0))
            Assert-Test ($marker -and $marker.Type -eq 1 -and $marker.RawBase64 -ceq $expected) 'fixture owner token被改，拒绝删除'
            $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
            $physical = [P1RegistryRawV1]::PhysicalName($key)
            Assert-Test ($physical.Equals('\REGISTRY\USER\' + $sid + '\' + $script:testOwnedRoot, [StringComparison]::OrdinalIgnoreCase)) 'exactroot native身份不符，拒绝删除'
        } finally { $key.Dispose() }
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
        try {
            $base.DeleteSubKeyTree($script:testOwnedRoot, $true)
            $left = $base.OpenSubKey($script:testOwnedRoot, $false)
            Assert-Test ($null -eq $left) 'exactroot未清理'
            $record.cleanupVerified=$true
        } finally { $base.Dispose() }
    }
    $script:caseResults.Add([pscustomobject]$record)
    Write-Host ($record.name + ': ' + $record.status)
}

Case '01-all-types-aliases' {
    $fixtures = @(
        @{ Name=''; Type=11; Bytes=[byte[]](1,0,0,0,0,0,0,128) },
        @{ Name='Dword'; Type=4; Bytes=[byte[]](255,255,255,255) },
        @{ Name='Binary'; Type=3; Bytes=[byte[]](0,127,128,255) },
        @{ Name='EmptyBinary'; Type=3; Bytes=[byte[]]@() },
        @{ Name='None'; Type=0; Bytes=[byte[]](0,250,17) },
        @{ Name='String'; Type=1; Bytes=[Text.Encoding]::Unicode.GetBytes('unterminated fixture') },
        @{ Name='Expand'; Type=2; Bytes=[Text.Encoding]::Unicode.GetBytes('%TEMP%\P1_fixture' + [char]0) },
        @{ Name='Multi'; Type=7; Bytes=[Text.Encoding]::Unicode.GetBytes('first' + [char]0 + [char]0 + 'third' + [char]0 + [char]0) }
    )
    foreach ($fixture in $fixtures) { Set-TestRaw $fixture.Name $fixture.Type $fixture.Bytes }
    Set-TestRaw 'Old' 3 ([byte[]](11,12)) 'old_child'
    $context = Start-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($script:testOwnedRoot) -EnableRestore
    Assert-Test ($context.Before.Scopes.Count -eq 2) '必须读32/64两个视图'
    Assert-Test (@($context.Before.Keys.Values | Where-Object { $_.Aliases.Count -eq 2 }).Count -eq $context.Before.Keys.Count) '同物理键未识别alias'
    foreach ($fixture in $fixtures) { Set-TestRaw $fixture.Name 3 ([byte[]](9,8,7)) }
    $root = Get-TestKey
    try { $root.DeleteValue('None', $true) } finally { $root.Dispose() }
    Remove-TestChild 'old_child'
    Set-TestRaw 'Added' 3 ([byte[]](99)) 'added_parent\added_child'
    $context = Complete-P1RegistryGuard $context
    $valueOps = @($context.Delta | Where-Object { $_.Kind -in @('SetValue','DeleteValue') })
    Assert-Test ($valueOps.Count -eq @($valueOps | Group-Object PhysicalName, Name).Count) '两视图重复恢复同物理值'
    $result = Restore-P1RegistryGuard $context
    Assert-Test ($result.Status -eq 'restored') ($result.Conflict)
    Assert-Test (@($result.AppliedOperations | Where-Object { $_.PSObject.Properties['Name'] -and $_.Name -eq 'P1OwnerToken' }).Count -eq 0) '未变值被触碰'
    foreach ($fixture in $fixtures) {
        $actual = Read-TestRaw $fixture.Name
        Assert-Test ($actual.Type -eq $fixture.Type -and $actual.RawBase64 -ceq [Convert]::ToBase64String($fixture.Bytes)) ('raw/type不保真: ' + $fixture.Name)
    }
    Assert-Test ((Read-TestRaw 'Old' 'old_child').RawBase64 -ceq [Convert]::ToBase64String([byte[]](11,12))) '原子键未恢复'
}

Case '02-current-after-value-conflict' {
    Set-TestRaw 'Value' 3 ([byte[]](1))
    $context = Start-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($script:testOwnedRoot) -EnableRestore
    Set-TestRaw 'Value' 3 ([byte[]](2)); $context = Complete-P1RegistryGuard $context
    Set-TestRaw 'External' 3 ([byte[]](3)) 'external_child'
    $beforeRestore = Get-P1RegistrySnapshot -RootPaths @($script:testOwnedRoot)
    $result = Restore-P1RegistryGuard $context
    $afterRestore = Get-P1RegistrySnapshot -RootPaths @($script:testOwnedRoot)
    Assert-Test ($result.Status -eq 'conflict' -and $result.AppliedOperations.Count -eq 0) '外部变化必须全拒绝'
    Assert-Test ($beforeRestore.Fingerprint -ceq $afterRestore.Fingerprint) '冲突时仍写了registry'
    Assert-Test ($null -ne (Read-TestRaw 'External' 'external_child')) '外部子键未保留'
}

Case '03-new-empty-key-preserves-later-child' {
    $context = Start-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($script:testOwnedRoot) -EnableRestore
    $new = Get-TestKey 'new_key' -Create; $new.Dispose()
    $context = Complete-P1RegistryGuard $context
    $hook = { param($operation, $applied) if ($operation.Kind -eq 'DeleteNewKey') { Set-TestRaw 'External' 3 ([byte[]](5)) 'new_key\external_child' } }
    $result = Restore-P1RegistryGuard -Context $context -BeforeOperation $hook
    Assert-Test ($result.Status -eq 'conflict') '非空新增键必须保留'
    Assert-Test ($null -ne (Read-TestRaw 'External' 'new_key\external_child')) '后续外部子键被删除'
    Assert-Test ($result.RemainingDelta.Count -gt 0) '没有remainingdelta证据'
}

Case '04-mid-restore-conflict-remaining' {
    Set-TestRaw 'A' 3 ([byte[]](1)); Set-TestRaw 'B' 3 ([byte[]](2))
    $context = Start-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($script:testOwnedRoot) -EnableRestore
    Set-TestRaw 'A' 3 ([byte[]](7)); Set-TestRaw 'B' 3 ([byte[]](8)); $context = Complete-P1RegistryGuard $context
    $hook = { param($operation, $applied) if ($applied -eq 1 -and $operation.Name -eq 'B') { Set-TestRaw 'B' 3 ([byte[]](9)) } }
    $result = Restore-P1RegistryGuard -Context $context -BeforeOperation $hook
    Assert-Test ($result.Status -eq 'conflict' -and $result.AppliedOperations.Count -eq 1) '没有条件恢复/部分冲突证据'
    Assert-Test ((Read-TestRaw 'A').RawBase64 -ceq 'AQ==') '先前成功恢复丢失'
    Assert-Test ((Read-TestRaw 'B').RawBase64 -ceq 'CQ==') '外部变更被覆盖'
    Assert-Test ($result.RemainingDelta.Count -gt 0) 'remainingdelta缺失'
}

Case '05-failure-finally' {
    Set-TestRaw 'Value' 3 ([byte[]](1))
    $before = Get-P1RegistrySnapshot -RootPaths @($script:testOwnedRoot)
    $outcome = Invoke-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($script:testOwnedRoot) -EnableRestore -Action {
        Set-TestRaw 'Value' 3 ([byte[]](99))
        throw 'fixture-command-failure'
    }
    Assert-Test ($outcome.CommandError -match 'fixture-command-failure') 'command失败未记录'
    Assert-Test ($outcome.Restoration.Status -eq 'restored') 'finally未恢复'
    Assert-Test ((Get-P1RegistrySnapshot -RootPaths @($script:testOwnedRoot)).Fingerprint -ceq $before.Fingerprint) 'finally后原树未还原'
}

Case '06-absent-root-restored-to-absence' {
    $missing = $script:testOwnedRoot + '\missing_root'
    $context = Start-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($missing) -EnableRestore
    Assert-Test (@($context.Before.Scopes | Where-Object Exists).Count -eq 0) 'missing fixture不是原始不存在'
    Set-TestRaw '' 0 ([byte[]](5,6)) 'missing_root\nested'
    $context = Complete-P1RegistryGuard $context
    $result = Restore-P1RegistryGuard $context
    Assert-Test ($result.Status -eq 'restored') $result.Conflict
    $key = Get-TestKey 'missing_root'
    Assert-Test ($null -eq $key) '本次新root未回到不存在'
}

Case '07-bounded-bytes' {
    Set-TestRaw 'Blob' 3 ([byte[]]::new(4096))
    $rawBefore = Read-TestRaw 'Blob'; $rejected = $false
    try { Get-P1RegistrySnapshot -RootPaths @($script:testOwnedRoot) -MaxBytes 128 | Out-Null } catch { $rejected=$true }
    Assert-Test $rejected '预算未拒绝'
    Assert-Test ((Read-TestRaw 'Blob').RawBase64 -ceq $rawBefore.RawBase64) '预算失败仍改值'
}

Case '08-exclusive-evidence-and-readonly-default' {
    $context = Start-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($script:testOwnedRoot)
    $rejected = $false
    try { Start-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($script:testOwnedRoot) | Out-Null } catch { $rejected=$true }
    Assert-Test $rejected '重复证据未拒绝'
    $context = Complete-P1RegistryGuard $context
    $rejected = $false
    try { Restore-P1RegistryGuard $context | Out-Null } catch { $rejected=$true }
    Assert-Test $rejected '缺EnableRestore仍可写'
}

Case '09-author-editor-denial-fixture' {
    $rejected = $false
    try {
        function global:Get-Process { [pscustomobject]@{ ProcessName='worldedit'; Id=900001 } }
        try { Start-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($script:testOwnedRoot) | Out-Null } catch { $rejected=$true }
    } finally { Remove-Item -LiteralPath 'Function:\Get-Process' }
    Assert-Test $rejected '进程query夹具未拒绝作者WE'
    Assert-Test (-not (Test-Path -LiteralPath $script:caseDirectory)) '作者WE拒绝后仍创建证据'
}

Case '10-structured-argv' {
    [IO.Directory]::CreateDirectory($script:caseDirectory) | Out-Null
    $writer = Join-Path $script:caseDirectory 'owned-writer.ps1'
    @'
param([string]$Library, [string]$Root, [string]$Token, [string]$Literal)
$ErrorActionPreference='Stop'
. $Library -Mode Library
if ($Root -cne ('Software\FORGE-P1-RegistryTests\' + $Token) -or $Token -cnotmatch '^[a-f0-9]{32}$') { throw 'fixture exactroot rejected' }
$base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$key=$base.OpenSubKey($Root,$true)
try {
    $marker=[P1RegistryRawV1]::Read($key,'P1OwnerToken',1024)
    if ($marker.RawBase64 -cne [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Token+[char]0))) { throw 'fixture marker mismatch' }
    [P1RegistryRawV1]::Set($key,'ArgvValue',1,[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Literal+[char]0)))
    Write-Output $Literal
} finally { $key.Dispose();$base.Dispose() }
'@ | Set-Content -LiteralPath $writer -Encoding utf8NoBOM
    $literal = 'literal "quotes" $() & ; backtick`'
    $executable = (Get-Process -Id $PID).Path
    $guardDirectory = Join-Path $script:caseDirectory 'guard'
    $outcome = Invoke-P1RegistryGuard -EvidenceDirectory $guardDirectory -RootPaths @($script:testOwnedRoot) -EnableRestore -CommandArgv @($executable, '-NoProfile', '-File', $writer, '-Library', $library, '-Root', $script:testOwnedRoot, '-Token', $script:testOwnerToken, '-Literal', $literal)
    Assert-Test ($null -eq $outcome.CommandError) $outcome.CommandError
    Assert-Test ($outcome.Restoration.Status -eq 'restored') 'argv命令后未恢复'
    Assert-Test (($outcome.CommandOutput -join '').Contains($literal)) 'argv发生shell展开/引号改变'
    Assert-Test ($null -eq (Read-TestRaw 'ArgvValue')) 'argv本次值未清理'
}

Case '11-missing-parent-no-ancestor-rebuild' {
    Set-TestRaw 'Original' 3 ([byte[]](21)) 'outside_parent\scope_root'
    $scope = $script:testOwnedRoot + '\outside_parent\scope_root'
    $context = Start-P1RegistryGuard -EvidenceDirectory $script:caseDirectory -RootPaths @($scope) -EnableRestore
    Remove-TestChild 'outside_parent\scope_root'
    $context = Complete-P1RegistryGuard $context
    # scope 外父键在 after 后消失；restore 不能以整条路径创建它。
    Remove-TestChild 'outside_parent'
    $result = Restore-P1RegistryGuard $context
    Assert-Test ($result.Status -eq 'conflict' -and $result.AppliedOperations.Count -eq 0) '父键消失未在写入前拒绝'
    $parent = Get-TestKey 'outside_parent'
    Assert-Test ($null -eq $parent) '恢复越出 scope 重建父键'
    Assert-Test ($result.RemainingDelta.Count -gt 0) '祖先冲突 remainingdelta 缺失'
}

$summary = [ordered]@{ testCount=$script:caseResults.Count; passedCount=@($script:caseResults | Where-Object status -eq 'passed').Count; failedCount=@($script:caseResults | Where-Object status -eq 'failed').Count; allExactRootsCleaned=@($script:caseResults | Where-Object { -not $_.cleanupVerified }).Count -eq 0; authorRegistryWritten=$false; weOrGameStarted=$false; cases=@($script:caseResults) }
Write-P1NewJson (Join-Path $testParent 'summary.local.json') $summary
[pscustomobject]$summary | Select-Object testCount, passedCount, failedCount, allExactRootsCleaned, authorRegistryWritten, weOrGameStarted | ConvertTo-Json
if ($summary.failedCount -gt 0) { exit 1 }
