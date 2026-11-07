param([Parameter(Mandatory)][string]$Freezer,[Parameter(Mandatory)][string]$Tools)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
$root=Join-Path ([IO.Path]::GetTempPath()) ('audastra-runtime-legacy-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root|Out-Null
function Check([bool]$Value,[string]$Message){if(!$Value){throw $Message}}
function Rejected([scriptblock]$Action){$rejected=$false;try{& $Action}catch{$rejected=$true};Check $rejected 'Invalid legacy/current set accepted'}
function Fixture([string]$Name,[string]$Client,[bool]$Approved){
    $fixtureRoot=Join-Path $root ($Name+'-input');New-Item -ItemType Directory $fixtureRoot|Out-Null
    $files=@(foreach($role in @('bootstrap','payload','module','client','license','origin')){
        $path=Join-Path $fixtureRoot ($role+'.blob')
        $text=if($role -eq 'client'){$Client}else{'original-'+$Name+'-'+$role}
        [IO.File]::WriteAllBytes($path,[Text.Encoding]::UTF8.GetBytes($text))
        @{role=$role;source=$path;size=(Get-Item $path).Length;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    })
    $plan=Join-Path $fixtureRoot plan.json
    @{schema=2;scope='public';id=$Name;version='1.0.0';files=$files}|ConvertTo-Json -Depth 5|Set-Content $plan -Encoding utf8NoBOM
    $planSha=(Get-FileHash $plan).Hash.ToLowerInvariant();$pending=Join-Path $root ($Name+'-pending')
    & "$Tools/install-map-runtime.ps1" -FreezerExecutable $Freezer -Plan $plan -PlanSha256 $planSha -OutputDirectory $pending|Out-Null
    $target=$pending
    if($Approved){
        $catalog=Get-Content "$pending/runtime/catalog.json" -Raw|ConvertFrom-Json
        $hashes=@{};foreach($role in @('bootstrap','payload','module','client')){$hashes[$role]=($files|Where-Object role -CEQ $role).sha256}
        $approval=Join-Path $fixtureRoot approval.json
        @{schema=1;id=$Name;version='1.0.0';packageSha256=$catalog.packages[0].manifestSha256;
            decisions=@{publicDistribution='author-2026-10-27';unobfuscatedBootstrap='author-2026-10-30';wholeFileCopyOnly=$true};files=$hashes}|ConvertTo-Json -Depth 5|Set-Content $approval -Encoding utf8NoBOM
        $target=Join-Path $root ($Name+'-approved')
        & "$Tools/install-map-runtime.ps1" -FreezerExecutable $Freezer -Plan $plan -PlanSha256 $planSha -OutputDirectory $target -ApprovalRecord $approval -ApprovalSha256 ((Get-FileHash $approval).Hash.ToLowerInvariant())|Out-Null
    }
    return $target
}
try{
    . "$Tools/StageMapRuntime.ps1"
    $current=Fixture 'original-current' 'current-client' $true
    $legacy=Fixture 'original-legacy' 'legacy-client' $true
    $pending=Fixture 'original-unapproved' 'unapproved-client' $false
    $secondCurrent=Fixture 'original-current-two' 'current-client' $true
    $client="$root/original-current-input/client.blob"
    $plan=Get-MapRuntimeDistributionStagePlan -CurrentSource $current -LegacySources @($legacy) -BundledClient $client
    Check (@($plan.catalog.packages).Count -eq 2) 'Both versions must be registered'
    $portable=Join-Path $root portable;New-Item -ItemType Directory $portable|Out-Null
    Write-MapRuntimeInstallationStagePlan -Plan $plan -Target "$portable/runtime"
    foreach($source in @($current,$legacy)){
        $entry=(Get-Content "$source/runtime/catalog.json" -Raw|ConvertFrom-Json).packages[0]
        foreach($file in Get-ChildItem "$source/runtime/packages/$($entry.directory)" -File){
            Check ((Get-FileHash $file.FullName).Hash -ceq (Get-FileHash "$portable/runtime/packages/$($entry.directory)/$($file.Name)").Hash) 'Legacy bytes changed'
        }
    }
    Rejected {Get-MapRuntimeDistributionStagePlan -CurrentSource $current -LegacySources @($pending) -BundledClient $client}
    Rejected {Get-MapRuntimeDistributionStagePlan -CurrentSource '' -BundledClient $client}
    $empty=Join-Path $root empty;New-Item -ItemType Directory "$empty/runtime" -Force|Out-Null
    @{schema=1;packages=@()}|ConvertTo-Json|Set-Content "$empty/runtime/catalog.json" -Encoding utf8NoBOM
    Rejected {Get-MapRuntimeDistributionStagePlan -CurrentSource $empty -BundledClient $client}
    Rejected {Get-MapRuntimeDistributionStagePlan -CurrentSource $current -LegacySources @($legacy,$legacy) -BundledClient $client}
    $two=Join-Path $root two;Copy-Item -LiteralPath $current -Destination $two -Recurse
    $catalog=Get-Content "$two/runtime/catalog.json" -Raw|ConvertFrom-Json
    $second=(Get-Content "$secondCurrent/runtime/catalog.json" -Raw|ConvertFrom-Json).packages[0]
    $catalog.packages+=,$second;$catalog|ConvertTo-Json -Depth 5|Set-Content "$two/runtime/catalog.json" -Encoding utf8NoBOM
    Copy-Item -LiteralPath "$secondCurrent/runtime/packages/$($second.directory)" -Destination "$two/runtime/packages" -Recurse
    Rejected {Get-MapRuntimeDistributionStagePlan -CurrentSource $two -BundledClient $client}
    Rejected {Get-MapRuntimeDistributionStagePlan -CurrentSource $legacy -BundledClient $client}
    Rejected {Get-MapRuntimeDistributionStagePlan -CurrentSource $current -LegacySources @($current) -BundledClient $client}
    # 更新载荷复用便携清单，确认旧版成员不会在生成更新包时被过滤掉。
    $inventory=@(Get-ChildItem $portable -File -Recurse|ForEach-Object {@{path=[IO.Path]::GetRelativePath($portable,$_.FullName).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash $_.FullName).Hash.ToLowerInvariant()}})
    @{format=1;source_clean=$true;source_commit=('a'*40);version='0.4.0';files=$inventory}|ConvertTo-Json -Depth 5|Set-Content "$portable/manifest.json" -Encoding utf8NoBOM
    & "$Tools/package-update.ps1" -PortableDirectory $portable -OutputDirectory "$root/update" -MinimumDirectUpgrade '0.3.1'|Out-Null
    $update=Get-Content "$root/update/update-manifest.json" -Raw|ConvertFrom-Json
    foreach($name in @('original-current-1.0.0','original-legacy-1.0.0')){Check (@($update.files|Where-Object path -CEQ "runtime/packages/$name/client.j").Count -eq 1) 'Update lost runtime package'}
    Write-Output 'RUNTIME_LEGACY_DISTRIBUTION_FIXTURES_PASSED'
}finally{
    $full=[IO.Path]::GetFullPath($root);$temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    Check ($full.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -and (Split-Path $full -Leaf) -match '^audastra-runtime-legacy-[a-f0-9]{32}$') 'Unexpected cleanup path'
    Remove-Item -LiteralPath $full -Recurse -Force
}
