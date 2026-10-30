# 组装安装目录；公开批准须逐包核对发布记录，不修改已有安装。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$FreezerExecutable,
    [Parameter(Mandatory)][string]$Plan,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{64}$')][string]$PlanSha256,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$ApprovalRecord = '',
    [string]$ApprovalSha256 = ''
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
if([bool]$ApprovalRecord -ne [bool]$ApprovalSha256){throw 'RUNTIME_PUBLIC_APPROVAL_INVALID: 批准记录与摘要须同时提供。'}
if($ApprovalRecord){
    $ApprovalRecord=Assert-PlainLocalPath $ApprovalRecord
    if($ApprovalSha256 -cnotmatch '^[0-9a-f]{64}$' -or (Get-Item -LiteralPath $ApprovalRecord).Length -gt 8192 -or
        (Get-FileHash -LiteralPath $ApprovalRecord).Hash.ToLowerInvariant() -cne $ApprovalSha256){throw 'RUNTIME_PUBLIC_APPROVAL_INVALID'}
}
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
# 引导输入只能整体复制和核对身份。冻结器只取得不透明封装文件，不打开原引导文件。
$bootstrap=@($planData.files|Where-Object {$_.role -ceq 'bootstrap'})
if($bootstrap.Count -ne 1){throw 'RUNTIME_BOOTSTRAP_ROLE_INVALID'}
$bootstrapSource=Assert-PlainLocalPath $bootstrap[0].source
if((Get-Item -LiteralPath $bootstrapSource).Length -ne $bootstrap[0].size -or
    (Get-FileHash -LiteralPath $bootstrapSource).Hash.ToLowerInvariant() -cne $bootstrap[0].sha256){throw 'RUNTIME_BOOTSTRAP_IDENTITY_MISMATCH'}
New-Item -ItemType Directory "$output/freeze-input" -ErrorAction Stop|Out-Null
$bootstrapCopy="$output/freeze-input/bootstrap.blob"
Copy-Item -LiteralPath $bootstrapSource -Destination $bootstrapCopy -ErrorAction Stop
if((Get-Item -LiteralPath $bootstrapCopy).Length -ne $bootstrap[0].size -or
    (Get-FileHash -LiteralPath $bootstrapCopy).Hash.ToLowerInvariant() -cne $bootstrap[0].sha256){throw 'RUNTIME_BOOTSTRAP_IDENTITY_MISMATCH'}
$bootstrap[0].source=$bootstrapCopy
$freezePlan="$output/freeze-input/plan.json"
$planData|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $freezePlan -Encoding utf8NoBOM
$freezeSha=(Get-FileHash -LiteralPath $freezePlan).Hash.ToLowerInvariant()
& $freezer $freezePlan $freezeSha $package > "$output/freeze-receipt.txt"
if($LASTEXITCODE){throw 'RUNTIME_FREEZE_REJECTED: incomplete output retained; no catalog installed'}
$manifest="$package/manifest.json"
$digest=(Get-FileHash -LiteralPath $manifest).Hash.ToLowerInvariant()
$receipt=Get-Content -LiteralPath "$output/freeze-receipt.txt" -Raw
if($receipt -notmatch ('(?m)^package_sha256='+$digest+'\r?$') -or $receipt -notmatch '(?m)^public_distribution_approved=false\r?$'){
    throw 'RUNTIME_FREEZE_RECEIPT_INVALID'
}
$approved=$false
if($ApprovalRecord){
    . "$PSScriptRoot/RuntimePublicApproval.ps1"
    $null=Read-MapRuntimePublicApproval -ApprovalPath $ApprovalRecord -ManifestPath $manifest
    if((Get-FileHash -LiteralPath $ApprovalRecord).Hash.ToLowerInvariant() -cne $ApprovalSha256){throw 'RUNTIME_PUBLIC_APPROVAL_CHANGED'}
    Copy-Item -LiteralPath $ApprovalRecord -Destination "$package/approval.json" -ErrorAction Stop
    if((Get-FileHash -LiteralPath "$package/approval.json").Hash.ToLowerInvariant() -cne $ApprovalSha256){throw 'RUNTIME_PUBLIC_APPROVAL_CHANGED'}
    $approved=$true
}
@{schema=1;packages=@(@{directory=$directory;manifestSha256=$digest;publicDistributionApproved=$approved})}|
    ConvertTo-Json -Depth 4|Set-Content -LiteralPath "$output/runtime/catalog.json" -Encoding utf8NoBOM
@{schema=1;packageSha256=$digest;publicDistributionApproved=$approved;normalSaveAvailable=$approved}|
    ConvertTo-Json|Set-Content -LiteralPath "$output/install-receipt.json" -Encoding utf8NoBOM
Write-Output "RUNTIME_INSTALLED: $digest public_distribution_approved=$($approved.ToString().ToLowerInvariant())"
