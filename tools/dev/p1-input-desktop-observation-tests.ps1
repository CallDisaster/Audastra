#requires -Version 7.2
# 原创Default观察回归；复用现有readonly保存合同和自有窗口夹具，不启动WE、不写registry。
param([string]$WorkDirectory)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$tokens=$null; $parseErrors=$null
$saveAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'ydwe-save.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'save parser errors'}
$saveType=$saveAst.FindAll({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Add-Type'},$true)[0]
Add-Type -TypeDefinition $saveType.CommandElements[1].Value
foreach($function in $saveAst.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true)) {
    if($function.Name -in @('Get-DirectDialogState','Test-NativeVersionWarning')) { Invoke-Expression $function.Extent.Text }
}
$fixtureAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'ydwe-save-direct-tests.ps1'),[ref]$tokens,[ref]$parseErrors)
$fixtureType=@($fixtureAst.FindAll({param($node) $node -is [Management.Automation.Language.StringConstantExpressionAst] -and
    $node.Value.Contains('public static class YdweFixtureWin')},$true))
if($fixtureType.Count -ne 1){throw 'shared fixture class not unique'}
Add-Type -TypeDefinition $fixtureType[0].Value
. (Join-Path $PSScriptRoot 'p1-input-desktop.ps1')
. (Join-Path $PSScriptRoot 'p1-input-desktop-observation.ps1')
$w=[P1InputDesktopOwner]::Witness()
try{$w.RequireActiveDefault()}catch{Write-Output "SKIP Default observation: $($_.Exception.Message)";exit 77}
$autoCleanup=-not $WorkDirectory
if(-not $WorkDirectory){$WorkDirectory=Join-Path ([IO.Path]::GetTempPath()) ('forge-native09-observation-'+[Guid]::NewGuid().ToString('N'))}
$root=[IO.Path]::GetFullPath($WorkDirectory)
if(Test-Path -LiteralPath $root){throw 'new fixture directory required'}
[void][IO.Directory]::CreateDirectory($root)
$script:passed=0; $script:failed=[Collections.Generic.List[object]]::new()
$ownPid=$PID
function Check([bool]$Value,[string]$Message){if(-not $Value){throw $Message}}
function Case([string]$Name,[scriptblock]$Action){try{& $Action;$script:passed++;"PASS $Name"}catch{$script:failed.Add(@{name=$Name;error=$_.Exception.Message});"FAIL $Name : $($_.Exception.Message)"}}
function Dialog([IntPtr]$Handle){return @([YdweWin]::GetDialogs($ownPid,$true)|Where-Object Handle -EQ $Handle)[0]}
$foreground=[YdweFixtureWin]::GetForegroundWindow()
try {
    [YdweFixtureWin]::Initialize()
    Case 'Default visible frame disabled by another modal leaves unshown OWN tool shell inactive' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','native09 main',[IntPtr]::Zero,$true,$false)
        $tool=[YdweFixtureWin]::Create('#32770','工具面板',$frame,$false,$false)
        try {
            [YdweFixtureWin]::Enabled($frame,$false);$d=Dialog $tool
            Check ((Get-DirectDialogState $d $frame $ownPid) -ceq 'ActiveModal') '旧inactive合同应保持原有保守行为'
            Check ((Get-InputDesktopDialogState $d $frame $ownPid $true) -ceq 'Inactive') '真实Default可见frame下未显示tool壳不应成为第二modal'
        } finally{[YdweFixtureWin]::Remove($tool);[YdweFixtureWin]::Remove($frame)}
    }
    foreach($spec in @(@{name='unproven input Desktop';input=$false;frameVisible=$true;toolTitle='工具面板';toolVisible=$false},
        @{name='noninput hidden frame';input=$true;frameVisible=$false;toolTitle='工具面板';toolVisible=$false},
        @{name='unknown hidden modal title';input=$true;frameVisible=$true;toolTitle='未知模态';toolVisible=$false},
        @{name='actual visible tool dialog';input=$true;frameVisible=$true;toolTitle='工具面板';toolVisible=$true})) {
        $caseSpec=$spec
        Case ('old modal contract remains for '+$caseSpec.name) {
            $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$caseSpec.frameVisible,$false)
            $tool=[YdweFixtureWin]::Create('#32770',$caseSpec.toolTitle,$frame,$caseSpec.toolVisible,$false)
            try {
                [YdweFixtureWin]::Enabled($frame,$false);$d=Dialog $tool
                Check ((Get-InputDesktopDialogState $d $frame $ownPid $caseSpec.input) -ceq 'ActiveModal') '不得泛化隐藏modal豁免'
            } finally{[YdweFixtureWin]::Remove($tool);[YdweFixtureWin]::Remove($frame)}
        }
    }
    Case 'same title from another frame remains rejected' {
        $one=[YdweFixtureWin]::Create('Warcraft III','one',[IntPtr]::Zero,$true,$false)
        $two=[YdweFixtureWin]::Create('Warcraft III','two',[IntPtr]::Zero,$true,$false)
        $tool=[YdweFixtureWin]::Create('#32770','工具面板',$two,$false,$false)
        try {
            [YdweFixtureWin]::Enabled($one,$false);[YdweFixtureWin]::Enabled($two,$false)
            Check ((Get-InputDesktopDialogState (Dialog $tool) $one $ownPid $true) -ceq 'UnprovenHidden') '其他main owner工具壳不能套本次观察'
        } finally{foreach($h in @($tool,$two,$one)){[YdweFixtureWin]::Remove($h)}}
    }
    Case 'actual control reader retains HWND pid tid parent id text and own visibility' {
        $frame=[YdweFixtureWin]::Create('Warcraft III','main',[IntPtr]::Zero,$true,$false)
        $dialog=[YdweFixtureWin]::Create('#32770','owned fixture',$frame,$true,$false)
        $button=[YdweFixtureWin]::Child($dialog,'Button','原创按钮',321,$true)
        try {
            $controls=@([P1InputDialogControls]::Read($dialog,$ownPid))
            Check ($controls.Count -eq 1 -and $controls[0].Handle -eq $button -and $controls[0].Parent -eq $dialog -and
                $controls[0].Id -eq 321 -and $controls[0].Text -ceq '原创按钮' -and $controls[0].Pid -eq $ownPid -and
                $controls[0].ThreadId -gt 0 -and $controls[0].Enabled -and $controls[0].Visible -and $controls[0].OwnVisibleStyle) '控件真实归属/字段缺失'
        } finally{[YdweFixtureWin]::Remove($dialog);[YdweFixtureWin]::Remove($frame)}
    }
} finally{[YdweFixtureWin]::Cleanup()}
Case 'fixtures do not change foreground' {Check ([YdweFixtureWin]::GetForegroundWindow() -eq $foreground) '自有夹具抢前台'}
Write-P1InputDesktopNewJson (Join-Path $root 'summary.local.json') @{
    schema='native09-default-observation-original-tests-v1';passed=$script:passed;failed=@($script:failed);
    worldEditorStarted=$false;registryWritten=$false;nativeSaveValidated=$false;runtimeValidated=$false
}
if($autoCleanup -and $script:failed.Count -eq 0){
    $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if(-not $root.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or
        ([IO.File]::GetAttributes($root) -band [IO.FileAttributes]::ReparsePoint)){throw 'fixture cleanup target outside TEMP'}
    Remove-Item -LiteralPath $root -Recurse -Force
}
"native09 observation: passed=$script:passed failed=$($script:failed.Count)"
if($script:failed.Count){exit 1}
