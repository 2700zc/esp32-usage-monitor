<#
.SYNOPSIS
    快速测试脚本：将 LazyTyper 目录下的 a.m4a 转写为文字
#>

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# ── 填入你的凭证 ────────────────────────────────────────
$env:DOUBAO_STT_APP_ID     = "9552573009"
$env:DOUBAO_STT_ACCESS_KEY = "6wGTuNrhusyzz33qMOYbQP6wkIDoWWCp"
# ────────────────────────────────────────────────────────

$audioFile = "C:\Users\admin\AppData\Local\Programs\LazyTyper\a.m4a"

& "$scriptDir\doubao-stt.ps1" -AudioFile $audioFile
