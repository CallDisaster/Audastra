# 从已组装的干净便携目录生成 GitHub Release 资产：
#   Audastra-<版本>-windows-x64.zip         给用户下载的整包（顶层 Audastra\，含旁置签名清单，解压即可启动并作为以后自动更新的可信基线）
#   Audastra-<版本>-windows-x64-update.zip  自动更新下载的载荷包（由签名清单描述）
#   update-manifest.json / update-manifest.json.sig  签名清单及 64 字节 Ed25519 签名
#   SHA256SUMS.txt
# 私钥只从发布者本机文件读取，不进入输出、日志或仓库。只生成文件，不上传、不推送。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PortableDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$KeyFile,
    [Parameter(Mandatory)][string]$SignTool,
    [Parameter(Mandatory)][string]$MinimumDirectUpgrade,
    [string]$ReleaseNotes = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem

$portable = (Resolve-Path -LiteralPath $PortableDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "输出目录已存在：$output" }
$cli = Join-Path $portable 'bin/war3-map-cli.exe'
$identity = & $cli --version-json | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or !$identity.version) { throw '无法读取便携包版本身份。' }
if (!$identity.update_public_key_hex) { throw '便携包未配置更新公钥，发布后无法验证更新。' }
$version = $identity.version

# 1. 载荷包与未签名清单（package-update 负责逐文件核对便携清单）。
$update = Join-Path $output 'update'
& "$PSScriptRoot/package-update.ps1" -PortableDirectory $portable -OutputDirectory $update `
    -MinimumDirectUpgrade $MinimumDirectUpgrade -ReleaseNotes $ReleaseNotes | Out-Null
$manifest = Join-Path $update 'update-manifest.json'
$payload = Join-Path $update "Audastra-$version-windows-x64-update.zip"
if (!(Test-Path -LiteralPath $manifest) -or !(Test-Path -LiteralPath $payload)) { throw '更新载荷生成失败。' }

# 2. 签名，并确认签名用的密钥就是包内信任的公钥。
$signature = "$manifest.sig"
$signed = & $SignTool --manifest $manifest --key-file $KeyFile --signature-out $signature
if ($LASTEXITCODE -ne 0) { throw '签名失败。' }
$publicKey = ([regex]::Match(($signed -join "`n"), 'Public key: ([0-9a-f]{64})')).Groups[1].Value
if ($publicKey -cne $identity.update_public_key_hex) { throw "签名密钥与包内信任公钥不一致（$publicKey）。" }

# 3. 用包内 CLI（只信任构建时配置的公钥）回验清单、签名和载荷包。
$checked = & $cli update-check --manifest $manifest --signature $signature --package $payload --current-version 0.0.0 | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or !$checked.signature_verified -or !$checked.package_verified -or $checked.target_version -ne $version) {
    throw '包内程序未能验证签名清单或载荷包。'
}

# 4. 用户整包：便携目录 + 原字节旁置清单/签名，顶层目录名 Audastra。
$stage = Join-Path $output 'stage'
$stageRoot = Join-Path $stage 'Audastra'
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item -Recurse -LiteralPath $portable -Destination $stageRoot
Copy-Item -LiteralPath $manifest, $signature -Destination $stageRoot
$assets = Join-Path $output 'assets'
New-Item -ItemType Directory -Path $assets | Out-Null
$full = Join-Path $assets "Audastra-$version-windows-x64.zip"
[IO.Compression.ZipFile]::CreateFromDirectory($stageRoot, $full, [IO.Compression.CompressionLevel]::Optimal, $true, [Text.Encoding]::UTF8)
Copy-Item -LiteralPath $payload, $manifest, $signature -Destination $assets

# 5. 回读用户整包：旁置清单和签名必须与发布资产逐字节相同，程序文件与便携清单一致。
$archive = [IO.Compression.ZipFile]::OpenRead($full)
try {
    foreach ($name in 'update-manifest.json', 'update-manifest.json.sig') {
        $entry = $archive.GetEntry("Audastra/$name")
        if (!$entry) { throw "整包缺少 $name。" }
        $memory = [IO.MemoryStream]::new(); $stream = $entry.Open(); $stream.CopyTo($memory); $stream.Dispose()
        $expected = [IO.File]::ReadAllBytes((Join-Path $assets $name))
        if ([Convert]::ToHexString($memory.ToArray()) -cne [Convert]::ToHexString($expected)) { throw "整包内 $name 与发布资产不一致。" }
    }
    $listed = @((Get-Content -Raw -LiteralPath (Join-Path $portable 'manifest.json') | ConvertFrom-Json).files)
    if ($archive.Entries.Count -lt $listed.Count + 3) { throw '整包文件数少于便携清单。' }
} finally { $archive.Dispose() }

$sums = Get-ChildItem -LiteralPath $assets -File | Sort-Object Name | ForEach-Object {
    "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $($_.Name)"
}
[IO.File]::WriteAllText((Join-Path $assets 'SHA256SUMS.txt'), ($sums -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
Remove-Item -Recurse -Force -LiteralPath $stage
[ordered]@{ version = $version; source_commit = $identity.commit; public_key = $publicKey; assets = $assets
    manifest_sha256 = $checked.manifest_sha256 } | ConvertTo-Json
