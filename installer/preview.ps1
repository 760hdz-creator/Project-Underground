# Renders the installer window to a PNG (PrintWindow, works behind other windows), then closes it.
# usage: powershell -File preview.ps1 <out.png> <ProjectUnderground-Setup.exe or launcher.exe> [cropBottomPx] [exe arguments,
#        e.g. "--launcher --dir <install>"]
param([string]$Out, [string]$Exe, [int]$CropBottom = 0, [string]$ExeArgs = "")
Add-Type @"
using System; using System.Runtime.InteropServices;
public class UgWin {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
[UgWin]::SetProcessDPIAware() | Out-Null
if ($ExeArgs) { $p = Start-Process $Exe -ArgumentList $ExeArgs -PassThru } else { $p = Start-Process $Exe -PassThru }
# move the window to a secondary monitor when there is one (tests stay off the main screen)
for ($i = 0; $i -lt 30 -and $p.MainWindowHandle -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh() }
$second = [System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary } | Select-Object -First 1
if ($second) { [UgWin]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, $second.Bounds.X + 100, $second.Bounds.Y + 100, 0, 0, 0x15) | Out-Null }
Start-Sleep -Seconds 3
$p.Refresh()
$r = New-Object UgWin+RECT
[UgWin]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc(); [UgWin]::PrintWindow($p.MainWindowHandle, $dc, 3) | Out-Null; $g.ReleaseHdc($dc)
if ($CropBottom -gt 0) {
  $bmp = $bmp.Clone((New-Object System.Drawing.Rectangle 0, ($r.B - $CropBottom), $r.R, $CropBottom), $bmp.PixelFormat)
}
$bmp.Save($Out)
Stop-Process -Id $p.Id
