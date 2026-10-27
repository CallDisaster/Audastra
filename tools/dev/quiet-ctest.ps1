# 后台验证：先检查 app-local Qt/VC 依赖，系统加载错误只记退出码，避免前台弹窗。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$Log,
    [string]$Regex = '',
    [string]$Exclude = ''
)
$ErrorActionPreference = 'Stop'
$Log = [IO.Path]::GetFullPath($Log)
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$cache = Join-Path $build 'CMakeCache.txt'
$ctest = ((Select-String -LiteralPath $cache -Pattern '^CMAKE_CTEST_COMMAND:INTERNAL=').Line -split '=', 2)[1]
$bin = Join-Path $build 'Release'
. "$PSScriptRoot/quiet-runtime.ps1"
Initialize-ForgeQuietRuntime
$previousMode = [ForgeQuietRuntimeNative]::SetErrorMode(0x8007)
$previousPlatform = $env:QT_QPA_PLATFORM
$started = [DateTime]::UtcNow
$actualExit = -1
$launched = $false
try {
    Assert-ForgeAppLocalRuntime -Directory $bin -RequireGui *> "$Log.preflight.log"
    if (!(Test-Path -LiteralPath (Join-Path $bin 'Qt6Test.dll') -PathType Leaf)) { throw 'Missing Qt6Test.dll; tests not launched' }
    $env:QT_QPA_PLATFORM = 'offscreen'
    $arguments = @('--test-dir', $build, '-C', 'Release', '-j', '1', '--no-tests=error', '--timeout', '900', '--output-on-failure',
        '--test-output-size-passed', '1048576', '--output-junit', "$Log.xml")
    if ($Regex) { $arguments += @('-R', $Regex) }
    if ($Exclude) { $arguments += @('-E', $Exclude) }
    $launched = $true
    & $ctest @arguments *> $Log
    $actualExit = $LASTEXITCODE
} finally {
    $env:QT_QPA_PLATFORM = $previousPlatform
    [void][ForgeQuietRuntimeNative]::SetErrorMode($previousMode)
    [ordered]@{ started_utc=$started.ToString('o'); ended_utc=[DateTime]::UtcNow.ToString('o');
        actual_exit=$actualExit; regex=$Regex; exclude=$Exclude; log=[IO.Path]::GetFullPath($Log);
        launch_attempted=$launched; preflight_log=[IO.Path]::GetFullPath("$Log.preflight.log");
        xml=[IO.Path]::GetFullPath("$Log.xml") } |
        ConvertTo-Json | Set-Content -LiteralPath "$Log.receipt.json" -Encoding utf8
}
Write-Output "CTest actual exit=$actualExit"
Get-Content -LiteralPath $Log -Tail 60
exit $actualExit
