@echo off
rem 后台常驻桥接：无窗口，日志写 bridge.log。
rem 给"不挂在 ZCode 进程树下"的方式启动用——从 ZCode 会话里拉起来的桥接会随
rem ZCode 重启一起被杀（实测踩过）。给人看事件日志请用 start-bridge.bat。
cd /d "%~dp0"
python bridge.py > bridge.log 2>&1
