@echo off
rem ============================================================
rem  启动 LibreHardwareMonitor + 自动发现广播
rem  需要管理员权限：读取传感器、放行 8085 端口
rem  用法：把本文件与 lhm_beacon.ps1 / lhm_beacon.vbs 放进 LHM 目录，
rem        右键本文件 → 以管理员身份运行
rem ============================================================
pushd "%~dp0" >nul 2>&1
if errorlevel 1 cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% equ 0 goto :admin

echo [X] 需要管理员权限。
echo     右键这个 bat，选「以管理员身份运行」。
echo.
pause
exit /b

:admin
if not exist "%~dp0LibreHardwareMonitor.exe" (
  echo [X] 当前目录没有 LibreHardwareMonitor.exe。
  echo     请把本文件与 lhm_beacon.ps1 / lhm_beacon.vbs 一起复制到 LHM 解压目录后再运行。
  echo.
  pause
  exit /b
)

netsh advfirewall firewall delete rule name="LibreHardwareMonitor 8085" >nul 2>&1
netsh advfirewall firewall add rule name="LibreHardwareMonitor 8085" dir=in action=allow protocol=TCP localport=8085 >nul

echo ============================================================
echo   LibreHardwareMonitor 正在以管理员权限启动
echo.
echo   1) 8085 端口已加入防火墙白名单
echo   2) 首次使用请在 LHM 菜单里开启 Web 服务：
echo        Options - Remote Web Server - Run
echo   3) 浏览器打开下面地址验证：
echo        http://127.0.0.1:8085/data.json
echo ============================================================
echo.

start "" "%~dp0LibreHardwareMonitor.exe"

rem 自动发现广播：ESP32 靠它得知数据源 IP，无需手填
start "" wscript.exe //nologo "%~dp0lhm_beacon.vbs"

echo   窗口 8 秒后自动关闭，LHM 会留在托盘里。
ping -n 8 127.0.0.1 >nul
exit