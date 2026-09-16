# build_local.ps1 - LOCAL Windows build of the GameBaiters TTS TeamSpeak plugin.
#
#   .\build_local.ps1 -Local                 # configure + build + package
#   .\build_local.ps1 -Local -Clean          # wipe the CMake build dir first
#   .\build_local.ps1 -Local -Install        # also copy into %APPDATA%\TS3Client\plugins (TS3 must be closed)
#   .\build_local.ps1 -Local -ConfigureOnly
#
# Output: dist\gb_tts_<version>_win64.ts3_plugin
#   package.ini
#   plugins\gb_tts_win64.dll
#   plugins\gb_tts\gbtts_16.png, THIRD_PARTY_NOTICES.md
#   plugins\gb_tts\backend\{gbtts\*, tools\download_models.py, requirements.txt, install_backend.ps1}
# (staging + zip + verification: tools\package_plugin.ps1, shared with CI)
#
# Never pushes, never publishes. Releases: tag vX.Y.Z (see README.md, "Releasing").
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][switch]$Local,
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$Install,
    [switch]$Tests,        # also build the self-tests and the host harness (build\tests)
    [string]$QtDir = "D:\QT\Qt64\5.15.2\msvc2019_64",
    [string]$VsDir = "",   # empty = the newest Visual Studio 2022 / Build Tools found by vswhere
    [string]$SoundboardRoot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pluginSrc = Join-Path $root "plugin"
$buildDir = Join-Path $root "build"
$distDir = Join-Path $root "dist"
if (-not $VsDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $VsDir = & $vswhere -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    }
    if (-not $VsDir) { Write-Host "ERROR: Visual Studio 2022 / Build Tools with the C++ x64 tools not found (-VsDir)" -ForegroundColor Red; exit 2 }
}
$vcvars = Join-Path $VsDir "VC\Auxiliary\Build\vcvars64.bat"
if (-not $SoundboardRoot) { $SoundboardRoot = Join-Path (Split-Path -Parent $root) "SOUNDBOARD_4.0" }

$version = (Select-String -Path (Join-Path $pluginSrc "CMakeLists.txt") -Pattern 'GBTTS_VERSION "([\d\.]+)"').Matches[0].Groups[1].Value

Write-Host "[build] root:       $root"
Write-Host "[build] version:    $version"
Write-Host "[build] Qt:         $QtDir"
Write-Host "[build] Soundboard: $SoundboardRoot"

foreach ($p in @("$QtDir\lib\cmake\Qt5", $vcvars, "$SoundboardRoot\upstream-clone\src\dsp\SlotDsp.cpp")) {
    if (-not (Test-Path $p)) { Write-Host "ERROR: missing $p" -ForegroundColor Red; exit 2 }
}

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$cmakeArgs = "-S `"$pluginSrc`" -B `"$buildDir`" -G `"Visual Studio 17 2022`" -A x64 " +
             "-DCMAKE_PREFIX_PATH=`"$QtDir`" -DSOUNDBOARD_ROOT=`"$SoundboardRoot`"" +
             $(if ($Tests) { " -DGBTTS_BUILD_TESTS=ON" } else { "" })
# NOTE: "|| exit /b %errorlevel%" is wrong in a batch file - %errorlevel% is
# expanded when the line is parsed (0), so a failed build would exit 0.
$buildCmd = if ($ConfigureOnly) { "" } else { "cmake --build `"$buildDir`" --config Release --parallel`r`nif errorlevel 1 exit /b 1" }
$wrapper = Join-Path $env:TEMP ("gbtts_build_" + [guid]::NewGuid().ToString('N') + ".bat")
@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cmake $cmakeArgs
if errorlevel 1 exit /b 1
$buildCmd
exit /b 0
"@ | Set-Content -Path $wrapper -Encoding ASCII
try {
    # stderr merged inside cmd: a CMake warning must not become a terminating PowerShell error.
    & cmd /c "`"$wrapper`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Write-Host "[build] FAILED ($LASTEXITCODE)" -ForegroundColor Red; exit $LASTEXITCODE }
} finally {
    Remove-Item -Force $wrapper -ErrorAction SilentlyContinue
}
if ($ConfigureOnly) { Write-Host "[build] configure OK" -ForegroundColor Green; exit 0 }

$dll = Join-Path $buildDir "release\gb_tts_win64.dll"
if (-not (Test-Path $dll)) { Write-Host "ERROR: $dll not produced" -ForegroundColor Red; exit 3 }
Write-Host ("[build] DLL: {0:N1} MB" -f ((Get-Item $dll).Length / 1MB)) -ForegroundColor Green

# ---- stage + package (same script as the GitHub release pipeline) -------------------
$stage = Join-Path $buildDir "stage"
$assets = Join-Path $stage "plugins\gb_tts"
$out = Join-Path $distDir "gb_tts_${version}_win64.ts3_plugin"
& (Join-Path $root "tools\package_plugin.ps1") -Dll $dll -Version $version -StageDir $stage -OutFile $out | Out-Null
Write-Host ("[build] PACKAGE: {0} ({1:N1} MB)" -f $out, ((Get-Item $out).Length / 1MB)) -ForegroundColor Green

# ---- optional local install -----------------------------------------------------------
if ($Install) {
    if (Get-Process -Name "ts3client_win64" -ErrorAction SilentlyContinue) {
        Write-Host "[build] TeamSpeak is running: close it, then run with -Install again." -ForegroundColor Yellow
        exit 4
    }
    $plugins = Join-Path $env:APPDATA "TS3Client\plugins"
    Copy-Item -Force $dll $plugins
    $destAssets = Join-Path $plugins "gb_tts"
    New-Item -ItemType Directory -Force -Path $destAssets | Out-Null
    Copy-Item -Recurse -Force "$assets\*" $destAssets
    Write-Host "[build] installed into $plugins" -ForegroundColor Green
}
