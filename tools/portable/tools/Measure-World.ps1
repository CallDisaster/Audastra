# 作者主动启动的可见基线；只复制地图，不向其他窗口发送输入，不自动保存地图。
[CmdletBinding()]
param([string]$Map, [string]$GameRoot, [string]$UiRoot, [string]$OutputRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# 脚本位于便携包的 tools 子目录，程序与清单在上一级。
$packageRoot = Split-Path -Parent $PSScriptRoot
if (!$Map) { $Map = Read-Host '地图文件路径（会自动创建独立副本）' }
if (!$GameRoot) { $GameRoot = Read-Host '游戏资源目录（含 war3.mpq）' }
$sourceMap = (Resolve-Path -LiteralPath $Map).Path
$game = (Resolve-Path -LiteralPath $GameRoot).Path
if (!(Test-Path -LiteralPath $sourceMap -PathType Leaf)) { throw 'Map is not a file' }
if (!(Test-Path -LiteralPath $game -PathType Container)) { throw 'Game root is not a directory' }
$editor = Join-Path $packageRoot 'bin/war3-map-editor.exe'
if (!(Test-Path -LiteralPath $editor)) { throw 'Complete portable GUI missing' }
if (!$OutputRoot) { $OutputRoot = Join-Path $packageRoot 'measurements' }
$run = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
[void][IO.Directory]::CreateDirectory((Join-Path $run 'input'))
$mapCopy = Join-Path $run ('input/map' + [IO.Path]::GetExtension($sourceMap))
$before = (Get-FileHash -LiteralPath $sourceMap -Algorithm SHA256).Hash
Copy-Item -LiteralPath $sourceMap -Destination $mapCopy
$copyHash = (Get-FileHash -LiteralPath $mapCopy -Algorithm SHA256).Hash
if ($before -ne $copyHash) { throw 'Input copy hash mismatch; editor was not started' }
$identity = [ordered]@{ source_path=$sourceMap; source_sha256=$before; copy_path=$mapCopy; copy_sha256=$copyHash;
    game_root=$game; ui_root=$UiRoot; mode='author-visible-passive'; original_read_only=$true;
    editor_sha256=(Get-FileHash -LiteralPath $editor -Algorithm SHA256).Hash;
    renderer_sha256=(Get-FileHash -LiteralPath (Join-Path $packageRoot 'bin/war3-render-d3d11.dll') -Algorithm SHA256).Hash }
if (Test-Path -LiteralPath (Join-Path $packageRoot 'manifest.json')) {
    $identity['source_commit'] = (Get-Content -LiteralPath (Join-Path $packageRoot 'manifest.json') -Raw | ConvertFrom-Json).source_commit
}
$identity | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'input-identity.json') -Encoding utf8
$editorArgs = @('--open-map', $mapCopy, '--game-root', $game, '--measure-world', (Join-Path $run 'recording'))
if ($UiRoot) { $editorArgs += @('--ui-root', (Resolve-Path -LiteralPath $UiRoot).Path) }
Write-Host "测量目录：$run"
Write-Host '世界加载后选择活动视区，确认两个复选框，再开始记录。连续操作约十分钟后停止并导出。'
& $editor @editorArgs
$processExit = $LASTEXITCODE
$identity['editor_exit_code'] = $processExit
$identity['source_sha256_after'] = (Get-FileHash -LiteralPath $sourceMap -Algorithm SHA256).Hash
$identity['source_unchanged'] = ($before -eq $identity['source_sha256_after'])
$identity['copy_sha256_after'] = (Get-FileHash -LiteralPath $mapCopy -Algorithm SHA256).Hash
$identity | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'input-identity.json') -Encoding utf8
Write-Host "记录保存在 $run；原图未变：$($identity['source_unchanged'])；进程退出码：$processExit"
exit $processExit
