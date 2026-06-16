param(
  [string]$Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) "dist\stemtex-tlmgr"),
  [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot),
  [string]$InstallTl,
  [string]$Repository = "https://mirror.ctan.org/systems/texlive/tlnet",
  [string]$Scheme = "scheme-infraonly",
  [string[]]$Packages = @(
    "latex-bin",
    "latex",
    "amsmath",
    "fontspec",
    "unicode-math",
    "xecjk",
    "lm",
    "xits",
    "mhchem",
    "physics",
    "xcolor",
    "graphics",
    "tools"
  ),
  [string]$CacheWarmupTex,
  [switch]$SkipPackageInstall,
  [switch]$SkipFontCacheWarmup,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
  param([string]$Path, [string]$Name)
  if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
    throw "$Name not found: $Path"
  }
  return (Resolve-Path -LiteralPath $Path).Path
}

function Find-InstallTl {
  if ($InstallTl) {
    return Resolve-RepoPath -Path $InstallTl -Name "InstallTl"
  }

  $cmd = Get-Command install-tl-windows.bat -ErrorAction SilentlyContinue
  if ($cmd) {
    return $cmd.Source
  }

  $candidates = @(
    (Join-Path $env:TEMP "install-tl-windows.bat"),
    (Join-Path (Split-Path -Parent $PSScriptRoot) "install-tl-windows.bat")
  ) | Where-Object { $_ }

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  throw "Could not find install-tl-windows.bat. Pass -InstallTl or put it on PATH."
}

function Copy-RequiredFile {
  param([string]$Source, [string]$Destination)
  if (-not (Test-Path -LiteralPath $Source)) {
    throw "Required file not found: $Source"
  }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
  Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Invoke-Native {
  param(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$WorkingDirectory
  )

  Push-Location -LiteralPath $WorkingDirectory
  try {
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
      throw "'$FilePath $($Arguments -join ' ')' failed with exit code $LASTEXITCODE."
    }
  } finally {
    Pop-Location
  }
}

function Write-InstallProfile {
  param(
    [string]$ProfilePath,
    [string]$TexLiveRoot,
    [string]$Scheme
  )

  $root = $TexLiveRoot.Replace("\", "/")
  $content = @"
selected_scheme $Scheme
TEXDIR $root
TEXMFCONFIG $root/texmf-config
TEXMFHOME $root/texmf-home
TEXMFLOCAL $root/texmf-local
TEXMFSYSCONFIG $root/texmf-config
TEXMFSYSVAR $root/texmf-var
TEXMFVAR $root/texmf-var
binary_win32 1
instopt_adjustpath 0
instopt_adjustrepo 1
instopt_letter 0
instopt_portable 1
instopt_write18_restricted 1
tlpdbopt_autobackup 1
tlpdbopt_backupdir tlpkg/backups
tlpdbopt_create_formats 1
tlpdbopt_desktop_integration 0
tlpdbopt_file_assocs 0
tlpdbopt_generate_updmap 0
tlpdbopt_install_docfiles 0
tlpdbopt_install_srcfiles 0
tlpdbopt_post_code 1
tlpdbopt_sys_bin ~
tlpdbopt_sys_info ~
tlpdbopt_sys_man ~
"@
  Set-Content -LiteralPath $ProfilePath -Value $content -Encoding ascii
}

function Write-RendererTexmfCnfOverlay {
  param([string]$TexLiveRoot)

  $cnf = Join-Path $TexLiveRoot "texmf-dist\web2c\texmf.cnf"
  if (-not (Test-Path -LiteralPath $cnf)) {
    throw "texmf.cnf not found: $cnf"
  }

  $original = Get-Content -LiteralPath $cnf -Raw
  $overlay = @"
% XeTeX service renderer overrides.
% Keep this block at the top: kpathsea uses the first assignment it sees.
TEXMFROOT = `$SELFAUTOPARENT
TEXMFDIST = `$TEXMFROOT/texmf-dist
TEXMFMAIN = `$TEXMFDIST
TEXMFSYSVAR = `$TEXMFROOT/texmf-var
TEXMFSYSCONFIG = `$TEXMFROOT/texmf-config
TEXMFVAR = `$TEXMFROOT/texmf-var
TEXMFCONFIG = `$TEXMFROOT/texmf-config
TEXMFLOCAL = `$TEXMFROOT/texmf-local
TEXMFHOME = `$TEXMFROOT/texmf-home
OSFONTDIR = C:/Windows/fonts
FC_CACHEDIR = `$TEXMFVAR/fonts/cache
XE_FC_CACHEDIR = `$TEXMFVAR/fonts/cache
FONTCONFIG_PATH = `$TEXMFVAR/fonts/conf
XE_FONTCONFIG_PATH = `$TEXMFVAR/fonts/conf

"@

  if ($original -notmatch "XeTeX service renderer overrides") {
    Set-Content -LiteralPath $cnf -Value ($overlay + $original) -Encoding utf8
  }
}

function Write-Fontconfig {
  param([string]$TexLiveRoot)

  $rootForXml = $TexLiveRoot.Replace("\", "/")
  $confDir = Join-Path $TexLiveRoot "texmf-var\fonts\conf"
  New-Item -ItemType Directory -Force -Path (Join-Path $confDir "conf.d") | Out-Null

  $fontsConf = @"
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>C:/Windows/fonts</dir>
  <dir>$rootForXml/texmf-dist/fonts/opentype</dir>
  <dir>$rootForXml/texmf-dist/fonts/truetype</dir>
  <cachedir>$rootForXml/texmf-var/fonts/cache</cachedir>
  <include ignore_missing="yes">conf.d</include>
  <config>
    <rescan><int>30</int></rescan>
  </config>
</fontconfig>
"@
  Set-Content -LiteralPath (Join-Path $confDir "fonts.conf") -Value $fontsConf -Encoding utf8

  $localConf = @"
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
</fontconfig>
"@
  Set-Content -LiteralPath (Join-Path $confDir "conf.d\51-local.conf") -Value $localConf -Encoding utf8
}

function Write-WarmupTemplate {
  param([string]$Root)

  $warmupDir = Join-Path $Root "cache-warmup"
  New-Item -ItemType Directory -Force -Path $warmupDir | Out-Null
  $content = @'
\documentclass{article}
\usepackage{unicode-math}
\setmainfont{Times New Roman}
\setsansfont{Arial}
\setmonofont{Consolas}
\setmathfont{XITSMath-Regular.otf}[BoldFont=XITSMath-Bold.otf]
\usepackage{xeCJK}
\setCJKmainfont{SimSun}
\setCJKsansfont{SimHei}
\setCJKmonofont{SimSun}
\usepackage[version=4]{mhchem}
\usepackage{physics}
\usepackage{xcolor}

\begin{document}
中文缓存预热，标点测试：，。！？；：“”
$\symbf{\alpha}$ \ce{H2O} \color{blue} $\ip{1}{0}$
\end{document}
'@
  Set-Content -LiteralPath (Join-Path $warmupDir "warmup.tex") -Value $content -Encoding utf8
}

function Write-ServiceScripts {
  param([string]$Root)

  $runXeTeX = @(
    '@echo off',
    'setlocal',
    'set "ROOT=%~dp0"',
    'if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"',
    'set "TLROOT=%ROOT%\texlive"',
    'set "PATCHEDBIN=%ROOT%\patched-bin\windows"',
    'set "TLBIN=%TLROOT%\bin\windows"',
    'set "PATH=%PATCHEDBIN%;%TLBIN%;%SystemRoot%\System32"',
    'set "TEXMFROOT=%TLROOT%"',
    'set "TEXMFCNF=%TLROOT%\texmf-dist\web2c"',
    'set "XE_FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"',
    'set "FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"',
    'set "XE_FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"',
    'set "FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"',
    'if exist "%PATCHEDBIN%\icu-data\icudt76l.dat" set "ICU_DATA=%PATCHEDBIN%\icu-data"',
    '"%PATCHEDBIN%\xetexdaemon.exe" %*'
  )
  Set-Content -LiteralPath (Join-Path $Root "run-xetexdaemon.bat") -Value $runXeTeX -Encoding ascii

  $runXeLaTeX = @(
    '@echo off',
    'setlocal',
    'set "ROOT=%~dp0"',
    'if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"',
    'set "TLROOT=%ROOT%\texlive"',
    'set "PATCHEDBIN=%ROOT%\patched-bin\windows"',
    'set "TLBIN=%TLROOT%\bin\windows"',
    'set "PATH=%PATCHEDBIN%;%TLBIN%;%SystemRoot%\System32"',
    'set "TEXMFROOT=%TLROOT%"',
    'set "TEXMFCNF=%TLROOT%\texmf-dist\web2c"',
    'set "XE_FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"',
    'set "FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"',
    'set "XE_FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"',
    'set "FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"',
    'if exist "%PATCHEDBIN%\icu-data\icudt76l.dat" set "ICU_DATA=%PATCHEDBIN%\icu-data"',
    '"%PATCHEDBIN%\xetexdaemon.exe" -fmt=xelatex --no-font-cache-refresh %*'
  )
  Set-Content -LiteralPath (Join-Path $Root "run-xelatexdaemon.bat") -Value $runXeLaTeX -Encoding ascii

  $tlmgr = @(
    '@echo off',
    'setlocal',
    'set "ROOT=%~dp0"',
    'if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"',
    'set "TLROOT=%ROOT%\texlive"',
    'set "PATH=%TLROOT%\bin\windows;%SystemRoot%\System32"',
    '"%TLROOT%\bin\windows\tlmgr.bat" %*'
  )
  Set-Content -LiteralPath (Join-Path $Root "renderer-tlmgr.bat") -Value $tlmgr -Encoding ascii

  $refresh = @'
param(
  [string]$WarmupTex,
  [string]$OutputDirectory,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$TlRoot = Join-Path $Root "texlive"
$TlBin = Join-Path $TlRoot "bin\windows"
$PatchedBin = Join-Path $Root "patched-bin\windows"
$CacheDir = Join-Path $TlRoot "texmf-var\fonts\cache"
$ConfDir = Join-Path $TlRoot "texmf-var\fonts\conf"

if (-not $WarmupTex) {
  $WarmupTex = Join-Path $Root "cache-warmup\warmup.tex"
}
if (-not (Test-Path -LiteralPath $WarmupTex)) {
  throw "Warmup TeX file not found: $WarmupTex"
}
if (-not $OutputDirectory) {
  $OutputDirectory = Join-Path $TlRoot "texmf-var\cache-warmup"
}

if ($Clean -and (Test-Path -LiteralPath $CacheDir)) {
  Remove-Item -LiteralPath $CacheDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $CacheDir, $OutputDirectory | Out-Null

$saved = @{}
foreach ($name in @("PATH", "TEXMFROOT", "TEXMFCNF", "ICU_DATA", "XE_FONTCONFIG_PATH", "FONTCONFIG_PATH", "XE_FC_CACHEDIR", "FC_CACHEDIR", "FONTCONFIG_NO_CACHE_REFRESH")) {
  $saved[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

try {
  $env:PATH = "$PatchedBin;$TlBin;$env:SystemRoot\System32"
  $env:TEXMFROOT = $TlRoot
  $env:TEXMFCNF = Join-Path $TlRoot "texmf-dist\web2c"
  $icuData = Join-Path $PatchedBin "icu-data"
  if (Test-Path -LiteralPath (Join-Path $icuData "icudt76l.dat")) {
    $env:ICU_DATA = $icuData
  }
  $env:XE_FONTCONFIG_PATH = $ConfDir
  $env:FONTCONFIG_PATH = $ConfDir
  $env:XE_FC_CACHEDIR = $CacheDir
  $env:FC_CACHEDIR = $CacheDir
  Remove-Item Env:FONTCONFIG_NO_CACHE_REFRESH -ErrorAction SilentlyContinue

  & (Join-Path $TlBin "mktexlsr.exe") $TlRoot
  if ($LASTEXITCODE -ne 0) {
    throw "mktexlsr failed with exit code $LASTEXITCODE."
  }

  $resolved = (Resolve-Path -LiteralPath $WarmupTex).Path
  Push-Location -LiteralPath (Split-Path -Parent $resolved)
  try {
    & (Join-Path $PatchedBin "xetexdaemon.exe") -fmt=xelatex -no-pdf -interaction=nonstopmode -halt-on-error -output-directory="$OutputDirectory" (Split-Path -Leaf $resolved)
    if ($LASTEXITCODE -ne 0) {
      throw "Font cache refresh failed with exit code $LASTEXITCODE."
    }
  } finally {
    Pop-Location
  }

  Write-Host "Renderer TeX Live refreshed: $TlRoot"
} finally {
  foreach ($name in $saved.Keys) {
    if ($null -ne $saved[$name]) {
      Set-Item -Path "Env:$name" -Value $saved[$name]
    } else {
      Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
    }
  }
}
'@
  Set-Content -LiteralPath (Join-Path $Root "refresh-renderer.ps1") -Value $refresh -Encoding ascii

  $refreshBat = @(
    '@echo off',
    'setlocal',
    'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0refresh-renderer.ps1" %*'
  )
  Set-Content -LiteralPath (Join-Path $Root "refresh-renderer.bat") -Value $refreshBat -Encoding ascii
}

function Install-PatchedEngine {
  param([string]$RepoRoot, [string]$Root)

  $patchedBin = Join-Path $Root "patched-bin\windows"
  New-Item -ItemType Directory -Force -Path $patchedBin | Out-Null

  $xetexDll = Join-Path $RepoRoot "ptx\texk\web2c\xetex.dll"
  $launcher = Join-Path $RepoRoot "ktx\texk\calldll\xetexdaemon.exe"
  if (-not (Test-Path -LiteralPath $xetexDll) -or -not (Test-Path -LiteralPath $launcher)) {
    throw "Patched XeTeX build not found. Run scripts\build-windows-native.ps1 -Target All first."
  }

  Copy-RequiredFile -Source $xetexDll -Destination (Join-Path $patchedBin "xetexdaemon.dll")
  Copy-RequiredFile -Source $launcher -Destination (Join-Path $patchedBin "xetexdaemon.exe")

  $xelatexServ = @(
    '@echo off',
    'setlocal',
    '"%~dp0xetexdaemon.exe" -fmt=xelatex %*'
  )
  Set-Content -LiteralPath (Join-Path $patchedBin "xelatexdaemon.bat") -Value $xelatexServ -Encoding ascii

  $icuData = Join-Path $RepoRoot "ptx\libs\icu-src\source\data\in\icudt76l.dat"
  if (Test-Path -LiteralPath $icuData) {
    New-Item -ItemType Directory -Force -Path (Join-Path $patchedBin "icu-data") | Out-Null
    Copy-RequiredFile -Source $icuData -Destination (Join-Path $patchedBin "icu-data\icudt76l.dat")
  }
  $icuDll = Join-Path $RepoRoot "ptx\libs\icu-src\bin64\icudt76.dll"
  if (Test-Path -LiteralPath $icuDll) {
    Copy-RequiredFile -Source $icuDll -Destination (Join-Path $patchedBin "icudt76.dll")
  }
}

function Build-DaemonFormat {
  param([string]$Root)

  $tlRoot = Join-Path $Root "texlive"
  $tlBin = Join-Path $tlRoot "bin\windows"
  $patchedBin = Join-Path $Root "patched-bin\windows"
  $fmtDir = Join-Path $tlRoot "texmf-var\web2c\xetex"
  New-Item -ItemType Directory -Force -Path $fmtDir | Out-Null

  $saved = @{}
  foreach ($name in @("PATH", "TEXMFROOT", "TEXMFCNF", "TEXFORMATS", "ICU_DATA", "XE_FONTCONFIG_PATH", "FONTCONFIG_PATH", "XE_FC_CACHEDIR", "FC_CACHEDIR")) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
  }

  try {
    $env:PATH = "$patchedBin;$tlBin;$env:SystemRoot\System32"
    $env:TEXMFROOT = $tlRoot
    $env:TEXMFCNF = Join-Path $tlRoot "texmf-dist\web2c"
    $env:TEXFORMATS = "$fmtDir;$fmtDir\"
    $icuData = Join-Path $patchedBin "icu-data"
    if (Test-Path -LiteralPath (Join-Path $icuData "icudt76l.dat")) {
      $env:ICU_DATA = $icuData
    }
    $env:XE_FONTCONFIG_PATH = Join-Path $tlRoot "texmf-var\fonts\conf"
    $env:FONTCONFIG_PATH = Join-Path $tlRoot "texmf-var\fonts\conf"
    $env:XE_FC_CACHEDIR = Join-Path $tlRoot "texmf-var\fonts\cache"
    $env:FC_CACHEDIR = Join-Path $tlRoot "texmf-var\fonts\cache"

    Push-Location -LiteralPath $fmtDir
    try {
      & (Join-Path $patchedBin "xetexdaemon.exe") -ini -etex -jobname=xelatex xelatex.ini
      if ($LASTEXITCODE -ne 0) {
        throw "Failed to build xelatex.fmt with xetexdaemon."
      }
    } finally {
      Pop-Location
    }
  } finally {
    foreach ($name in $saved.Keys) {
      if ($null -ne $saved[$name]) {
        Set-Item -Path "Env:$name" -Value $saved[$name]
      } else {
        Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
      }
    }
  }
}

$repoRoot = Resolve-RepoPath -Path $SourceRoot -Name "SourceRoot"
$root = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
$tlRoot = Join-Path $root "texlive"
$profile = Join-Path $root "install-tl.profile"

if (-not $CacheWarmupTex) {
  $CacheWarmupTex = Join-Path $repoRoot "test\test_5.tex"
}

if ($Clean -and (Test-Path -LiteralPath $root)) {
  Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $root | Out-Null

$installTlPath = Find-InstallTl
Write-InstallProfile -ProfilePath $profile -TexLiveRoot $tlRoot -Scheme $Scheme

if (-not (Test-Path -LiteralPath (Join-Path $tlRoot "tlpkg\texlive.tlpdb"))) {
  Invoke-Native -FilePath $installTlPath -Arguments @("-profile", $profile, "-repository", $Repository, "-no-interaction") -WorkingDirectory $root
}

$tlmgr = Join-Path $tlRoot "bin\windows\tlmgr.bat"
if (-not (Test-Path -LiteralPath $tlmgr)) {
  throw "tlmgr.bat not found after install: $tlmgr"
}

if (-not $SkipPackageInstall) {
  Invoke-Native -FilePath $tlmgr -Arguments (@("option", "repository", $Repository)) -WorkingDirectory $root
  Invoke-Native -FilePath $tlmgr -Arguments (@("option", "docfiles", "0")) -WorkingDirectory $root
  Invoke-Native -FilePath $tlmgr -Arguments (@("option", "srcfiles", "0")) -WorkingDirectory $root
  Invoke-Native -FilePath $tlmgr -Arguments (@("install") + $Packages) -WorkingDirectory $root
}

Write-RendererTexmfCnfOverlay -TexLiveRoot $tlRoot
Write-Fontconfig -TexLiveRoot $tlRoot
Write-WarmupTemplate -Root $root
Write-ServiceScripts -Root $root
Install-PatchedEngine -RepoRoot $repoRoot -Root $root
Build-DaemonFormat -Root $root

if (-not $SkipFontCacheWarmup) {
  Invoke-Native -FilePath (Join-Path $root "refresh-renderer.bat") -Arguments @("-WarmupTex", $CacheWarmupTex, "-Clean") -WorkingDirectory $root
}

Write-Host "Built tlmgr-maintainable XeTeX service dist: $root"
Write-Host "Use renderer-tlmgr.bat to install/update TeX Live packages."
Write-Host "Use run-xelatexdaemon.bat for rendering with the patched engine."

