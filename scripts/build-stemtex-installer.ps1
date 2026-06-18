param(
  [string]$TeXLiveRoot,
  [string]$SourceRoot,
  [string]$StageRoot,
  [string]$OutputDir,
  [string]$AppVersion,
  [string]$IsccPath,
  [switch]$SkipRuntimeBuild,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $AppVersion) {
  $versionFile = Join-Path $repoRoot "VERSION"
  if (Test-Path -LiteralPath $versionFile) {
    $AppVersion = (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim()
  } else {
    throw "VERSION not found: $versionFile"
  }
}
if (-not $AppVersion) {
  throw "VERSION is empty."
}
if (-not $SourceRoot) {
  $repoParent = Split-Path -Parent $repoRoot
  if ((Test-Path (Join-Path $repoParent "ptx\texk\web2c\xetex.dll")) -and
      (Test-Path (Join-Path $repoParent "ktx\texk\calldll\xetexdaemon.exe"))) {
    $SourceRoot = $repoParent
  } else {
    $SourceRoot = $repoRoot
  }
}
if (-not $StageRoot) {
  $StageRoot = Join-Path $repoRoot "dist\stemtex-installer\StemTeX"
}
if (-not $OutputDir) {
  $OutputDir = Join-Path $repoRoot "dist\installer"
}

function Resolve-Iscc {
  param([string]$Path)

  if ($Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
      throw "ISCC.exe not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
  }

  $cmd = Get-Command iscc.exe -ErrorAction SilentlyContinue
  if ($cmd) {
    return $cmd.Source
  }

  $registryRoots = @(
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
    "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*"
  )
  foreach ($registryRoot in $registryRoots) {
    $matches = Get-ItemProperty $registryRoot -ErrorAction SilentlyContinue |
      Where-Object { $_.DisplayName -like "Inno Setup*" -and $_.InstallLocation }
    foreach ($match in $matches) {
      $candidate = Join-Path $match.InstallLocation "ISCC.exe"
      if (Test-Path -LiteralPath $candidate) {
        return (Resolve-Path -LiteralPath $candidate).Path
      }
    }
  }

  $candidates = @(
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe",
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe")
  )
  foreach ($candidate in $candidates) {
    if ($candidate -and (Test-Path -LiteralPath $candidate)) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  throw "Could not find ISCC.exe. Pass -IsccPath."
}

function Remove-CachePayload {
  param([string]$Root)

  $fontCache = Join-Path $Root "texmf-var\fonts\cache"
  if (Test-Path -LiteralPath $fontCache) {
    Get-ChildItem -LiteralPath $fontCache -Force -ErrorAction SilentlyContinue |
      Remove-Item -Recurse -Force
  } else {
    New-Item -ItemType Directory -Force -Path $fontCache | Out-Null
  }

  $warmupOutput = Join-Path $Root "texmf-var\cache-warmup"
  if (Test-Path -LiteralPath $warmupOutput) {
    Remove-Item -LiteralPath $warmupOutput -Recurse -Force
  }
}

function Assert-NoFontCachePayload {
  param([string]$Root)

  $cacheDir = Join-Path $Root "texmf-var\fonts\cache"
  $cacheFiles = @()
  if (Test-Path -LiteralPath $cacheDir) {
    $cacheFiles = @(Get-ChildItem -LiteralPath $cacheDir -Recurse -File -Force -ErrorAction SilentlyContinue)
  }
  if ($cacheFiles.Count -ne 0) {
    throw "Staging tree still contains font cache files under $cacheDir."
  }
}

$stageRootFull = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($StageRoot)
$outputDirFull = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
New-Item -ItemType Directory -Force -Path $outputDirFull | Out-Null

if (-not $SkipRuntimeBuild) {
  $runtimeScript = Join-Path $PSScriptRoot "build-stemtex-runtime.ps1"
  $runtimeArgs = @{
    Destination = $stageRootFull
    SourceRoot = $SourceRoot
    SkipFontCacheWarmup = $true
  }
  if ($TeXLiveRoot) {
    $runtimeArgs.TeXLiveRoot = $TeXLiveRoot
  }
  if ($Clean) {
    $runtimeArgs.Clean = $true
  }

  & $runtimeScript @runtimeArgs
  if ($LASTEXITCODE -ne 0) {
    throw "StemTeX runtime build failed with exit code $LASTEXITCODE."
  }
} elseif (-not (Test-Path -LiteralPath $stageRootFull)) {
  throw "StageRoot not found: $stageRootFull"
}

Remove-CachePayload -Root $stageRootFull
Assert-NoFontCachePayload -Root $stageRootFull

$iscc = Resolve-Iscc -Path $IsccPath
$iss = Join-Path $repoRoot "installer\stemtex.iss"

$isccArgs = @(
  "/DSourceDir=$stageRootFull",
  "/DOutputDir=$outputDirFull",
  "/DAppVersion=$AppVersion",
  $iss
)

& $iscc @isccArgs
if ($LASTEXITCODE -ne 0) {
  throw "Inno Setup compiler failed with exit code $LASTEXITCODE."
}

$installer = Join-Path $outputDirFull "StemTeX-$AppVersion-Setup.exe"
if (-not (Test-Path -LiteralPath $installer)) {
  throw "Installer was not created: $installer"
}

$item = Get-Item -LiteralPath $installer
Write-Host "Built installer: $($item.FullName)"
Write-Host ("Installer bytes: {0}" -f $item.Length)
