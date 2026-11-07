# 安装根只接受普通本机目录；原始引导和许可始终作为不透明封装处理。
function Assert-PlainMapRuntimePath([string]$Path){
    $full=[IO.Path]::GetFullPath($Path)
    if($full -notmatch '^[A-Za-z]:[\\/]' -or $full.Substring(2).Contains(':')){throw 'RUNTIME_STAGE_PATH_INVALID'}
    for($p=$full;$p;$p=[IO.Path]::GetDirectoryName($p)){
        if((Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'RUNTIME_STAGE_PATH_INVALID'}
    }
    return $full
}

# 先取得全部已核对的复制项。调用方可在创建输出之前合并、检查多个安装根。
function Get-MapRuntimeInstallationStagePlan {
    param([Parameter(Mandatory)][string]$Source)
    $ErrorActionPreference='Stop'
    . "$PSScriptRoot/RuntimePublicApproval.ps1"
    $sourceRoot=Assert-PlainMapRuntimePath $Source
    $catalogPath=Assert-PlainMapRuntimePath "$sourceRoot/runtime/catalog.json"
    if((Get-Item -LiteralPath $catalogPath).Length -gt 32768){throw 'RUNTIME_CATALOG_INVALID'}
    $catalog=Get-Content -LiteralPath $catalogPath -Raw|ConvertFrom-Json
    if($catalog.schema -ne 1 -or $catalog.packages.Count -gt 16){throw 'RUNTIME_CATALOG_INVALID'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $copies=[Collections.Generic.List[object]]::new()
    $clients=[Collections.Generic.List[object]]::new()
    foreach($entry in $catalog.packages){
        if($entry.directory -notmatch '^[A-Za-z0-9_.-]+$' -or $entry.directory -in @('.','..') -or !$seen.Add($entry.directory) -or
            $entry.manifestSha256 -notmatch '^[0-9a-f]{64}$' -or $entry.publicDistributionApproved -isnot [bool]){throw 'RUNTIME_CATALOG_INVALID'}
        $package=Assert-PlainMapRuntimePath "$sourceRoot/runtime/packages/$($entry.directory)"
        $manifest=Assert-PlainMapRuntimePath "$package/manifest.json"
        if((Get-Item $manifest).Length -gt 65536 -or (Get-FileHash -LiteralPath $manifest).Hash.ToLowerInvariant() -ne $entry.manifestSha256){throw 'RUNTIME_HASH_MISMATCH'}
        $data=Get-Content -LiteralPath $manifest -Raw|ConvertFrom-Json
        if($data.schema -ne 2 -or $data.scope -ne 'public' -or $data.files.Count -ne 6){throw 'RUNTIME_PUBLIC_PACKAGE_REQUIRED'}
        if($entry.publicDistributionApproved){
            $approval=Assert-PlainMapRuntimePath "$package/approval.json"
            $approvalSha=(Get-FileHash -LiteralPath $approval).Hash.ToLowerInvariant()
            $null=Read-MapRuntimePublicApproval -ApprovalPath $approval -ManifestPath $manifest
            if((Get-FileHash -LiteralPath $approval).Hash.ToLowerInvariant() -cne $approvalSha){throw 'RUNTIME_PUBLIC_APPROVAL_CHANGED'}
            $copies.Add(@{source=$approval;relative="packages/$($entry.directory)/approval.json";sha=$approvalSha})
        }
        $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $roles=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        $allowed=@{bootstrap='bootstrap.blob';payload='payload.blob';module='module.blob';client='client.j';notice=@('license.txt','origin.txt')}
        $total=0L
        foreach($file in $data.files){
            if(!$allowed.ContainsKey($file.role) -or $file.path -notin @($allowed[$file.role]) -or !$paths.Add($file.path) -or
                $file.sha256 -notmatch '^[0-9a-f]{64}$' -or $file.size -le 0 -or $file.size -gt 16777216){throw 'RUNTIME_FILE_INVALID'}
            [void]$roles.Add($file.role);$total+=$file.size
            if($total -gt 33554432){throw 'RUNTIME_PACKAGE_BUDGET'}
            $path=Assert-PlainMapRuntimePath "$package/$($file.path)"
            if((Get-Item -LiteralPath $path).Length -ne $file.size -or (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -ne $file.sha256){throw 'RUNTIME_HASH_MISMATCH'}
            $copies.Add(@{source=$path;relative="packages/$($entry.directory)/$($file.path)";sha=$file.sha256})
        }
        if($roles.Count -ne 5){throw 'RUNTIME_ROLES_INCOMPLETE'}
        $clients.Add(@{directory=$entry.directory;sha256=($data.files|Where-Object role -CEQ 'client').sha256})
        $copies.Add(@{source=$manifest;relative="packages/$($entry.directory)/manifest.json";sha=$entry.manifestSha256})
    }
    return [pscustomobject]@{catalog=$catalog;copies=$copies.ToArray();clients=$clients.ToArray()}
}

function Write-MapRuntimeInstallationStagePlan {
    param([Parameter(Mandatory)]$Plan,[Parameter(Mandatory)][string]$Target)
    $ErrorActionPreference='Stop'
    $targetRoot=Assert-PlainMapRuntimePath $Target
    if(Test-Path -LiteralPath $targetRoot){throw 'RUNTIME_STAGE_TARGET_EXISTS'}
    # 来源在计划完成后仍按摘要复核，目录登记最后写入。
    New-Item -ItemType Directory -Path $targetRoot -ErrorAction Stop|Out-Null
    foreach($item in $Plan.copies){
        if((Get-FileHash -LiteralPath $item.source).Hash.ToLowerInvariant() -cne $item.sha){throw 'RUNTIME_SOURCE_CHANGED'}
        $destination=Join-Path $targetRoot $item.relative
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force|Out-Null
        Copy-Item -LiteralPath $item.source -Destination $destination -ErrorAction Stop
        if((Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant() -ne $item.sha){throw 'RUNTIME_SOURCE_CHANGED'}
    }
    $Plan.catalog|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$targetRoot/catalog.json" -Encoding utf8NoBOM
}

function Copy-PendingMapRuntimeInstallation {
    param([Parameter(Mandatory)][string]$Source,[Parameter(Mandatory)][string]$Target)
    $plan=Get-MapRuntimeInstallationStagePlan -Source $Source
    Write-MapRuntimeInstallationStagePlan -Plan $plan -Target $Target
}

# 当前包由内置客户端身份确定；不同客户端仅能从显式指定的已批准旧版根进入。
function Get-MapRuntimeDistributionStagePlan {
    param([string]$CurrentSource,[string[]]$LegacySources=@(),[Parameter(Mandatory)][string]$BundledClient)
    $ErrorActionPreference='Stop'
    if([string]::IsNullOrWhiteSpace($CurrentSource)){throw 'RUNTIME_CURRENT_PACKAGE_REQUIRED'}
    if($LegacySources.Count -gt 15){throw 'RUNTIME_CATALOG_INVALID'}
    $bundle=Assert-PlainMapRuntimePath $BundledClient
    $bundleSha=(Get-FileHash -LiteralPath $bundle).Hash.ToLowerInvariant()
    $current=Get-MapRuntimeInstallationStagePlan -Source $CurrentSource
    if(@($current.catalog.packages).Count -ne 1){throw 'RUNTIME_CURRENT_PACKAGE_COUNT'}
    if(@($current.clients).Count -ne 1 -or $current.clients[0].sha256 -cne $bundleSha){throw 'RUNTIME_CURRENT_CLIENT_MISMATCH'}
    $entries=[Collections.Generic.List[object]]::new();$copies=[Collections.Generic.List[object]]::new()
    $directories=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $identities=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $plans=@($current)
    foreach($source in $LegacySources){
        $legacy=Get-MapRuntimeInstallationStagePlan -Source $source
        if(!@($legacy.catalog.packages).Count){throw 'RUNTIME_LEGACY_PACKAGE_REQUIRED'}
        foreach($entry in $legacy.catalog.packages){if(!$entry.publicDistributionApproved){throw 'RUNTIME_LEGACY_DISTRIBUTION_REQUIRED'}}
        foreach($client in $legacy.clients){if($client.sha256 -ceq $bundleSha){throw 'RUNTIME_CURRENT_PACKAGE_COUNT'}}
        $plans+=,$legacy
    }
    foreach($plan in $plans){
        foreach($entry in $plan.catalog.packages){
            if(!$identities.Add($entry.manifestSha256)){throw 'RUNTIME_PACKAGE_DUPLICATE'}
            if(!$directories.Add($entry.directory)){throw 'RUNTIME_PACKAGE_DIRECTORY_CONFLICT'}
            $entries.Add($entry)
            if($entries.Count -gt 16){throw 'RUNTIME_CATALOG_INVALID'}
        }
        foreach($item in $plan.copies){$copies.Add($item)}
    }
    return [pscustomobject]@{catalog=[ordered]@{schema=1;packages=$entries.ToArray()};copies=$copies.ToArray()}
}
