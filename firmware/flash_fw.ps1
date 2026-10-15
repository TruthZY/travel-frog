param([string]$Port = "")
Remove-Item Env:MSYSTEM, Env:MSYS, Env:SHELL -ErrorAction SilentlyContinue

# Same discovery as build_fw.ps1: the installer's path is machine-specific.
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
if ($Port -eq "") { idf.py flash } else { idf.py -p $Port flash }
exit $LASTEXITCODE
