<#
.SYNOPSIS
    调用豆包语音（火山引擎）录音文件识别 API 转写音频为文字
.DESCRIPTION
    两步流程：提交音频 → 轮询结果。支持 wav/mp3/m4a/ogg/flac 格式。
.PARAMETER AudioFile
    音频文件路径（必填）
.PARAMETER AppId
    火山引擎 APP ID，不传则读环境变量 DOUBAO_STT_APP_ID
.PARAMETER AccessKey
    火山引擎 Access Token，不传则读环境变量 DOUBAO_STT_ACCESS_KEY
.PARAMETER ResourceId
    资源 ID，默认 volc.seedasr.auc
.PARAMETER PollInterval
    轮询间隔（秒），默认 2
.PARAMETER MaxPoll
    最大轮询次数，默认 30（即最多等 60 秒）
.PARAMETER Format
    音频格式，默认从文件扩展名自动推断
.PARAMETER Uid
    用户标识，默认 "doubao-stt-user"
.EXAMPLE
    .\doubao-stt.ps1 -AudioFile "C:\audio.m4a"
.EXAMPLE
    $env:DOUBAO_STT_APP_ID = "your_app_id"
    $env:DOUBAO_STT_ACCESS_KEY = "your_access_token"
    .\doubao-stt.ps1 -AudioFile "C:\audio.wav"
#>

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$AudioFile,

    [string]$AppId,
    [string]$AccessKey,
    [string]$ResourceId = "volc.seedasr.auc",
    [int]$PollInterval = 2,
    [int]$MaxPoll = 30,
    [string]$Format = "",
    [string]$Uid = "doubao-stt-user"
)

# ── 凭证 ────────────────────────────────────────────────
if (-not $AppId)  { $AppId  = $env:DOUBAO_STT_APP_ID }
if (-not $AccessKey) { $AccessKey = $env:DOUBAO_STT_ACCESS_KEY }

if (-not $AppId -or -not $AccessKey) {
    Write-Error "Set APP_ID and ACCESS_TOKEN via -AppId/-AccessKey params or DOUBAO_STT_APP_ID/DOUBAO_STT_ACCESS_KEY env vars"
    exit 1
}

# ── 文件检查 ─────────────────────────────────────────────
if (-not (Test-Path -LiteralPath $AudioFile)) {
    Write-Error "File not found: $AudioFile"
    exit 1
}

# ── 推断格式 ─────────────────────────────────────────────
if (-not $Format) {
    $ext = [IO.Path]::GetExtension($AudioFile).TrimStart('.').ToLower()
    $Format = $ext -replace '^m4a$', 'm4a' `
                   -replace '^mp3$', 'mp3' `
                   -replace '^wav$', 'wav' `
                   -replace '^flac$', 'flac' `
                   -replace '^ogg$', 'ogg'
    if ($Format -notin @('wav', 'mp3', 'm4a', 'flac', 'ogg')) {
        Write-Error "Unsupported audio format: $ext. Use -Format to specify (wav/mp3/m4a/flac/ogg)"
        exit 1
    }
}

$fileSize = (Get-Item -LiteralPath $AudioFile).Length
Write-Output "[INFO] Audio: $AudioFile ($([math]::Round($fileSize / 1MB, 2)) MB, format: $Format)"

# ── 读取并编码 ──────────────────────────────────────────
Write-Output "[INFO] Reading and encoding to base64 ..."
$sw = [Diagnostics.Stopwatch]::StartNew()
$base64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($AudioFile))
$sw.Stop()
Write-Output "[INFO] Encoding done in $($sw.Elapsed.TotalSeconds.ToString('0.0'))s, base64 length: $($base64.Length)"

# ── 提交 ────────────────────────────────────────────────
$uuid = [Guid]::NewGuid().ToString()
Write-Output "[INFO] Task ID: $uuid"

$submitBody = @{
    user  = @{ uid = $Uid }
    audio = @{
        data   = $base64
        format = $Format
    }
} | ConvertTo-Json -Compress

$headers = @{
    "Content-Type"     = "application/json"
    "X-Api-App-Key"    = $AppId
    "X-Api-Access-Key" = $AccessKey
    "X-Api-Resource-Id"= $ResourceId
    "X-Api-Request-Id" = $uuid
}

Write-Output "[INFO] Submitting task ..."
try {
    $null = Invoke-RestMethod -Uri "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit" `
        -Method Post -Headers $headers -Body $submitBody -ContentType "application/json"
} catch {
    Write-Error "Submit failed: $_"
    exit 1
}

Write-Output "[INFO] Submit OK, polling for result ..."

# ── 轮询 ────────────────────────────────────────────────
for ($i = 0; $i -lt $MaxPoll; $i++) {
    Start-Sleep -Seconds $PollInterval

    try {
        $parsed = Invoke-RestMethod -Uri "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query" `
            -Method Post -Headers $headers -Body "{}" -ContentType "application/json"
    } catch {
        Write-Output "[POLL $($i+1)] Query error: $_"
        continue
    }

    Write-Output "[POLL $($i+1)] $(($parsed | ConvertTo-Json -Compress))"

    if ($parsed.result -and $parsed.result.text) {
        Write-Output ""
        Write-Output "========================================"
        Write-Output "  Transcription done! Duration: $($parsed.audio_info.duration) ms"
        Write-Output "========================================"
        Write-Output $parsed.result.text
        Write-Output "========================================"
        return $parsed.result.text
    }

    # 检查是否返回了错误
    if ($parsed.code -and $parsed.code -ne 0) {
        Write-Error "API error: code=$($parsed.code) message=$($parsed.message)"
        exit 1
    }
}

Write-Error "Poll timeout: no result after $($MaxPoll * $PollInterval) seconds"
exit 1
