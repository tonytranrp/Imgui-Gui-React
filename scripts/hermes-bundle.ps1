param()

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$npmRoot = Join-Path $repoRoot "npm"

Push-Location $npmRoot
cmd /c "npm run build"
if ($LASTEXITCODE -ne 0) {
  Pop-Location
  exit $LASTEXITCODE
}

cmd /c "npm run hermes:bundle"
$exitCode = $LASTEXITCODE
Pop-Location

exit $exitCode
