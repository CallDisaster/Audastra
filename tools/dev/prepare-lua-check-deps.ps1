# 仅准备官方 Lua 5.3.6 源码缓存；编辑器以文本 compile-only 方式静态使用解析器。
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskLuaCache = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $taskLuaCache) { throw 'Choose a new Lua dependency cache directory' }
New-Item -ItemType Directory -Path $taskLuaCache | Out-Null
$taskLuaArchive = Join-Path $taskLuaCache 'lua-5.3.6.tar.gz'
Invoke-WebRequest -Uri 'https://www.lua.org/ftp/lua-5.3.6.tar.gz' -OutFile $taskLuaArchive
if ((Get-FileHash -LiteralPath $taskLuaArchive).Hash.ToLowerInvariant() -cne 'fc5fd69bb8736323f026672b1b7235da613d7177e72558893a0bdcd320466d60') {
    throw 'Lua 5.3.6 archive SHA256 mismatch'
}
& tar -xf $taskLuaArchive -C $taskLuaCache
if ($LASTEXITCODE -ne 0) { throw 'Lua source extraction failed' }
$taskLuaPaths = [ordered]@{ AUD_LUA53_SOURCE_DIR = (Join-Path $taskLuaCache 'lua-5.3.6/src') }
$taskLuaPaths | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskLuaCache 'cmake-paths.local.json') -Encoding utf8
$taskLuaPaths | ConvertTo-Json
