# 列出窗口菜单（含子菜单）的命令 ID，或向该窗口投递菜单命令。不抢焦点、不发送按键。
# 只用于本会话自己启动的编辑器实例（Windows PowerShell 5.1 或 pwsh 均可）。
# 用法：win-menu.ps1 -Hwnd <hwnd> [-List] [-Command <id>]
param([long]$Hwnd, [switch]$List, [int]$Command = 0)
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Menu {
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr m);
  [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr m, int i);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int i);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetMenuString(IntPtr m, uint id, StringBuilder s, int max, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
"@
$window = [IntPtr]$Hwnd
function Walk([IntPtr]$menu, [string]$indent) {
    $count = [Menu]::GetMenuItemCount($menu)
    for ($i = 0; $i -lt $count; $i++) {
        $text = New-Object System.Text.StringBuilder 256
        [void][Menu]::GetMenuString($menu, [uint32]$i, $text, 256, 0x400)
        $sub = [Menu]::GetSubMenu($menu, $i)
        if ($sub -ne [IntPtr]::Zero) { "$indent[$($text.ToString())]"; Walk $sub ($indent + '  ') }
        else { "$indent$([Menu]::GetMenuItemID($menu, $i))`t$($text.ToString())" }
    }
}
if ($List) { Walk ([Menu]::GetMenu($window)) '' }
if ($Command -ne 0) { [Menu]::PostMessage($window, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) }
