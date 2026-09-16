# ---------------------------------------------------------------------------
# GameBaiters TTS - voice engine installer (fully automatic)
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File install_backend.ps1
#       [-InstallDir <dir>] [-Mode auto|gpu|cpu] [-Models kokoro,base-1.7b,...] [-SkipModels]
#
# Normally started HIDDEN by the plugin (Settings / "Install the voice engine"
# window), which shows the progress read from <InstallDir>\install\status.json.
# Run by hand it prints the same steps in the console.
#
# Nothing has to be installed beforehand and nothing outside <InstallDir> is
# touched (except the HKCU\Software\GameBaiters\TTS\backend "home" value):
#   <InstallDir>\tools\uv.exe     uv (pinned, SHA-256 verified) - Python + package manager
#   <InstallDir>\python\          private Python 3.12 (uv-managed, not registered in Windows)
#   <InstallDir>\venv\            engine environment: PyTorch (CUDA 12.8 with an NVIDIA GPU, CPU otherwise)
#   <InstallDir>\app\             engine code (copied from the plugin package)
#   <InstallDir>\models\          voice models
#   <InstallDir>\cache\uv\        package cache (hard-linked into venv: a repair re-downloads nothing)
#   <InstallDir>\install\         status.json (progress for the plugin) + install.log
#
# Idempotent: run it again to repair or update; every step resumes what is already there.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "GameBaitersTTS"),
    [string]$SourceDir = "",
    [ValidateSet("auto", "gpu", "cpu")][string]$Mode = "auto",
    [string[]]$Models = @(),
    [switch]$SkipModels
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest's progress bar slows downloads 10x in PS 5.1
Set-StrictMode -Version 2.0

$UvVersion = "0.12.15"
$UvSha256 = "477bd99a84e34891f2bd4c9152ddeb74e971accccbc59c0f0301f11f08a32d46"
$PythonVersion = "3.12"
$TorchVersion = "2.11.0"
# Package cache growth while installing, measured on clean installs (vault
# build-release/engine-installer.md): only used to move the progress bar.
$ExpectedBytes = @{ "torch-gpu" = 4.1GB; "torch-cpu" = 470MB; "libs" = 620MB }

try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$InstallDir = [IO.Path]::GetFullPath($InstallDir)
$statusDir = Join-Path $InstallDir "install"
New-Item -ItemType Directory -Force -Path $statusDir | Out-Null
$statusFile = Join-Path $statusDir "status.json"
$logFile = Join-Path $statusDir "install.log"
$utf8 = New-Object Text.UTF8Encoding $false
$started = Get-Date

# ---- status + log ------------------------------------------------------------------
$script:stages = @()
$script:stageIndex = -1
$script:stageBest = 0.0
$script:status = [ordered]@{
    version = 1; state = "running"; pid = $PID; mode = ""; installDir = $InstallDir
    started = $started.ToString("o"); updated = ""; stage = ""; stageIndex = 0; stageCount = 0
    stages = @(); overall = 0.0; stageFraction = -1.0; bytesDone = 0; bytesTotal = 0; item = ""
    error = ""; errorCode = ""; gpu = ""; sizeBytes = 0
}

function Log([string]$msg, [string]$color = "Gray") {
    $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $msg
    [IO.File]::AppendAllText($logFile, $line + "`r`n", $utf8)
    Write-Host $msg -ForegroundColor $color
}

function Save-Status {
    $script:status.updated = (Get-Date).ToString("o")
    $json = ConvertTo-Json $script:status -Depth 4 -Compress
    $tmp = $statusFile + ".tmp"
    # The plugin reads while we write: write aside, then swap (retry if it has the file open).
    for ($i = 0; $i -lt 20; $i++) {
        try {
            [IO.File]::WriteAllText($tmp, $json, $utf8)
            # [NullString]::Value: a plain $null becomes "" when PowerShell passes it to a .NET string.
            if (Test-Path $statusFile) { [IO.File]::Replace($tmp, $statusFile, [NullString]::Value) } else { [IO.File]::Move($tmp, $statusFile) }
            return
        } catch { Start-Sleep -Milliseconds 50 }
    }
}

function Set-Progress([double]$fraction, [int64]$done = 0, [int64]$total = 0, [string]$item = "") {
    if ($fraction -gt 1) { $fraction = 1 }
    # Never backwards inside a step: the cache probe shrinks while uv removes temporary files.
    if ($fraction -ge 0) {
        if ($fraction -lt $script:stageBest) { $fraction = $script:stageBest }
        $script:stageBest = $fraction
    }
    $before = 0.0
    for ($i = 0; $i -lt $script:stageIndex; $i++) { $before += $script:stages[$i].weight }
    $w = $script:stages[$script:stageIndex].weight
    $inStage = if ($fraction -ge 0) { $fraction } else { 0 }
    $script:status.overall = [Math]::Round(($before + $w * $inStage) / $script:weightSum, 4)
    $script:status.stageFraction = [Math]::Round($fraction, 4)
    $script:status.bytesDone = $done
    $script:status.bytesTotal = $total
    if ($item) { $script:status.item = $item }
    Save-Status
}

function Start-Stage([string]$id) {
    for ($i = 0; $i -lt $script:stages.Count; $i++) {
        if ($script:stages[$i].id -eq $id) { $script:stageIndex = $i; break }
    }
    $s = $script:stages[$script:stageIndex]
    $script:status.stage = $id
    $script:status.stageIndex = $script:stageIndex
    $script:status.item = ""
    $script:stageBest = 0.0
    Log ("[{0}/{1}] {2}" -f ($script:stageIndex + 1), $script:stages.Count, $s.label) "Cyan"
    Set-Progress 0
}

class InstallError : Exception {
    [string]$Code
    InstallError([string]$code, [string]$message) : base($message) { $this.Code = $code }
}

function Fail([string]$code, [string]$msg) { throw [InstallError]::new($code, $msg) }

function Get-DirBytes([string]$path) {
    if (-not (Test-Path $path)) { return [int64]0 }
    $sum = [int64]0
    try {
        foreach ($f in [IO.Directory]::EnumerateFiles($path, "*", [IO.SearchOption]::AllDirectories)) {
            try { $sum += (New-Object IO.FileInfo $f).Length } catch { }
        }
    } catch { }
    return $sum
}

# Runs a tool hidden, logging its output; while it runs, `probe` (a directory) growth
# against `expected` bytes moves the bar. Returns the exit code.
function Invoke-Tool([string]$exe, [string[]]$arguments, [string]$probe = "", [int64]$expected = 0, [string]$item = "") {
    $quoted = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join " "
    Log "> $(Split-Path -Leaf $exe) $quoted" "DarkGray"
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $quoted
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    foreach ($k in $script:toolEnv.Keys) { $psi.EnvironmentVariables[$k] = $script:toolEnv[$k] }
    $p = New-Object Diagnostics.Process
    $p.StartInfo = $psi
    try {
        [void]$p.Start()
        # Both pipes drained continuously with async line reads (event handlers do not
        # run while this loop waits, and an undrained pipe would block the tool).
        $readers = @($p.StandardOutput, $p.StandardError)
        $pending = @($readers[0].ReadLineAsync(), $readers[1].ReadLineAsync())
        $open = @($true, $true)
        $base = if ($probe) { Get-DirBytes $probe } else { 0 }
        $lastProbe = Get-Date
        while ($open[0] -or $open[1]) {
            $idle = $true
            for ($i = 0; $i -lt 2; $i++) {
                while ($open[$i] -and $pending[$i].IsCompleted) {
                    $idle = $false
                    $line = $pending[$i].Result
                    if ($null -eq $line) { $open[$i] = $false; break }
                    Handle-ToolLine $line
                    $pending[$i] = $readers[$i].ReadLineAsync()
                }
            }
            if ($probe -and ((Get-Date) - $lastProbe).TotalSeconds -ge 2) {
                $lastProbe = Get-Date
                $grown = [Math]::Max([int64]0, (Get-DirBytes $probe) - $base)
                $frac = if ($expected -gt 0) { [Math]::Min(0.97, $grown / $expected) } else { -1 }
                Set-Progress $frac $grown $expected $item
            }
            if ($idle) { Start-Sleep -Milliseconds 100 }
        }
        $p.WaitForExit()
        return $p.ExitCode
    } finally {
        $p.Dispose()
    }
}

function Handle-ToolLine([string]$line) {
    if ($line.StartsWith("@@progress ")) {
        try {
            $j = ConvertFrom-Json $line.Substring(11)
            $frac = if ($j.total -gt 0) { [Math]::Min(1.0, $j.done / $j.total) } else { -1 }
            Set-Progress $frac ([int64]$j.done) ([int64]$j.total) ([string]$j.item)
        } catch { }
        return
    }
    if ($line.Trim()) { Log ("  " + $line) "DarkGray" }
}

function Download-File([string]$url, [string]$target, [string]$sha256) {
    $part = $target + ".part"
    Log "  download $url"
    $req = [Net.HttpWebRequest]::Create($url)
    $req.UserAgent = "GameBaiters TTS installer"
    $req.Timeout = 60000
    $req.ReadWriteTimeout = 60000
    $resp = $req.GetResponse()
    try {
        $total = $resp.ContentLength
        $in = $resp.GetResponseStream()
        $out = [IO.File]::Create($part)
        try {
            $buf = New-Object byte[] (1MB)
            $done = [int64]0
            $last = Get-Date
            while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                $out.Write($buf, 0, $n)
                $done += $n
                if (((Get-Date) - $last).TotalMilliseconds -ge 300) {
                    $last = Get-Date
                    Set-Progress ($(if ($total -gt 0) { $done / $total } else { -1 })) $done $total (Split-Path -Leaf $target)
                }
            }
        } finally { $out.Dispose(); $in.Dispose() }
    } finally { $resp.Dispose() }
    $actual = (Get-FileHash $part -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $sha256) {
        Remove-Item -Force $part
        Fail "checksum" "download danneggiato di $(Split-Path -Leaf $target) (SHA-256 $actual)"
    }
    Move-Item -Force $part $target
}

# ---- main --------------------------------------------------------------------------
$mutex = New-Object Threading.Mutex($false, "Local\GameBaitersTTS-engine-install")
$exitCode = 0
try {
    if (-not $mutex.WaitOne(0)) { Fail "busy" "un'altra installazione del motore vocale e' gia' in corso" }
    Log "=== GameBaiters TTS - installazione del motore vocale ($(Get-Date -Format 'yyyy-MM-dd HH:mm')) ===" "Cyan"

    # ---- sources ---------------------------------------------------------------------
    if (-not $SourceDir) {
        $here = Split-Path -Parent $MyInvocation.MyCommand.Path
        foreach ($cand in @($here, (Join-Path $here "..\backend"))) {
            if (Test-Path (Join-Path $cand "gbtts\server.py")) { $SourceDir = (Resolve-Path $cand).Path; break }
        }
    }
    if (-not $SourceDir -or -not (Test-Path (Join-Path $SourceDir "gbtts\server.py"))) {
        Fail "sources" "codice del motore non trovato accanto all'installer (gbtts\server.py): reinstalla il plugin"
    }

    # ---- GPU ---------------------------------------------------------------------------
    $gpuName = ""
    $driver = 0.0
    $smi = Join-Path $env:SystemRoot "System32\nvidia-smi.exe"
    if (-not (Test-Path $smi)) { $cmd = Get-Command nvidia-smi -ErrorAction SilentlyContinue; if ($cmd) { $smi = $cmd.Source } else { $smi = "" } }
    if ($smi) {
        try {
            $q = & $smi --query-gpu=name,driver_version --format=csv,noheader 2>$null | Select-Object -First 1
            if ($q) {
                $parts = $q.Split(",")
                $gpuName = $parts[0].Trim()
                [double]::TryParse($parts[1].Trim(), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$driver) | Out-Null
            }
        } catch { }
    }
    if ($Mode -eq "auto") { $Mode = if ($gpuName) { "gpu" } else { "cpu" } }
    if ($Mode -eq "gpu" -and -not $gpuName) { Fail "no_gpu" "modalita' GPU richiesta ma nessuna scheda NVIDIA trovata" }
    if ($Mode -eq "gpu" -and $driver -gt 0 -and $driver -lt 570) {
        Fail "gpu_driver" "il driver NVIDIA $driver e' troppo vecchio per PyTorch CUDA 12.8 (serve 570 o piu' recente): aggiornalo da nvidia.com"
    }
    if ($Models.Count -eq 0 -and -not $SkipModels) {
        # The Italian voices ship ready-made (gbtts/stock_voices): the VoiceDesign model (4.2 GB)
        # is only needed to create a new voice and downloads itself the first time.
        $Models = if ($Mode -eq "gpu") { @("kokoro", "base-1.7b") } else { @("kokoro") }
    }
    $Models = @($Models | ForEach-Object { $_ -split "," } | Where-Object { $_ })
    $script:status.mode = $Mode
    $script:status.gpu = $gpuName
    $gpu = $Mode -eq "gpu"

    $script:stages = @(
        @{ id = "check"; label = "Controllo del sistema"; weight = 1 },
        @{ id = "uv"; label = "Strumenti di installazione (uv)"; weight = 2 },
        @{ id = "python"; label = "Python $PythonVersion"; weight = 3 },
        @{ id = "venv"; label = "Ambiente del motore"; weight = 1 },
        @{ id = "torch"; label = $(if ($gpu) { "PyTorch per GPU NVIDIA (circa 2,6 GB)" } else { "PyTorch per CPU" }); weight = $(if ($gpu) { 34 } else { 6 }) },
        @{ id = "libs"; label = "Librerie del motore vocale"; weight = 14 },
        @{ id = "code"; label = "Codice del motore"; weight = 1 },
        @{ id = "verify"; label = "Verifica"; weight = 2 }
    )
    if ($Models.Count) {
        $script:stages += @{ id = "models"; label = "Modelli vocali"; weight = $(if ($gpu) { 26 } else { 4 }) }
    }
    $script:stages += @{ id = "finish"; label = "Completamento"; weight = 1 }
    $script:weightSum = 0.0
    foreach ($s in $script:stages) { $script:weightSum += $s.weight }
    $script:status.stageCount = $script:stages.Count
    $script:status.stages = @($script:stages | ForEach-Object { $_.id })

    # ---- 1. check ------------------------------------------------------------------------
    Start-Stage "check"
    Log "  cartella: $InstallDir"
    Log "  sorgenti: $SourceDir"
    Log ("  modalita': {0}{1}" -f $Mode, $(if ($gpuName) { " ($gpuName, driver $driver)" } else { "" }))
    Log "  modelli: $($Models -join ', ')"
    $needGb = if ($gpu) { 12 } else { 5 }
    if ($Models.Count -eq 0) { $needGb = if ($gpu) { 7 } else { 3 } }
    $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($InstallDir))
    $freeGb = [Math]::Round($drive.AvailableFreeSpace / 1GB, 1)
    $already = [Math]::Round((Get-DirBytes $InstallDir) / 1GB, 1)
    Log "  spazio libero: $freeGb GB (gia' installati $already GB, servono circa $needGb GB)"
    if ($freeGb + $already -lt $needGb) {
        Fail "disk_space" "spazio insufficiente su $($drive.Name): liberi $freeGb GB, servono circa $needGb GB"
    }

    $tools = Join-Path $InstallDir "tools"
    $pythonDir = Join-Path $InstallDir "python"
    $cacheDir = Join-Path $InstallDir "cache\uv"
    $venv = Join-Path $InstallDir "venv"
    $vpy = Join-Path $venv "Scripts\python.exe"
    $app = Join-Path $InstallDir "app"
    New-Item -ItemType Directory -Force -Path $tools, $pythonDir, $cacheDir | Out-Null
    $script:toolEnv = @{
        "UV_PYTHON_INSTALL_DIR" = $pythonDir
        "UV_PYTHON_PREFERENCE" = "only-managed"   # never a Python of the system (could vanish or be the wrong version)
        "UV_CACHE_DIR" = $cacheDir
        "UV_NATIVE_TLS" = "1"                     # Windows certificate store: works behind antivirus HTTPS scanning
        "UV_NO_PROGRESS" = "1"
        "UV_HTTP_TIMEOUT" = "120"
        "PYTHONUTF8" = "1"
        "PYTHONIOENCODING" = "utf-8"
        "VIRTUAL_ENV" = ""
        "PYTHONHOME" = ""
        "PYTHONPATH" = ""
    }
    Set-Progress 1

    # ---- 2. uv ------------------------------------------------------------------------------
    Start-Stage "uv"
    $uv = Join-Path $tools "uv.exe"
    $uvOk = $false
    if (Test-Path $uv) {
        $v = (& $uv --version 2>$null) -join ""
        $uvOk = $v -match [regex]::Escape($UvVersion)
    }
    if (-not $uvOk) {
        $zip = Join-Path $tools "uv-$UvVersion.zip"
        try {
            Download-File "https://github.com/astral-sh/uv/releases/download/$UvVersion/uv-x86_64-pc-windows-msvc.zip" $zip $UvSha256
        } catch [InstallError] { throw } catch {
            Fail "network" "download di uv non riuscito: $($_.Exception.Message)"
        }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            foreach ($entry in $archive.Entries) {
                if ($entry.Name -in @("uv.exe", "uvx.exe")) {
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $tools $entry.Name), $true)
                }
            }
        } finally { $archive.Dispose() }
        Remove-Item -Force $zip
    }
    Log "  $((& $uv --version 2>$null) -join '')"
    Set-Progress 1

    # ---- 3. python ------------------------------------------------------------------------------
    Start-Stage "python"
    if ((Invoke-Tool $uv @("python", "install", $PythonVersion) $pythonDir 40MB "Python $PythonVersion") -ne 0) {
        Fail "python" "installazione di Python $PythonVersion non riuscita (vedi il log)"
    }
    Set-Progress 1

    # ---- 4. venv ------------------------------------------------------------------------------------
    Start-Stage "venv"
    $venvOk = $false
    if (Test-Path $vpy) {
        # A venv created by an older installer points to a Python of the system: after a
        # Windows reinstall that Python is gone and the venv is dead.
        try {
            $out = & $vpy -c "import sys; print('%d.%d' % sys.version_info[:2]); print(sys.base_prefix)" 2>$null
            $venvOk = ($LASTEXITCODE -eq 0) -and ($out[0] -eq $PythonVersion) -and ($out[1] -like "$pythonDir*")
        } catch { $venvOk = $false }
        if (-not $venvOk) { Log "  ambiente esistente non utilizzabile o non privato: lo ricreo" "Yellow" }
    }
    if (-not $venvOk) {
        if (Test-Path $venv) { Remove-Item -Recurse -Force $venv }
        if ((Invoke-Tool $uv @("venv", $venv, "--python", $PythonVersion, "--seed")) -ne 0) {
            Fail "venv" "creazione dell'ambiente Python non riuscita (vedi il log)"
        }
    }
    Set-Progress 1

    # ---- 5. torch ----------------------------------------------------------------------------------------
    Start-Stage "torch"
    $index = if ($gpu) { "https://download.pytorch.org/whl/cu128" } else { "https://download.pytorch.org/whl/cpu" }
    $torchArgs = @("pip", "install", "--python", $vpy, "torch==$TorchVersion", "torchaudio==$TorchVersion", "--index-url", $index)
    $expected = if ($gpu) { $ExpectedBytes["torch-gpu"] } else { $ExpectedBytes["torch-cpu"] }
    if ((Invoke-Tool $uv $torchArgs $cacheDir $expected "torch $TorchVersion") -ne 0) {
        Fail "network" "installazione di PyTorch non riuscita: controlla la connessione e riprova (riprende da dove si e' fermata)"
    }
    Set-Progress 1

    # ---- 6. libs --------------------------------------------------------------------------------------------
    Start-Stage "libs"
    $req = Join-Path $SourceDir "requirements.txt"
    # torch is already satisfied from its own index: PyPI must never replace the CUDA build.
    if ((Invoke-Tool $uv @("pip", "install", "--python", $vpy, "-r", $req) $cacheDir $ExpectedBytes["libs"] "requirements") -ne 0) {
        Fail "network" "installazione delle librerie non riuscita: controlla la connessione e riprova"
    }
    Set-Progress 1

    # ---- 7. code -------------------------------------------------------------------------------------------------
    Start-Stage "code"
    $appNew = Join-Path $InstallDir "app.new"
    if (Test-Path $appNew) { Remove-Item -Recurse -Force $appNew }
    New-Item -ItemType Directory -Force -Path (Join-Path $appNew "tools") | Out-Null
    Copy-Item -Recurse -Force (Join-Path $SourceDir "gbtts") $appNew
    Copy-Item -Force (Join-Path $SourceDir "tools\download_models.py") (Join-Path $appNew "tools")
    # Remembered so the plugin can tell after an update whether the libraries must be reinstalled.
    Copy-Item -Force $req $appNew
    Get-ChildItem -Path $appNew -Recurse -Directory -Filter "__pycache__" | Remove-Item -Recurse -Force
    if (Test-Path $app) { Remove-Item -Recurse -Force $app }
    Move-Item $appNew $app
    Set-Progress 1

    # ---- 8. verify --------------------------------------------------------------------------------------------------
    Start-Stage "verify"
    $check = if ($gpu) {
        "import torch, faster_qwen3_tts, supertonic, kokoro_onnx, soxr, einops, safetensors, torchaudio, truststore; assert torch.cuda.is_available(), 'CUDA non disponibile'; print('OK', torch.__version__, torch.cuda.get_device_name(0))"
    } else {
        "import torch, supertonic, kokoro_onnx, soxr, einops, safetensors, torchaudio, truststore; print('OK', torch.__version__, 'CPU')"
    }
    if ((Invoke-Tool $vpy @("-c", $check)) -ne 0) {
        Fail "verify" "le librerie installate non funzionano (vedi il log): riprova l'installazione"
    }
    Set-Progress 1

    # ---- 9. models --------------------------------------------------------------------------------------------------------
    if ($Models.Count) {
        Start-Stage "models"
        $dm = @((Join-Path $app "tools\download_models.py"), "--home", $InstallDir, "--progress", "--models") + $Models
        if ((Invoke-Tool $vpy $dm) -ne 0) {
            Fail "models" "download dei modelli non riuscito: riprova, riprende da dove si e' fermato"
        }
        Set-Progress 1
    }

    # ---- 10. finish ---------------------------------------------------------------------------------------------------------
    Start-Stage "finish"
    $key = "HKCU:\Software\GameBaiters\TTS\backend"
    New-Item -Path $key -Force | Out-Null
    Set-ItemProperty -Path $key -Name "home" -Value $InstallDir
    if (-not $gpu) { Set-ItemProperty -Path $key -Name "engine" -Value "kokoro" }
    # The package cache is hard-linked into venv\: counting it would report the libraries twice.
    $script:status.sizeBytes = [int64]0
    foreach ($sub in Get-ChildItem -LiteralPath $InstallDir -Directory) {
        if ($sub.Name -ne "cache") { $script:status.sizeBytes += Get-DirBytes $sub.FullName }
    }
    Set-Progress 1
    $script:status.state = "done"
    $script:status.overall = 1.0
    Save-Status
    $mins = [Math]::Round(((Get-Date) - $started).TotalMinutes, 1)
    Log ("=== FATTO: motore installato in {0} ({1:N1} GB, {2} min) ===" -f $InstallDir, ($script:status.sizeBytes / 1GB), $mins) "Green"
} catch {
    $exitCode = 1
    $code = "unexpected"
    $msg = $_.Exception.Message
    if ($_.Exception -is [InstallError]) { $code = $_.Exception.Code }
    else { Log ("  " + ($_.ScriptStackTrace -replace "`n", " | ")) "DarkGray" }
    # "busy": status.json belongs to the installation that is running - leave it alone.
    if ($code -ne "busy") {
        $script:status.state = "error"
        $script:status.errorCode = $code
        $script:status.error = $msg
        Save-Status
    }
    Log "ERRORE ($code): $msg" "Red"
    Log "Puoi rilanciare l'installazione quando vuoi: riprende da dove si e' fermata." "Yellow"
} finally {
    try { $mutex.ReleaseMutex() } catch { }
    $mutex.Dispose()
}
exit $exitCode
