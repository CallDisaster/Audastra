# Post mouse clicks / keys to OUR OWN editor window by HWND (PostMessage: no focus change, no real input,
# works even when other apps cover it). Never use this on World Editor / YDWE or other users' windows.
# Qt child widgets are not native: post to the top-level HWND (dialogs and combo popups have their own).
# Usage: powershell.exe -File tools/dev/post-input.ps1 -Hwnd <hwnd> -X <x> -Y <y> [-Double]   (client coordinates)
#        powershell.exe -File tools/dev/post-input.ps1 -Hwnd <hwnd> -Key 27                    (virtual-key code)
#        powershell.exe -File tools/dev/post-input.ps1 -Hwnd <hwnd> -X <x> -Y <y> -Wheel 120      (wheel notches x120; + = away)
#        powershell.exe -File tools/dev/post-input.ps1 -Hwnd <hwnd> -X <x> -Y <y> -DragX <x2> -DragY <y2> [-Button right|middle|left]
param([long]$Hwnd, [int]$X, [int]$Y, [switch]$Double, [int]$Key = 0, [int]$Wheel = 0,
    [int]$DragX = -1, [int]$DragY = -1, [ValidateSet('left', 'right', 'middle')][string]$Button = 'right')
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
}
"@
[void][W]::SetProcessDPIAware()
$h = [IntPtr]$Hwnd
if ($Key) {
  [void][W]::PostMessage($h, 0x100, [IntPtr]$Key, [IntPtr]1)
  [void][W]::PostMessage($h, 0x101, [IntPtr]$Key, [IntPtr]0xC0000001)
  "key $Key"; return
}
if ($Wheel) {
  # WM_MOUSEWHEEL 的坐标是屏幕坐标。
  $pt = New-Object W+P; $pt.x = $X; $pt.y = $Y
  [void][W]::ClientToScreen($h, [ref]$pt)
  $wp = [IntPtr]([long]($Wheel -band 0xFFFF) -shl 16)
  $slp = [IntPtr]((($pt.y -band 0xFFFF) -shl 16) -bor ($pt.x -band 0xFFFF))
  [void][W]::PostMessage($h, 0x20A, $wp, $slp)
  "wheel $Wheel at client=($X,$Y)"; return
}
$lp = [IntPtr](($Y -shl 16) -bor ($X -band 0xFFFF))
if ($DragX -ge 0 -and $DragY -ge 0) {
  # 拖动：按下 → 分 12 步移动（带按键状态）→ 松开。
  $down, $up, $state = switch ($Button) { 'left' { 0x201, 0x202, 1 } 'right' { 0x204, 0x205, 2 } 'middle' { 0x207, 0x208, 0x10 } }
  [void][W]::PostMessage($h, 0x200, [IntPtr]0, $lp)
  [void][W]::PostMessage($h, $down, [IntPtr]$state, $lp)
  for ($i = 1; $i -le 12; $i++) {
    $mx = [int]($X + ($DragX - $X) * $i / 12); $my = [int]($Y + ($DragY - $Y) * $i / 12)
    [void][W]::PostMessage($h, 0x200, [IntPtr]$state, [IntPtr](($my -shl 16) -bor ($mx -band 0xFFFF)))
    Start-Sleep -Milliseconds 15
  }
  [void][W]::PostMessage($h, $up, [IntPtr]0, [IntPtr](($DragY -shl 16) -bor ($DragX -band 0xFFFF)))
  "drag $Button ($X,$Y)->($DragX,$DragY)"; return
}
[void][W]::PostMessage($h, 0x200, [IntPtr]0, $lp)
[void][W]::PostMessage($h, 0x201, [IntPtr]1, $lp)
[void][W]::PostMessage($h, 0x202, [IntPtr]0, $lp)
if ($Double) {
  [void][W]::PostMessage($h, 0x203, [IntPtr]1, $lp)
  [void][W]::PostMessage($h, 0x202, [IntPtr]0, $lp)
}
"posted to $Hwnd client=($X,$Y)"
