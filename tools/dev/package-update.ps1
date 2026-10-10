# 从新便携候选生成ZIP和旁置更新清单。默认未签名，绝不可用于正式发布。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PortableDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$MinimumDirectUpgrade,
    [string]$ReleaseNotes = ''
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$portable=(Resolve-Path -LiteralPath $PortableDirectory).Path
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'Choose a new update output directory'}
if($output.StartsWith($portable.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Update output must be outside portable input'}
$manifestPath=Join-Path $portable 'manifest.json'
$portableManifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if($portableManifest.format -ne 1 -or !$portableManifest.source_clean -or $portableManifest.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.+-]+)?$' -or
    $portableManifest.source_commit -notmatch '^[0-9a-f]{40}$'){throw 'Portable candidate identity is invalid'}
if($MinimumDirectUpgrade -notmatch '^[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.+-]+)?$'){throw 'MinimumDirectUpgrade must be SemVer'}
if([Text.Encoding]::UTF8.GetByteCount($ReleaseNotes) -gt 8192){throw 'Release notes exceed 8192 UTF-8 bytes'}
# 盘点所有当前文件并校验便携清单的每项身份；更新清单额外包含原manifest.json。
$files=@(Get-ChildItem -LiteralPath $portable -Recurse -Force -File | Sort-Object FullName | ForEach-Object {
    if($_.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse input refused'}
    $relative=[IO.Path]::GetRelativePath($portable,$_.FullName).Replace('\','/')
    if($relative -notmatch '^[\x20-\x7e]+$' -or $relative -match '[:\\<>"|?*]' -or $_.Length -gt 1GB){throw 'Unsupported program file path or size'}
    [ordered]@{path=$relative;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
})
if($files.Count -gt 4096 -or $files.Count -ne @($portableManifest.files).Count+1){throw 'Portable file count changed or exceeds update limit'}
foreach($entry in $portableManifest.files){
    $actual=@($files | Where-Object path -CEQ $entry.path)
    if($actual.Count -ne 1 -or $actual[0].bytes -ne $entry.bytes -or $actual[0].sha256 -cne $entry.sha256){throw 'Portable content no longer matches its clean build manifest'}
}
foreach($directory in @(Get-ChildItem -LiteralPath $portable -Recurse -Force -Directory)){
    if($directory.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse directory refused'}
}
New-Item -ItemType Directory -Path $output | Out-Null
# 更新器下载的载荷包；给用户手动下载的整包（含旁置签名清单）另由 make-release-assets.ps1 生成。
$zipName='Audastra-'+$portableManifest.version+'-windows-x64-update.zip'
$zipPath=Join-Path $output $zipName
[IO.Compression.ZipFile]::CreateFromDirectory($portable,$zipPath,[IO.Compression.CompressionLevel]::Optimal,$false,[Text.Encoding]::UTF8)
$zip=Get-Item -LiteralPath $zipPath
if($zip.Length -gt 2GB){throw 'Update archive exceeds 2GiB'}
$update=[ordered]@{
    format='audastra-update';schema=1;product='Audastra';platform='windows-x64';version=$portableManifest.version;
    minimum_direct_upgrade=$MinimumDirectUpgrade;source_commit=$portableManifest.source_commit;
    zip=[ordered]@{filename=$zipName;bytes=$zip.Length;sha256=(Get-FileHash -LiteralPath $zipPath).Hash.ToLowerInvariant()};
    release_notes=$ReleaseNotes;files=$files
}
$json=$update | ConvertTo-Json -Depth 8
if([Text.Encoding]::UTF8.GetByteCount($json) -gt 1MB){throw 'Update manifest exceeds 1MiB'}
[IO.File]::WriteAllText((Join-Path $output 'update-manifest.json'),$json+"`n",[Text.UTF8Encoding]::new($false))
Write-Output 'UNSIGNED: 不可用于正式发布。使用发布者本机 audastra-sign-update 签名，并保留原始清单字节。'
Write-Output $output
