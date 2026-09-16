# Fake engine installer for tests/host_harness.cpp --setup-fake: same arguments and
# status.json/install.log contract as installer/install_backend.ps1, no downloads.
# A long-running child process (ping) stands for uv/pip, so cancelling must kill a tree.
#   $env:GBTTS_FAKE_STEP_MS   duration of each step (default 700)
param(
    [string]$InstallDir,
    [string]$Mode = "auto",
    [string[]]$Models = @()
)
$ErrorActionPreference = "Stop"
$statusDir = Join-Path $InstallDir "install"
New-Item -ItemType Directory -Force -Path $statusDir | Out-Null
$statusFile = Join-Path $statusDir "status.json"
$logFile = Join-Path $statusDir "install.log"
$utf8 = New-Object Text.UTF8Encoding $false
$stepMs = if ($env:GBTTS_FAKE_STEP_MS) { [int]$env:GBTTS_FAKE_STEP_MS } else { 700 }
if ($Mode -eq "auto") { $Mode = "cpu" }
$stages = @("check", "uv", "python", "venv", "torch", "libs", "code", "verify", "models", "finish")
$status = [ordered]@{
    version = 1; state = "running"; pid = $PID; mode = $Mode; installDir = $InstallDir
    started = (Get-Date).ToString("o"); stage = "check"; stageIndex = 0; stageCount = $stages.Count; stages = $stages
    overall = 0.0; stageFraction = 0.0; bytesDone = 0; bytesTotal = 0; item = ""; error = ""; errorCode = ""; gpu = ""; sizeBytes = 0
}
function Save {
    # Same retry as the real installer: the plugin may have status.json open for a few ms,
    # and File.Replace fails while it does (Qt opens files without FILE_SHARE_DELETE).
    $tmp = "$statusFile.tmp"
    for ($i = 0; $i -lt 20; $i++) {
        try {
            [IO.File]::WriteAllText($tmp, (ConvertTo-Json $status -Compress), $utf8)
            if (Test-Path $statusFile) { [IO.File]::Replace($tmp, $statusFile, [NullString]::Value) } else { [IO.File]::Move($tmp, $statusFile) }
            return
        } catch { Start-Sleep -Milliseconds 50 }
    }
}
function Log([string]$m) { [IO.File]::AppendAllText($logFile, ("{0:HH:mm:ss} {1}`r`n" -f (Get-Date), $m), $utf8) }

$child = Start-Process -FilePath "$env:SystemRoot\System32\PING.EXE" -ArgumentList "-n", "3600", "127.0.0.1" -WindowStyle Hidden -PassThru
[IO.File]::WriteAllText((Join-Path $statusDir "fake_child.pid"), [string]$child.Id, $utf8)
Log "fake installer pid $PID, child $($child.Id), mode $Mode"
for ($i = 0; $i -lt $stages.Count; $i++) {
    $status.stage = $stages[$i]
    $status.stageIndex = $i
    Log "[$($i + 1)/$($stages.Count)] $($stages[$i])"
    for ($k = 1; $k -le 5; $k++) {
        $status.stageFraction = $k / 5
        $status.bytesDone = [int64]($k * 100MB)
        $status.bytesTotal = [int64]500MB
        $status.item = "fake-$($stages[$i])"
        $status.overall = [Math]::Round(($i + $k / 5) / $stages.Count, 4)
        Save
        Start-Sleep -Milliseconds ([int]($stepMs / 5))
    }
}
Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue
$status.state = "done"
$status.overall = 1.0
$status.sizeBytes = [int64]1.5GB
Save
Log "=== FATTO (fake) ==="
exit 0
