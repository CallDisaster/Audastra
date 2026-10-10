# 先复制候选，再调用现有专用 YDWE 真保存入口，最后逐成员审计。
# 此脚本不自行控制窗口；ydwe-save.ps1 负责既有实例拒绝与专用实例身份门禁。
param(
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][string]$WorkDirectory,
    [Parameter(Mandatory)][string]$YdweRoot,
    [Parameter(Mandatory)][string]$Cli,
    [string]$Python = 'python',
    [string]$Explanations,
    [int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
function Assert-PlainPath([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "拒绝链接或 reparse point: $cursor"
            }
        }
        $parent = [IO.Directory]::GetParent($cursor)
        $cursor = if ($parent) { $parent.FullName } else { $null }
    }
}
Assert-PlainPath $Candidate
Assert-PlainPath $WorkDirectory
$candidatePath = (Resolve-Path -LiteralPath $Candidate).Path
if ([IO.Path]::GetExtension($candidatePath).ToLowerInvariant() -notin @('.w3x', '.w3m')) {
    throw '候选必须为 w3x/w3m 文件'
}
$workPath = [IO.Path]::GetFullPath($WorkDirectory)
if (Test-Path -LiteralPath $workPath) { throw '工作目录必须不存在，不能覆盖旧证据' }
$sourceDirectory = [IO.Path]::GetDirectoryName($candidatePath).TrimEnd('\') + '\'
if ($workPath.StartsWith($sourceDirectory, [StringComparison]::OrdinalIgnoreCase)) {
    throw '工作目录不能放在只读来源目录内'
}
$sourceHash = (Get-FileHash -LiteralPath $candidatePath -Algorithm SHA256).Hash.ToLowerInvariant()
New-Item -ItemType Directory -Path $workPath | Out-Null
$nativeCopy = Join-Path $workPath ('native-save' + [IO.Path]::GetExtension($candidatePath))
Copy-Item -LiteralPath $candidatePath -Destination $nativeCopy
if ((Get-FileHash -LiteralPath $nativeCopy -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHash) {
    throw '原版真保存输入副本SHA不符'
}
$receipt = [ordered]@{ schema='p1-ydwe-native-roundtrip-v1'; status='running'; input_sha256=$sourceHash; runtime_validated=$false }
try {
    $saveLog = Join-Path $workPath 'ydwe-save.log'
    & (Join-Path $PSScriptRoot 'ydwe-save.ps1') -YdweRoot $YdweRoot -Map $nativeCopy -TimeoutSeconds $TimeoutSeconds *>&1 |
        Tee-Object -FilePath $saveLog
    $log = Get-Content -LiteralPath $saveLog -Raw
    $saved = [regex]::Match($log, '(?m)^saved pid=(\d+) inputSha256=([0-9A-Fa-f]{64}) outputSha256=([0-9A-Fa-f]{64}) size=(\d+)\s*$')
    if (-not $saved.Success -or $log -notmatch '(?m)^closed\s*$') { throw '没有本次完整 saved/closed 回执' }
    $savedHash = (Get-FileHash -LiteralPath $nativeCopy -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($saved.Groups[2].Value.ToLowerInvariant() -cne $sourceHash -or $saved.Groups[3].Value.ToLowerInvariant() -cne $savedHash) {
        throw '原版保存回执SHA不符'
    }
    if ((Get-FileHash -LiteralPath $candidatePath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHash) {
        throw '只读 Audastra 候选已变化'
    }
    $receipt.original_editor_validated = $true
    $receipt.editor_pid = [int]$saved.Groups[1].Value
    $receipt.output_sha256 = $savedHash
    $compareArgs = @((Join-Path $PSScriptRoot 'p1-archive-compare.py'), '--cli', $Cli, '--before', $candidatePath,
        '--after', $nativeCopy, '--report', (Join-Path $workPath 'member-comparison.json'))
    if ($Explanations) { $compareArgs += @('--explanations', $Explanations) }
    & $Python @compareArgs
    if ($LASTEXITCODE -ne 0) { throw '逐成员比较未收口；见 member-comparison.json' }
    $receipt.status = 'native-saved-differences-reviewable'
} catch {
    $receipt.status = 'failed'
    $receipt.reason = $_.Exception.Message
    throw
} finally {
    $receipt.source_unchanged = (Get-FileHash -LiteralPath $candidatePath -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $sourceHash
    $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $workPath 'native-save-receipt.json') -Encoding utf8
}
