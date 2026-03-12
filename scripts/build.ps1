param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Debug",
  [string]$BuildDirName,
  [switch]$EnableGlslShaders
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
$buildDirName = if ([string]::IsNullOrWhiteSpace($BuildDirName)) {
  if ($Configuration -eq "Debug") { "b" } else { "b-release" }
} else {
  $BuildDirName
}
$buildDir = Join-Path $repoRoot $buildDirName
$glslOption = if ($EnableGlslShaders.IsPresent) { "ON" } else { "OFF" }

if (Test-Path $buildDir) {
  foreach ($entry in Get-ChildItem -Path $buildDir -Force | Where-Object { $_.Name -ne "Testing" }) {
    try {
      Remove-Item -LiteralPath $entry.FullName -Recurse -Force -ErrorAction Stop
    } catch {
      Start-Sleep -Milliseconds 150
      try {
        Remove-Item -LiteralPath $entry.FullName -Recurse -Force -ErrorAction Stop
      } catch {
        if ((Test-Path -LiteralPath $entry.FullName) -and
            ($entry.Name -notlike "*.obj.d") -and
            ($entry.Name -notlike "*.obj") -and
            ($entry.Name -ne "CMakeFiles")) {
          throw
        }
      }
    }
  }
}

$cmd = @(
  "call `"$vsDevCmd`" -arch=x64",
  "cmake -S `"$repoRoot`" -B `"$buildDir`" -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=$Configuration -DIGR_ENABLE_HERMES=ON -DIGR_ENABLE_GLSL_SHADERS=$glslOption",
  "cmake --build `"$buildDir`""
) -join " && "

cmd /c $cmd
exit $LASTEXITCODE
