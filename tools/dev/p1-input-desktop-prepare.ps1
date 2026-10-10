#requires -Version 7.2
# native08只读准备：冻结同host/来源、显式环境、工具与新执行目录；不启动WE、不写registry、不复制host/game。
param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$ExpectedSourceSha256,
    [Parameter(Mandatory)][long]$ExpectedSourceBytes,
    [Parameter(Mandatory)][string]$GameRoot,
    [Parameter(Mandatory)][string]$HostRoot,
    [Parameter(Mandatory)][string]$Prepared,
    [Parameter(Mandatory)][string]$ExpectedPreparedSha256,
    [Parameter(Mandatory)][string]$EnvironmentBaseline,
    [Parameter(Mandatory)][string]$ExpectedEnvironmentBaselineSha256,
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$Cli,
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$RunRoot,
    [ValidateRange(20,600)][int]$TimeoutSeconds=180,
    [ValidateSet(65001,936)][int]$PjassCodePage=65001
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'p1-registry-guard.ps1') -Mode Library
. (Join-Path $PSScriptRoot 'p1-input-desktop.ps1')
$tokens=$null; $parseErrors=$null
$runnerAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'p1-input-desktop-run.ps1'),[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw 'native08 runner语法错误' }
# 只复用已审runner的冻结文件验证函数；不执行runner顶层操作。
$pinFunctions=@($runnerAst.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Assert-Native08Pin'},$true))
if ($pinFunctions.Count -ne 1) { throw 'native08冻结文件公共函数不唯一' }
Invoke-Expression $pinFunctions[0].Extent.Text
$Source=Assert-Native08Pin $Source $ExpectedSourceSha256.ToLowerInvariant() $ExpectedSourceBytes
$Prepared=Assert-Native08Pin $Prepared $ExpectedPreparedSha256.ToLowerInvariant()
$EnvironmentBaseline=Assert-Native08Pin $EnvironmentBaseline $ExpectedEnvironmentBaselineSha256.ToLowerInvariant()
$GameRoot=Assert-P1PlainFilePath $GameRoot; $HostRoot=Assert-P1PlainFilePath $HostRoot
$RunRoot=Assert-P1PlainFilePath $RunRoot; $Manifest=Assert-P1PlainFilePath $Manifest
foreach($output in @($RunRoot,$Manifest)) {
    if (Test-Path -LiteralPath $output) { throw 'native08输出必须不存在' }
    if (-not [IO.Directory]::Exists([IO.Path]::GetDirectoryName($output))) { throw 'native08输出父目录必须存在' }
    foreach($inputPath in @($Source,$GameRoot,$HostRoot)) {
        if ($output.Equals($inputPath,[StringComparison]::OrdinalIgnoreCase) -or
            $output.StartsWith($inputPath.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or
            $inputPath.StartsWith($output.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'native08输出/输入交叠' }
    }
}
$preparedReceipt=Get-Content -LiteralPath $Prepared -Raw | ConvertFrom-Json
if (-not [string]::Equals($preparedReceipt.gameRoot,$GameRoot,[StringComparison]::OrdinalIgnoreCase) -or
    -not [string]::Equals($preparedReceipt.hostRoot,$HostRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'prepared game/host根不符' }
$baseline=Get-Content -LiteralPath $EnvironmentBaseline -Raw | ConvertFrom-Json
if ($baseline.schema -cne 'p1-desktop-native-save-run-v1' -or $baseline.hostRoot -cne $HostRoot -or
    $baseline.gameRoot -cne $GameRoot -or $baseline.saveCommand -ne 4 -or -not $baseline.unpackagedDescendants) {
    throw 'native07基线模式/同host条件不符'
}
if ($baseline.sourceMap.path -cne $Source -or $baseline.sourceMap.bytes -ne $ExpectedSourceBytes -or
    $baseline.sourceMap.sha256 -cne $ExpectedSourceSha256.ToLowerInvariant() -or
    $baseline.prepared.path -cne $Prepared -or $baseline.prepared.sha256 -cne $ExpectedPreparedSha256.ToLowerInvariant()) {
    throw 'native08须与冻结07同来源/同prepared身份'
}
$selected=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
$allowed=@('SystemRoot','WINDIR','PATH','TEMP','TMP','SystemDrive','ComSpec','PATHEXT','USERPROFILE','LOCALAPPDATA','APPDATA','ProgramData')
foreach($property in $baseline.selectedEnvironment.PSObject.Properties) {
    if ($property.Name -notin $allowed -or $property.Value -isnot [string] -or
        [string]::IsNullOrEmpty($property.Value) -or $property.Value.Contains([char]0)) { throw 'native07环境键值不符' }
    $selected.Add($property.Name,$property.Value)
}
if ($selected.Count -ne 12) { throw 'native08必须使用native07已冻结的12个Windows环境键' }
$scriptNames=@('p1-input-desktop-run.ps1','p1-input-desktop.ps1','p1-input-desktop-inventory.py','p1-input-desktop-prepare.ps1',
    'ydwe-save.ps1','p1-native-loader.py','p1-registry-guard.ps1','p1-archive-compare.py','p1-support.py')
$scripts=@(foreach($name in $scriptNames) {
    $path=Assert-P1PlainFilePath (Join-Path $PSScriptRoot $name)
    [ordered]@{path=$path;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
})
function FrozenRecord([string]$Path) {
    $path=Assert-P1PlainFilePath $Path
    return [ordered]@{path=$path;bytes=(Get-Item -LiteralPath $path).Length;
        sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$witness=[P1InputDesktopOwner]::Witness(); $witness.RequireActiveDefault()
Test-P1NoEditorProcesses
$inventoryPath=$Manifest+'.inventory-before.local.json'
$nativeLoaderPin=@($scripts | Where-Object { [IO.Path]::GetFileName($_.path) -ceq 'p1-native-loader.py' })[0]
& $Python -B (Join-Path $PSScriptRoot 'p1-input-desktop-inventory.py') --prepared $Prepared `
    --prepared-sha256 $ExpectedPreparedSha256 --native-loader-sha256 $nativeLoaderPin.sha256 --receipt $inventoryPath
if ($LASTEXITCODE -ne 0) { throw 'native08完整清单准备失败' }
$inventory=Get-Content -LiteralPath $inventoryPath -Raw | ConvertFrom-Json
if (-not $inventory.matchesPrepared) { throw 'native08准备清单未匹配prepared，不产生可执行manifest' }
$record=[ordered]@{
    schema='p1-input-desktop-native-save-v1'; desktop='WinSta0\Default'; saveCommand=4;
    unpackagedDescendants=$true; timeoutSeconds=$TimeoutSeconds; pjassCodePage=$PjassCodePage;
    Source=(FrozenRecord $Source); gameRoot=$GameRoot; hostRoot=$HostRoot; Prepared=(FrozenRecord $Prepared);
    Python=(FrozenRecord $Python); Cli=(FrozenRecord $Cli); baseline07=(FrozenRecord $EnvironmentBaseline);
    scripts=$scripts; runRoot=$RunRoot; selectedEnvironment=$selected;
    environmentSha256=(Get-P1InputEnvironmentFingerprint $selected); prepareWitness=$witness;
    executionAllowed=$false; nonInterferenceValidated=$false; requestedDisplay='\\.\DISPLAY1';
    executionBlocker='DISPLAY1坐标定位不能保证不激活；native08已有前台干扰，尚无pre-resume完整隔离';
    prepareInventory=(FrozenRecord $inventoryPath); launched=$false; registryWritten=$false; nativeSaveValidated=$false; runtimeValidated=$false
}
Write-P1InputDesktopNewJson $Manifest $record
[pscustomobject]@{status='prepared-blocked-no-we-launch';executionAllowed=$false;nonInterferenceValidated=$false;manifest=$Manifest;
    manifestSha256=(Get-FileHash -LiteralPath $Manifest -Algorithm SHA256).Hash.ToLowerInvariant();
    runRoot=$RunRoot;files=$inventory.dependencies.Count;environmentKeys=@($selected.Keys | Sort-Object);
    launched=$false;registryWritten=$false;nativeSaveValidated=$false} | ConvertTo-Json -Depth 5
