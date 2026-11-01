[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Cli,
    [Parameter(Mandatory)][string]$GameRoot,
    [Parameter(Mandatory)][string[]]$MapRoots,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1,600)][int]$TimeoutSeconds=120
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a fresh evidence directory.'}
$cliPath=(Resolve-Path -LiteralPath $Cli).Path
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$seen=@{};$inventory=@();$results=@()
foreach($root in $MapRoots){
    $stack=[Collections.Generic.Stack[string]]::new();$stack.Push((Resolve-Path -LiteralPath $root).Path)
    while($stack.Count){foreach($file in Get-ChildItem -LiteralPath $stack.Pop() -Force){
        if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){continue}
        if($file.PSIsContainer){$stack.Push($file.FullName);continue}
        if($file.Extension -notin @('.w3x','.w3m')){continue}
        $hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $inventory+=[pscustomobject]@{path=$file.FullName;sha256=$hash;bytes=$file.Length}
        if(-not $seen.ContainsKey($hash)){$seen[$hash]=$file.FullName}
    }}
}
if($seen.Count -gt 4096){throw 'Corpus exceeds the explicit 4096-map budget.'}
# Paths stay in the private inventory; public review rows use only content identities.
$inventory|ConvertTo-Json -Depth 4|Set-Content -LiteralPath "$OutputDirectory/private-inventory.json" -Encoding UTF8
foreach($hash in @($seen.Keys|Sort-Object)){
    $source=$seen[$hash];$watch=[Diagnostics.Stopwatch]::StartNew()
    $info=[Diagnostics.ProcessStartInfo]::new($cliPath)
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($arg in @('minimap-audit',$source,$GameRoot)){$info.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    if(-not $process.Start()){throw 'Could not launch the owned audit process.'}
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $timedOut=-not $process.WaitForExit($TimeoutSeconds*1000)
    if($timedOut){$process.Kill($true);$process.WaitForExit()}
    $text=$stdout.GetAwaiter().GetResult();$errorText=$stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText("$OutputDirectory/$hash.json",$text)
    if($errorText){[IO.File]::WriteAllText("$OutputDirectory/$hash.stderr.txt",$errorText)}
    $row=$null
    if(-not $timedOut){try{$row=$text|ConvertFrom-Json}catch{}}
    if(-not $row){$row=[pscustomobject]@{generated=$false;phase='process';diagnostics=@(@{code=$(if($timedOut){'AUDIT_TIMEOUT'}else{'AUDIT_RECEIPT_INVALID'})})}}
    if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash){throw 'Source changed during read-only corpus audit.'}
    $results+=[pscustomobject]@{sha256=$hash;generated=$row.generated;phase=$row.phase;diagnostics=$row.diagnostics;
        exitCode=$process.ExitCode;elapsedMs=$watch.ElapsedMilliseconds;peakWorkingSetBytes=$process.PeakWorkingSet64;sourceUnchanged=$true}
    $results|ConvertTo-Json -Depth 8|Set-Content -LiteralPath "$OutputDirectory/results.json" -Encoding UTF8
    $process.Dispose()
}
$counts=@{}
foreach($row in $results){foreach($code in @($row.diagnostics.code|Sort-Object -Unique)){if($code){$counts[$code]=1+$counts[$code]}}}
$summary=[ordered]@{schema=1;cliSha256=(Get-FileHash -LiteralPath $cliPath).Hash.ToLowerInvariant();
    mapFiles=$inventory.Count;uniqueMaps=$seen.Count;generated=@($results|Where-Object generated).Count;
    failures=@($results|Where-Object {-not $_.generated}).Count;allSourcesUnchanged=$true;
    failureCodeCounts=@($counts.GetEnumerator()|Sort-Object Value -Descending|ForEach-Object {@{code=$_.Key;maps=$_.Value}});
    coverage='First failed generation stage per map; not an exhaustive inventory of all defects.'}
$summary|ConvertTo-Json -Depth 7|Set-Content -LiteralPath "$OutputDirectory/summary.json" -Encoding UTF8
$summary|ConvertTo-Json -Depth 7
