param([Parameter(Mandatory)][string]$Executable, [string]$LiveAdapter,
      [string]$FixtureRoot = (Get-Location).Path)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name TrafficMonitor -ErrorAction SilentlyContinue) {
    throw 'This test requires an isolated Windows session.'
}
$exe = (Resolve-Path -LiteralPath $Executable).Path
# TrafficMonitor forces AppData mode when installed in the Windows temp folder.
$base = (Resolve-Path -LiteralPath $FixtureRoot).Path
$temp = [IO.Path]::GetTempPath().TrimEnd('\') + '\'
if (($base.TrimEnd('\') + '\').StartsWith($temp,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose a fixture directory outside the Windows temp folder for portable mode.'
}
$dir = Join-Path $base ('tm-history-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $dir | Out-Null
Copy-Item -LiteralPath $exe -Destination $dir
Get-ChildItem -LiteralPath (Split-Path $exe) -Filter '*.dll' | Copy-Item -Destination $dir
Set-Content (Join-Path $dir global_cfg.ini) "[config]`nportable_mode = true" -Encoding ascii
Set-Content (Join-Path $dir config.ini) "[general]`ncheck_update_when_start = false" -Encoding ascii
if ($LiveAdapter) {
    Add-Content (Join-Path $dir config.ini) "[connection]`nauto_select = false`nselect_all = false`nconnection_name = $LiveAdapter" -Encoding utf8
}
$day = [datetime]::Today.AddDays(-1).ToString('yyyy/MM/dd', [cultureinfo]::InvariantCulture)
$old = [datetime]::Today.AddDays(-2).ToString('yyyy/MM/dd', [cultureinfo]::InvariantCulture)
$recent = "$day 4096/6144 scope1 1048576 2097152 3145728 4194304"
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
    [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr hwnd);
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
    if ($LiveAdapter) {
        # Exercise the first lock taken by the Npcap worker on a real interface.
        $until = [datetime]::UtcNow.AddSeconds(15)
        while ([datetime]::UtcNow -lt $until) {
            Start-Sleep -Milliseconds 500
            $log = Join-Path $dir error.log
            if ((Test-Path $log) -and (Select-String -LiteralPath $log -Pattern 'Stack trace:' -Quiet)) {
                throw "Application recorded a crash during capture. Fixture: $dir"
            }
            if ($process.HasExited) { throw "Application exited during capture: $($process.ExitCode)" }
        }
        $runtime = @($process.Modules | Where-Object ModuleName -eq 'msvcp140.dll')
        Write-Host "Live capture runtime: $($runtime.FileVersionInfo.FileVersion)"
    }
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
    Write-Host "All, LAN, Internet, All row counts: $counts"
    $tab = [HistoryWindow]::GetDlgItem($dlg,1035)
    [void][HistoryWindow]::SendMessage($tab,0x100,0x27,0) # Right arrow selects Calendar
    $year = [HistoryWindow]::Child($dlg,1077)
    if (!([HistoryWindow]::IsWindowVisible($year))){throw 'Calendar view is not visible'}
    foreach($i in @(1,2,0)){
        [void][HistoryWindow]::SendMessage($filter,0x14e,$i,0)
        [void][HistoryWindow]::SendMessage($dlg,0x111,([int]0x10000+1221),$filter)
        if(!([HistoryWindow]::IsWindowVisible($year))){throw 'Scope switch hid calendar'}
    }
    [void][HistoryWindow]::SendMessage($dlg,0x111,1,0)
    [void][HistoryWindow]::PostMessage($main,0x10,0,0) # WM_CLOSE flushes history
    if(!$process.WaitForExit(15000)){throw 'Application did not close cleanly'}
    $saved = @(Get-Content -LiteralPath $history)
    if($saved -notcontains $recent -or $saved -notcontains "$old 1000/2000"){
        throw 'History round trip lost classified bytes or legacy totals'
    }
    if ($LiveAdapter) {
        $today = [datetime]::Today.ToString('yyyy/MM/dd', [cultureinfo]::InvariantCulture)
        $row = @($saved | Where-Object { $_.StartsWith("$today ") })
        if ($row.Count -ne 1 -or $row[0] -notmatch ' scope1 (\d+) (\d+) (\d+) (\d+)$') {
            throw 'Live adapter did not produce a classified record'
        }
        $bytes = @(1..4 | ForEach-Object { [uint64]$Matches[$_] })
        if (($bytes | Measure-Object -Sum).Sum -le 0) { throw 'No classified traffic was recorded' }
        Write-Host "Captured LAN up/down, Internet up/down bytes: $bytes"
    }
    Write-Host 'Calendar scope switches and history round trip passed'
} finally {
    if(!$process.HasExited){$process.Kill(); $process.WaitForExit()}
}

