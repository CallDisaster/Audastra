# 公开内容扫描：禁用文件类型、超大文件、本机路径/账号/令牌，以及不入库的私有词（地图名等）。
# 规则在 tools/ci/public-content-rules.json。私有词来源：-PrivatePatternsFile 指定的文件，
# 或环境变量 AUDASTRA_PRIVATE_PATTERNS（每行一个字面词，CI 中由仓库机密注入）。命中私有词时不回显内容。
# 目录是 git 仓库时只扫已跟踪文件；否则扫目录下全部文件。发现问题时退出码为 1。
[CmdletBinding()]
param(
    [string]$Root = '',
    [string]$PrivatePatternsFile = '',
    [string]$Report = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$rulesPath = Join-Path $PSScriptRoot 'public-content-rules.json'
$rules = Get-Content -Raw -Encoding UTF8 -LiteralPath $rulesPath | ConvertFrom-Json
if (!$Root) { $Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')

$patterns = @($rules.forbiddenPatterns | ForEach-Object { [pscustomobject]@{ regex = $_.regex; reason = $_.reason } })
$privateWords = @()
if ($PrivatePatternsFile) {
    if (!(Test-Path -LiteralPath $PrivatePatternsFile)) { throw "私有词表不存在：$PrivatePatternsFile" }
    $privateWords += Get-Content -Encoding UTF8 -LiteralPath $PrivatePatternsFile
}
if ($env:AUDASTRA_PRIVATE_PATTERNS) { $privateWords += $env:AUDASTRA_PRIVATE_PATTERNS -split "`r?`n" }
$privateWords = @($privateWords | ForEach-Object { $_.Trim().TrimStart([char]0xFEFF) } | Where-Object { $_ -and !$_.StartsWith('#') })
foreach ($word in $privateWords) { $patterns += [pscustomobject]@{ regex = [regex]::Escape($word); reason = '私有词表' } }
if (!$privateWords.Count) { Write-Warning '没有提供私有词；只做通用扫描。' }

if (Test-Path -LiteralPath (Join-Path $Root '.git')) {
    $files = @(git -C $Root ls-files -z | ForEach-Object { $_ -split "`0" } | Where-Object { $_ } |
        ForEach-Object { Get-Item -LiteralPath (Join-Path $Root $_) -Force -ErrorAction SilentlyContinue } | Where-Object { $_ })
} else {
    $files = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force)
}

$findings = [Collections.Generic.List[object]]::new()
$utf8 = [Text.UTF8Encoding]::new($false, $false)
foreach ($file in $files) {
    $rel = $file.FullName.Substring($Root.Length + 1) -replace '\\', '/'
    if ($rel.StartsWith('.git/')) { continue }
    if ($rules.forbiddenExtensions -contains $file.Extension.ToLowerInvariant()) {
        $findings.Add([pscustomobject]@{ File = $rel; Line = 0; Reason = '禁用文件类型'; Text = $file.Extension }); continue
    }
    if ($file.Length -gt $rules.maxFileBytes) {
        $findings.Add([pscustomobject]@{ File = $rel; Line = 0; Reason = '文件超过上限'; Text = "$($file.Length) 字节" })
    }
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    if ($bytes.Length -gt 0 -and ([Array]::IndexOf($bytes, [byte]0) -ge 0)) { continue } # 二进制（图标等）只查类型和大小
    $lines = $utf8.GetString($bytes) -split "`n"
    # 规则文件本身写着通用规则的正则，只对它查私有词。
    $rulesFile = $rel -eq 'tools/ci/public-content-rules.json'
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        # 白名单只豁免通用规则；私有词表始终生效。
        $allowed = $rulesFile -or @($rules.allowPatterns | Where-Object { $lines[$i] -match $_.regex }).Count -gt 0
        foreach ($p in $patterns) {
            if ($allowed -and $p.reason -ne '私有词表') { continue }
            if ($lines[$i] -match $p.regex) {
                $text = $lines[$i].Trim(); if ($text.Length -gt 160) { $text = $text.Substring(0, 160) + '…' }
                if ($p.reason -eq '私有词表') { $text = '（命中私有词，内容不回显）' }
                $findings.Add([pscustomobject]@{ File = $rel; Line = $i + 1; Reason = $p.reason; Text = $text }); break
            }
        }
    }
}

$summary = "扫描 $($files.Count) 个文件，私有词 $($privateWords.Count) 个，问题 $($findings.Count)。"
if ($Report) {
    $body = @('# 公开内容扫描', '', "- 目录：$Root", "- $summary", '')
    if ($findings.Count) {
        $body += '| 文件 | 行 | 原因 | 内容 |'; $body += '|---|---|---|---|'
        $body += $findings | ForEach-Object { "| $($_.File) | $($_.Line) | $($_.Reason) | $($_.Text -replace '\|', '\|') |" }
    }
    $body | Set-Content -Encoding UTF8 -LiteralPath $Report
}
foreach ($finding in $findings) { Write-Host "$($finding.File):$($finding.Line): $($finding.Reason) $($finding.Text)" }
Write-Host $summary
if ($findings.Count) { exit 1 }
