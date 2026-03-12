param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path $vswhere)) {
  throw "vswhere.exe was not found. Install Visual Studio or Build Tools."
}

$vsInfo = & $vswhere -latest -products * -format json | ConvertFrom-Json | Select-Object -First 1
if ($null -eq $vsInfo) {
  throw "No Visual Studio instance with C++ tools was found."
}

$vsDevCmd = Join-Path $vsInfo.installationPath "Common7\Tools\VsDevCmd.bat"
$buildDirName = if ($Configuration -eq "Debug") { "b" } else { "b-release" }
$buildDir = Join-Path $repoRoot $buildDirName

if (Test-Path $buildDir) {
  Get-ChildItem -Path $buildDir -Force |
    Where-Object { $_.Name -ne "Testing" } |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

$cmd = @(
  "call `"$vsDevCmd`" -arch=x64",
  "cmake -S `"$repoRoot`" -B `"$buildDir`" -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=$Configuration -DIGR_ENABLE_HERMES=ON",
  "cmake --build `"$buildDir`""
) -join " && "

cmd /c $cmd
exit $LASTEXITCODE
