# 共享后台入口：缺 app-local 依赖时不启动 EXE，系统加载错误不弹到作者桌面。
function Initialize-ForgeQuietRuntime {
    if (-not ('ForgeQuietRuntimeNative' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ForgeQuietRuntimeNative {
    [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr AddDllDirectory(string path);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] public static extern bool RemoveDllDirectory(IntPtr cookie);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] public static extern bool FreeLibrary(IntPtr handle);
}
'@
    }
}

function Assert-ForgeAppLocalRuntime {
    param([Parameter(Mandatory)][string]$Directory, [switch]$RequireGui)
    $directoryPath = (Resolve-Path -LiteralPath $Directory).Path
    $dependencies = @('Qt6Core.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
        'vcruntime140.dll', 'vcruntime140_1.dll')
    if ($RequireGui) {
        $dependencies += @('Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'Qt6Svg.dll')
        foreach ($pluginName in @('qoffscreen.dll', 'qwindows.dll')) {
            $plugin = Join-Path $directoryPath "plugins/platforms/$pluginName"
            if (!(Test-Path -LiteralPath $plugin -PathType Leaf)) {
                $plugin = Join-Path $directoryPath "platforms/$pluginName"
            }
            if (!(Test-Path -LiteralPath $plugin -PathType Leaf)) {
                throw "Missing platform plugin; executable not launched: $pluginName"
            }
            $dependencies += $plugin
        }
    }
    $directoryCookie = [ForgeQuietRuntimeNative]::AddDllDirectory($directoryPath)
    if ($directoryCookie -eq [IntPtr]::Zero) { throw 'Cannot establish app-local DLL lookup; executable not launched' }
    try {
    foreach ($dependency in $dependencies) {
        $dependencyPath = if ([IO.Path]::IsPathRooted($dependency)) { $dependency } else { Join-Path $directoryPath $dependency }
        if (!(Test-Path -LiteralPath $dependencyPath -PathType Leaf)) {
            throw "Missing app-local dependency; executable not launched: $dependency"
        }
        # 平台插件在子目录；其 Qt 依赖仍来自 EXE 目录，不能只搜索插件旁边。
        $handle = [ForgeQuietRuntimeNative]::LoadLibraryExW($dependencyPath, [IntPtr]::Zero, 0x1100)
        if ($handle -eq [IntPtr]::Zero) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "App-local dependency load failed; executable not launched: $dependency (Win32 $errorCode)"
        }
        [void][ForgeQuietRuntimeNative]::FreeLibrary($handle)
        Write-Output "$dependency app-local load OK"
    }
    } finally {
        [void][ForgeQuietRuntimeNative]::RemoveDllDirectory($directoryCookie)
    }
}
