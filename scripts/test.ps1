param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDirName = if ($Configuration -eq "Debug") { "b" } else { "b-release" }
$buildDir = Join-Path $repoRoot $buildDirName
$npmRoot = Join-Path $repoRoot "npm"

Push-Location $npmRoot
cmd /c "npm run export:fixtures"
$npmExitCode = $LASTEXITCODE
if ($npmExitCode -eq 0) {
  cmd /c "npm run hermes:bundle"
  $npmExitCode = $LASTEXITCODE
}
Pop-Location

if ($npmExitCode -ne 0) {
  exit $npmExitCode
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInfo = & $vswhere -latest -products * -format json | ConvertFrom-Json | Select-Object -First 1
$vsDevCmd = Join-Path $vsInfo.installationPath "Common7\Tools\VsDevCmd.bat"

$cmd = @(
  "call `"$vsDevCmd`" -arch=x64",
  "ctest --test-dir `"$buildDir`" --output-on-failure -C $Configuration"
) -join " && "

cmd /c $cmd
exit $LASTEXITCODE
