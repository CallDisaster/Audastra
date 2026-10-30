# 只复制目录明示且摘要匹配的封装成员，批准位保持关闭。
function Copy-PendingMapRuntimeInstallation {
    param([Parameter(Mandatory)][string]$Source,[Parameter(Mandatory)][string]$Target)
    $ErrorActionPreference='Stop'
    function Plain([string]$Path){
        $full=[IO.Path]::GetFullPath($Path)
        if($full -notmatch '^[A-Za-z]:[\\/]' -or $full.Substring(2).Contains(':')){throw 'RUNTIME_STAGE_PATH_INVALID'}
        for($p=$full;$p;$p=[IO.Path]::GetDirectoryName($p)){
            if((Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'RUNTIME_STAGE_PATH_INVALID'}
        }
        return $full
    }
    $sourceRoot=Plain $Source;$targetRoot=Plain $Target
    if(Test-Path -LiteralPath $targetRoot){throw 'RUNTIME_STAGE_TARGET_EXISTS'}
    $catalogPath=Plain "$sourceRoot/runtime/catalog.json"
    if((Get-Item -LiteralPath $catalogPath).Length -gt 32768){throw 'RUNTIME_CATALOG_INVALID'}
    $catalog=Get-Content -LiteralPath $catalogPath -Raw|ConvertFrom-Json
    if($catalog.schema -ne 1 -or $catalog.packages.Count -gt 16){throw 'RUNTIME_CATALOG_INVALID'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $copies=[Collections.Generic.List[object]]::new()
    foreach($entry in $catalog.packages){
        if($entry.directory -notmatch '^[A-Za-z0-9_.-]+$' -or $entry.directory -in @('.','..') -or !$seen.Add($entry.directory) -or
            $entry.manifestSha256 -notmatch '^[0-9a-f]{64}$' -or $entry.publicDistributionApproved -isnot [bool] -or $entry.publicDistributionApproved){throw 'RUNTIME_CATALOG_INVALID_OR_ALREADY_APPROVED'}
        $package=Plain "$sourceRoot/runtime/packages/$($entry.directory)"
        $manifest=Plain "$package/manifest.json"
        if((Get-Item $manifest).Length -gt 65536 -or (Get-FileHash -LiteralPath $manifest).Hash.ToLowerInvariant() -ne $entry.manifestSha256){throw 'RUNTIME_HASH_MISMATCH'}
        $data=Get-Content -LiteralPath $manifest -Raw|ConvertFrom-Json
        if($data.schema -ne 2 -or $data.scope -ne 'public' -or $data.files.Count -ne 6){throw 'RUNTIME_PUBLIC_PACKAGE_REQUIRED'}
        $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $roles=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        $allowed=@{bootstrap='bootstrap.blob';payload='payload.blob';module='module.blob';client='client.j';notice=@('license.txt','origin.txt')}
        $total=0L
        foreach($file in $data.files){
            if(!$allowed.ContainsKey($file.role) -or $file.path -notin @($allowed[$file.role]) -or !$paths.Add($file.path) -or
                $file.sha256 -notmatch '^[0-9a-f]{64}$' -or $file.size -le 0 -or $file.size -gt 16777216){throw 'RUNTIME_FILE_INVALID'}
            [void]$roles.Add($file.role);$total+=$file.size
            if($total -gt 33554432){throw 'RUNTIME_PACKAGE_BUDGET'}
            $path=Plain "$package/$($file.path)"
            if((Get-Item -LiteralPath $path).Length -ne $file.size -or (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -ne $file.sha256){throw 'RUNTIME_HASH_MISMATCH'}
            $copies.Add(@{source=$path;relative="packages/$($entry.directory)/$($file.path)";sha=$file.sha256})
        }
        if($roles.Count -ne 5){throw 'RUNTIME_ROLES_INCOMPLETE'}
        $copies.Add(@{source=$manifest;relative="packages/$($entry.directory)/manifest.json";sha=$entry.manifestSha256})
    }
    # 先完整复核再写；失败的未登记目录保留，不覆盖已有发行包。
    New-Item -ItemType Directory -Path $targetRoot -ErrorAction Stop|Out-Null
    foreach($item in $copies){
        $destination=Join-Path $targetRoot $item.relative
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force|Out-Null
        Copy-Item -LiteralPath $item.source -Destination $destination -ErrorAction Stop
        if((Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant() -ne $item.sha){throw 'RUNTIME_SOURCE_CHANGED'}
    }
    $catalog|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$targetRoot/catalog.json" -Encoding utf8NoBOM
}
