[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$LuaHost,
    [Parameter(Mandatory)][string]$LuaRuntime,
    [Parameter(Mandatory)][string]$ModulesRoot,
    [Parameter(Mandatory)][string]$CoreRoot,
    [Parameter(Mandatory)][string]$InstalledRoot,
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Locale = 'zh-CN',
    [ValidatePattern('^[A-Za-z0-9_.-]+$')][string]$DataId = 'zhCN-1.24.4',
    [ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedSourceManifestSha256,
    [switch]$PrepareOnly
)
# PowerShell 7；仅原创 fixture + 锁定的已安装工具/资源。不会写安装目录或地图。
# backend sandbox 是目标模块加载器，不是未知 Lua 的文件/网络安全边界。
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$luaScript = Join-Path $PSScriptRoot 'slk_transaction_oracle.lua'
$fixturePath = Join-Path $repoRoot 'tests/fixtures/slk_transaction_cases.lua'
$hostLua = Join-Path $repoRoot 'src/ScriptBuild/LuaTemplateHost.lua'
$evidenceRoot = Join-Path $repoRoot 'build/accept/slk-transaction-oracle-20260926'
$runPath = Join-Path $evidenceRoot ('run-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff'))
[IO.Directory]::CreateDirectory($runPath) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$leases = [Collections.Generic.List[IDisposable]]::new()
$files = [Collections.Generic.List[object]]::new()
$known = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$oracleProcess = $null
$stderrTask = $null
$started = $false
$receipt = [ordered]@{
    schema='slk-transaction-original-oracle-v1'; startedUtc=[DateTime]::UtcNow.ToString('o'); completedUtc=$null
    status='not-started'; coverage='OriginalRestrictedWholeChunkTransactions'; error=$null
    processId=$null; moduleIdentityVerified=$false; sourceManifestSha256=$null; sources=@(); modules=@()
    sourceBytes=0L; caseCount=0; failedExpectations=$null; exitCode=$null; cases=@()
}
function FullPath([string]$path, [bool]$directory) {
    $item = Get-Item -LiteralPath $path
    if ($item.PSIsContainer -ne $directory -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Wrong file kind or reparse point: $path"
    }
    return $item.FullName.TrimEnd('\')
}
function SamePath([string]$a,[string]$b) {
    return [string]::Equals([IO.Path]::GetFullPath($a),[IO.Path]::GetFullPath($b),[StringComparison]::OrdinalIgnoreCase)
}
function AddSource([string]$path,[string]$role) {
    $path = FullPath $path $false
    if (-not $known.Add($path)) { return }
    $item = Get-Item -LiteralPath $path
    if ($files.Count -ge 384 -or $item.Length -gt 8MB -or $receipt.sourceBytes + $item.Length -gt 32MB) {
        throw 'Source count/size budget exceeded (384 files, 8 MiB/file, 32 MiB total)'
    }
    $leases.Add([IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read))
    $entry = [ordered]@{role=$role;path=$path;bytes=$item.Length;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
    $receipt.sourceBytes += $item.Length
    $files.Add($entry)
}
function LuaString([string]$value) {
    # 数字转义保留 UTF-8 字节，避免路径/换行成为 Lua 源码。
    $parts = foreach ($byte in $utf8.GetBytes($value)) { '\' + $byte.ToString('000') }
    return '"' + ($parts -join '') + '"'
}
function SnapshotModules($process) {
    $process.Refresh()
    return @($process.Modules | ForEach-Object {
        [ordered]@{name=$_.ModuleName;path=$_.FileName;memoryBytes=$_.ModuleMemorySize;
            sha256=(Get-FileHash -LiteralPath $_.FileName -Algorithm SHA256).Hash}
    })
}
try {
    $LuaHost=FullPath $LuaHost $false; $LuaRuntime=FullPath $LuaRuntime $false
    $ModulesRoot=FullPath $ModulesRoot $true; $CoreRoot=FullPath $CoreRoot $true; $InstalledRoot=FullPath $InstalledRoot $true
    $sandbox = FullPath (Join-Path $CoreRoot '../backend/sandbox.lua') $false
    $templateLibrary = FullPath (Join-Path $InstalledRoot 'compiler/script/template.lua') $false
    $slkLibrary = FullPath (Join-Path $InstalledRoot 'compiler/script/w3x2lni/slk_lib.lua') $false
    $required = @(
        @{role='host';path=$LuaHost;sha='2237CE0004D7B8DED7804BF78206FB72988C7149B3D38E0F7469142D2FB19208'},
        @{role='runtime';path=$LuaRuntime;sha='27269D53120BB25EE8210BB6DE453D0BA3795015FF46332DED79D4531D1B5E77'},
        @{role='lni';path=(Join-Path $ModulesRoot 'lni.dll');sha='84CEBB73E348C457C88A24A4BCC3A9F037CFC699429533558B498291A44F0E54'},
        @{role='w3xparser';path=(Join-Path $ModulesRoot 'w3xparser.dll');sha='95C092EBCBAE69C35D617191F730B1E7F40EF17498FF20B310A7FCDA18C1F0DD'},
        @{role='lpeg';path=(Join-Path $ModulesRoot 'lpeg.dll');sha='8183FB83D22AADB8721CBCA45E16820EA859606F456B41496EF8FB4BE6E20A61'},
        @{role='lml';path=(Join-Path $ModulesRoot 'lml.dll');sha='B5319AD3FB15305C1315532229FA6B426346AB870A876D9F4919B2AC9D4B0B2B'}
    )
    foreach ($item in $required) {
        AddSource $item.path $item.role
        if ((Get-FileHash -LiteralPath $item.path -Algorithm SHA256).Hash -ne $item.sha) { throw "Pinned binary mismatch: $($item.role)" }
    }
    foreach ($path in @($PSCommandPath,$luaScript,$fixturePath,$hostLua)) { AddSource $path 'original-oracle' }
    AddSource $sandbox 'target-loader'
    AddSource $templateLibrary 'target-template'
    AddSource $slkLibrary 'target-slk-library'
    if ((Get-FileHash -LiteralPath $templateLibrary -Algorithm SHA256).Hash -ne 'A90B1D795038B25EAE14C47F1DB1B0102DBCCFAA5DB61C25E394D1B6B2B39085' -or
        (Get-FileHash -LiteralPath $slkLibrary -Algorithm SHA256).Hash -ne '9BA2C9B8E5B53005CF5714E5188E533186D97F770C398E47E998B2B3D3CC74F1') {
        throw 'Fixed template/slk library identity differs; upvalue inspection is not valid for another version'
    }
    # 核心目录很小；锁住全部候选模块及定义，而不是先猜动态 require 的子集。
    foreach ($item in Get-ChildItem -LiteralPath $CoreRoot -Recurse -File) {
        if ($item.Extension -in '.lua','.ini','.txt') { AddSource $item.FullName 'target-core' }
    }
    $prebuilt = FullPath (Join-Path $InstalledRoot "plugin/w3x2lni/data/$DataId/prebuilt") $true
    foreach ($version in 'Custom','Melee') {
        foreach ($name in 'unit','ability','item','buff','upgrade','doodad','destructable','misc','txt') {
            AddSource (Join-Path $prebuilt "$version/$name.ini") 'prebuilt'
        }
    }
    AddSource (Join-Path $prebuilt 'keydata.ini') 'prebuilt'
    $share = FullPath (Join-Path $InstalledRoot "share/$Locale/mpq") $true
    # 仅展开的文本数据，未枚举纹理、游戏档案，也不复制安装内容。
    foreach ($item in Get-ChildItem -LiteralPath $share -Recurse -File) {
        if ($item.Extension -in '.txt','.slk') { AddSource $item.FullName 'installed-mpq-text' }
    }
    $sorted = @($files | Sort-Object path)
    $receipt.sources=$sorted
    $manifestText = ($sorted | ForEach-Object { "$($_.role)`t$($_.path)`t$($_.bytes)`t$($_.sha256)" }) -join "`n"
    $digest = [Security.Cryptography.SHA256]::HashData($utf8.GetBytes($manifestText))
    $receipt.sourceManifestSha256=[Convert]::ToHexString($digest)
    if ($PrepareOnly) { $receipt.status='prepared-no-execution' }
    else {
        if (-not $ExpectedSourceManifestSha256 -or $ExpectedSourceManifestSha256 -ne $receipt.sourceManifestSha256) {
            throw 'Source manifest not explicitly pinned; run -PrepareOnly, inspect report, then pass its exact SHA'
        }
        $moduleEntries = foreach ($item in $required | Where-Object {$_.role -notin 'host','runtime'}) {
            '['+(LuaString $item.role)+']='+(LuaString ([IO.Path]::GetFullPath($item.path)))
        }
        $sourceEntries = foreach ($item in $sorted) {
            '{path='+(LuaString $item.path)+',sha256='+(LuaString $item.sha256)+',bytes='+$item.bytes+'}'
        }
        $manifestLua = 'return {coreRoot='+(LuaString $CoreRoot)+',corePrefix='+(LuaString ($CoreRoot.ToLowerInvariant()+'\'))+
            ',installedRoot='+(LuaString $InstalledRoot)+',installedPrefix='+(LuaString ($InstalledRoot.ToLowerInvariant()+'\'))+
            ',sandbox='+(LuaString $sandbox)+',hostLua='+(LuaString $hostLua)+
            ',templateLibrary='+(LuaString $templateLibrary)+',slkLibrary='+(LuaString $slkLibrary)+
            ',locale='+(LuaString $Locale)+',dataId='+(LuaString $DataId)+
            ',modules={'+($moduleEntries -join ',')+'},files={'+($sourceEntries -join ',')+'}}'
        $manifestPath=Join-Path $runPath 'locked-inputs.lua'
        [IO.File]::WriteAllText($manifestPath,$manifestLua,$utf8)
        $leases.Add([IO.File]::Open($manifestPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read))
        $start=[Diagnostics.ProcessStartInfo]::new()
        $start.FileName=$LuaHost; $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.WorkingDirectory=$runPath
        $start.RedirectStandardInput=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
        $start.StandardOutputEncoding=$utf8; $start.StandardErrorEncoding=$utf8
        foreach ($key in @($start.Environment.Keys)) {
            if ($key.StartsWith('LUA',[StringComparison]::OrdinalIgnoreCase)) { $start.Environment.Remove($key) | Out-Null }
        }
        foreach ($value in @('-E',$luaScript,$manifestPath,$fixturePath)) { $start.ArgumentList.Add($value) }
        $oracleProcess=[Diagnostics.Process]::new(); $oracleProcess.StartInfo=$start
        if (-not $oracleProcess.Start()) { throw 'Could not start pinned host' }
        $started=$true; $receipt.processId=$oracleProcess.Id
        $stderrTask=$oracleProcess.StandardError.ReadToEndAsync()
        $readyTask=$oracleProcess.StandardOutput.ReadLineAsync()
        if (-not $readyTask.Wait(10000) -or $readyTask.Result -ne 'SLK_TRANSACTION_ORACLE_READY') { throw 'Module identity handshake did not arrive in 10 seconds' }
        $before=SnapshotModules $oracleProcess; $receipt.modules=$before
        foreach ($item in $required) {
            $loaded=@($before | Where-Object { SamePath $_.path $item.path })
            if ($loaded.Count -ne 1 -or $loaded[0].sha256 -ne $item.sha) { throw "Actual module identity differs: $($item.role)" }
        }
        $receipt.moduleIdentityVerified=$true
        $stdoutTask=$oracleProcess.StandardOutput.ReadToEndAsync()
        $oracleProcess.StandardInput.WriteLine('RUN'); $oracleProcess.StandardInput.Flush(); $oracleProcess.StandardInput.Close()
        # 仅固定源码+原创有界输入。此开发runner不代替未来C++启动器的Job/身份/硬内存监督。
        if (-not $oracleProcess.WaitForExit(60000)) { throw 'Wholechunk transaction oracle exceeded 60 seconds' }
        $stdout=$stdoutTask.GetAwaiter().GetResult(); $stderr=$stderrTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $runPath 'typed-results.ndjson'),$stdout,$utf8)
        [IO.File]::WriteAllText((Join-Path $runPath 'stderr.log'),$stderr,$utf8)
        $receipt.exitCode=$oracleProcess.ExitCode
        if ($utf8.GetByteCount($stdout) -gt 4MB -or $utf8.GetByteCount($stderr) -gt 64KB) { throw 'Bounded original oracle produced unexpectedly large output' }
        $rows=@($stdout -split '\r?\n' | Where-Object {$_} | ForEach-Object {$_ | ConvertFrom-Json})
        $summaries=@($rows | Where-Object {$_.PSObject.Properties.Name -contains 'summary'})
        $caseRows=@($rows | Where-Object {$_.PSObject.Properties.Name -contains 'caseId'})
        if ($summaries.Count -ne 1 -or $summaries[0].caseCount -ne 32 -or $caseRows.Count -ne 32) { throw 'Incomplete original transaction output' }
        $receipt.caseCount=$caseRows.Count; $receipt.failedExpectations=$summaries[0].failedExpectations
        $receipt.cases=@($caseRows | ForEach-Object {
            [ordered]@{id=$_.caseId;passed=$_.passed;physicalReads=$_.physicalReads;physicalBytes=$_.physicalBytes}
        })
        foreach ($entry in $sorted) {
            if ((Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Locked source changed: $($entry.path)" }
        }
        $receipt.status=if ($receipt.exitCode -eq 0 -and $receipt.failedExpectations -eq 0 -and $stderr.Length -eq 0) {'passed'} else {'fixture-expectation-failed'}
    }
} catch {
    $receipt.status='failed'; $receipt.error=$_.Exception.Message
} finally {
    if ($null -ne $oracleProcess) {
        if ($started -and -not $oracleProcess.HasExited) { $oracleProcess.Kill(); $oracleProcess.WaitForExit(5000) | Out-Null }
        if ($null -ne $stderrTask -and $stderrTask.IsCompleted) {
            [IO.File]::WriteAllText((Join-Path $runPath 'stderr.log'),$stderrTask.GetAwaiter().GetResult(),$utf8)
        }
        $oracleProcess.Dispose()
    }
    foreach ($lease in $leases) { $lease.Dispose() }
    $receipt.completedUtc=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText((Join-Path $runPath 'report.json'),($receipt | ConvertTo-Json -Depth 20),$utf8)
}
Write-Output ("status={0} sources={1} sourceManifestSha256={2} cases={3}" -f $receipt.status,$receipt.sources.Count,$receipt.sourceManifestSha256,$receipt.caseCount)
Write-Output (Join-Path $runPath 'report.json')
if ($receipt.error) { Write-Output $receipt.error }
if ($receipt.status -notin 'passed','prepared-no-execution') { exit 1 }
