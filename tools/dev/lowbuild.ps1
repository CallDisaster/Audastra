# Low-priority build: BelowNormal priority (inherited by MSBuild/cl), few nodes, one cl per project.
# Usage: tools/dev/lowbuild.ps1 [-Targets 'a,b'] [-Configure] [-Log path] [-Nodes 1] [-BuildDir path]
param([string[]]$Targets = @(), [string]$Log = '', [int]$Nodes = 1, [switch]$Configure, [string]$BuildDir = '')
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$buildDir = if ($BuildDir) { (Resolve-Path $BuildDir).Path } else { Join-Path $root 'build\vs2026' }
$cache = Join-Path $buildDir 'CMakeCache.txt'
if (-not (Test-Path $cache)) { throw "missing $cache; configure the selected build directory first" }
$cmake = ((Select-String -LiteralPath $cache -Pattern '^CMAKE_COMMAND:INTERNAL=').Line -split '=', 2)[1]
if (-not $Log) { $Log = Join-Path $buildDir 'lowbuild.log' }
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
$os = Get-CimInstance Win32_OperatingSystem
$commitFree = $os.FreeVirtualMemory / 1MB
# Pagefiles with an explicit maximum can still grow on demand; count that headroom too.
$growMB = 0
foreach ($setting in @(Get-CimInstance Win32_PageFileSetting -ErrorAction SilentlyContinue)) {
    if ($setting.MaximumSize -le 0) { continue }
    $usage = Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq $setting.Name }
    if ($usage) { $growMB += [math]::Max(0, $setting.MaximumSize - $usage.AllocatedBaseSize) }
}
"commit free {0:N1} GB (+{1:N1} GB pagefile growth), phys free {2:N1} GB" -f $commitFree, ($growMB / 1KB), ($os.FreePhysicalMemory / 1MB)
if ($commitFree + $growMB / 1KB -lt 3) { 'commit memory too low; aborting'; exit 9 }
if ($Configure) {
    $sourceDir = ((Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=').Line -split '=', 2)[1]
    & $cmake -S $sourceDir -B $buildDir *> "$Log.configure"
}
$buildArgs = @('--build', $buildDir, '--config', 'Release')
$names = @($Targets | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($names.Count) { $buildArgs += '--target'; $buildArgs += $names }
$buildArgs += @('--', "/m:$Nodes", '/nr:false', '/p:CL_MPCount=1', '/v:minimal')
& $cmake @buildArgs *> $Log
$actualBuildExit = $LASTEXITCODE
"exit=$actualBuildExit"
Select-String -Path $Log -Pattern ' error |error C|error LNK|warning C' | Select-Object -First 40 | ForEach-Object { $_.Line }
exit $actualBuildExit
