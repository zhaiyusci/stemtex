param(
  [string]$OutDir,
  [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) {
  $OutDir = Join-Path $root "dist\cpp-daemon"
}

function Resolve-VsDevCmd {
  $vswhereCandidates = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
  )
  foreach ($candidate in $vswhereCandidates) {
    if ($candidate -and (Test-Path -LiteralPath $candidate)) {
      $installPath = & $candidate -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      if ($installPath) {
        $devCmd = Join-Path $installPath "Common7\Tools\VsDevCmd.bat"
        if (Test-Path -LiteralPath $devCmd) {
          return $devCmd
        }
      }
    }
  }
  throw "Could not find VsDevCmd.bat. Install MSVC C++ tools or run from a Developer PowerShell."
}

function Resolve-CMake {
  $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
  if ($cmd) {
    return $cmd.Source
  }
  $candidates = @(
    "${env:ProgramFiles}\CMake\bin\cmake.exe",
    "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
  )
  foreach ($candidate in $candidates) {
    if ($candidate -and (Test-Path -LiteralPath $candidate)) {
      return $candidate
    }
  }
  return $null
}

$cmake = Resolve-CMake
$buildDir = Join-Path $OutDir "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$sourceDir = $PSScriptRoot
$dll = Join-Path $buildDir "stemtex-renderer.dll"
$smokeExe = Join-Path $buildDir "stemtex-renderer-smoke.exe"

if ($cmake) {
  & $cmake -S $sourceDir -B $buildDir -G "Visual Studio 17 2022" -A x64
  if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
  }
  & $cmake --build $buildDir --config $Configuration
  if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE."
  }
} else {
  $devCmd = Resolve-VsDevCmd
  $src = Join-Path $sourceDir "stemtex_renderer.cpp"
  $smoke = Join-Path $sourceDir "stemtex_renderer_smoke.cpp"
  $cmd = @"
call "$devCmd" -arch=x64 -host_arch=x64
cl.exe /nologo /EHsc /std:c++17 /O2 /LD /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DSTEMTEX_RENDERER_EXPORTS /Fe:"$dll" "$src"
cl.exe /nologo /EHsc /std:c++17 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fe:"$smokeExe" "$smoke" "$buildDir\stemtex-renderer.lib"
"@
  $bat = Join-Path $buildDir "build-stemtex-renderer.bat"
  Set-Content -LiteralPath $bat -Value $cmd -Encoding ascii
  cmd.exe /d /s /c "`"$bat`""
  if ($LASTEXITCODE -ne 0) {
    throw "C++ daemon build failed with exit code $LASTEXITCODE."
  }
}

$actualDll = if (Test-Path -LiteralPath $dll) {
  $dll
} else {
  Join-Path $buildDir "$Configuration\stemtex-renderer.dll"
}
$actualSmokeExe = if (Test-Path -LiteralPath $smokeExe) {
  $smokeExe
} else {
  Join-Path $buildDir "$Configuration\stemtex-renderer-smoke.exe"
}

if (-not (Test-Path -LiteralPath $actualDll)) {
  throw "Expected output missing: $actualDll"
}
Write-Host "Built: $actualDll"
if (Test-Path -LiteralPath $actualSmokeExe) {
  Write-Host "Built: $actualSmokeExe"
}
