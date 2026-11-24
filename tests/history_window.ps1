param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name TrafficMonitor -ErrorAction SilentlyContinue) {
    throw 'This test requires an isolated Windows session.'
}
$exe = (Resolve-Path -LiteralPath $Executable).Path
$dir = Join-Path ([IO.Path]::GetTempPath()) ('tm-history-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $dir | Out-Null
Copy-Item -LiteralPath $exe -Destination $dir
Get-ChildItem -LiteralPath (Split-Path $exe) -Filter '*.dll' | Copy-Item -Destination $dir
Set-Content (Join-Path $dir global_cfg.ini) "[config]`nportable_mode = true" -Encoding ascii
Set-Content (Join-Path $dir config.ini) "[general]`ncheck_update_when_start = false" -Encoding ascii
$day = [datetime]::Today.AddDays(-1).ToString('yyyy/MM/dd', [cultureinfo]::InvariantCulture)
$old = [datetime]::Today.AddDays(-2).ToString('yyyy/MM/dd', [cultureinfo]::InvariantCulture)
$recent = "$day 2048/4096 scope1 1048576 2097152 1048576 2097152"
$history = Join-Path $dir history_traffic.dat
Set-Content $history @('lines: "2"', $recent, "$old 1000/2000") -Encoding ascii

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class HistoryWindow {
    public delegate bool Visit(IntPtr hwnd, IntPtr arg);
    [DllImport("user32")] static extern bool EnumWindows(Visit cb, IntPtr arg);
    [DllImport("user32")] static extern bool EnumChildWindows(IntPtr root, Visit cb, IntPtr arg);
    [DllImport("user32")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd,StringBuilder name,int len);
    [DllImport("user32")] static extern int GetDlgCtrlID(IntPtr hwnd);
    [DllImport("user32")] public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
    [DllImport("user32")] public static extern bool PostMessage(IntPtr hwnd,int msg,IntPtr wp,IntPtr lp);
    [DllImport("user32")] public static extern IntPtr SendMessage(IntPtr hwnd,int msg,IntPtr wp,IntPtr lp);
    public static IntPtr Find(uint pid, bool dialog) {
        IntPtr result=IntPtr.Zero;
        EnumWindows((w,a)=>{
            uint owner;
            GetWindowThreadProcessId(w,out owner);
            if(owner!=pid)return true;
            var name=new StringBuilder(256);
            GetClassName(w,name,256);
            if(dialog ? GetDlgItem(w,1221)!=IntPtr.Zero : name.ToString()=="TrafficMonitor_r7XZaS4p") {
                result=w; return false;
            }
            return true;
        },IntPtr.Zero);
        return result;
    }
    public static IntPtr Child(IntPtr root,int id) {
        IntPtr result=IntPtr.Zero;
        EnumChildWindows(root,(w,a)=>{if(GetDlgCtrlID(w)!=id)return true;result=w;return false;},IntPtr.Zero);
        return result;
    }
}
'@
function Wait-ForHandle([scriptblock]$Query) {
    for($i=0;$i -lt 100;$i++) {
        $handle=& $Query
        if($handle -ne [IntPtr]::Zero){return $handle}
        Start-Sleep -Milliseconds 100
    }
    throw 'Expected window did not appear.'
}
$process=Start-Process -FilePath (Join-Path $dir TrafficMonitor.exe) -WorkingDirectory $dir -WindowStyle Hidden -PassThru
try {
    $main = Wait-ForHandle { [HistoryWindow]::Find($process.Id,$false) }
    [void][HistoryWindow]::PostMessage($main,0x111,32826,0)
    $dlg = Wait-ForHandle { [HistoryWindow]::Find($process.Id,$true) }
    $list = Wait-ForHandle { [HistoryWindow]::Child($dlg,1017) }
    $filter=[HistoryWindow]::GetDlgItem($dlg,1221)
    if([HistoryWindow]::SendMessage($filter,0x146,0,0).ToInt32() -ne 3){throw 'Expected three scopes'}
    $counts=foreach($i in @(0,1,2,0)) {
        [void][HistoryWindow]::SendMessage($filter,0x14e,$i,0)
        [void][HistoryWindow]::SendMessage($dlg,0x111,([int]0x10000+1221),$filter)
        [HistoryWindow]::SendMessage($list,0x1004,0,0).ToInt32()
    }
    if($counts[0] -lt 2 -or $counts[1] -lt 1 -or $counts[1] -ge $counts[0] -or
       $counts[2] -ne $counts[1] -or $counts[3] -ne $counts[0]) {throw "Unexpected filter counts: $counts"}
    [void][HistoryWindow]::PostMessage($dlg,0x111,1,0)
    Write-Host "All, LAN, Internet, All row counts: $counts"
} finally {
    if(!$process.HasExited){$process.Kill(); $process.WaitForExit()}
}

