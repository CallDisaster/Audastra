param([Parameter(Mandatory)][string]$Freezer,[Parameter(Mandatory)][string]$Tools)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Join-Path ([IO.Path]::GetTempPath()) ('audastra-runtime-distribution-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -ErrorAction Stop|Out-Null
function Check([bool]$Value,[string]$Message){if(!$Value){throw $Message}}
function Rejected([scriptblock]$Action,[string]$Message){$failed=$false;try{& $Action}catch{$failed=$true};Check $failed $Message}
try{
    $files=@()
    foreach($role in @('bootstrap','payload','module','client','license','origin')){
        # 原创占位字节，绝不使用真实引导器或第三方二进制。
        $extension=if($role -eq 'client'){'.j'}else{'.blob'}
        $path=Join-Path $root ($role+$extension)
        [IO.File]::WriteAllBytes($path,[Text.Encoding]::UTF8.GetBytes('original-fixture-'+$role))
        $files+=@{role=$role;source=$path;size=(Get-Item $path).Length;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $plan=Join-Path $root plan.json
    @{schema=2;scope='public';id='original-fixture';version='1.3.0';files=$files}|ConvertTo-Json -Depth 5|Set-Content $plan -Encoding utf8NoBOM
    $digest=(Get-FileHash $plan).Hash.ToLowerInvariant()
    $installed=Join-Path $root install
    & "$Tools/install-map-runtime.ps1" -FreezerExecutable $Freezer -Plan $plan -PlanSha256 $digest -OutputDirectory $installed
    $catalog=Get-Content "$installed/runtime/catalog.json" -Raw|ConvertFrom-Json
    Check ($catalog.packages.Count -eq 1 -and $catalog.packages[0].publicDistributionApproved -eq $false) 'Installation must retain unapproved catalog'
    $package="$installed/runtime/packages/original-fixture-1.3.0"
    $manifest=Get-Content "$package/manifest.json" -Raw|ConvertFrom-Json
    Check ($manifest.scope -eq 'public' -and $manifest.files.Count -eq 6) 'Public candidate roles missing'
    . "$Tools/StageMapRuntime.ps1"
    Copy-PendingMapRuntimeInstallation -Source $installed -Target "$root/staged"
    $staged=Get-Content "$root/staged/catalog.json" -Raw|ConvertFrom-Json
    Check ($staged.packages[0].publicDistributionApproved -eq $false) 'Portable staging must not approve distribution'
    Check ((Get-FileHash "$root/staged/packages/original-fixture-1.3.0/module.blob").Hash -eq (Get-FileHash "$package/module.blob").Hash) 'Module identity changed'
    Rejected {Copy-PendingMapRuntimeInstallation -Source $installed -Target "$root/staged"} 'Existing target must remain untouched'
    Rejected {& "$Tools/install-map-runtime.ps1" -FreezerExecutable $Freezer -Plan $plan -PlanSha256 ('0'*64) -OutputDirectory "$root/bad-plan"} 'Changed plan accepted'
    Check (!(Test-Path "$root/bad-plan")) 'Plan failure created output'
    # 批准只属于这一份内容身份；待审批包不能靠目录布尔值提升。
    $catalog.packages[0].publicDistributionApproved=$true
    $catalog|ConvertTo-Json -Depth 5|Set-Content "$installed/runtime/catalog.json" -Encoding utf8NoBOM
    Rejected {Copy-PendingMapRuntimeInstallation -Source $installed -Target "$root/forged"} 'Catalog flag without approval accepted'
    $catalog.packages[0].publicDistributionApproved=$false
    $catalog|ConvertTo-Json -Depth 5|Set-Content "$installed/runtime/catalog.json" -Encoding utf8NoBOM
    $approvalFile="$root/approval.json"
    $hashes=@{};foreach($role in 'bootstrap','payload','module','client'){$hashes[$role]=@($files|Where-Object {$_.role -eq $role})[0].sha256}
    $approval=@{schema=1;id=$manifest.id;version=$manifest.version;packageSha256=$catalog.packages[0].manifestSha256;
        decisions=@{publicDistribution='author-2026-10-27';unobfuscatedBootstrap='author-2026-10-30';wholeFileCopyOnly=$true};files=$hashes}
    $approval|ConvertTo-Json -Depth 6|Set-Content $approvalFile -Encoding utf8NoBOM
    $approvalSha=(Get-FileHash $approvalFile).Hash.ToLowerInvariant()
    & "$Tools/install-map-runtime.ps1" -FreezerExecutable $Freezer -Plan $plan -PlanSha256 $digest -OutputDirectory "$root/released" -ApprovalRecord $approvalFile -ApprovalSha256 $approvalSha
    $release=Get-Content "$root/released/install-receipt.json" -Raw|ConvertFrom-Json
    Check ($release.publicDistributionApproved -eq $true -and $release.normalSaveAvailable -eq $true) 'Matching approval not applied'
    Copy-PendingMapRuntimeInstallation -Source "$root/released" -Target "$root/released-stage"
    $releaseCatalog=Get-Content "$root/released-stage/catalog.json" -Raw|ConvertFrom-Json
    Check ($releaseCatalog.packages[0].publicDistributionApproved -eq $true) 'Portable staging lost verified approval'
    Check ((Get-FileHash "$root/released-stage/packages/original-fixture-1.3.0/approval.json").Hash -eq $approvalSha.ToUpperInvariant()) 'Approval identity changed'
    . "$Tools/RuntimePublicApproval.ps1"
    foreach($field in 'bootstrap','payload','module','client'){
        $saved=$approval.files[$field];$approval.files[$field]='0'*64
        $approval|ConvertTo-Json -Depth 6|Set-Content "$root/bad-approval.json" -Encoding utf8NoBOM
        Rejected {Read-MapRuntimePublicApproval -ApprovalPath "$root/bad-approval.json" -ManifestPath "$package/manifest.json"} 'Changed role approved'
        $approval.files[$field]=$saved
    }
    foreach($field in 'id','version','packageSha256'){
        $saved=$approval[$field];$approval[$field]=if($field -eq 'packageSha256'){'0'*64}else{'different'}
        $approval|ConvertTo-Json -Depth 6|Set-Content "$root/bad-approval.json" -Encoding utf8NoBOM
        Rejected {Read-MapRuntimePublicApproval -ApprovalPath "$root/bad-approval.json" -ManifestPath "$package/manifest.json"} 'Approval accepted for another package'
        $approval[$field]=$saved
    }
    $original=Get-Content $approvalFile -Raw
    foreach($invalid in @($original.Replace('"schema": 1','"schema": 1, "schema": 1'),$original.Replace('"schema": 1','"schema": 1, "path": "arbitrary"'),$original.Replace('author-2026-10-27','unspecified'))){
        Set-Content "$root/bad-approval.json" $invalid -Encoding utf8NoBOM
        Rejected {Read-MapRuntimePublicApproval -ApprovalPath "$root/bad-approval.json" -ManifestPath "$package/manifest.json"} 'Ambiguous or unbound approval accepted'
    }
    Rejected {& "$Tools/install-map-runtime.ps1" -FreezerExecutable $Freezer -Plan $plan -PlanSha256 $digest -OutputDirectory "$root/bad-approval-sha" -ApprovalRecord $approvalFile -ApprovalSha256 ('0'*64)} 'Approval file identity not pinned'
    [IO.File]::AppendAllText("$package/client.j",'changed')
    Rejected {Copy-PendingMapRuntimeInstallation -Source $installed -Target "$root/stale"} 'Changed client accepted'
    Check (!(Test-Path "$root/stale")) 'Incomplete source staged before verification'
    $catalog.packages[0].publicDistributionApproved=$true
    $catalog|ConvertTo-Json -Depth 5|Set-Content "$installed/runtime/catalog.json" -Encoding utf8NoBOM
    Rejected {Copy-PendingMapRuntimeInstallation -Source $installed -Target "$root/approved"} 'Pending-only tool accepted elevated approval'
    Write-Output 'RUNTIME_DISTRIBUTION_ORIGINAL_FIXTURES_PASSED'
}finally{
    # 只清理本测试创建且身份固定的临时树。
    $full=[IO.Path]::GetFullPath($root)
    Check ($full.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase) -and (Split-Path $full -Leaf) -match '^audastra-runtime-distribution-[a-f0-9]{32}$') 'Unexpected cleanup path'
    Remove-Item -LiteralPath $full -Recurse -Force
}
