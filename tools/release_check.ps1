# release_check.ps1 - refuse a release whose version numbers disagree.
#
#   tools\release_check.ps1 -Version 1.5.0
#
# Checked (all must say the same version):
#   - the tag / requested version is X.Y.Z with parts 0..99 (the updater's build
#     number is major*10000 + minor*100 + patch);
#   - plugin/CMakeLists.txt  GBTTS_VERSION  (compiled into the DLL: what the updater compares);
#   - backend/gbtts/__init__.py  __version__  (the plugin refreshes <home>\app when it differs);
#   - release-notes.txt starts with "GameBaiters TTS vX.Y.Z" + an ===== underline
#     (the release body and the notes the update dialog shows).
# A DLL built with an older GBTTS_VERSION than the published feed would offer the
# same update forever - this is the check that makes that impossible.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Version)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$errors = New-Object System.Collections.Generic.List[string]

if ($Version -notmatch '^\d{1,2}\.\d{1,2}\.\d{1,2}$') {
    $errors.Add("version '$Version' is not X.Y.Z with parts 0..99")
}

$cmake = Select-String -Path (Join-Path $root "plugin\CMakeLists.txt") -Pattern 'set\(GBTTS_VERSION "([^"]+)"\)'
$cmakeVersion = if ($cmake) { $cmake.Matches[0].Groups[1].Value } else { "" }
if ($cmakeVersion -ne $Version) { $errors.Add("plugin/CMakeLists.txt GBTTS_VERSION is '$cmakeVersion', expected '$Version'") }

$py = Select-String -Path (Join-Path $root "backend\gbtts\__init__.py") -Pattern '^__version__ = "([^"]+)"'
$pyVersion = if ($py) { $py.Matches[0].Groups[1].Value } else { "" }
if ($pyVersion -ne $Version) { $errors.Add("backend/gbtts/__init__.py __version__ is '$pyVersion', expected '$Version'") }

$notesPath = Join-Path $root "release-notes.txt"
if (-not (Test-Path $notesPath)) {
    $errors.Add("release-notes.txt is missing")
} else {
    $lines = @(Get-Content $notesPath -Encoding UTF8 | Where-Object { $_.Trim() -ne "" } | Select-Object -First 2)
    $expected = "GameBaiters TTS v$Version"
    if ($lines.Count -lt 2 -or $lines[0].Trim() -ne $expected -or $lines[1].Trim() -notmatch '^=+$') {
        $first = if ($lines.Count) { $lines[0] } else { "(empty)" }
        $errors.Add("release-notes.txt must start with '$expected' and an ===== line (found '$first')")
    }
}

if ($errors.Count) {
    foreach ($e in $errors) { Write-Host "RELEASE CHECK: $e" -ForegroundColor Red }
    exit 1
}
Write-Host "[release-check] $Version consistent: CMake, backend, release notes" -ForegroundColor Green
exit 0
