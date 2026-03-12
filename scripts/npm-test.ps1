$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location (Join-Path $repoRoot "npm")
try {
  cmd /c npm test
  exit $LASTEXITCODE
} finally {
  Pop-Location
}

