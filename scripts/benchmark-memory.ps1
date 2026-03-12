param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Debug",
  [string]$BuildDirName,
  [int]$WarmupSeconds = 3,
  [int]$Samples = 1,
  [int]$SampleIntervalSeconds = 1,
  [string]$Dx11Args = "",
  [string]$Dx12Args = "",
  [string]$MenuArgs = ""
)

$ErrorActionPreference = "Stop"

$buildDirName = if (-not [string]::IsNullOrWhiteSpace($BuildDirName)) {
  $BuildDirName
} elseif ($Configuration -eq "Debug") {
  "b"
} else {
  "b-release"
}
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repoRoot $buildDirName

if (-not [string]::IsNullOrWhiteSpace($BuildDirName)) {
  foreach ($binary in @("igr_win32_dx11_sample.exe", "igr_win32_dx12_sample.exe", "igr_react_menu_harness.exe")) {
    $path = Join-Path $buildDir $binary
    if (-not (Test-Path $path)) {
      throw "Expected benchmark binary was not found: $path"
    }
  }
} elseif (-not (Test-Path (Join-Path $buildDir "igr_win32_dx11_sample.exe"))) {
  $fallbackBuildDirName = if ($Configuration -eq "Debug") { "b-test" } else { "b-test-release" }
  $fallbackBuildDir = Join-Path $repoRoot $fallbackBuildDirName
  if (Test-Path (Join-Path $fallbackBuildDir "igr_win32_dx11_sample.exe")) {
    $buildDirName = $fallbackBuildDirName
    $buildDir = $fallbackBuildDir
  } else {
    throw "Expected benchmark binaries were not found in $buildDir or $fallbackBuildDir"
  }
}

function Measure-App {
  param(
    [string]$Name,
    [string]$Path,
    [string]$ArgumentLine,
    [int]$DelaySeconds,
    [int]$SampleCount,
    [int]$SampleInterval
  )

  $startProcessArgs = @{
    FilePath = $Path
    PassThru = $true
  }
  if (-not [string]::IsNullOrWhiteSpace($ArgumentLine)) {
    $startProcessArgs.ArgumentList = $ArgumentLine
  }
  $process = Start-Process @startProcessArgs
  Start-Sleep -Seconds $DelaySeconds

  $samples = @()
  for ($index = 0; $index -lt $SampleCount; ++$index) {
    $sample = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
    if ($null -eq $sample) {
      break
    }
    $samples += $sample
    if ($index + 1 -lt $SampleCount) {
      Start-Sleep -Seconds $SampleInterval
    }
  }

  if ($samples.Count -eq 0) {
    return [pscustomobject]@{
      Name = $Name
      Status = "exited-before-measurement"
    }
  }

  $gpuCounters = @()
  $instancePrefix = "pid_$($process.Id)_"
  try {
    $gpuCounters =
      Get-Counter '\GPU Process Memory(*)\*' |
      ForEach-Object CounterSamples |
      Where-Object { $_.InstanceName -like "$instancePrefix*" }
  } catch {
    $gpuCounters = @()
  }

  Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue

  $avgPrivate = ($samples | Measure-Object -Property PrivateMemorySize64 -Average).Average
  $peakPrivate = ($samples | Measure-Object -Property PrivateMemorySize64 -Maximum).Maximum
  $avgWorkingSet = ($samples | Measure-Object -Property WorkingSet64 -Average).Average
  $peakWorkingSet = ($samples | Measure-Object -Property WorkingSet64 -Maximum).Maximum

  return [pscustomobject]@{
    Name = $Name
    Status = "ok"
    Pid = $process.Id
    Samples = $samples.Count
    AvgPrivateMB = [math]::Round($avgPrivate / 1MB, 1)
    PeakPrivateMB = [math]::Round($peakPrivate / 1MB, 1)
    AvgWorkingSetMB = [math]::Round($avgWorkingSet / 1MB, 1)
    PeakWorkingSetMB = [math]::Round($peakWorkingSet / 1MB, 1)
    GpuDedicatedMB = [math]::Round((($gpuCounters | Where-Object Path -like '*dedicated usage' | Measure-Object CookedValue -Sum).Sum) / 1MB, 1)
    GpuSharedMB = [math]::Round((($gpuCounters | Where-Object Path -like '*shared usage' | Measure-Object CookedValue -Sum).Sum) / 1MB, 1)
    GpuCommittedMB = [math]::Round((($gpuCounters | Where-Object Path -like '*total committed' | Measure-Object CookedValue -Sum).Sum) / 1MB, 1)
  }
}

$results = @(
  Measure-App -Name "DX11 sample" -Path (Join-Path $buildDir "igr_win32_dx11_sample.exe") -ArgumentLine $Dx11Args -DelaySeconds $WarmupSeconds -SampleCount $Samples -SampleInterval $SampleIntervalSeconds
  Measure-App -Name "DX12 sample" -Path (Join-Path $buildDir "igr_win32_dx12_sample.exe") -ArgumentLine $Dx12Args -DelaySeconds $WarmupSeconds -SampleCount $Samples -SampleInterval $SampleIntervalSeconds
  Measure-App -Name "React menu harness" -Path (Join-Path $buildDir "igr_react_menu_harness.exe") -ArgumentLine $MenuArgs -DelaySeconds ([math]::Max($WarmupSeconds, 2)) -SampleCount $Samples -SampleInterval $SampleIntervalSeconds
)

$results | Format-Table -AutoSize