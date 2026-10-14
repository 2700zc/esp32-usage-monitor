@echo off
rem ZCode 状态桥接。这个窗口关掉就等于停止桥接——设备会显示"连不上桥接"，
rem 其他页面不受影响。窗口里会实时打印收到的每个事件，排查时就靠它。
rem
rem 想让桥接随开机自动运行：Win+R 输入 shell:startup，把本文件的快捷方式丢进去。
cd /d "%~dp0"
python bridge.py %*
echo.
echo bridge exited.
pause
