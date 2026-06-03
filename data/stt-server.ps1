<#
.SYNOPSIS
    STT server: receives WAV from ESP32, calls doubao-stt.ps1, returns text.
.DESCRIPTION
    Listens on http://0.0.0.0:12345/stt for POST requests containing WAV audio,
    saves to a temp file, runs doubao-stt.ps1 for transcription, returns the
    result text as plain HTTP response. Also prints to console and copies to
    clipboard.
.PARAMETER Port
    Listening port, default 12345
.PARAMETER DoudaoSttPath
    Path to doubao-stt.ps1, default same directory as this script
.EXAMPLE
    .\stt-server.ps1
    .\stt-server.ps1 -Port 8080
#>

param(
    [int]$Port = 12345,
    [string]$DoubaoSttPath = ""
)

$ErrorActionPreference = "Stop"

if (-not $DoubaoSttPath) {
    $DoubaoSttPath = Join-Path $PSScriptRoot "doubao-stt.ps1"
}

if (-not (Test-Path -LiteralPath $DoubaoSttPath)) {
    Write-Error "doubao-stt.ps1 not found at: $DoubaoSttPath"
    exit 1
}

if (-not $env:DOUBAO_STT_APP_ID -or -not $env:DOUBAO_STT_ACCESS_KEY) {
    Write-Error @"
Set doubao credentials first:
  `$env:DOUBAO_STT_APP_ID = 'your_app_id'
  `$env:DOUBAO_STT_ACCESS_KEY = 'your_access_key'
"@
    exit 1
}

Add-Type @"
using System;
using System.Net;
using System.Text;
using System.Threading;

public class SttServer {
    public HttpListener Listener;
    public int Port;

    public void Start() {
        Listener = new HttpListener();
        Listener.Prefixes.Add("http://0.0.0.0:" + Port + "/stt/");
        Listener.Start();
    }

    public void Stop() {
        if (Listener != null) {
            Listener.Stop();
            Listener.Close();
        }
    }
}
"@

Write-Output ""
Write-Output "========================================="
Write-Output "  ESP32 STT Server"
Write-Output "  Listening on http://0.0.0.0:$Port/stt/"
Write-Output "========================================="
Write-Output ""
Write-Output "Waiting for audio from ESP32..."
Write-Output "Press Ctrl+C to stop."
Write-Output ""

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://0.0.0.0:$Port/stt/")

try {
    $listener.Start()
} catch {
    Write-Error "Failed to start HTTP listener on port $Port: $_"
    Write-Output "Try: netsh http add urlacl url=http://0.0.0.0:$Port/stt/ user=Everyone"
    exit 1
}

try {
    while ($listener.IsListening) {
        $ctx = $null
        try {
            $ctx = $listener.GetContext()
        } catch {
            break
        }

        if ($null -eq $ctx) { break }

        $request = $ctx.Request
        $response = $ctx.Response

        if ($request.HttpMethod -ne "POST" -or -not $request.HasEntityBody) {
            $response.StatusCode = 400
            $buffer = [System.Text.Encoding]::UTF8.GetBytes("Bad request: POST WAV data to /stt/")
            $response.ContentLength64 = $buffer.Length
            $response.OutputStream.Write($buffer, 0, $buffer.Length)
            $response.OutputStream.Close()
            continue
        }

        $bodyLen = [int]$request.ContentLength64
        Write-Output "[$(Get-Date -Format 'HH:mm:ss')] Received POST /stt ($bodyLen bytes)"

        $ms = New-Object System.IO.MemoryStream
        $request.InputStream.CopyTo($ms)
        $wavBytes = $ms.ToArray()
        $ms.Dispose()

        $tmpFile = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "esp32_stt_$(Get-Random).wav")
        [System.IO.File]::WriteAllBytes($tmpFile, $wavBytes)
        Write-Output "[$(Get-Date -Format 'HH:mm:ss')] Saved to $tmpFile ($($wavBytes.Length) bytes)"

        $resultText = ""
        $success = $false

        try {
            $output = & $DoubaoSttPath -AudioFile $tmpFile 2>&1
            $lastLine = ""
            $inResult = $false
            $resultLines = @()

            foreach ($line in $output) {
                $strLine = $line.ToString()
                Write-Output "  $strLine"
                if ($strLine -match "^={10,}") {
                    if ($inResult) { break }
                    $inResult = $true
                    continue
                }
                if ($inResult) {
                    $resultLines += $strLine
                }
            }

            $resultText = ($resultLines -join "").Trim()
            if ($resultText.Length -gt 0) {
                $success = $true
            }
        } catch {
            Write-Output "[$(Get-Date -Format 'HH:mm:ss')] doubao-stt error: $_"
            $resultText = ""
        } finally {
            Remove-Item -LiteralPath $tmpFile -ErrorAction SilentlyContinue
        }

        if ($success) {
            Write-Output ""
            Write-Output "========================================="
            Write-Output "  Result: $resultText"
            Write-Output "========================================="
            Write-Output ""

            try {
                Set-Clipboard -Value $resultText
                Write-Output "(Copied to clipboard)"
            } catch {
                Write-Output "(Failed to copy to clipboard: $_)"
            }
        } else {
            Write-Output "[$(Get-Date -Format 'HH:mm:ss')] Transcription failed or empty result"
        }

        $respText = if ($success) { $resultText } else { "ERROR: Transcription failed" }
        $respBytes = [System.Text.Encoding]::UTF8.GetBytes($respText)
        $response.StatusCode = 200
        $response.ContentType = "text/plain; charset=utf-8"
        $response.ContentLength64 = $respBytes.Length
        $response.OutputStream.Write($respBytes, 0, $respBytes.Length)
        $response.OutputStream.Close()

        Write-Output ""
        Write-Output "Waiting for next audio..."
    }
} finally {
    $listener.Stop()
    $listener.Close()
    Write-Output "Server stopped."
}