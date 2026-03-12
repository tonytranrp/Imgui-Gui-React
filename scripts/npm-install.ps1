$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location (Join-Path $repoRoot "npm")
try {
  cmd /c npm install
  exit $LASTEXITCODE
} finally {
  Pop-Location
}

