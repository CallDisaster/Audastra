# Read-only capture of every visible top-level window of one process (PrintWindow, no focus, no input).
# Run under Windows PowerShell 5.1 (powershell.exe): pwsh 7 lacks System.Drawing.
# Usage: powershell.exe -File tools/dev/capture-windows.ps1 -ProcessId <pid> -OutDir <dir> [-Prefix win] [-Resize]
param([int]$ProcessId, [string]$OutDir, [string]$Prefix = 'win', [switch]$Resize)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Win {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hwnd, int x, int y, int w, int h, bool repaint);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static List<IntPtr> Windows(uint pid) {
    var list = new List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
    return list;
  }
}
"@
# The desktop may run at 125% DPI; without this, rects are virtualized and captures are cropped.
[void][Win]::SetProcessDPIAware()
New-Item -ItemType Directory -Force $OutDir | Out-Null
$proc = Get-Process -Id $ProcessId
if ($Resize) { [void][Win]::MoveWindow($proc.MainWindowHandle, 20, 20, 1920, 1080, $true); Start-Sleep -Milliseconds 1500 }
$i = 0
foreach ($h in [Win]::Windows([uint32]$proc.Id)) {
    $rect = New-Object Win+RECT
    [void][Win]::GetWindowRect($h, [ref]$rect)
    $w = $rect.Right - $rect.Left; $hh = $rect.Bottom - $rect.Top
    if ($w -lt 50 -or $hh -lt 50) { continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [void][Win]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
    $file = Join-Path $OutDir ("$Prefix$i.png"); $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "$Prefix$i $w x $hh hwnd=$h"
    $i++
}
