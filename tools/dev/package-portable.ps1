# 仅组装已构建的完整桌面候选，不编译、不覆盖旧包、不包含地图或游戏资源。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinaryDirectory,
    [Parameter(Mandatory)][string]$QtDirectory,
    [Parameter(Mandatory)][string[]]$QtSourceRoots,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$AgentRuntimeDirectory = '',
    [string]$NodeExecutable = '',
    [string]$NodeLicense = '',
    [string]$UpdateOutputDirectory = '',
    [string]$MinimumDirectUpgrade = '',
    [string]$ReleaseNotes = ''
)
$ErrorActionPreference = 'Stop'
if ($UpdateOutputDirectory -and !$MinimumDirectUpgrade) { throw 'Update manifest requires -MinimumDirectUpgrade' }
Set-StrictMode -Version Latest
$repo = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$binaries = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$qt = (Resolve-Path -LiteralPath $QtDirectory).Path
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Output already exists; choose a new directory: $output" }
$names = @('war3-map-editor.exe', 'war3-map-cli.exe', 'war3-map-mcp.exe', 'audastra-updater.exe',
    'war3-model-preview.exe', 'war3-model-animation.exe', 'war3-terrain-preview.exe', 'war3-mdx-scan.exe')
$renderer = 'war3-render-d3d11.dll'
$gameTestHelpers = @('audastra-warcraft-launcher.exe', 'audastra-warcraft-bootstrap.dll')
$agentRuntime = if ($AgentRuntimeDirectory) { (Resolve-Path -LiteralPath $AgentRuntimeDirectory).Path } else { '' }
if ($agentRuntime) {
    foreach ($inputFile in @('worker.bundle.mjs', 'runtime-manifest.json')) {
        if (!(Test-Path -LiteralPath (Join-Path $agentRuntime $inputFile) -PathType Leaf)) {
            throw "Missing Agent runtime: $inputFile"
        }
    }
    foreach ($inputFile in @($NodeExecutable, $NodeLicense)) {
        if (!$inputFile -or !(Test-Path -LiteralPath $inputFile -PathType Leaf)) { throw 'Node binary and complete license are required' }
    }
}
$deploy = Join-Path $qt 'bin/windeployqt.exe'
foreach ($path in @($deploy, "$repo/LICENSE.txt", "$repo/third_party/stormlib-audio/LICENSE", "$repo/third_party/detours/LICENSE.md",
        "$repo/third_party/lua53/LICENSE.txt") +
        @($names + $renderer + $gameTestHelpers | ForEach-Object { Join-Path $binaries $_ })) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" }
}
$runtimeFiles = @(Get-ChildItem -LiteralPath $runtime -Filter '*.dll' -File)
foreach ($runtimeFile in $runtimeFiles) {
    if ($runtimeFile.Name -notmatch '^(?:msvcp140(?:_[0-9]+|_atomic_wait)?|vcruntime140(?:_1)?|concrt140|vccorlib140|ucrtbase|api-ms-win-crt-[a-z0-9-]+)\.dll$') {
        throw "RuntimeDirectory accepts only app-local VC/UCRT files: $($runtimeFile.Name)"
    }
}
foreach ($required in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
        'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    if ($required -notin $runtimeFiles.Name) { throw "Missing app-local runtime: $required" }
}
$fonts = @(Get-ChildItem -LiteralPath "$repo/third_party/fonts/files" -File |
    Where-Object { $_.Extension -in '.ttf', '.otf' })
if (!$fonts.Count) { throw 'No bundled fonts; run the documented font fetch first' }
$sourceRoots = @($QtSourceRoots | ForEach-Object { (Resolve-Path -LiteralPath $_).Path })
foreach ($sourceRoot in $sourceRoots) {
    if (!(Test-Path -LiteralPath "$sourceRoot/LICENSES" -PathType Container)) {
        throw "Qt source license directory missing: $sourceRoot"
    }
}
$commit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit' }
$versionInfo = (& (Join-Path $binaries 'war3-map-cli.exe') --version-json | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or $versionInfo.product -ne 'Audastra' -or $versionInfo.commit -ne $commit) {
    throw 'Built CLI version identity does not match the source checkpoint; rebuild before packaging'
}
$dirty = @(& git -C $repo status --porcelain)
if ($LASTEXITCODE -ne 0 -or $dirty.Count) { throw 'Commit the source checkpoint before packaging' }

$bin = Join-Path $output 'bin'
$licenses = Join-Path $output 'licenses'
New-Item -ItemType Directory -Path $bin, $licenses | Out-Null
foreach ($name in $names + $renderer + $gameTestHelpers) {
    Copy-Item -LiteralPath (Join-Path $binaries $name) -Destination $bin
}
# 根目录只放引导程序 Audastra.exe（静态运行库，不需要 DLL）；它启动 bin 下的编辑器后立即退出。
Copy-Item -LiteralPath (Join-Path $binaries 'Audastra.exe') -Destination $output
$deployArgs = @('--release', '--verbose', '1', '--no-compiler-runtime', '--no-system-d3d-compiler',
    '--no-opengl-sw', '--include-plugins', 'qoffscreen', '--dir', $bin,
    '--plugindir', "$bin/plugins", '--translations', 'zh_CN') +
    @($names | ForEach-Object { Join-Path $bin $_ })
& $deploy @deployArgs
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed: $LASTEXITCODE (partial output retained)" }
"[Paths]`nPrefix = .`nPlugins = plugins`nTranslations = translations`n" |
    Set-Content -LiteralPath "$bin/qt.conf" -Encoding ascii
$runtimeFiles | Copy-Item -Destination $bin
New-Item -ItemType Directory -Path "$bin/fonts", "$licenses/fonts", "$licenses/Qt" | Out-Null
$fonts | Copy-Item -Destination "$bin/fonts"
Copy-Item -Path "$repo/third_party/fonts/LICENSES/*" -Destination "$licenses/fonts" -Recurse
Copy-Item -LiteralPath "$repo/LICENSE.txt" -Destination $licenses
Copy-Item -LiteralPath "$repo/third_party/stormlib-audio/LICENSE" -Destination "$licenses/StormLib-audio.txt"
Copy-Item -LiteralPath "$repo/third_party/detours/LICENSE.md" -Destination "$licenses/Detours-LICENSE.txt"
Copy-Item -LiteralPath "$repo/third_party/lua53/LICENSE.txt" -Destination "$licenses/Lua53-LICENSE.txt"
Copy-Item -LiteralPath "$repo/third_party/audastra-update" -Destination $licenses -Recurse
Copy-Item -LiteralPath "$repo/assets/icons/lucide/LICENSE" -Destination "$licenses/Lucide-ISC-Feather-MIT.txt"
foreach ($notice in @('NOTICE.md', 'THIRD_PARTY_NOTICES.md', 'COMMERCIAL-LICENSE.md')) {
    Copy-Item -LiteralPath "$repo/$notice" -Destination $licenses
}
Copy-Item -Path "$repo/tools/portable/*" -Destination $output -Recurse
if (Test-Path -LiteralPath "$repo/.agents/skills" -PathType Container) {
    Copy-Item -LiteralPath "$repo/.agents/skills" -Destination "$bin/skills" -Recurse
}
if ($agentRuntime) {
    Copy-Item -LiteralPath $agentRuntime -Destination "$bin/agent-runtime" -Recurse
    Copy-Item -LiteralPath $NodeExecutable -Destination "$bin/agent-runtime/node.exe"
    Copy-Item -LiteralPath $NodeLicense -Destination "$bin/agent-runtime/Node-LICENSE.txt"
    Copy-Item -LiteralPath "$bin/agent-runtime/licenses" -Destination "$licenses/Agent-runtime" -Recurse
    Copy-Item -LiteralPath $NodeLicense -Destination "$licenses/Node-LICENSE.txt"
}

# 保留实际构建模块中的许可全文、第三方通知和 SBOM。DLL 动态链接、可替换。
foreach ($sourceRoot in $sourceRoots) {
    $module = Split-Path $sourceRoot -Leaf
    $destination = Join-Path "$licenses/Qt" $module
    New-Item -ItemType Directory -Path $destination | Out-Null
    Copy-Item -LiteralPath "$sourceRoot/LICENSES" -Destination $destination -Recurse
    $notices = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File |
        Where-Object { $_.Name -match 'LICENSE|LICENCE|COPYING|NOTICE|COPYRIGHT|^qt_attribution\.json$' })
    foreach ($attribution in @($notices | Where-Object Name -eq 'qt_attribution.json')) {
        foreach ($entry in @(Get-Content -LiteralPath $attribution.FullName -Raw | ConvertFrom-Json)) {
            $referencedNotices = @()
            foreach ($field in @('LicenseFile', 'LicenseFiles', 'CopyrightFile', 'CopyrightFiles')) {
                if ($entry.PSObject.Properties[$field]) { $referencedNotices += @($entry.$field) }
            }
            foreach ($name in $referencedNotices) {
                $licenseFile = [IO.Path]::GetFullPath((Join-Path $attribution.DirectoryName $name))
                if (!$licenseFile.StartsWith($sourceRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "License file outside selected source: $licenseFile"
                }
                if (!(Test-Path -LiteralPath $licenseFile -PathType Leaf)) { throw "Missing Qt notice: $licenseFile" }
                $notices += Get-Item -LiteralPath $licenseFile
            }
        }
    }
    foreach ($notice in $notices) {
        $relative = [IO.Path]::GetRelativePath($sourceRoot, $notice.FullName)
        $target = Join-Path $destination $relative
        if (Test-Path -LiteralPath $target) { continue }
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        Copy-Item -LiteralPath $notice.FullName -Destination $target
    }
}
if (Test-Path -LiteralPath "$qt/sbom") { Copy-Item -LiteralPath "$qt/sbom" -Destination "$licenses/Qt" -Recurse }
@'
Qt is dynamically linked; compatible DLLs may be replaced. See bundled module licenses,
third-party notices and SBOM. Qt 6.8.3 source: https://download.qt.io/archive/qt/6.8/6.8.3/
MSVC runtime DLLs are app-local Microsoft Visual C++ redistributable components.
Game archives, maps, YDWE and external compilers are not part of this package.
'@ | Set-Content -LiteralPath "$licenses/THIRD-PARTY.txt" -Encoding utf8
foreach ($required in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Multimedia.dll',
        'Qt6Svg.dll', 'Qt6Network.dll', 'plugins/platforms/qwindows.dll', 'plugins/platforms/qoffscreen.dll',
        'translations/qt_zh_CN.qm', 'qt.conf')) {
    if (!(Test-Path -LiteralPath "$bin/$required" -PathType Leaf)) { throw "Incomplete deployment: $required" }
}
$files = @(Get-ChildItem -LiteralPath $output -Recurse -Force -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path = [IO.Path]::GetRelativePath($output, $_.FullName).Replace('\', '/');
        bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
[ordered]@{
    format = 1; product = 'Audastra complete desktop portable candidate'; platform = 'Windows x64';
    version = $versionInfo.version;
    source_commit = $commit; created_utc = [DateTime]::UtcNow.ToString('o');
    source_clean = $true; binary_provenance = 'Verify build receipt and executable hashes; packaging does not compile';
    files = $files; manifest_self_excluded = $true
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$output/manifest.json" -Encoding utf8
Write-Output "Portable candidate: $output ($($files.Count) files + manifest.json)"
if ($UpdateOutputDirectory) {
    & "$PSScriptRoot/package-update.ps1" -PortableDirectory $output -OutputDirectory $UpdateOutputDirectory -MinimumDirectUpgrade $MinimumDirectUpgrade -ReleaseNotes $ReleaseNotes
}
