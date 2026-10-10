[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$LuaHost,
    [Parameter(Mandatory)][string]$LuaRuntime,
    [Parameter(Mandatory)][string]$SlkLibrary,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedHostSha256,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedRuntimeSha256,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedLibrarySha256
)
# 固定外部宿主/实际runtime DLL与库；只运行仓库原创fixture。没有地图/安装写入接口。
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$scriptPath = Join-Path $PSScriptRoot 'slk_creation_oracle.lua'
$fixturePath = Join-Path $repoRoot 'tests/fixtures/slk_creation_oracle_cases.lua'
$expectedPath = Join-Path $repoRoot 'tests/fixtures/slk_creation_oracle_expected.json'
$evidenceRoot = Join-Path $repoRoot 'build/accept/slk-creation-oracle-20260926'
$runPath = Join-Path $evidenceRoot ('run-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff'))
[IO.Directory]::CreateDirectory($runPath) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
function FullFile([string]$path) {
    $item = Get-Item -LiteralPath $path
    if ($item.PSIsContainer) { throw "Not a file: $path" }
    return $item.FullName
}
function Identity([string]$path) {
    $item = Get-Item -LiteralPath $path
    return [ordered]@{path=$item.FullName;byteCount=$item.Length;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
}
function EqualPath([string]$first, [string]$second) {
    return [string]::Equals([IO.Path]::GetFullPath($first), [IO.Path]::GetFullPath($second), [StringComparison]::OrdinalIgnoreCase)
}
$receipt = [ordered]@{
    schema='slk-creation-original-oracle-v1'; coverage='OriginalInjectedOldNewFillRefreshOnly'
    expectedProvenance='AuthoredExpectationsOnly'; status='not-started'; startedUtc=[DateTime]::UtcNow.ToString('o'); completedUtc=$null
    host=$null; runtime=$null; library=$null; scripts=@(); modules=@(); processId=$null; moduleIdentityVerified=$false
    luaVersion=$null; isolatedLibraryLoads=0; exitCode=$null; caseCount=0; failedExpectations=$null; expectationsMatched=$false; cases=@(); error=$null
}
$process = $null; $started = $false
$leases = [Collections.Generic.List[IDisposable]]::new()
try {
    $LuaHost=FullFile $LuaHost; $LuaRuntime=FullFile $LuaRuntime; $SlkLibrary=FullFile $SlkLibrary
    $required = @(
        @{role='host';path=$LuaHost;expected=$ExpectedHostSha256;module=$true},
        @{role='runtime';path=$LuaRuntime;expected=$ExpectedRuntimeSha256;module=$true},
        @{role='library';path=$SlkLibrary;expected=$ExpectedLibrarySha256;module=$false}
    )
    foreach ($source in $required) {
        $leases.Add([IO.File]::Open($source.path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read))
        $value = Identity $source.path
        if ($value.sha256 -ne $source.expected) { throw "Pinned SHA mismatch: $($source.role)" }
        $receipt[$source.role]=$value
    }
    foreach ($path in @($PSCommandPath,$scriptPath,$fixturePath,$expectedPath)) {
        $leases.Add([IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read))
        $receipt.scripts += Identity $path
    }
    $expected = Get-Content -LiteralPath $expectedPath -Raw -Encoding utf8 | ConvertFrom-Json
    if ($expected.schema -ne 'slk-creation-authored-expectations-v1' -or $expected.provenance -ne 'AuthoredExpectationsOnly' -or
        $expected.targetLibrarySha256 -ne $ExpectedLibrarySha256 -or $expected.fixtureSha256 -ne $receipt.scripts[2].sha256) {
        throw 'Authored expectations do not bind this library and original fixture'
    }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$LuaHost; $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.WorkingDirectory=$runPath
    $start.RedirectStandardInput=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.StandardOutputEncoding=$utf8; $start.StandardErrorEncoding=$utf8
    foreach ($key in @($start.Environment.Keys)) {
        if ($key.StartsWith('LUA',[StringComparison]::OrdinalIgnoreCase)) { $start.Environment.Remove($key) | Out-Null }
    }
    foreach ($argument in @('-E',$scriptPath,$SlkLibrary,$fixturePath)) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo=$start
    if (-not $process.Start()) { throw 'Could not start pinned Lua host' }
    $started=$true; $receipt.processId=$process.Id
    $stderrTask=$process.StandardError.ReadToEndAsync()
    $readyTask=$process.StandardOutput.ReadLineAsync()
    if (-not $readyTask.Wait(10000) -or $readyTask.Result -ne 'SLK_CREATION_ORACLE_READY') { throw 'Target did not reach READY within 10 seconds' }
    $process.Refresh()
    $receipt.modules=@($process.Modules | ForEach-Object { [ordered]@{name=$_.ModuleName;path=$_.FileName;memoryBytes=$_.ModuleMemorySize} })
    foreach ($source in $required | Where-Object { $_.module }) {
        $loaded=@($receipt.modules | Where-Object { EqualPath $_.path $source.path })
        if ($loaded.Count -ne 1) { throw "Actual module identity mismatch: $($source.role)" }
        $actual=Identity $loaded[0].path
        if ($actual.sha256 -ne $source.expected) { throw "Actual module SHA mismatch: $($source.role)" }
        $receipt[$source.role].loadedPath=$loaded[0].path; $receipt[$source.role].loadedSha256=$actual.sha256
    }
    $receipt.moduleIdentityVerified=$true
    $stdoutTask=$process.StandardOutput.ReadToEndAsync()
    $process.StandardInput.WriteLine('RUN'); $process.StandardInput.Flush(); $process.StandardInput.Close()
    if (-not $process.WaitForExit(15000)) { throw 'Original cases exceeded 15 seconds' }
    $stdout=$stdoutTask.GetAwaiter().GetResult(); $stderr=$stderrTask.GetAwaiter().GetResult()
    # Lua侧先约束节点、每步/每case/总输出；此处再次检查完整流，不截断伪造通过。
    if ($utf8.GetByteCount($stdout) -gt 16*1024*1024 -or $utf8.GetByteCount($stderr) -gt 65536) { throw 'Oracle output exceeds receipt budget' }
    [IO.File]::WriteAllText((Join-Path $runPath 'typed-results.ndjson'),$stdout,$utf8)
    [IO.File]::WriteAllText((Join-Path $runPath 'stderr.log'),$stderr,$utf8)
    $receipt.exitCode=$process.ExitCode
    $rows=@($stdout -split '\r?\n' | Where-Object { $_.Length -gt 0 } | ForEach-Object { ConvertFrom-Json -InputObject $_ -Depth 64 })
    $summaries=@($rows | Where-Object { $_.PSObject.Properties.Name -contains 'summary' })
    $cases=@($rows | Where-Object { $_.PSObject.Properties.Name -contains 'caseId' })
    if ($summaries.Count -ne 1 -or $summaries[0].coverage -ne $receipt.coverage -or $summaries[0].caseCount -ne $cases.Count -or
        $summaries[0].isolatedLibraryLoads -ne $cases.Count -or $expected.cases.Count -ne $cases.Count) { throw 'Incomplete case/module-state output' }
    $receipt.caseCount=$cases.Count; $receipt.failedExpectations=$summaries[0].failedExpectations
    $receipt.luaVersion=$summaries[0].luaVersion; $receipt.isolatedLibraryLoads=$summaries[0].isolatedLibraryLoads
    for ($index=0; $index -lt $cases.Count; ++$index) {
        $actual=$cases[$index]; $wanted=$expected.cases[$index]
        if ($actual.caseId -ne $wanted.id -or $actual.cppPolicy -ne $wanted.cppPolicy -or $actual.operationCount -ne $wanted.operations -or
            $actual.checkCount -ne $wanted.checks -or $actual.steps.Count -ne $actual.operationCount+2 -or
            $actual.saveCount -ne $wanted.save -or $actual.removeCount -ne $wanted.remove -or $actual.errorCount -ne $wanted.errors -or
            -not $actual.executionOk -or -not $actual.expectationPassed) { throw "Authored expectation mismatch: $($actual.caseId)" }
        for ($step=0; $step -lt $actual.steps.Count; ++$step) {
            if ($actual.steps[$step].step -ne $step -or -not $actual.steps[$step].ok) { throw "Missing/failed step in $($actual.caseId)" }
        }
        $receipt.cases += [ordered]@{id=$actual.caseId;cppPolicy=$actual.cppPolicy;steps=$actual.steps.Count;checks=$actual.checkCount;
            save=$actual.saveCount;remove=$actual.removeCount;errors=$actual.errorCount;expectationPassed=$actual.expectationPassed}
    }
    $receipt.expectationsMatched=$true
    foreach ($source in $required) {
        $after=Identity $source.path; $receipt[$source.role].afterSha256=$after.sha256
        if ($after.sha256 -ne $source.expected) { throw "Pinned source changed during run: $($source.role)" }
    }
    foreach ($script in $receipt.scripts) {
        if ((Identity $script.path).sha256 -ne $script.sha256) { throw "Original harness changed during run: $($script.path)" }
    }
    $receipt.status=if ($receipt.exitCode -eq 0 -and $receipt.failedExpectations -eq 0 -and $stderr.Length -eq 0) { 'passed' } else { 'fixture-expectation-failed' }
} catch {
    $receipt.status='failed'; $receipt.error=$_.Exception.Message
} finally {
    if ($null -ne $process) {
        if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
        $process.Dispose()
    }
    foreach ($lease in $leases) { $lease.Dispose() }
    $receipt.completedUtc=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText((Join-Path $runPath 'report.json'),($receipt | ConvertTo-Json -Depth 20),$utf8)
}
Write-Output ("status={0} cases={1} failedExpectations={2} moduleIdentityVerified={3}" -f $receipt.status,$receipt.caseCount,$receipt.failedExpectations,$receipt.moduleIdentityVerified)
Write-Output (Join-Path $runPath 'report.json')
if ($receipt.error) { Write-Output $receipt.error }
if ($receipt.status -ne 'passed') { exit 1 }
