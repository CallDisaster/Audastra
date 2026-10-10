[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Log,
    [string[]]$Arguments = @(),
    [switch]$RequireGui
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/quiet-runtime.ps1"
Initialize-ForgeQuietRuntime
$previousMode = [ForgeQuietRuntimeNative]::SetErrorMode(0x8007)
$previousPlatform = $env:QT_QPA_PLATFORM
$started = [DateTime]::UtcNow
$actualExit = -1
$launched = $false
$ownedProcessId = $null
try {
    $executablePath = (Resolve-Path -LiteralPath $Executable).Path
    Assert-ForgeAppLocalRuntime -Directory (Split-Path -Parent $executablePath) -RequireGui:$RequireGui *> "$Log.preflight.log"
    if ($RequireGui) { $env:QT_QPA_PLATFORM = 'offscreen' }
    # WIN32 GUI 程序不保证 PowerShell 的 & 调用同步等待或设置 LASTEXITCODE。
    # 使用实际 Process 对象，Unicode 参数不经 shell 拼接，异步读两条管道避免互相堵塞。
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $executablePath
    $startInfo.WorkingDirectory = (Get-Location).Path
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$startInfo.ArgumentList.Add($argument) }
    $ownedProcess = [Diagnostics.Process]::new()
    $ownedProcess.StartInfo = $startInfo
    try {
        if (!$ownedProcess.Start()) { throw 'Executable process not started' }
        $launched = $true
        $ownedProcessId = $ownedProcess.Id
        $stdoutTask = $ownedProcess.StandardOutput.ReadToEndAsync()
        $stderrTask = $ownedProcess.StandardError.ReadToEndAsync()
        $ownedProcess.WaitForExit()
        $actualExit = $ownedProcess.ExitCode
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText([IO.Path]::GetFullPath($Log), $stdout + $stderr, [Text.UTF8Encoding]::new($false))
    } finally { $ownedProcess.Dispose() }
} finally {
    $env:QT_QPA_PLATFORM = $previousPlatform
    [void][ForgeQuietRuntimeNative]::SetErrorMode($previousMode)
    [ordered]@{ started_utc=$started.ToString('o'); ended_utc=[DateTime]::UtcNow.ToString('o');
        actual_exit=$actualExit; launch_attempted=$launched; pid=$ownedProcessId; log=[IO.Path]::GetFullPath($Log);
        preflight_log=[IO.Path]::GetFullPath("$Log.preflight.log"); gui_platform='offscreen' } |
        ConvertTo-Json | Set-Content -LiteralPath "$Log.receipt.json" -Encoding utf8
}
Write-Output "Executable actual exit=$actualExit"
Get-Content -LiteralPath $Log -Tail 40
exit $actualExit
