[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$LuaHost,
    [Parameter(Mandatory)][string]$LuaRuntime,
    [Parameter(Mandatory)][string]$LniDll,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedHostSha256,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedRuntimeSha256,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedLniSha256
)
# 只加载明确指定且固定 SHA 的本机工具；不复制 DLL、不执行地图、不改安装或系统环境。
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$luaScript = Join-Path $PSScriptRoot 'slk_lni_oracle.lua'
$fixturePath = Join-Path $repoRoot 'tests/fixtures/slk_lni_oracle_cases.lua'
$goldenPath = Join-Path $repoRoot 'tests/fixtures/slk_lni_oracle_expected.json'
$evidenceRoot = Join-Path $repoRoot 'build/accept/slk-lni-oracle-20260926'
$runPath = Join-Path $evidenceRoot ('run-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff'))
[IO.Directory]::CreateDirectory($runPath) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
function FullFile([string]$path) {
    $item = Get-Item -LiteralPath $path
    if ($item.PSIsContainer) { throw "Not a file: $path" }
    return $item.FullName
}
function FileIdentity([string]$path) {
    $item = Get-Item -LiteralPath $path
    return [ordered]@{ path=$item.FullName; byteCount=$item.Length; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
}
function EqualPath([string]$first, [string]$second) {
    return [string]::Equals([IO.Path]::GetFullPath($first), [IO.Path]::GetFullPath($second), [StringComparison]::OrdinalIgnoreCase)
}
$receipt = [ordered]@{
    schema='slk-lni-original-oracle-v1'; startedUtc=[DateTime]::UtcNow.ToString('o'); completedUtc=$null
    coverage='OriginalInputsOnly'; status='not-started'; moduleIdentityVerified=$false; processId=$null
    host=$null; runtime=$null; lni=$null; scripts=@(); moduleSnapshot=@(); exitCode=$null
    caseCount=0; failedExpectations=$null; goldenMatched=$false; cases=@(); error=$null
}
$oracleProcess = $null
$processStarted = $false
$leases = [Collections.Generic.List[IDisposable]]::new()
try {
    $LuaHost = FullFile $LuaHost; $LuaRuntime = FullFile $LuaRuntime; $LniDll = FullFile $LniDll
    $required = @(
        @{role='host';path=$LuaHost;expected=$ExpectedHostSha256},
        @{role='runtime';path=$LuaRuntime;expected=$ExpectedRuntimeSha256},
        @{role='lni';path=$LniDll;expected=$ExpectedLniSha256}
    )
    foreach ($source in $required) {
        # 保持只读租约直到子进程退出，防止核验与实际加载之间文件被替换。
        $leases.Add([IO.File]::Open($source.path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read))
        $identity = FileIdentity $source.path
        if ($identity.sha256 -ne $source.expected) { throw "Pinned SHA mismatch for $($source.role): $($identity.sha256)" }
        $receipt[$source.role] = $identity
    }
    $receipt.scripts = @((FileIdentity $PSCommandPath), (FileIdentity $luaScript), (FileIdentity $fixturePath), (FileIdentity $goldenPath))
    $golden = Get-Content -LiteralPath $goldenPath -Raw -Encoding utf8 | ConvertFrom-Json
    if ($golden.schema -ne 'slk-lni-typed-golden-v1' -or $golden.targetLniSha256 -ne $ExpectedLniSha256 -or
        $golden.fixtureSha256 -ne $receipt.scripts[2].sha256) { throw 'Typed golden does not bind the selected DLL and original fixture' }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $LuaHost
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WorkingDirectory = $runPath
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = $utf8
    $start.StandardErrorEncoding = $utf8
    foreach ($key in @($start.Environment.Keys)) {
        if ($key.StartsWith('LUA', [StringComparison]::OrdinalIgnoreCase)) { $start.Environment.Remove($key) | Out-Null }
    }
    foreach ($argument in @('-E', $luaScript, $LniDll, $fixturePath)) { $start.ArgumentList.Add($argument) }
    $oracleProcess = [Diagnostics.Process]::new()
    $oracleProcess.StartInfo = $start
    if (-not $oracleProcess.Start()) { throw 'Could not start pinned Lua host' }
    $processStarted = $true
    $receipt.processId = $oracleProcess.Id
    $stderrTask = $oracleProcess.StandardError.ReadToEndAsync()
    $readyTask = $oracleProcess.StandardOutput.ReadLineAsync()
    if (-not $readyTask.Wait(10000) -or $readyTask.Result -ne 'SLK_LNI_ORACLE_READY') {
        throw 'LNI module did not reach identity handshake in 10 seconds'
    }
    $oracleProcess.Refresh()
    $modules = @($oracleProcess.Modules | ForEach-Object {
        [ordered]@{name=$_.ModuleName;path=$_.FileName;memoryBytes=$_.ModuleMemorySize}
    })
    $receipt.moduleSnapshot = $modules
    foreach ($source in $required) {
        $matches = @($modules | Where-Object { EqualPath $_.path $source.path })
        if ($matches.Count -ne 1) { throw "Expected one actual loaded module for $($source.role), observed $($matches.Count)" }
        $observed = FileIdentity $matches[0].path
        if ($observed.sha256 -ne $source.expected) { throw "Actual loaded module SHA mismatch for $($source.role)" }
        $receipt[$source.role].loadedPath = $matches[0].path
        $receipt[$source.role].loadedSha256 = $observed.sha256
    }
    $receipt.moduleIdentityVerified = $true
    $stdoutTask = $oracleProcess.StandardOutput.ReadToEndAsync()
    $oracleProcess.StandardInput.WriteLine('RUN')
    $oracleProcess.StandardInput.Flush()
    $oracleProcess.StandardInput.Close()
    if (-not $oracleProcess.WaitForExit(10000)) { throw 'Original fixture run exceeded 10 seconds' }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $runPath 'typed-results.ndjson'), $stdout, $utf8)
    [IO.File]::WriteAllText((Join-Path $runPath 'stderr.log'), $stderr, $utf8)
    $receipt.exitCode = $oracleProcess.ExitCode
    $rows = @($stdout -split '\r?\n' | Where-Object { $_.Length -gt 0 } | ForEach-Object { ConvertFrom-Json -InputObject $_ })
    $summaries = @($rows | Where-Object { $_.PSObject.Properties.Name -contains 'summary' })
    $caseRows = @($rows | Where-Object { $_.PSObject.Properties.Name -contains 'caseId' })
    if ($summaries.Count -ne 1 -or $summaries[0].caseCount -ne $caseRows.Count) { throw 'Incomplete typed output' }
    $receipt.caseCount = $caseRows.Count
    $receipt.failedExpectations = $summaries[0].failedExpectations
    $receipt.luaVersion = $summaries[0].luaVersion
    $receipt.cases = @($caseRows | ForEach-Object {
        [ordered]@{id=$_.caseId;cppPolicy=$_.cppPolicy;parseOk=$_.parseOk;expectationPassed=$_.expectationPassed;failures=$_.failures}
    })
    if ($golden.cases.Count -ne $caseRows.Count) { throw 'Typed golden case count differs' }
    for ($index = 0; $index -lt $caseRows.Count; ++$index) {
        $actual = $caseRows[$index]
        $expected = $golden.cases[$index]
        if ($actual.caseId -ne $expected.caseId -or $actual.cppPolicy -ne $expected.cppPolicy -or
            $actual.inputHex -ne $expected.inputHex -or $actual.parseOk -ne $expected.parseOk -or
            (ConvertTo-Json -InputObject $actual.typed -Depth 64 -Compress) -cne (ConvertTo-Json -InputObject $expected.typed -Depth 64 -Compress)) {
            throw "Typed golden mismatch: $($actual.caseId)"
        }
    }
    $receipt.goldenMatched = $true
    foreach ($source in $required) {
        $after = FileIdentity $source.path
        $receipt[$source.role].afterSha256 = $after.sha256
        if ($after.sha256 -ne $source.expected) { throw "Pinned source changed during run: $($source.role)" }
    }
    foreach ($script in $receipt.scripts) {
        $after = FileIdentity $script.path
        if ($after.sha256 -ne $script.sha256) { throw "Oracle script changed during run: $($script.path)" }
    }
    $receipt.status = if ($receipt.exitCode -eq 0 -and $receipt.failedExpectations -eq 0 -and $stderr.Length -eq 0) { 'passed' } else { 'fixture-expectation-failed' }
} catch {
    $receipt.status = 'failed'
    $receipt.error = $_.Exception.Message
} finally {
    if ($null -ne $oracleProcess) {
        if ($processStarted -and -not $oracleProcess.HasExited) { $oracleProcess.Kill(); $oracleProcess.WaitForExit(5000) | Out-Null }
        $oracleProcess.Dispose()
    }
    foreach ($lease in $leases) { $lease.Dispose() }
    $receipt.completedUtc = [DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText((Join-Path $runPath 'report.json'), ($receipt | ConvertTo-Json -Depth 20), $utf8)
}
Write-Output ("status={0} cases={1} failedExpectations={2} moduleIdentityVerified={3}" -f $receipt.status,$receipt.caseCount,$receipt.failedExpectations,$receipt.moduleIdentityVerified)
Write-Output (Join-Path $runPath 'report.json')
if ($receipt.error) { Write-Output $receipt.error }
if ($receipt.status -ne 'passed') { exit 1 }
