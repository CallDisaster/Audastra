# 按 third_party/fonts/fonts.json 下载界面字体：固定版本、校验 SHA-256，结果放在已忽略的 files 目录。
# 已存在且哈希一致的文件直接跳过；任何哈希不符都会失败并删除临时文件，不留下半成品。
# 用法：powershell -ExecutionPolicy Bypass -File tools/fetch-fonts.ps1 [-Force]
[CmdletBinding()]
param(
    [string]$Manifest,
    [string]$OutDir,
    [switch]$Force
)

# Windows PowerShell 5.1 在 param 默认值里拿不到 $PSScriptRoot，因此在这里补默认路径。
$fontsRoot = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\third_party\fonts'
if (-not $Manifest) { $Manifest = Join-Path $fontsRoot 'fonts.json' }
if (-not $OutDir) { $OutDir = Join-Path $fontsRoot 'files' }

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Sha256([string]$Path, [string]$Expected, [string]$Label) {
    $actual = Get-Sha256 $Path
    if ($actual -ne $Expected.ToLowerInvariant()) {
        Remove-Item -LiteralPath $Path -Force
        throw "SHA-256 mismatch for ${Label}: expected $Expected, got $actual"
    }
}

function Test-Installed($Font, [string]$Dir) {
    foreach ($file in $Font.files) {
        $path = Join-Path $Dir $file.output
        if (-not (Test-Path -LiteralPath $path)) { return $false }
        if ((Get-Sha256 $path) -ne $file.sha256.ToLowerInvariant()) { return $false }
    }
    return $true
}

# 从 zip 中取出单个成员到临时文件，校验后再移动到位。
function Export-ZipMember([string]$Zip, [string]$Member, [string]$Target) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $entry = $archive.GetEntry($Member)
        if ($null -eq $entry) { throw "Archive member not found: $Member" }
        $in = $entry.Open()
        try {
            $out = [IO.File]::Create($Target)
            try { $in.CopyTo($out) } finally { $out.Dispose() }
        } finally { $in.Dispose() }
    } finally { $archive.Dispose() }
}

$spec = Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8 | ConvertFrom-Json
if ($spec.schema -ne 1) { throw "Unsupported font manifest schema: $($spec.schema)" }

$OutDir = [IO.Path]::GetFullPath($OutDir)
$staging = Join-Path $OutDir '.download'
New-Item -ItemType Directory -Force -Path $OutDir, $staging | Out-Null

try {
    foreach ($font in $spec.fonts) {
        if (-not $Force -and (Test-Installed $font $OutDir)) {
            Write-Host "[skip] $($font.id) $($font.version)"
            continue
        }

        Write-Host "[get ] $($font.id) $($font.version) <- $($font.url)"
        $download = Join-Path $staging ([IO.Path]::GetFileName(([Uri]$font.url).AbsolutePath))
        Invoke-WebRequest -Uri $font.url -OutFile $download -UseBasicParsing
        Assert-Sha256 $download $font.sha256 $font.url

        foreach ($file in $font.files) {
            $partial = Join-Path $staging ($file.output + '.partial')
            if ($file.PSObject.Properties.Name -contains 'member') {
                Export-ZipMember $download $file.member $partial
            } else {
                Copy-Item -LiteralPath $download -Destination $partial -Force
            }
            Assert-Sha256 $partial $file.sha256 $file.output
            Move-Item -LiteralPath $partial -Destination (Join-Path $OutDir $file.output) -Force
            Write-Host "[ ok ] $($file.output)"
        }
    }
} finally {
    # 失败时也清掉下载暂存目录，目标目录里只会出现校验通过的完整文件。
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Fonts ready in $OutDir"
