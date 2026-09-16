# package_plugin.ps1 - stage + zip the GameBaiters TTS .ts3_plugin package.
#
# ONE packaging implementation for both the local build (build_local.ps1) and the
# release pipeline (.github/workflows/release.yml), so what CI publishes is exactly
# what is tested locally.
#
#   tools\package_plugin.ps1 -Dll build\release\gb_tts_win64.dll -Version 1.5.0 -OutFile dist\gb_tts_1.5.0_win64.ts3_plugin
#
# Package layout (TeamSpeak extracts everything under plugins\ into its plugin folder):
#   package.ini
#   plugins\gb_tts_win64.dll
#   plugins\gb_tts\gbtts_16.png
#   plugins\gb_tts\THIRD_PARTY_NOTICES.md
#   plugins\gb_tts\backend\{gbtts\*, tools\download_models.py, requirements.txt, install_backend.ps1}
#
# The archive is verified after writing (required entries, forward slashes, no
# __pycache__). Prints the package path on success.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [string]$StageDir = "",
    [string]$Root = ""
)

$ErrorActionPreference = "Stop"
function Resolve-Full([string]$p) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($p) }

if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }
$Root = Resolve-Full $Root
$Dll = Resolve-Full $Dll
$OutFile = Resolve-Full $OutFile
$StageDir = if ($StageDir) { Resolve-Full $StageDir } else { Join-Path ([IO.Path]::GetTempPath()) ("gbtts_stage_" + [guid]::NewGuid().ToString('N')) }
if (-not (Test-Path $Dll)) { throw "plugin DLL not found: $Dll" }

# ---- stage ---------------------------------------------------------------------------
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
$plugins = Join-Path $StageDir "plugins"
$assets = Join-Path $plugins "gb_tts"
$backendStage = Join-Path $assets "backend"
New-Item -ItemType Directory -Force -Path (Join-Path $backendStage "tools") | Out-Null

Copy-Item $Dll $plugins
$utf8 = New-Object System.Text.UTF8Encoding $false   # no BOM, in Windows PowerShell 5.1 too
$ini = (Get-Content (Join-Path $Root "plugin\package.ini.in") -Raw).Replace("@version@", $Version)
[IO.File]::WriteAllText((Join-Path $StageDir "package.ini"), $ini, $utf8)

$backendSrc = Join-Path $Root "backend"
Copy-Item -Recurse (Join-Path $backendSrc "gbtts") $backendStage
Copy-Item (Join-Path $backendSrc "tools\download_models.py") (Join-Path $backendStage "tools")
Copy-Item (Join-Path $backendSrc "requirements.txt") $backendStage
Copy-Item (Join-Path $Root "installer\install_backend.ps1") $backendStage
Copy-Item (Join-Path $Root "THIRD_PARTY_NOTICES.md") $assets
Get-ChildItem -Path $backendStage -Recurse -Directory -Filter "__pycache__" | Remove-Item -Recurse -Force
Get-ChildItem -Path $backendStage -Recurse -File -Include "*.pyc" | Remove-Item -Force

# 16 px menu icon (TeamSpeak loads plugin menu icons from plugins\<name>\).
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 16, 16
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::Transparent)
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 34, 37, 43))), 0, 0, 16, 16)
$g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 74, 144, 226))), 2, 3, 9, 7)
$g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 74, 144, 226))),
    [System.Drawing.Point[]]@((New-Object System.Drawing.Point 4, 9), (New-Object System.Drawing.Point 3, 13), (New-Object System.Drawing.Point 7, 9)))
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 87, 196, 94)), 1.4
$g.DrawArc($pen, 8, 4, 5, 7, -60, 120)
$pen2 = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 230, 168, 60)), 1.4
$g.DrawArc($pen2, 8, 2, 7, 11, -60, 120)
$g.Dispose()
$bmp.Save((Join-Path $assets "gbtts_16.png"), [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

# ---- zip -------------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutFile) | Out-Null
if (Test-Path $OutFile) { Remove-Item -Force $OutFile }
$sevenzip = Join-Path $env:ProgramFiles "7-Zip\7z.exe"
if (Test-Path $sevenzip) {
    Push-Location $StageDir
    try {
        & $sevenzip a $OutFile -tzip -mx=9 "*" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip failed ($LASTEXITCODE)" }
    } finally { Pop-Location }
} else {
    # Explicit entry names: Compress-Archive in Windows PowerShell 5.1 can write
    # backslash separators, which are not valid zip paths.
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($OutFile, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem $StageDir -Recurse -File) {
            $rel = $f.FullName.Substring($StageDir.Length).TrimStart('\', '/').Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
}

# ---- verify ------------------------------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($OutFile)
try { $entries = @($archive.Entries | ForEach-Object { $_.FullName }) } finally { $archive.Dispose() }
$required = @(
    "package.ini",
    "plugins/gb_tts_win64.dll",
    "plugins/gb_tts/gbtts_16.png",
    "plugins/gb_tts/THIRD_PARTY_NOTICES.md",
    "plugins/gb_tts/backend/gbtts/__init__.py",
    "plugins/gb_tts/backend/gbtts/server.py",
    "plugins/gb_tts/backend/gbtts/vc/third_party/NOTICE.md",
    "plugins/gb_tts/backend/gbtts/stock_voices/stock_voices.json",
    "plugins/gb_tts/backend/gbtts/stock_voices/giulia/ref.wav",
    "plugins/gb_tts/backend/tools/download_models.py",
    "plugins/gb_tts/backend/requirements.txt",
    "plugins/gb_tts/backend/install_backend.ps1"
)
$missing = @($required | Where-Object { $entries -notcontains $_ })
if ($missing.Count) { throw "package is missing: $($missing -join ', ')" }
$bad = @($entries | Where-Object { $_.Contains('\') -or $_ -match '__pycache__|\.pyc$' })
if ($bad.Count) { throw "package has invalid entries: $($bad -join ', ')" }
$packagedVersion = (Select-String -InputObject ((Get-Content (Join-Path $backendStage "gbtts\__init__.py")) -join "`n") -Pattern '__version__ = "([^"]+)"').Matches[0].Groups[1].Value
Write-Host ("[package] {0}: {1} entries, {2:N1} MB, backend {3}" -f (Split-Path -Leaf $OutFile), $entries.Count, ((Get-Item $OutFile).Length / 1MB), $packagedVersion) -ForegroundColor Green
if (-not $PSBoundParameters.ContainsKey('StageDir')) { Remove-Item -Recurse -Force $StageDir }
Write-Output $OutFile
