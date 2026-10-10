# 按 War3RenderCore.lock.json 取得共享渲染核心：克隆（或在已有干净检出中切换）到锁定提交。
# 默认放在本仓库相邻的 War3RenderCore 目录，即 CMake 的默认 WAR3_RENDER_CORE_SOURCE_DIR。
# 文件逐字节校验由 CMake 配置和每次构建完成；这里只负责把检出对到锁定提交。
# 用法：pwsh tools/fetch-render-core.ps1 [-Directory <路径>] [-Repository <地址>]
[CmdletBinding()]
param(
    [string]$Directory = '',
    [string]$Repository = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -Raw -LiteralPath (Join-Path $root 'War3RenderCore.lock.json') | ConvertFrom-Json
if ($lock.commit -notmatch '^[0-9a-f]{40}$') { throw '锁定文件中的提交无效。' }
if (!$Repository) { $Repository = $lock.repository }
if (!$Repository) { throw '锁定文件没有 repository，请用 -Repository 指定取源地址。' }
if (!$Directory) { $Directory = Join-Path (Split-Path -Parent $root) 'War3RenderCore' }
$Directory = [IO.Path]::GetFullPath($Directory)

function Invoke-Git { & git @args; if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') 失败。" } }

if (Test-Path -LiteralPath (Join-Path $Directory '.git')) {
    if (@(git -C $Directory status --porcelain --untracked-files=normal).Count) {
        throw "$Directory 有未提交内容，不会切换。请先自行处理。"
    }
    $present = $false
    git -C $Directory cat-file -e "$($lock.commit)^{commit}" 2>$null
    if ($LASTEXITCODE -eq 0) { $present = $true }
    if (!$present) { Invoke-Git -C $Directory fetch --quiet $Repository $lock.commit }
} elseif (Test-Path -LiteralPath $Directory) {
    throw "$Directory 已存在但不是 git 检出，不会覆盖。"
} else {
    Invoke-Git clone --quiet --no-checkout $Repository $Directory
}
Invoke-Git -C $Directory -c advice.detachedHead=false checkout --quiet $lock.commit
$head = (git -C $Directory rev-parse HEAD).Trim()
if ($head -ne $lock.commit) { throw "检出结果 $head 与锁定提交不一致。" }
"render_core=$Directory commit=$head version=$($lock.version)"
