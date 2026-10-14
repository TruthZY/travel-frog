Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MSYS -ErrorAction SilentlyContinue
Remove-Item Env:SHELL -ErrorAction SilentlyContinue
. C:\Users\dadachu\esp\v5.5\esp-idf\Export.ps1
if (-not $?) { Write-Host "EXPORT FAILED"; exit 2 }
Set-Location $PSScriptRoot
idf.py build
exit $LASTEXITCODE
