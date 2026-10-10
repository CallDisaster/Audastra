# 用一个新的专用 YDWE 实例打开地图副本并执行“保存地图”，完成后关闭该实例。
# 只操作本脚本启动的进程；运行前若已有 World Editor 在运行则拒绝（可能是作者的未保存窗口）。
# 地图会被 YDWE 原地改写，务必传入副本。
# 用法：ydwe-save.ps1 -YdweRoot <YDWE目录> -Map <副本.w3x> [-SaveCommand 4] [-TimeoutSeconds 600]
param([string]$YdweRoot, [string]$Map, [int]$SaveCommand = 4, [int]$TimeoutSeconds = 600,
    [string]$DirectGameRoot, [string]$DirectLoaderReceipt, [string]$DirectLoaderReceiptSha256,
    [string]$InputDesktopEvidence, [ref]$InputDesktopOwner, [int]$InputDesktopPjassCodePage = 65001,
    [Collections.Generic.Dictionary[string,string]]$InputDesktopEnvironment)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

public sealed class YdweDialog {
    public IntPtr Handle;
    public IntPtr Owner;
    public int OwnerPid;
    public string Title;
    public string Body;
    public IntPtr YesButton;
    public string YesButtonText;
    public bool YesButtonEnabled;
    public bool IsToolWindow;
    public bool IsEnabled;
    public bool OwnerEnabled;
    public bool HasProgressControl;
    public bool IsVisible;
    public bool OwnVisibleStyle;
}

public sealed class YdweFrame {
    public IntPtr Handle;
    public string Title;
    public bool IsVisible;
    public bool OwnVisibleStyle;
    public bool IsEnabled;
}

public static class YdweWin {
    private delegate bool EnumWindowProc(IntPtr hwnd, IntPtr data);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowProc callback, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, EnumWindowProc callback, IntPtr data);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr hwnd, uint command);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int index);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowTextLength(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int capacity);

    private static string WindowText(IntPtr hwnd) {
        var text = new StringBuilder(GetWindowTextLength(hwnd) + 1);
        GetWindowText(hwnd, text, text.Capacity);
        return text.ToString();
    }

    private static string WindowClass(IntPtr hwnd) {
        var name = new StringBuilder(256);
        GetClassName(hwnd, name, name.Capacity);
        return name.ToString();
    }

    public static YdweDialog[] GetDialogs(int editorPid) {
        return GetDialogs(editorPid, false);
    }

    public static YdweDialog[] GetDialogs(int editorPid, bool includeHidden) {
        var dialogs = new List<YdweDialog>();
        EnumWindows((hwnd, data) => {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            if (pid != (uint)editorPid || (!includeHidden && !IsWindowVisible(hwnd)) || WindowClass(hwnd) != "#32770") return true;
            var dialog = new YdweDialog {
                Handle = hwnd,
                Title = WindowText(hwnd),
                Owner = GetWindow(hwnd, 4),
                IsToolWindow = (GetWindowLongPtr(hwnd, -20).ToInt64() & 0x80) != 0,
                IsEnabled = IsWindowEnabled(hwnd),
                IsVisible = IsWindowVisible(hwnd),
                OwnVisibleStyle = (GetWindowLongPtr(hwnd, -16).ToInt64() & 0x10000000) != 0
            };
            if (dialog.Owner != IntPtr.Zero) {
                uint ownerPid;
                GetWindowThreadProcessId(dialog.Owner, out ownerPid);
                dialog.OwnerPid = (int)ownerPid;
                dialog.OwnerEnabled = IsWindowEnabled(dialog.Owner);
            }
            var lines = new List<string>();
            EnumChildWindows(hwnd, (child, ignored) => {
                var kind = WindowClass(child);
                if (kind == "Static") {
                    var value = WindowText(child).Trim();
                    if (value.Length != 0) lines.Add(value);
                } else if (kind == "msctls_progress32") {
                    dialog.HasProgressControl = true;
                } else if (kind == "Button" && GetDlgCtrlID(child) == 6) {
                    dialog.YesButton = child;
                    dialog.YesButtonText = WindowText(child);
                    bool ownVisible = (GetWindowLongPtr(child, -16).ToInt64() & 0x10000000) != 0;
                    dialog.YesButtonEnabled = (includeHidden ? ownVisible : IsWindowVisible(child)) && IsWindowEnabled(child);
                }
                return true;
            }, IntPtr.Zero);
            dialog.Body = string.Join("\n", lines);
            dialogs.Add(dialog);
            return true;
        }, IntPtr.Zero);
        return dialogs.ToArray();
    }

    public static YdweFrame[] GetEditorFrames(int editorPid) {
        return GetEditorFrames(editorPid, false);
    }

    public static YdweFrame[] GetEditorFrames(int editorPid, bool includeHidden) {
        var frames = new List<YdweFrame>();
        EnumWindows((hwnd, data) => {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            if (pid == (uint)editorPid && (includeHidden || IsWindowVisible(hwnd)) &&
                WindowClass(hwnd) == "Warcraft III" && GetWindow(hwnd, 4) == IntPtr.Zero) {
                frames.Add(new YdweFrame { Handle = hwnd, Title = WindowText(hwnd),
                    IsVisible = IsWindowVisible(hwnd), IsEnabled = IsWindowEnabled(hwnd),
                    OwnVisibleStyle = (GetWindowLongPtr(hwnd, -16).ToInt64() & 0x10000000) != 0 });
            }
            return true;
        }, IntPtr.Zero);
        return frames.ToArray();
    }

    public static bool IsEditorFrame(IntPtr hwnd, int editorPid) {
        uint pid;
        GetWindowThreadProcessId(hwnd, out pid);
        return hwnd != IntPtr.Zero && pid == (uint)editorPid && WindowClass(hwnd) == "Warcraft III" &&
            GetWindow(hwnd, 4) == IntPtr.Zero;
    }

    public static bool IsEditorFrameEnabled(IntPtr hwnd, int editorPid) {
        return IsEditorFrame(hwnd, editorPid) && IsWindowEnabled(hwnd);
    }

    public static bool IsOwnedDisabledModal(IntPtr dialog, IntPtr frame, int editorPid) {
        uint pid;
        GetWindowThreadProcessId(dialog, out pid);
        if (pid != (uint)editorPid || WindowClass(dialog) != "#32770" || !IsWindowEnabled(dialog) ||
            (GetWindowLongPtr(dialog, -20).ToInt64() & 0x80) != 0 || !IsEditorFrame(frame, editorPid) ||
            IsWindowEnabled(frame)) return false;
        IntPtr owner = GetWindow(dialog, 4);
        if (owner == IntPtr.Zero || IsWindowEnabled(owner)) return false;
        var seen = new HashSet<IntPtr>();
        for (int i = 0; owner != IntPtr.Zero && i < 16; ++i) {
            if (!seen.Add(owner)) return false;
            GetWindowThreadProcessId(owner, out pid);
            if (pid != (uint)editorPid) return false;
            if (owner == frame) return true;
            owner = GetWindow(owner, 4);
        }
        return false;
    }
}

public sealed class YdweNativeInventory {
    public string[] Files;
    public string[] Directories;
}
public sealed class YdweNativeSnapshot {
    public long Length;
    public string Sha256;
    public byte[] Bytes;
}
public static class YdweNativeFile {
    [StructLayout(LayoutKind.Sequential)] private struct ByHandleFileInformation {
        public uint Attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        public uint VolumeSerialNumber, FileSizeHigh, FileSizeLow, NumberOfLinks, FileIndexHigh, FileIndexLow;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security,
        uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out ByHandleFileInformation info);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFileAttributesW(string path);
    // 只在已验证的本机 drive 路径上添加 Win32 长路径前缀。
    private static string NativePath(string path) { return @"\\?\" + path; }
    private static uint Attributes(string path, bool allowMissing) {
        uint value = GetFileAttributesW(NativePath(path));
        if (value != 0xffffffffu) return value;
        int error = Marshal.GetLastWin32Error();
        if (allowMissing && (error == 2 || error == 3)) return value;
        throw new System.ComponentModel.Win32Exception(error);
    }
    private static string CanonicalPath(string path) {
        if (String.IsNullOrEmpty(path) || path.Length < 3 || !Path.IsPathFullyQualified(path) ||
            path.StartsWith(@"\\", StringComparison.Ordinal) || path.IndexOf('\0') >= 0 ||
            path.Substring(2).IndexOf(':') >= 0 || path[1] != ':' ||
            !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
            throw new InvalidOperationException("native 路径须为本机绝对普通路径");
        foreach (string part in path.Substring(2).Split(new [] { '\\', '/' })) {
            if (part == "." || part == ".." || part.EndsWith(" ", StringComparison.Ordinal) ||
                part.EndsWith(".", StringComparison.Ordinal) || part.IndexOfAny(new [] { '<', '>', '|', '?', '*' }) >= 0)
                throw new InvalidOperationException("native 路径含 Windows 别名或越界段");
        }
        return Path.GetFullPath(path);
    }
    private static uint CheckAncestors(string path, bool allowMissingLeaf) {
        uint leaf = Attributes(path, allowMissingLeaf);
        string cursor = path;
        while (cursor != null) {
            uint attributes = cursor == path ? leaf : Attributes(cursor, true);
            if (attributes != 0xffffffffu && (attributes & 0x400u) != 0)
                throw new InvalidOperationException("native 路径含 reparse point，拒绝");
            cursor = Path.GetDirectoryName(cursor);
        }
        return leaf;
    }
    private static ByHandleFileInformation FileInfo(SafeFileHandle handle) {
        ByHandleFileInformation info;
        if (!GetFileInformationByHandle(handle, out info))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        if ((info.Attributes & (0x400u | 0x10u)) != 0 || info.NumberOfLinks != 1)
            throw new InvalidOperationException("native file contains reparse/directory/hardlink alias");
        return info;
    }
    private static bool SameFile(ByHandleFileInformation a, ByHandleFileInformation b) {
        return a.VolumeSerialNumber == b.VolumeSerialNumber && a.FileIndexHigh == b.FileIndexHigh &&
            a.FileIndexLow == b.FileIndexLow && a.FileSizeHigh == b.FileSizeHigh && a.FileSizeLow == b.FileSizeLow &&
            a.LastWriteTime.dwHighDateTime == b.LastWriteTime.dwHighDateTime &&
            a.LastWriteTime.dwLowDateTime == b.LastWriteTime.dwLowDateTime;
    }
    public static string PlainPath(string path) {
        string full = CanonicalPath(path);
        uint attributes = CheckAncestors(full, true);
        if (attributes != 0xffffffffu && (attributes & 0x10u) == 0) AssertSingleLinkFile(full);
        return full;
    }
    public static bool IsDirectory(string path) {
        string full = CanonicalPath(path);
        uint attributes = CheckAncestors(full, true);
        return attributes != 0xffffffffu && (attributes & 0x10u) != 0;
    }
    public static YdweNativeSnapshot Snapshot(string path, long captureLimit, long expectedLength = -1) {
        string full = CanonicalPath(path);
        CheckAncestors(full, false);
        // 同一 leaf handle 验证 reparse、hardlink、长度、SHA 和读后身份；不缓存祖先安全状态。
        using (var handle = CreateFileW(NativePath(full), 0x80000080u, 1, IntPtr.Zero, 3, 0x0a200000u, IntPtr.Zero)) {
            if (handle.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            ByHandleFileInformation before = FileInfo(handle);
            ulong unsignedLength = ((ulong)before.FileSizeHigh << 32) | before.FileSizeLow;
            if (unsignedLength > Int64.MaxValue) throw new InvalidOperationException("native 文件长度越过预算");
            long length = (long)unsignedLength;
            if (expectedLength >= 0 && length != expectedLength)
                throw new InvalidOperationException("native 依赖文件不存在或字节身份改变");
            if (captureLimit >= 0 && (length > captureLimit || length > Int32.MaxValue))
                throw new InvalidOperationException("direct-loader 回执越过8MiB预算");
            using (var stream = new FileStream(handle, FileAccess.Read, 65536, false)) {
                byte[] captured = null;
                byte[] digest;
                using (var sha = SHA256.Create()) {
                    if (captureLimit >= 0) {
                        captured = new byte[(int)length];
                        int offset = 0;
                        while (offset < captured.Length) {
                            int read = stream.Read(captured, offset, captured.Length - offset);
                            if (read == 0) throw new InvalidOperationException("native 文件读取中长度变化");
                            offset += read;
                        }
                        if (stream.ReadByte() != -1) throw new InvalidOperationException("native 文件读取中长度变化");
                        digest = sha.ComputeHash(captured);
                    } else { digest = sha.ComputeHash(stream); }
                }
                if (!SameFile(before, FileInfo(handle))) throw new InvalidOperationException("native 文件读取中身份变化");
                CheckAncestors(full, false);
                using (var current = CreateFileW(NativePath(full), 0x80u, 7, IntPtr.Zero, 3, 0x02200000u, IntPtr.Zero)) {
                    if (current.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                    if (!SameFile(before, FileInfo(current))) throw new InvalidOperationException("native 文件路径读取中身份变化");
                }
                return new YdweNativeSnapshot { Length = length, Sha256 = Convert.ToHexString(digest).ToLowerInvariant(), Bytes = captured };
            }
        }
    }
    public static void AssertFileRecord(string path, long length, string sha256) {
        YdweNativeSnapshot snapshot = Snapshot(path, -1, length);
        if (snapshot.Length != length || snapshot.Sha256 != sha256)
            throw new InvalidOperationException("native 依赖文件不存在或字节身份改变");
    }
    public static string ReadBoundText(string path, string sha256) {
        YdweNativeSnapshot snapshot = Snapshot(path, 8388608);
        if (snapshot.Sha256 != sha256) throw new InvalidOperationException("direct-loader 回执 SHA 不符");
        return new UTF8Encoding(false, true).GetString(snapshot.Bytes).TrimStart('\ufeff');
    }
    public static YdweNativeInventory Inventory(string root, string excludedRoot) {
        var pending = new Stack<string>();
        var files = new List<string>();
        var directories = new List<string>();
        pending.Push(root);
        var options = new EnumerationOptions { AttributesToSkip = 0, IgnoreInaccessible = false,
            RecurseSubdirectories = false, ReturnSpecialDirectories = false };
        while (pending.Count > 0) {
            string current = PlainPath(pending.Pop());
            if (!IsDirectory(current)) throw new InvalidOperationException("native 清单根不是普通目录");
            foreach (string entry in Directory.EnumerateFileSystemEntries(current, "*", options)) {
                string plain = PlainPath(entry);
                bool directory = IsDirectory(plain);
                if (!String.IsNullOrEmpty(excludedRoot) && String.Equals(plain, excludedRoot, StringComparison.OrdinalIgnoreCase)) {
                    if (!directory) throw new InvalidOperationException("native 排除的宿主不是普通目录");
                    continue;
                }
                if (directory) { directories.Add(plain); pending.Push(plain); }
                else { files.Add(plain); }
                if (files.Count > 8192 || directories.Count > 10000)
                    throw new InvalidOperationException("native 目录清单越过预算");
            }
        }
        return new YdweNativeInventory { Files = files.ToArray(), Directories = directories.ToArray() };
    }
    public static void AssertSingleLinkFile(string path) {
        using (var handle = CreateFileW(NativePath(path), 0x80, 7, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero)) {
            if (handle.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            FileInfo(handle);
        }
    }
}
"@
function Get-MapFingerprint([string]$Path) {
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    $file = Get-Item -LiteralPath $Path
    [pscustomobject]@{
        Sha256 = $hash
        Length = $file.Length
        WriteTicks = $file.LastWriteTimeUtc.Ticks
    }
}

function Assert-PlainNativePath([string]$Path) {
    return [YdweNativeFile]::PlainPath($Path)
}

function Assert-NativeMember([object]$Member) {
    if ($Member -isnot [string] -or [string]::IsNullOrWhiteSpace($Member) -or
        [IO.Path]::IsPathRooted($Member) -or $Member.Contains(':') -or $Member.Contains([char]0) -or
        @($Member -split '[\\/]' | Where-Object {
            $_ -in @('', '.', '..') -or $_.EndsWith(' ') -or $_.EndsWith('.') -or $_ -match '[<>|?*]'
        }).Count -gt 0) { throw 'native 依赖相对路径无效' }
}

function Assert-NativeFileRecord([string]$Path, [object]$Record) {
    if (($Record.bytes -isnot [int] -and $Record.bytes -isnot [long]) -or $Record.bytes -lt 0 -or
        $Record.sha256 -isnot [string] -or $Record.sha256 -cnotmatch '^[a-f0-9]{64}$') {
        throw 'native 依赖文件不存在或字节身份改变'
    }
    [YdweNativeFile]::AssertFileRecord($Path, $Record.bytes, $Record.sha256)
}

function Get-NativeDirectoryInventory([string]$Root, [string]$ExcludedRoot) {
    return [YdweNativeFile]::Inventory($Root, $ExcludedRoot)
}

function Assert-DirectNativeLoader([string]$GameRoot, [string]$HostRoot, [string]$ReceiptPath, [string]$ExpectedSha) {
    $preflightTimer = [Diagnostics.Stopwatch]::StartNew()
    Write-Information 'direct-preflight phase=receipt begin' -InformationAction Continue
    $game = Assert-PlainNativePath $GameRoot
    $nativeHost = Assert-PlainNativePath $HostRoot
    $receiptFile = Assert-PlainNativePath $ReceiptPath
    if ($ExpectedSha -cnotmatch '^[a-f0-9]{64}$') {
        throw 'direct-loader 回执 SHA 不符'
    }
    $receipt = [YdweNativeFile]::ReadBoundText($receiptFile, $ExpectedSha) | ConvertFrom-Json
    if ($receipt.schema -cne 'p1-native-loader-v1' -or $receipt.status -cne 'prepared' -or
        $receipt.sourceUnchanged -isnot [bool] -or -not $receipt.sourceUnchanged -or
        $receipt.launched -isnot [bool] -or $receipt.launched -or
        $receipt.registryWritten -isnot [bool] -or $receipt.registryWritten -or
        $receipt.nativeSaveValidated -isnot [bool] -or $receipt.nativeSaveValidated -or
        -not [string]::Equals($game, [IO.Path]::GetFullPath($receipt.gameRoot), [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals($nativeHost, [IO.Path]::GetFullPath($receipt.hostRoot), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'direct-loader 回执与本次独立游戏/宿主身份不符'
    }
    $gamePrefix = $game.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $hostPrefix = $nativeHost.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ([string]::Equals($game, $nativeHost, [StringComparison]::OrdinalIgnoreCase) -or
        $game.StartsWith($hostPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $receiptFile.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $receiptFile.StartsWith($hostPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'native 游戏、宿主或外部回执目录关系无效'
    }
    if ($receipt.loader.relativePath -cne 'worldeditydwe.exe' -or
        $receipt.config.relativePath -cne 'worldeditydwe.exe.config') { throw '非固定 native loader 名称' }
    if ($receipt.config.bytes -ne 121 -or
        $receipt.config.sha256 -cne '8a66aa443cb364fb9e2c76548e75efe537c0f50190081a9f66a6c8ae95e5e10e') {
        throw 'native config 必须为已审查的固定121字节模板'
    }
    $dependencies = @($receipt.dependencies)
    if ($dependencies.Count -lt 16 -or $dependencies.Count -gt 8192) { throw 'native 依赖清单数量无效' }
    $expectedFiles = @{
        game = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        host = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    }
    $validatedFiles = 0
    Write-Information "direct-preflight phase=files begin expected=$($dependencies.Count)" -InformationAction Continue
    foreach ($file in $dependencies) {
        if ($file.root -cnotin @('game', 'host')) { throw 'native 依赖根类型无效' }
        Assert-NativeMember $file.relativePath
        $root = if ($file.root -ceq 'game') { $game } else { $nativeHost }
        $path = Assert-PlainNativePath (Join-Path $root $file.relativePath)
        if (-not $path.StartsWith($root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
            -not $expectedFiles[$file.root].Add($path) -or
            ($file.root -ceq 'game' -and $nativeHost.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase) -and
                $path.StartsWith($hostPrefix, [StringComparison]::OrdinalIgnoreCase))) {
            throw 'native 依赖成员重复、越界或错误归入宿主子树'
        }
        Assert-NativeFileRecord $path $file
        $validatedFiles++
        if ($validatedFiles % 256 -eq 0 -or $validatedFiles -eq $dependencies.Count) {
            Write-Information "direct-preflight phase=files validated=$validatedFiles expected=$($dependencies.Count) elapsedMs=$($preflightTimer.ElapsedMilliseconds)" -InformationAction Continue
        }
    }
    if ($null -eq $receipt.directories -or
        @($receipt.directories.PSObject.Properties.Name).Count -ne 2 -or
        @($receipt.directories.PSObject.Properties.Name | Where-Object { $_ -cnotin @('game', 'host') }).Count -gt 0) {
        throw 'native 完整目录清单缺失或根类型无效'
    }
    foreach ($name in @('game', 'host')) {
        $root = if ($name -ceq 'game') { $game } else { $nativeHost }
        $expectedDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $directoryRows = $receipt.directories.$name
        if ($directoryRows -isnot [array] -or $directoryRows.Count -gt 10000) { throw 'native 目录清单必须为有界数组' }
        foreach ($directory in $directoryRows) {
            Assert-NativeMember $directory
            $path = Assert-PlainNativePath (Join-Path $root $directory)
            if (-not $expectedDirectories.Add($path) -or -not [YdweNativeFile]::IsDirectory($path)) {
                throw 'native 目录不存在或重复'
            }
        }
        $excluded = if ($name -ceq 'game' -and $nativeHost.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase)) { $nativeHost } else { $null }
        $actual = Get-NativeDirectoryInventory $root $excluded
        if (-not $expectedFiles[$name].SetEquals([string[]]$actual.Files) -or
            -not $expectedDirectories.SetEquals([string[]]$actual.Directories)) {
            throw 'native 完整文件或目录集合漂移，拒绝未绑定来源和DLL shadow'
        }
        Write-Information "direct-preflight phase=inventory root=$name files=$($actual.Files.Count) directories=$($actual.Directories.Count) elapsedMs=$($preflightTimer.ElapsedMilliseconds)" -InformationAction Continue
    }
    $loader = Join-Path $game $receipt.loader.relativePath
    $config = Join-Path $game $receipt.config.relativePath
    foreach ($identity in @(@{Path=$loader;Record=$receipt.loader}, @{Path=$config;Record=$receipt.config})) {
        Assert-NativeFileRecord $identity.Path $identity.Record
    }
    $reference = Assert-PlainNativePath $receipt.loaderReference.path
    Assert-NativeFileRecord $reference $receipt.loaderReference
    if ($reference.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $reference.StartsWith($hostPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $receipt.loaderReference.sha256 -cne $receipt.loader.sha256) {
        throw 'native loader 与原版引用或自有宿主不一致'
    }
    Assert-NativeFileRecord (Join-Path $nativeHost 'bin/worldedit.exe') $receipt.loader
    Write-Information "direct-preflight phase=complete files=$validatedFiles elapsedMs=$($preflightTimer.ElapsedMilliseconds)" -InformationAction Continue
    return $loader
}

function Assert-DirectLauncherRunning([object]$Launcher, [bool]$DirectMode) {
    if (-not $DirectMode) { return }
    if ($null -eq $Launcher) { throw 'direct-loader 缺少本次启动进程身份' }
    if ($Launcher.HasExited) {
        $actualExitCode = [int]$Launcher.ExitCode
        $exitHex = '0x' + [BitConverter]::ToUInt32([BitConverter]::GetBytes($actualExitCode), 0).ToString('X8')
        throw "direct-loader 已退出 launcherPid=$($Launcher.Id) actualExitCode=$actualExitCode exitHex=$exitHex"
    }
}

function Get-DirectDialogState([YdweDialog]$Dialog, [IntPtr]$MainFrame, [int]$EditorPid) {
    $progressShape = $Dialog.Title -ceq '进程' -and $Dialog.Owner -eq [IntPtr]::Zero -and
        $Dialog.HasProgressControl -and $Dialog.YesButton -eq [IntPtr]::Zero
    if ($Dialog.IsVisible) {
        if ($progressShape) { return 'ActiveProgress' }
        if ($Dialog.IsEnabled -and -not $Dialog.IsToolWindow -and
            ($Dialog.Owner -eq [IntPtr]::Zero -or -not $Dialog.OwnerEnabled)) { return 'ActiveModal' }
        return 'Inactive'
    }
    $validFrame = [YdweWin]::IsEditorFrame($MainFrame, $EditorPid)
    $frameEnabled = $validFrame -and [YdweWin]::IsEditorFrameEnabled($MainFrame, $EditorPid)
    if ($progressShape) {
        if ($Dialog.IsEnabled -and ($Dialog.OwnVisibleStyle -or ($validFrame -and -not $frameEnabled))) {
            return 'ActiveProgress'
        }
        # 已隐藏、主窗口仍可用的进度壳不是本次活跃进度，避免永久等候。
        return 'Inactive'
    }
    if ([YdweWin]::IsOwnedDisabledModal($Dialog.Handle, $MainFrame, $EditorPid)) { return 'ActiveModal' }
    if ($Dialog.IsEnabled -and -not $Dialog.IsToolWindow -and
        ($Dialog.Owner -eq [IntPtr]::Zero -or ($validFrame -and -not $frameEnabled))) {
        return 'UnprovenHidden'
    }
    return 'Inactive'
}

function Test-NativeVersionWarning([YdweDialog]$Dialog, [int]$EditorPid, [string]$VersionText,
    [bool]$DirectMode, [IntPtr]$MainFrame) {
    $body = $Dialog.Body -replace '\s+', ''
    $ownerMatches = $Dialog.Owner -eq [IntPtr]::Zero -or $Dialog.OwnerPid -eq $EditorPid
    $warningMatches = $Dialog.Title -ceq '警告' -and $body -ceq $VersionText -and
        $Dialog.YesButton -ne [IntPtr]::Zero -and $Dialog.YesButtonText -ceq '是(&Y)' -and
        $Dialog.YesButtonEnabled -and $ownerMatches
    if ($DirectMode -and -not $Dialog.IsVisible) {
        $warningMatches = $warningMatches -and [YdweWin]::IsOwnedDisabledModal($Dialog.Handle, $MainFrame, $EditorPid)
    }
    return $warningMatches
}

function Get-EditorWindowObservation([int]$EditorPid, [bool]$DirectMode) {
    # 在属性中保留真正数组，避免 if/function 管道把零项变成 null、一项变成标量。
    return [pscustomobject]@{
        Frames = @([YdweWin]::GetEditorFrames($EditorPid, $DirectMode))
        Dialogs = @([YdweWin]::GetDialogs($EditorPid, $DirectMode))
    }
}

function Get-EditorLoadTimeoutMessage([object]$Editor, [bool]$AcceptedVersionWarning) {
    $observedPid = if ($null -ne $Editor) { $Editor.Id } else { 'not-observed' }
    return "地图未在期限内载入（pid=$observedPid，versionWarningAccepted=$AcceptedVersionWarning）"
}

function Assert-DirectEditorReady([int]$EditorPid, [IntPtr]$ExpectedFrame, [string]$LoadedTitle) {
    $readyObservation = Get-EditorWindowObservation $EditorPid $true
    $readyFrames = $readyObservation.Frames
    if ($readyFrames.Count -ne 1 -or $readyFrames[0].Handle -ne $ExpectedFrame -or
        $readyFrames[0].Title -cne $LoadedTitle -or -not [YdweWin]::IsEditorFrameEnabled($ExpectedFrame, $EditorPid)) {
        throw 'direct-loader 保存前的唯一专用主窗口或可用状态不符'
    }
    foreach ($dialog in $readyObservation.Dialogs) {
        if ((Get-DirectDialogState $dialog $ExpectedFrame $EditorPid) -cne 'Inactive') {
            throw 'direct-loader 保存前仍有活动或未证实状态的对话框'
        }
    }
}

function Assert-InputDesktopCommand([object]$Owner, [IntPtr]$Window, [bool]$BindGuiThread) {
    if ($null -ne $Owner) { $Owner.AssertWindow($Window, $BindGuiThread) }
}

if ($TimeoutSeconds -lt 20) { throw 'TimeoutSeconds 必须至少为 20 秒' }
if ($InputDesktopEvidence) {
    if (-not $DirectGameRoot -or -not $InputDesktopOwner -or -not $InputDesktopEnvironment -or $SaveCommand -ne 4) {
        throw 'native08 只支持原direct-loader、原创建句柄监管和save command=4'
    }
    . (Join-Path $PSScriptRoot 'p1-input-desktop.ps1')
    $InputDesktopEvidence = Assert-PlainNativePath $InputDesktopEvidence
    if (-not [IO.Directory]::Exists($InputDesktopEvidence)) { throw 'native08 证据目录必须预先存在' }
    [void]([P1InputDesktopOwner]::Witness().RequireActiveDefault())
}
if (Get-Process | Where-Object { $_.Name -match '^(worldedit|worldeditydwe)$' }) { throw 'World Editor 已在运行，拒绝操作' }
$YdweRoot = (Resolve-Path -LiteralPath $YdweRoot).Path
$launcherPath = (Resolve-Path -LiteralPath (Join-Path $YdweRoot 'YDWE.exe')).Path
if ($DirectGameRoot -or $DirectLoaderReceipt -or $DirectLoaderReceiptSha256) {
    if (-not ($DirectGameRoot -and $DirectLoaderReceipt -and $DirectLoaderReceiptSha256)) {
        throw 'direct-loader 三项参数须同时提供，不能退回全局YDWE路径选择器'
    }
    $launcherPath = Assert-DirectNativeLoader $DirectGameRoot $YdweRoot $DirectLoaderReceipt $DirectLoaderReceiptSha256
}
$Map = (Resolve-Path -LiteralPath $Map).Path
$pjass = Join-Path $YdweRoot 'logs\pjass.txt'
$started = Get-Date
$inputFingerprint = Get-MapFingerprint $Map
$inputSha256 = $inputFingerprint.Sha256
$launcher = $null
$editor = $null
$native08Owner = $null
$expectedCommandLine = '"' + $launcherPath + '" -loadfile "' + $Map + '"'
try {
$oldPath = $env:PATH
$oldProcessName = [Environment]::GetEnvironmentVariable('ydwe-process-name', 'Process')
try {
    if ($DirectGameRoot) {
        $env:PATH = (Join-Path $YdweRoot 'bin') + [IO.Path]::PathSeparator + $oldPath
        [Environment]::SetEnvironmentVariable('ydwe-process-name', 'ydwe', 'Process')
    }
    if ($InputDesktopEvidence) {
        $native08Environment = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach($key in $InputDesktopEnvironment.Keys) { $native08Environment.Add($key,$InputDesktopEnvironment[$key]) }
        $native08Environment['PATH'] = (Join-Path $YdweRoot 'bin') + [IO.Path]::PathSeparator + $native08Environment['PATH']
        $native08Environment.Add('ydwe-process-name','ydwe')
        $native08Owner = [P1InputDesktopOwner]::Start($launcherPath, $expectedCommandLine, $YdweRoot, $native08Environment)
        $InputDesktopOwner.Value = $native08Owner
        $launcher = $native08Owner
        Write-P1InputDesktopNewJson (Join-Path $InputDesktopEvidence 'launch.local.json') @{
            schema='p1-input-desktop-launch-v1'; parentBefore=$native08Owner.ParentBefore;
            editorOriginalIdentity=$native08Owner.Identity; commandLine=$expectedCommandLine;
            startupFailure=$native08Owner.StartupFailure; environmentSha256=(Get-P1InputEnvironmentFingerprint $native08Environment);
            environmentKeys=@($native08Environment.Keys | Sort-Object); unicodeEnvironmentBlock=$true; ambientEnvironmentInherited=$false
        }
        if ($native08Owner.StartupFailure) { throw $native08Owner.StartupFailure }
    } else {
        $launcher = Start-Process -FilePath $launcherPath -ArgumentList @('-loadfile', "`"$Map`"") -WorkingDirectory $YdweRoot -WindowStyle Hidden -PassThru
    }
} finally {
    $env:PATH = $oldPath
    [Environment]::SetEnvironmentVariable('ydwe-process-name', $oldProcessName, 'Process')
}
"launched launcherPid=$($launcher.Id) inputSha256=$inputSha256 map=$Map"
Assert-DirectLauncherRunning $launcher ([bool]$DirectGameRoot)
$stem = [IO.Path]::GetFileNameWithoutExtension($Map)
$titleStemPrefix = $stem.Substring(0, [Math]::Min(24, $stem.Length))
$titleNeedle = '\' + $titleStemPrefix
$loadedFrame = $null
$acceptedVersionWarning = $false
$loadedTitleSeen = $false
$lastLoadedTitle = ''
$ignoredWindows = @{}
$unknownModalKey = ''
$unknownModalSince = $null
$versionWarning = '该地图是由一个版本更新的编辑器保存的.或者它包含了一些现有版本并不支持的数据.继续载入地图吗'
for ($i = 0; $i -lt $TimeoutSeconds; $i += 2) {
    Start-Sleep -Seconds 2
    Assert-DirectLauncherRunning $launcher ([bool]$DirectGameRoot)
    if ($native08Owner) { $native08Owner.AssertRunningDefault() }
    $editors = @(Get-Process | Where-Object { $_.Name -match '^(worldedit|worldeditydwe)$' })
    if ($editors.Count -gt 1) { throw "检测到 $($editors.Count) 个编辑器实例，无法确认专用窗口" }
    if ($editors.Count -eq 0) { continue }
    $editor = if ($native08Owner) { $native08Owner } else { $editors[0] }
    if ($native08Owner -and $editors[0].Id -ne $native08Owner.Id) { throw 'WE枚举不能替代本次创建原句柄' }
    if ($editor.StartTime -lt $started.AddSeconds(-2)) { throw "编辑器进程不属于本次启动（pid $($editor.Id)）" }
    $processInfo = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($editor.Id)"
    if (-not $processInfo -or -not [string]::Equals($processInfo.CommandLine, $expectedCommandLine, [StringComparison]::OrdinalIgnoreCase)) {
        throw "编辑器启动命令行与本次地图副本不符（pid $($editor.Id)）"
    }
    if ($DirectGameRoot -and (-not [string]::Equals($processInfo.ExecutablePath, $launcherPath, [StringComparison]::OrdinalIgnoreCase) -or
        $editor.Id -ne $launcher.Id)) { throw 'direct-loader 的实际 image/PID 不属于本次独立游戏根' }
    $directMainFrame = [IntPtr]::Zero
    $windowObservation = Get-EditorWindowObservation $editor.Id ([bool]$DirectGameRoot)
    if ($DirectGameRoot) {
        $directFrames = $windowObservation.Frames
        if ($directFrames.Count -gt 1) { throw "多个 World Editor 主窗口（pid $($editor.Id)）" }
        if ($directFrames.Count -eq 1) {
            $directMainFrame = $directFrames[0].Handle
            Assert-InputDesktopCommand $native08Owner $directMainFrame $true
        }
    }
    $dialogs = $windowObservation.Dialogs
    $modalDialogs = @()
    $directActiveProgress = $false
    foreach ($dialog in $dialogs) {
        $progressWindow = $dialog.Title -ceq '进程' -and
            $dialog.Owner -eq [IntPtr]::Zero -and
            $dialog.HasProgressControl -and
            $dialog.YesButton -eq [IntPtr]::Zero
        $modalCandidate = $dialog.IsEnabled -and -not $dialog.IsToolWindow -and -not $progressWindow -and
            ($dialog.Owner -eq [IntPtr]::Zero -or -not $dialog.OwnerEnabled)
        if ($DirectGameRoot) {
            $directState = Get-DirectDialogState $dialog $directMainFrame $editor.Id
            $progressWindow = $directState -ceq 'ActiveProgress'
            $modalCandidate = $directState -cin @('ActiveModal', 'UnprovenHidden')
            if ($progressWindow) { $directActiveProgress = $true }
        }
        if (-not $modalCandidate) {
            $windowKey = [string]$dialog.Handle
            if (-not $ignoredWindows.ContainsKey($windowKey)) {
                "non-modal-window pid=$($editor.Id) hwnd=$($dialog.Handle) title=$($dialog.Title) enabled=$($dialog.IsEnabled) tool=$($dialog.IsToolWindow) progress=$progressWindow ownerPid=$($dialog.OwnerPid) ownerEnabled=$($dialog.OwnerEnabled) visible=$($dialog.IsVisible) ownVisible=$($dialog.OwnVisibleStyle)"
                $ignoredWindows[$windowKey] = $true
            }
            continue
        }
        $modalDialogs += $dialog
    }
    if ($modalDialogs.Count -gt 1) { throw "地图载入出现多个模态窗口（pid $($editor.Id)）" }
    if ($modalDialogs.Count -eq 1) {
        $dialog = $modalDialogs[0]
        if ($dialog.Owner -ne [IntPtr]::Zero -and $dialog.OwnerPid -ne $editor.Id) {
            throw "地图载入提示框属于外部窗口（pid $($editor.Id)，ownerPid=$($dialog.OwnerPid)）"
        }
        $body = $dialog.Body -replace '\s+', ''
        $knownWarning = Test-NativeVersionWarning $dialog $editor.Id $versionWarning ([bool]$DirectGameRoot) $directMainFrame
        if (-not $knownWarning) {
            $key = "$($dialog.Handle):$($dialog.Title)"
            if ($unknownModalKey -ne $key) {
                $unknownModalKey = $key
                $unknownModalSince = Get-Date
                "unknown-modal-pending pid=$($editor.Id) hwnd=$($dialog.Handle) title=$($dialog.Title) body=$body"
            } elseif (((Get-Date) - $unknownModalSince).TotalSeconds -ge 8) {
                throw "地图载入遇到未获准的提示框（pid $($editor.Id)，title=$($dialog.Title)，body=$body）"
            }
            $loadedTitleSeen = $false
            continue
        }
        if ($acceptedVersionWarning) { throw "版本提示框再次出现（pid $($editor.Id)）" }
        $unknownModalKey = ''
        "version-warning pid=$($editor.Id) hwnd=$($dialog.Handle) ownerPid=$($dialog.OwnerPid) body=$body"
        Assert-InputDesktopCommand $native08Owner $dialog.Handle $false
        if ($native08Owner) { Assert-InputDesktopCommand $native08Owner $dialog.YesButton $false }
        if (-not [YdweWin]::PostMessage($dialog.Handle, 0x0111, [IntPtr]6, $dialog.YesButton)) {
            throw "无法接受版本提示框（pid $($editor.Id)）"
        }
        $acceptedVersionWarning = $true
        "version-warning-accepted pid=$($editor.Id) buttonId=6"
        $loadedTitleSeen = $false
        continue
    }
    $unknownModalKey = ''
    if ($DirectGameRoot -and $directActiveProgress) {
        $loadedFrame = $null
        $loadedTitleSeen = $false
        continue
    }
    $frames = (Get-EditorWindowObservation $editor.Id ([bool]$DirectGameRoot)).Frames
    if ($frames.Count -gt 1) { throw "多个 World Editor 主窗口（pid $($editor.Id)）" }
    $titleMatchesMap = $frames.Count -eq 1 -and
        $frames[0].Title.StartsWith('YDWE [', [StringComparison]::Ordinal) -and
        $frames[0].Title.Contains(' - [') -and
        $frames[0].Title.IndexOf($titleNeedle, [StringComparison]::OrdinalIgnoreCase) -ge 0
    if ($titleMatchesMap) {
        $loadedFrame = $frames[0]
        if ($loadedTitleSeen -and $lastLoadedTitle -ceq $loadedFrame.Title) { break }
        $lastLoadedTitle = $loadedFrame.Title
        $loadedTitleSeen = $true
    } else {
        $loadedFrame = $null
        $loadedTitleSeen = $false
        $lastLoadedTitle = ''
    }
}
if (-not $editor -or -not $loadedFrame -or -not $loadedTitleSeen) {
    throw (Get-EditorLoadTimeoutMessage $editor $acceptedVersionWarning)
}
"loaded pid=$($editor.Id) hwnd=$($loadedFrame.Handle) title=$($loadedFrame.Title) matchedStemPrefix=$titleStemPrefix visible=$($loadedFrame.IsVisible) ownVisible=$($loadedFrame.OwnVisibleStyle) enabled=$($loadedFrame.IsEnabled)"
$hwnd = $loadedFrame.Handle
$beforeSaveFingerprint = Get-MapFingerprint $Map
if ($beforeSaveFingerprint.Sha256 -cne $inputSha256 -or
    $beforeSaveFingerprint.Length -ne $inputFingerprint.Length -or
    $beforeSaveFingerprint.WriteTicks -ne $inputFingerprint.WriteTicks) {
    throw '地图副本在载入期间已变化，拒绝发送保存命令'
}
if ($DirectGameRoot) {
    Assert-DirectEditorReady $editor.Id $hwnd $lastLoadedTitle
}
$savePostedAt = Get-Date
Assert-InputDesktopCommand $native08Owner $hwnd $true
if (-not [YdweWin]::PostMessage($hwnd, 0x0111, [IntPtr]$SaveCommand, [IntPtr]::Zero)) {
    throw "保存命令未能发送到专用窗口（pid $($editor.Id)，command=$SaveCommand）"
}
"save-command-posted pid=$($editor.Id) command=$SaveCommand"
if ($native08Owner) {
    Write-P1InputDesktopNewJson (Join-Path $InputDesktopEvidence 'save-command.local.json') @{
        pid=$native08Owner.Id; guiThreadId=$native08Owner.GuiThreadId; primaryThreadDesktop=$native08Owner.Identity.PrimaryThreadDesktop;
        command=4; loadedTitle=$lastLoadedTitle; inputFingerprint=$inputFingerprint; postedAtUtc=$savePostedAt.ToUniversalTime()
    }
}
$saveDeadline = (Get-Date).AddSeconds($TimeoutSeconds)
$stableFingerprint = $null
$stableSince = $null
$savedFingerprint = $null
$lastObservation = '尚未观察到地图写出'
while ((Get-Date) -lt $saveDeadline) {
    Start-Sleep -Seconds 5
    if ($editor.HasExited) { throw "保存完成前专用编辑器已退出（pid $($editor.Id)）" }
    if ($native08Owner) { $native08Owner.AssertRunningDefault() }
    $progressActive = $false
    $saveDialogs = (Get-EditorWindowObservation $editor.Id ([bool]$DirectGameRoot)).Dialogs
    foreach ($dialog in $saveDialogs) {
        $progressWindow = $dialog.Title -ceq '进程' -and
            $dialog.Owner -eq [IntPtr]::Zero -and
            $dialog.HasProgressControl -and
            $dialog.YesButton -eq [IntPtr]::Zero
        $modalCandidate = $dialog.IsEnabled -and -not $dialog.IsToolWindow -and
            ($dialog.Owner -eq [IntPtr]::Zero -or -not $dialog.OwnerEnabled)
        if ($DirectGameRoot) {
            $directState = Get-DirectDialogState $dialog $hwnd $editor.Id
            $progressWindow = $directState -ceq 'ActiveProgress'
            $modalCandidate = $directState -cin @('ActiveModal', 'UnprovenHidden')
        }
        if ($progressWindow) { $progressActive = $true; continue }
        if ($modalCandidate) {
            throw "保存期间遇到未获准的提示框（pid $($editor.Id)，title=$($dialog.Title)，body=$($dialog.Body)）"
        }
    }
    if ($progressActive) {
        $stableFingerprint = $null
        $stableSince = $null
        $lastObservation = 'YDWE 仍在显示进度窗口'
        continue
    }
    try {
        $current = Get-MapFingerprint $Map
    } catch {
        $stableFingerprint = $null
        $stableSince = $null
        $lastObservation = "地图暂不可读：$($_.Exception.Message)"
        continue
    }
    if ($current.Sha256 -ceq $inputSha256) {
        $stableFingerprint = $null
        $stableSince = $null
        $lastObservation = '地图 SHA-256 仍与输入副本相同'
        continue
    }
    $sameFingerprint = $stableFingerprint -and
        $current.Sha256 -ceq $stableFingerprint.Sha256 -and
        $current.Length -eq $stableFingerprint.Length -and
        $current.WriteTicks -eq $stableFingerprint.WriteTicks
    if (-not $sameFingerprint) {
        $stableFingerprint = $current
        $stableSince = Get-Date
        $lastObservation = "地图已变化，等待稳定：sha256=$($current.Sha256) size=$($current.Length)"
        continue
    }
    if (((Get-Date) - $stableSince).TotalSeconds -ge 15) {
        $savedFingerprint = $current
        break
    }
}
if (-not $savedFingerprint) { throw "保存未在 $TimeoutSeconds 秒内完成且稳定（$lastObservation）" }
if (-not (Test-Path -LiteralPath $pjass) -or (Get-Item -LiteralPath $pjass).LastWriteTime -lt $savePostedAt) {
    throw '本次保存没有新的 pjass 检查回执，不能判定脚本通过'
}
$compile = if ($native08Owner) {
    # 编码在本轮manifest显式冻结；解码失败不得用 replacement 字符继续签收。
    $strictEncoding = [Text.Encoding]::GetEncoding($InputDesktopPjassCodePage,
        [Text.EncoderFallback]::ExceptionFallback, [Text.DecoderFallback]::ExceptionFallback)
    $strictEncoding.GetString([IO.File]::ReadAllBytes($pjass))
} else { Get-Content -LiteralPath $pjass -Raw }
"pjass: $($compile.Trim() -replace '\s+', ' ')"
if ($compile -notmatch '(?m)^Parse successful:\s+\d+\s+lines:\s+<total>\s*$' -or
    $compile -match '(?im)\b[1-9]\d*\s+errors?\b|\b(?:error|failed|failure)\b') {
    throw 'pjass 未报告无错误的完整脚本检查通过'
}
"save-written pid=$($editor.Id) sha256=$($savedFingerprint.Sha256) size=$($savedFingerprint.Length)"
if ($native08Owner) { Assert-DirectEditorReady $editor.Id $hwnd $lastLoadedTitle }
Assert-InputDesktopCommand $native08Owner $hwnd $true
if (-not [YdweWin]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) {
    throw "关闭命令未能发送到专用窗口（pid $($editor.Id)）"
}
for ($i = 0; $i -lt 60; $i += 2) { Start-Sleep -Seconds 2; if ($editor.HasExited) { break } }
if (-not $editor.HasExited) { throw "YDWE 未正常关闭（pid $($editor.Id)），请检查是否有提示框" }
if ($native08Owner) {
    $native08Terminal = $native08Owner.Terminal()
    Write-P1InputDesktopNewJson (Join-Path $InputDesktopEvidence 'editor-terminal.local.json') $native08Terminal
    if (-not $native08Terminal.StableTerminalState -or $native08Terminal.ExitCode -ne 0) {
        throw 'native08 原process/thread句柄及必要关联项未达成功终态'
    }
}
$closedFingerprint = Get-MapFingerprint $Map
if ($closedFingerprint.Sha256 -cne $savedFingerprint.Sha256 -or
    $closedFingerprint.Length -ne $savedFingerprint.Length -or
    $closedFingerprint.WriteTicks -ne $savedFingerprint.WriteTicks) {
    throw '专用编辑器关闭后地图再次变化，先重新审计再判定保存成功'
}
"saved pid=$($editor.Id) inputSha256=$inputSha256 outputSha256=$($closedFingerprint.Sha256) size=$($closedFingerprint.Length)"
'closed'
} catch {
    $launcherPid = if ($launcher) { $launcher.Id } else { 'not-launched' }
    $editorPid = if ($editor) { $editor.Id } else { 'not-observed' }
    "save-failed launcherPid=$launcherPid editorPid=$editorPid inputSha256=$inputSha256 map=$Map reason=$($_.Exception.Message)"
    throw
}
