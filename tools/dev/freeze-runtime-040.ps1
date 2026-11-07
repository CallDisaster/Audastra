# 从0.4.0发布检出冻结运行库；引导和许可证只整文件复制、核大小与摘要。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceRoot,
    [Parameter(Mandatory)][string]$BuildRoot,
    [Parameter(Mandatory)][string]$RuntimeBuildRoot,
    [Parameter(Mandatory)][string]$OpaqueInputDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$releaseCommit = 'acc5755fec7ad6fb2f788b2264537130e83c8afc'
function Plain-Path([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^[A-Za-z]:[\\/]' -or $full.Substring(2).Contains(':')) { throw 'RUNTIME_FREEZE_LOCAL_PATH_REQUIRED' }
    for ($at = $full; $at; $at = [IO.Path]::GetDirectoryName($at)) {
        if ((Test-Path -LiteralPath $at) -and ((Get-Item -LiteralPath $at -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'RUNTIME_FREEZE_REPARSE_PATH'
        }
    }
    return $full
}
function Digest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Role([string]$Name, [string]$Path) {
    $file = Get-Item -LiteralPath (Plain-Path $Path)
    if ($file.PSIsContainer -or $file.Length -le 0 -or $file.Length -gt 16MB) { throw 'RUNTIME_FREEZE_ROLE_SIZE' }
    return [ordered]@{ role = $Name; source = $file.FullName; size = $file.Length; sha256 = Digest $file.FullName }
}
function Cache-Source([string]$Directory, [string]$Expected, [string]$Platform = '') {
    $cache = Join-Path $Directory 'CMakeCache.txt'
    $home = @(Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=')
    if ($home.Count -ne 1 -or [IO.Path]::GetFullPath(($home[0].Line -split '=', 2)[1]) -ine $Expected) {
        throw 'RUNTIME_FREEZE_BUILD_SOURCE_MISMATCH'
    }
    if ($Platform) {
        $arch = @(Select-String -LiteralPath $cache -Pattern '^CMAKE_GENERATOR_PLATFORM:INTERNAL=')
        if ($arch.Count -ne 1 -or ($arch[0].Line -split '=', 2)[1] -cne $Platform) { throw 'RUNTIME_FREEZE_BUILD_PLATFORM_MISMATCH' }
    }
}
$source = Plain-Path $SourceRoot
$build = Plain-Path $BuildRoot
$runtime = Plain-Path $RuntimeBuildRoot
$opaque = Plain-Path $OpaqueInputDirectory
$out = Plain-Path $OutputDirectory
if ((Test-Path -LiteralPath $out) -or $out.StartsWith($opaque + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'RUNTIME_FREEZE_NEW_OUTPUT_REQUIRED' }
$commit = (& git -C $source rev-parse HEAD).Trim()
if ($LASTEXITCODE -or $commit -cne $releaseCommit) { throw 'RUNTIME_FREEZE_RELEASE_COMMIT_MISMATCH' }
$dirty = @(& git -C $source status --porcelain --untracked-files=normal)
if ($LASTEXITCODE -or $dirty.Count) { throw 'RUNTIME_FREEZE_CLEAN_SOURCE_REQUIRED' }
Cache-Source $build $source
Cache-Source $runtime (Join-Path $source 'src/WarcraftRuntime32') 'Win32'
$freezer = Plain-Path (Join-Path $build 'Release/audastra-runtime-package-freezer.exe')
if (!(Test-Path -LiteralPath $freezer -PathType Leaf)) { throw 'RUNTIME_FREEZE_TOOL_MISSING' }
$basisPath = Join-Path $opaque 'public-approval-030.json'
$basis = Get-Content -LiteralPath $basisPath -Raw -Encoding utf8 | ConvertFrom-Json
if ($basis.schema -ne 1 -or $basis.id -cne 'audastra-runtime' -or $basis.version -cne '0.3.0' -or
    $basis.decisions.publicDistribution -cne 'author-2026-10-27' -or
    $basis.decisions.unobfuscatedBootstrap -cne 'author-2026-10-30' -or $basis.decisions.wholeFileCopyOnly -ne $true) {
    throw 'RUNTIME_FREEZE_APPROVAL_BASIS_MISMATCH'
}
New-Item -ItemType Directory -Path $out | Out-Null
$inputs = Join-Path $out 'inputs'
New-Item -ItemType Directory -Path $inputs | Out-Null
$rows = @()
foreach ($entry in @(
    @{ role = 'bootstrap'; name = 'bootstrap.blob'; size = 26772; sha = '7bbc430b8b2e4960dd6eb05ddd8f0f0607e5a1d8c9e1af12b726955848894eea' },
    @{ role = 'license'; name = 'license.txt'; size = 1077; sha = 'fb03920ee56deac8d99a7114b366308dc3330ae414ecf13586c6eb463fe3c06a' }
)) {
    $original = Plain-Path (Join-Path $opaque $entry.name)
    if ((Get-Item -LiteralPath $original).Length -ne $entry.size -or (Digest $original) -cne $entry.sha) { throw 'RUNTIME_FREEZE_OPAQUE_IDENTITY_MISMATCH' }
    $copy = Join-Path $inputs $entry.name
    Copy-Item -LiteralPath $original -Destination $copy
    if ((Get-Item -LiteralPath $copy).Length -ne $entry.size -or (Digest $copy) -cne $entry.sha -or (Digest $original) -cne $entry.sha) {
        throw 'RUNTIME_FREEZE_OPAQUE_COPY_MISMATCH'
    }
    $rows += Role $entry.role $copy
}
if ($basis.files.bootstrap -cne ($rows | Where-Object role -CEQ 'bootstrap').sha256) { throw 'RUNTIME_FREEZE_BOOTSTRAP_APPROVAL_MISMATCH' }
foreach ($entry in @(
    @{ role = 'payload'; path = (Join-Path $runtime 'Release/warvk_runtime.dll') },
    @{ role = 'module'; path = (Join-Path $runtime 'Release/warvk_runtime_module.dll') },
    @{ role = 'client'; path = (Join-Path $build 'ui-library/AudastraUI/jass/AudastraUI.j') }
)) {
    $row = Role $entry.role $entry.path
    $copy = Join-Path $inputs ([IO.Path]::GetFileName($entry.path))
    Copy-Item -LiteralPath $row.source -Destination $copy
    if ((Digest $copy) -cne $row.sha256) { throw 'RUNTIME_FREEZE_PRODUCT_COPY_MISMATCH' }
    $rows += Role $entry.role $copy
}
$orderedRows = @('bootstrap','payload','module','client','license') | ForEach-Object { $name = $_; $rows | Where-Object role -CEQ $name }
$origin = @(
    'Audastra map runtime 0.4.0 - source and freeze provenance',
    "Release source commit: $releaseCommit",
    'Build: CMake; Visual Studio 2026 MSVC; Win32 Release; static CRT.',
    'Targets: audastra-map-runtime and audastra-module-runtime.',
    'Client: CMake AudastraUiLibraryBundle output; normalized LF; SDK and UI client composed by the normal build.',
    ('Freezer executable SHA-256: ' + (Digest $freezer)),
    ('Approval basis SHA-256: ' + (Digest $basisPath)),
    'Author decisions: public distribution 2026-10-27; unobfuscated bootstrap 2026-10-30; whole-file copy only.',
    'Opaque inputs: bootstrap and license copied as whole files; only size and SHA-256 checked; no text decoding, parsing, editing or execution.',
    'Files:'
)
foreach ($row in $orderedRows) { $origin += ('  {0}: bytes={1}; SHA-256={2}' -f $row.role, $row.size, $row.sha256) }
$origin += @(
    'Freeze checks: clean release checkout and build source/platform, input identities, fixed roles/paths, package budgets and shared package capture.',
    'The successful freeze/install receipts record completion of those checks; build success is not gameplay proof.',
    'At this immutable freeze, normal-save and 1.27a gameplay acceptance are not yet claimed.',
    'Later acceptance is recorded separately against the final packageSha256; this origin file is not rewritten after gameplay.',
    'Other maps, configurations and complete lifecycle coverage are not certified by this provenance file.'
)
$originPath = Join-Path $inputs 'origin.txt'
$origin -join "`n" | Set-Content -LiteralPath $originPath -Encoding utf8NoBOM
$orderedRows += Role 'origin' $originPath
$plan = [ordered]@{ schema = 2; id = 'audastra-runtime'; version = '0.4.0'; scope = 'public'; files = @($orderedRows) }
$planPath = Join-Path $out 'plan.json'
$plan | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $planPath -Encoding utf8NoBOM
$planSha = Digest $planPath
$installer = Join-Path $source 'tools/dev/install-map-runtime.ps1'
& pwsh -NoProfile -File $installer -FreezerExecutable $freezer -Plan $planPath -PlanSha256 $planSha -OutputDirectory (Join-Path $out 'pending') *> (Join-Path $out 'pending.log')
if ($LASTEXITCODE) { throw 'RUNTIME_FREEZE_PENDING_FAILED' }
$relativeManifest = 'runtime/packages/audastra-runtime-0.4.0/manifest.json'
$packageSha = Digest (Join-Path (Join-Path $out 'pending') $relativeManifest)
$approvedFiles = [ordered]@{}
foreach ($role in @('bootstrap','payload','module','client')) { $approvedFiles[$role] = ($orderedRows | Where-Object role -CEQ $role).sha256 }
$approval = [ordered]@{ schema = 1; id = 'audastra-runtime'; version = '0.4.0'; packageSha256 = $packageSha; decisions = $basis.decisions; files = $approvedFiles }
$approvalPath = Join-Path $out 'public-approval.json'
$approval | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $approvalPath -Encoding utf8NoBOM
$approvalSha = Digest $approvalPath
$install = Join-Path $out 'install'
& pwsh -NoProfile -File $installer -FreezerExecutable $freezer -Plan $planPath -PlanSha256 $planSha -OutputDirectory $install -ApprovalRecord $approvalPath -ApprovalSha256 $approvalSha *> (Join-Path $out 'install.log')
if ($LASTEXITCODE -or (Digest (Join-Path $install $relativeManifest)) -cne $packageSha) { throw 'RUNTIME_FREEZE_APPROVED_INSTALL_FAILED' }
$receipt = [ordered]@{
    releaseCommit = $releaseCommit; client = $approvedFiles.client; clientBytes = ($orderedRows | Where-Object role -CEQ 'client').size
    payload = $approvedFiles.payload; module = $approvedFiles.module; packageSha256 = $packageSha; approvalSha256 = $approvalSha
    approvalBasisSha256 = Digest $basisPath; planSha256 = $planSha; install = $install
    wholeFileCopyOnly = $true; gameplayAcceptance = 'separate receipt required; not implied by freeze'
}
$receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'freeze-summary.json') -Encoding utf8NoBOM
$receipt | ConvertTo-Json -Depth 4
