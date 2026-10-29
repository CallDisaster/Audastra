# 组装待审批安装目录；不编译、不改变分发批准、不修改已有安装。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$FreezerExecutable,
    [Parameter(Mandatory)][string]$Plan,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{64}$')][string]$PlanSha256,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Assert-PlainLocalPath([string]$Path) {
    $full=[IO.Path]::GetFullPath($Path)
    if($full -notmatch '^[A-Za-z]:[\\/]' -or $full.Substring(2).Contains(':')){throw 'RUNTIME_INSTALL_PATH_INVALID'}
    for($at=$full;$at;$at=[IO.Path]::GetDirectoryName($at)){
        if((Test-Path -LiteralPath $at) -and ((Get-Item -LiteralPath $at -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){
            throw 'RUNTIME_INSTALL_PATH_INVALID: reparse point'
        }
    }
    return $full
}
$freezer=Assert-PlainLocalPath $FreezerExecutable
$planPath=Assert-PlainLocalPath $Plan
$output=Assert-PlainLocalPath $OutputDirectory
if((Test-Path -LiteralPath $output) -or !(Test-Path -LiteralPath $freezer -PathType Leaf)){
    throw 'RUNTIME_INSTALL_TARGET_EXISTS_OR_TOOL_MISSING'
}
$file=Get-Item -LiteralPath $planPath
if($file.Length -gt 65536 -or (Get-FileHash -LiteralPath $planPath).Hash.ToLowerInvariant() -ne $PlanSha256){throw 'RUNTIME_PLAN_IDENTITY_MISMATCH'}
$planData=Get-Content -LiteralPath $planPath -Raw -Encoding utf8|ConvertFrom-Json
if($planData.schema -ne 2 -or $planData.scope -ne 'public' -or $planData.id -notmatch '^[A-Za-z0-9_-]+$' -or $planData.version -notmatch '^[A-Za-z0-9_.-]+$'){
    throw 'RUNTIME_PUBLIC_PLAN_REQUIRED'
}
$directory=$planData.id+'-'+$planData.version
New-Item -ItemType Directory -Path "$output/runtime/packages" -ErrorAction Stop|Out-Null
$package="$output/runtime/packages/$directory"
& $freezer $planPath $PlanSha256 $package > "$output/freeze-receipt.txt"
if($LASTEXITCODE){throw 'RUNTIME_FREEZE_REJECTED: incomplete output retained; no catalog installed'}
$manifest="$package/manifest.json"
$digest=(Get-FileHash -LiteralPath $manifest).Hash.ToLowerInvariant()
$receipt=Get-Content -LiteralPath "$output/freeze-receipt.txt" -Raw
if($receipt -notmatch ('(?m)^package_sha256='+$digest+'\r?$') -or $receipt -notmatch '(?m)^public_distribution_approved=false\r?$'){
    throw 'RUNTIME_FREEZE_RECEIPT_INVALID'
}
@{schema=1;packages=@(@{directory=$directory;manifestSha256=$digest;publicDistributionApproved=$false})}|
    ConvertTo-Json -Depth 4|Set-Content -LiteralPath "$output/runtime/catalog.json" -Encoding utf8NoBOM
@{schema=1;packageSha256=$digest;publicDistributionApproved=$false;normalSaveAvailable=$false}|
    ConvertTo-Json|Set-Content -LiteralPath "$output/install-receipt.json" -Encoding utf8NoBOM
Write-Output "RUNTIME_INSTALLED_PENDING_APPROVAL: $digest"
