Remove-Item Env:MSYSTEM, Env:MSYS, Env:SHELL -ErrorAction SilentlyContinue

# The IDF export script lives wherever the installer put it, which differs per machine, so
# find it rather than baking in one user's path. Set TF_IDF_PROFILE to override.
$idfProfile = $env:TF_IDF_PROFILE
if (-not $idfProfile) {
    $eim = @(Get-ChildItem 'C:\Espressif\tools\Microsoft.v*.PowerShell_profile.ps1' -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending)
    if ($eim.Count -gt 0) { $idfProfile = $eim[0].FullName }
}
if (-not $idfProfile -and $env:IDF_PATH) { $idfProfile = Join-Path $env:IDF_PATH 'export.ps1' }
if (-not $idfProfile -or -not (Test-Path $idfProfile)) {
    Write-Host "no ESP-IDF profile found; set TF_IDF_PROFILE to its export.ps1"
    exit 2
}

. $idfProfile
if (-not $?) { Write-Host "EXPORT FAILED ($idfProfile)"; exit 2 }
Set-Location $PSScriptRoot
idf.py build
exit $LASTEXITCODE
