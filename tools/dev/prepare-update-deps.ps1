# 官方固定版本依赖，源码和二进制仅写新缓存目录，不写用户安装或已发包。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$MSBuildPath = ''
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$depsRoot=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $depsRoot){throw 'Choose a new dependency cache directory'}
if(!$MSBuildPath){
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if(!(Test-Path -LiteralPath $vswhere)){throw 'Supply -MSBuildPath for an installed Visual Studio 2026'}
    $MSBuildPath=& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild/**/Bin/MSBuild.exe' | Select-Object -First 1
}
$MSBuildPath=(Resolve-Path -LiteralPath $MSBuildPath).Path
New-Item -ItemType Directory -Path $depsRoot | Out-Null
function Download-Verified([string]$Url,[string]$Name,[string]$Sha){
    $target=Join-Path $depsRoot $Name
    Invoke-WebRequest -Uri $Url -OutFile $target
    if((Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant() -cne $Sha){throw "Dependency SHA mismatch: $Name"}
    return $target
}
$sodiumArchive=Download-Verified 'https://github.com/jedisct1/libsodium/releases/download/1.0.22-RELEASE/libsodium-1.0.22.tar.gz' 'libsodium-1.0.22.tar.gz' 'adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349'
& tar -xf $sodiumArchive -C $depsRoot
if($LASTEXITCODE -ne 0){throw 'libsodium source extraction failed'}
$minizArchive=Download-Verified 'https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip' 'miniz-3.1.2.zip' 'f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a'
Expand-Archive -LiteralPath $minizArchive -DestinationPath (Join-Path $depsRoot 'miniz-3.1.2')
$jsonHeader=Download-Verified 'https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp' 'json.hpp' 'aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63'
$jsonDir=Join-Path $depsRoot 'json-3.12.0/include/nlohmann'
New-Item -ItemType Directory -Path $jsonDir | Out-Null
Move-Item -LiteralPath $jsonHeader -Destination (Join-Path $jsonDir 'json.hpp')
$luaPaths = & (Join-Path $PSScriptRoot 'prepare-lua-check-deps.ps1') -OutputDirectory (Join-Path $depsRoot 'lua53') | Out-String | ConvertFrom-Json
$sodiumRoot=Join-Path $depsRoot 'libsodium-1.0.22'
$props=Join-Path $sodiumRoot 'builds/msvc/properties/ReleaseLIB.props'
$propsText=Get-Content -LiteralPath $props -Raw
$oldRuntime='<RuntimeLibrary>MultiThreaded</RuntimeLibrary>'
if(([regex]::Matches($propsText,[regex]::Escape($oldRuntime))).Count -ne 1){throw 'Unexpected upstream RuntimeLibrary configuration'}
# 只改宿主运行库配置；加密实现和编译器架构选项均保留上游。
[IO.File]::WriteAllText($props,$propsText.Replace($oldRuntime,'<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>'),[Text.UTF8Encoding]::new($false))
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass='BelowNormal'
$sodiumProject=Join-Path $sodiumRoot 'builds/msvc/vs2026/libsodium/libsodium.vcxproj'
& $MSBuildPath $sodiumProject /p:Configuration=ReleaseLIB /p:Platform=x64 /p:CL_MPCount=1 /m:1 /nr:false /v:minimal *> (Join-Path $depsRoot 'sodium-build.log')
if($LASTEXITCODE -ne 0){throw 'libsodium /MD build failed; inspect sodium-build.log'}
$sodiumLibrary=Join-Path $sodiumRoot 'bin/x64/Release/v145/static/libsodium.lib'
if(!(Test-Path -LiteralPath $sodiumLibrary)){throw 'Expected VS2026 static library not produced'}
$paths=[ordered]@{
    AUD_SODIUM_SOURCE_DIR=$sodiumRoot;AUD_SODIUM_LIBRARY=$sodiumLibrary;
    AUD_MINIZ_SOURCE_DIR=(Join-Path $depsRoot 'miniz-3.1.2');AUD_JSON_INCLUDE_DIR=(Join-Path $depsRoot 'json-3.12.0/include');
    AUD_LUA53_SOURCE_DIR=$luaPaths.AUD_LUA53_SOURCE_DIR
}
$paths | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $depsRoot 'cmake-paths.local.json') -Encoding utf8
$paths | ConvertTo-Json
