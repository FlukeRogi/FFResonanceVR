# Sends key presses to the game window (focuses it first), for automated tests.
#   tools\gamekeys.ps1 -Keys "Enter","Down","Enter" [-HoldMs 80] [-GapMs 600]
# Key names: Enter, Escape, Up, Down, Left, Right, Space, Tab, Backspace, F1..F12, A..Z, 0..9, W/A/S/D etc.
param([string[]]$Keys, [int]$HoldMs = 80, [int]$GapMs = 600)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class GK {
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; [FieldOffset(8)] public long pad0; [FieldOffset(16)] public long pad1; [FieldOffset(24)] public long pad2; [FieldOffset(32)] public long pad3; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public static uint Key(ushort vk, bool up, bool ext) {
    INPUT[] a = new INPUT[1]; a[0].type = 1; a[0].ki.wVk = vk; a[0].ki.wScan = (ushort)MapVirtualKey(vk, 0);
    a[0].ki.dwFlags = (uint)(0x0008 | (up ? 0x0002 : 0) | (ext ? 0x0001 : 0));
    return SendInput(1, a, Marshal.SizeOf(typeof(INPUT)));
  }
}
"@
$p = Get-Process FFRS-Win64-Shipping -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Host "game not running"; exit 1 }
[GK]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [GK]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
$fg = [GK]::SetForegroundWindow($p.MainWindowHandle); [GK]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero); if (-not $fg) { Write-Host "warning: could not focus the game window" }
Start-Sleep -Milliseconds 400
$map = @{ Enter = 0x0D; Escape = 0x1B; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27; Space = 0x20; Tab = 0x09; Backspace = 0x08; Insert = 0x2D; Ctrl = 0x11; Shift = 0x10; Alt = 0x12 }
$ext = @(0x26, 0x28, 0x25, 0x27, 0x2D)
foreach ($k in $Keys) {
    if ($k -match '^Wait(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
    $parts = $k -split '\+'
    $vks = foreach ($part in $parts) {
        if ($map.ContainsKey($part)) { $map[$part] }
        elseif ($part -match '^F(\d+)$') { 0x6F + [int]$Matches[1] }
        elseif ($part.Length -eq 1) { [int][char]$part.ToUpper() }
        else { throw "unknown key $part" }
    }
    foreach ($vk in $vks) { if ([GK]::Key([uint16]$vk, $false, $ext -contains $vk) -ne 1) { Write-Host "SendInput failed" } }
    Start-Sleep -Milliseconds $HoldMs
    [array]::Reverse($vks)
    foreach ($vk in $vks) { [GK]::Key([uint16]$vk, $true, $ext -contains $vk) | Out-Null }
    Start-Sleep -Milliseconds $GapMs
}

