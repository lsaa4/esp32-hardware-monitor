# ============================================================
#  LHM 自动发现广播（beacon）
#  作用：每隔 2 秒向局域网广播 "LHM|本机IP|端口|主机名"
#        ESP32 副屏开机收包即知道数据源在哪，不用手填 IP。
#  生命周期：LHM 进程不在时自动退出（LHM 关了就不用播了）
#  启动方式：由 start-monitor.bat 自动带起（无窗口）
# ============================================================

$ErrorActionPreference = 'SilentlyContinue'
$ProgressPreference    = 'SilentlyContinue'

$Port       = 8099
$IntervalMs = 2000
$EventName  = 'LHM_Beacon'          # 同名进程只留一个

# --- 单实例：已经有别的 beacon 在跑就直接退出 ---
$me = [System.Diagnostics.Process]::GetCurrentProcess().Id
$others = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
            Where-Object { $_.CommandLine -like '*lhm_beacon.ps1*' -and $_.ProcessId -ne $me })
if ($others.Count -gt 0) {
    [Console]::Out.Write("beacon already running (pid=$($others[0].ProcessId)), exit`n")
    exit 0
}

function Get-LhmPort {
    $cfg = Join-Path $PSScriptRoot 'LibreHardwareMonitor.config'
    if (Test-Path $cfg) {
        $m = Select-String -Path $cfg -Pattern 'name="listenerPort".*?>(\d+)<' -AllMatches
        if ($m -and $m.Matches.Count -gt 0) { return [int]$m.Matches[0].Groups[1].Value }
    }
    return 8085
}

$port8085 = Get-LhmPort
[Console]::Out.Write("beacon start: port=$port8085 udp=$Port pid=$me`n")

$udp = $null
$lastReport = 0

while ($true) {
    # --- LHM 不在了就退出 ---
    $lhm = @(Get-Process LibreHardwareMonitor -ErrorAction SilentlyContinue)
    if ($lhm.Count -eq 0) {
        [Console]::Out.Write("LHM not running, beacon exit`n")
        break
    }

    # --- 枚举所有可用的 IPv4（跳过回环 / 169.254 自分配） ---
    $ips = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
             Where-Object {
                 $_.IPAddress -notlike '127.*' -and
                 $_.IPAddress -notlike '169.254.*' -and
                 $_.PrefixOrigin -ne 'WellKnown'
             })

    foreach ($ip in $ips) {
        $msg   = "LHM|$($ip.IPAddress)|$port8085|$env:COMPUTERNAME"
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($msg)
        try {
            # 绑定到具体网卡再广播，多网卡机器才能两边都发出去
            $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($ip.IPAddress), 0)
            $u  = New-Object System.Net.Sockets.UdpClient($ep)
            $u.EnableBroadcast = $true
            [void]$u.Send($bytes, $bytes.Length, '255.255.255.255', $Port)
            # 顺便发一份到本网段的定向广播，某些路由器不放行 255.255.255.255
            $bcast = ($ip.IPAddress -replace '\.\d+$', '.255')
            [void]$u.Send($bytes, $bytes.Length, $bcast, $Port)
            $u.Close()
        } catch {
            # 单块网卡失败不影响其它网卡
        }
    }

    # --- 每 30 秒打一行日志，方便排查 ---
    if ([Environment]::TickCount - $lastReport -gt 30000) {
        $lastReport = [Environment]::TickCount
        $list = ($ips | ForEach-Object { $_.IPAddress }) -join ','
        [Console]::Out.Write("beacon alive -> $list`n")
    }

    Start-Sleep -Milliseconds $IntervalMs
}
