' ============================================================
'  lhm_beacon.vbs — 零窗口启动 lhm_beacon.ps1
'  由 start-monitor.bat 调用：wscript.exe //nologo lhm_beacon.vbs
' ============================================================
Dim sh, d, cmd
Set sh = CreateObject("WScript.Shell")
d = Left(WScript.ScriptFullName, InStrRev(WScript.ScriptFullName, "\"))
cmd = "powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File """ & d & "lhm_beacon.ps1"""
sh.Run cmd, 0, False
