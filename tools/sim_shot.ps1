param([int]$X = -1, [int]$Y = -1, [string]$Out = "shot.png")
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class SS {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc f, IntPtr l);
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  public static List<IntPtr> hits = new List<IntPtr>();
  public static uint target;
  public static bool Cb(IntPtr h, IntPtr l){
    uint p; GetWindowThreadProcessId(h, out p);
    if(p==target && IsWindowVisible(h)){ var r=new RECT(); GetWindowRect(h,out r); if((r.R-r.L)>50) hits.Add(h); }
    return true;
  }
}
"@
$p = Get-Process travel_frog_sim -ErrorAction SilentlyContinue | Select-Object -First 1
if(-not $p){ Write-Host 'no process'; exit 1 }
[SS]::target = [uint32]$p.Id
[SS]::EnumWindows([SS+EnumWindowsProc]{param($h,$l) [SS]::Cb($h,$l)}, [IntPtr]::Zero) | Out-Null
$h = [SS]::hits[0]
[SS]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
if($X -ge 0){
  $pt = New-Object SS+POINT
  [SS]::ClientToScreen($h, [ref]$pt) | Out-Null
  $cr = New-Object SS+RECT; [SS]::GetClientRect($h,[ref]$cr)
  Write-Host ("client {0}x{1} click {2},{3}" -f ($cr.R-$cr.L),($cr.B-$cr.T),$X,$Y)
  [SS]::SetCursorPos($pt.X+$X, $pt.Y+$Y) | Out-Null
  Start-Sleep -Milliseconds 200
  [SS]::SetCursorPos($pt.X+$X+1, $pt.Y+$Y+1) | Out-Null
  Start-Sleep -Milliseconds 120
  [SS]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 60
  [SS]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 500
}
$r = New-Object SS+RECT; [SS]::GetWindowRect($h,[ref]$r)
$w=$r.R-$r.L; $hh=$r.B-$r.T
$bmp=New-Object System.Drawing.Bitmap $w,$hh
$g=[System.Drawing.Graphics]::FromImage($bmp)
$ptr=$g.GetHdc(); [Void][SS]::PrintWindow($h,$ptr,2); $g.ReleaseHdc($ptr); $g.Dispose()
$bmp.Save("E:\HardWork\travel-frog\tools\$Out"); $bmp.Dispose()
Write-Host "saved $Out"
