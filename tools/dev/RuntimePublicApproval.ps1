# 发布记录只授权一份完整内容身份；安装目录仍由可信发布宿主提供。
function Read-MapRuntimePublicApproval {
    param([Parameter(Mandatory)][string]$ApprovalPath,[Parameter(Mandatory)][string]$ManifestPath)
    $ErrorActionPreference='Stop'
    function FailApproval { throw 'RUNTIME_PUBLIC_APPROVAL_INVALID: 公开批准记录缺失、损坏或与冻结包身份不符。' }
    function ExactKeys($Value,[string[]]$Names){
        $actual=@($Value.PSObject.Properties.Name)
        if($actual.Count -ne $Names.Count){FailApproval}
        foreach($name in $actual){if($name -cnotin $Names){FailApproval}}
    }
    function UniqueJsonKeys([System.Text.Json.JsonElement]$Element){
        if($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object){
            $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
            foreach($p in $Element.EnumerateObject()){if(!$seen.Add($p.Name)){FailApproval};UniqueJsonKeys $p.Value}
        }elseif($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array){
            foreach($item in $Element.EnumerateArray()){UniqueJsonKeys $item}
        }
    }
    if(!(Test-Path -LiteralPath $ApprovalPath -PathType Leaf) -or (Get-Item -LiteralPath $ApprovalPath).Length -gt 8192){FailApproval}
    $text=Get-Content -LiteralPath $ApprovalPath -Raw -Encoding utf8
    try{$document=[System.Text.Json.JsonDocument]::Parse($text)}catch{FailApproval}
    try{UniqueJsonKeys $document.RootElement}finally{$document.Dispose()}
    try{$record=$text|ConvertFrom-Json -ErrorAction Stop}catch{FailApproval}
    ExactKeys $record @('schema','id','version','packageSha256','decisions','files')
    ExactKeys $record.decisions @('publicDistribution','unobfuscatedBootstrap','wholeFileCopyOnly')
    ExactKeys $record.files @('bootstrap','payload','module','client')
    if($record.id -isnot [string] -or $record.version -isnot [string] -or $record.schema -isnot [long] -or
       $record.decisions.publicDistribution -isnot [string] -or $record.decisions.unobfuscatedBootstrap -isnot [string] -or
       $record.schema -ne 1 -or $record.decisions.publicDistribution -cne 'author-2026-10-27' -or
       $record.decisions.unobfuscatedBootstrap -cne 'author-2026-10-30' -or
       $record.decisions.wholeFileCopyOnly -isnot [bool] -or !$record.decisions.wholeFileCopyOnly){FailApproval}
    if($record.packageSha256 -isnot [string] -or $record.packageSha256 -cnotmatch '^[0-9a-f]{64}$' -or
       (Get-FileHash -LiteralPath $ManifestPath).Hash.ToLowerInvariant() -cne $record.packageSha256){FailApproval}
    $manifest=Get-Content -LiteralPath $ManifestPath -Raw -Encoding utf8|ConvertFrom-Json
    if($manifest.schema -ne 2 -or $manifest.scope -cne 'public' -or $manifest.id -cne $record.id -or $manifest.version -cne $record.version){FailApproval}
    foreach($role in 'bootstrap','payload','module','client'){
        $files=@($manifest.files|Where-Object {$_.role -ceq $role})
        if($files.Count -ne 1 -or $record.files.$role -isnot [string] -or $record.files.$role -cnotmatch '^[0-9a-f]{64}$' -or $files[0].sha256 -cne $record.files.$role){FailApproval}
    }
    return $record
}
