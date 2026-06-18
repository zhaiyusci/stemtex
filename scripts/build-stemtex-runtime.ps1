param(
  [string]$TeXLiveRoot,
  [string]$Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) "stemtex"),
  [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot),
  [string]$CacheWarmupTex,
  [switch]$SkipFontCacheWarmup,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $PSScriptRoot
$stemTeXVersionFile = Join-Path $scriptRoot "VERSION"
$stemTeXVersion = if (Test-Path -LiteralPath $stemTeXVersionFile) {
  (Get-Content -LiteralPath $stemTeXVersionFile -TotalCount 1).Trim()
} else {
  "0.2.0"
}

function Resolve-ExistingPath {
  param([string]$Path, [string]$Name)
  if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
    throw "$Name not found: $Path"
  }
  return (Resolve-Path -LiteralPath $Path).Path
}

function Find-TeXLiveRoot {
  if ($TeXLiveRoot) {
    return Resolve-ExistingPath -Path $TeXLiveRoot -Name "TeXLiveRoot"
  }

  if ($env:TEXLIVE_ROOT -and (Test-Path -LiteralPath $env:TEXLIVE_ROOT)) {
    return (Resolve-Path -LiteralPath $env:TEXLIVE_ROOT).Path
  }

  $kpsewhich = Get-Command kpsewhich.exe -ErrorAction SilentlyContinue
  if ($kpsewhich) {
    $root = & $kpsewhich.Source -var-value=TEXMFROOT
    if ($LASTEXITCODE -eq 0 -and $root -and (Test-Path -LiteralPath $root)) {
      return (Resolve-Path -LiteralPath $root).Path
    }
  }

  throw "Could not find TeX Live. Pass -TeXLiveRoot, set TEXLIVE_ROOT, or put kpsewhich.exe on PATH."
}

function Copy-One {
  param(
    [string]$FromRoot,
    [string]$ToRoot,
    [string]$RelativePath
  )

  $src = Join-Path $FromRoot $RelativePath
  if (-not (Test-Path -LiteralPath $src)) {
    throw "Required source file not found: $src"
  }
  $dst = Join-Path $ToRoot $RelativePath
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
  Copy-Item -LiteralPath $src -Destination $dst -Force
}

function Copy-Tree {
  param(
    [string]$FromRoot,
    [string]$ToRoot,
    [string]$RelativePath
  )

  $src = Join-Path $FromRoot $RelativePath
  if (-not (Test-Path -LiteralPath $src)) {
    throw "Required source directory not found: $src"
  }
  $dst = Join-Path $ToRoot $RelativePath
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
  Copy-Item -LiteralPath $src -Destination (Split-Path -Parent $dst) -Recurse -Force
}

function Write-RunScript {
  param([string]$DestRoot)

  $engineLine = '"%TLROOT%\bin\windows\xetexdaemon.exe" -fmt=xelatexdaemon --no-font-cache-refresh %*'

  $content = @(
    '@echo off',
    'setlocal',
    'set "TLROOT=%~dp0"',
    'if "%TLROOT:~-1%"=="\" set "TLROOT=%TLROOT:~0,-1%"',
    'set "PATH=%TLROOT%\bin\windows;%SystemRoot%\System32"',
    'set "TEXMFROOT=%TLROOT%"',
    'set "TEXMFCNF=%TLROOT%\texmf-dist\web2c"',
    'set "TEXFORMATS=%TLROOT%\texmf-var\web2c\xetex;%TLROOT%\texmf-var\web2c\xetex\"',
    'set "XE_FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"',
    'set "FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"',
    'set "XE_FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"',
    'set "FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"',
    'if exist "%TLROOT%\bin\windows\icu-data\icudt*l.dat" set "ICU_DATA=%TLROOT%\bin\windows\icu-data"',
    $engineLine
  )

  Set-Content -LiteralPath (Join-Path $DestRoot "run-xelatexdaemon.bat") -Value $content -Encoding ascii
}

function Write-CacheWarmupTemplate {
  param([string]$DestRoot)

  $warmupDir = Join-Path $DestRoot "cache-warmup"
  New-Item -ItemType Directory -Force -Path $warmupDir | Out-Null

  $repoWarmup = Join-Path $SourceRoot "cache-warmup\warmup.tex"
  if (Test-Path -LiteralPath $repoWarmup) {
    Copy-Item -LiteralPath $repoWarmup -Destination (Join-Path $warmupDir "warmup.tex") -Force
    return
  }

  $content = @'
\documentclass{article}
\usepackage{mathtools}
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
\usepackage{cancel}
\usepackage[active,tightpage]{preview}
\PreviewBorder=1pt

\begin{document}
\begin{preview}
中文缓存预热，标点测试：，。！？；：“”

English warmup: Times New Roman, \textbf{bold}, \textit{italic},
\textbf{\textit{bold italic}}, \textsf{Arial sans}, \texttt{Consolas mono}.

{\sffamily 中文黑体预热：向量、矩阵、化学、单位。}

{\ttfamily Mono warmup: abcXYZ0123 +-*/= () [] \{\}.}

Inline math warmup:
$E=mc^2$, $\alpha+\beta=\gamma$, $\symbf{\alpha}$, $\symcal{F}$,
$\mathbb{R}$, $\mathbf{x}$, $\mathrm{d}x$, $\sin x$, $\nabla\cdot\mathbf{E}$.

\[
  \int_0^1 x^2\,dx = \frac{1}{3},\qquad
  \sum_{n=1}^{\infty}\frac{1}{n^2}=\frac{\pi^2}{6},\qquad
  \sqrt{x^2+y^2}
\]

\[
  A =
  \begin{pmatrix}
    1 & 2\\
    3 & 4
  \end{pmatrix},
  \qquad
  f(x)=
  \begin{cases}
    x^2, & x \ge 0,\\
    -x,  & x < 0,
  \end{cases}
\]

Physics warmup:
\[
  \ip{\psi}{\phi}\quad
  \dv{x}\sin x=\cos x\quad
  \pdv{f}{x}\quad
  \vb{v}\cdot\vu{n}\quad
  \nabla f
\]

Chemistry warmup in text mode:
\ce{H2O}, \ce{CO2}, \ce{Na+}, \ce{SO4^2-}, \ce{A -> B}, \ce{2H2 + O2 -> 2H2O}.

Chemistry warmup in math/color context:
{\color{blue}$\ce{H2O}$}
{\color{red}$\ce{CO2 + C -> 2CO}$}
\[
  \ce{CH4 + 2O2 -> CO2 + 2H2O}
\]

Color and cancel warmup:
{\color{blue}blue text}
{\color{red}red text}
{\color{green!50!black}green text}
\[
  \cancel{x+y}\quad \bcancel{a-b}\quad \xcancel{z}
\]

Script-size warmup:
\[
  x_{i_j}^{k_\ell} + \frac{\frac{a}{b}}{\sqrt{c_d}}
\]

\end{preview}
\end{document}
'@

  Set-Content -LiteralPath (Join-Path $warmupDir "warmup.tex") -Value $content -Encoding utf8
}

function Write-RefreshCacheScript {
  param([string]$DestRoot)

  $ps1 = @'
param(
  [string]$WarmupTex,
  [string]$OutputDirectory,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

$RuntimeRoot = $PSScriptRoot
$bin = Join-Path $RuntimeRoot "bin\windows"
$launcher = Join-Path $bin "xetexdaemon.exe"
$fmtDir = Join-Path $RuntimeRoot "texmf-var\web2c\xetex"
$cacheDir = Join-Path $RuntimeRoot "texmf-var\fonts\cache"
$confDir = Join-Path $RuntimeRoot "texmf-var\fonts\conf"

function Write-FontconfigForRuntime {
  param([string]$Root)

  $rootForXml = $Root.Replace("\", "/")
  $configDir = Join-Path $Root "texmf-var\fonts\conf"
  New-Item -ItemType Directory -Force -Path (Join-Path $configDir "conf.d") | Out-Null

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
  Set-Content -LiteralPath (Join-Path $configDir "fonts.conf") -Value $fontsConf -Encoding utf8

  $localConf = @"
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
</fontconfig>
"@
  Set-Content -LiteralPath (Join-Path $configDir "conf.d\51-local.conf") -Value $localConf -Encoding utf8
}

if (-not (Test-Path -LiteralPath $launcher)) {
  throw "xetexdaemon.exe not found under runtime root: $RuntimeRoot"
}

if (-not $WarmupTex) {
  $WarmupTex = Join-Path $RuntimeRoot "cache-warmup\warmup.tex"
}
if (-not (Test-Path -LiteralPath $WarmupTex)) {
  throw "Warmup TeX file not found: $WarmupTex"
}

if (-not $OutputDirectory) {
  $OutputDirectory = Join-Path $RuntimeRoot "texmf-var\cache-warmup"
}

if ($Clean -and (Test-Path -LiteralPath $cacheDir)) {
  Remove-Item -LiteralPath $cacheDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $cacheDir, $OutputDirectory | Out-Null
Write-FontconfigForRuntime -Root $RuntimeRoot

$savedEnv = @{}
foreach ($name in @(
  "PATH",
  "TEXMFROOT",
  "TEXMFCNF",
  "TEXFORMATS",
  "ICU_DATA",
  "XE_FONTCONFIG_PATH",
  "FONTCONFIG_PATH",
  "XE_FC_CACHEDIR",
  "FC_CACHEDIR",
  "FONTCONFIG_NO_CACHE_REFRESH"
)) {
  $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

try {
  $env:PATH = "$bin;$env:SystemRoot\System32"
  $env:TEXMFROOT = $RuntimeRoot
  $env:TEXMFCNF = Join-Path $RuntimeRoot "texmf-dist\web2c"
  $env:TEXFORMATS = "$fmtDir;$fmtDir\"
  $icuData = Join-Path $bin "icu-data"
  if (Get-ChildItem -LiteralPath $icuData -Filter "icudt*l.dat" -File -ErrorAction SilentlyContinue) {
    $env:ICU_DATA = $icuData
  }
  $env:XE_FONTCONFIG_PATH = $confDir
  $env:FONTCONFIG_PATH = $confDir
  $env:XE_FC_CACHEDIR = $cacheDir
  $env:FC_CACHEDIR = $cacheDir
  Remove-Item Env:FONTCONFIG_NO_CACHE_REFRESH -ErrorAction SilentlyContinue

  $resolvedWarmupTex = (Resolve-Path -LiteralPath $WarmupTex).Path
  $warmupDir = Split-Path -Parent $resolvedWarmupTex
  $warmupName = Split-Path -Leaf $resolvedWarmupTex

  Push-Location -LiteralPath $warmupDir
  try {
    & $launcher -fmt=xelatexdaemon -no-pdf -interaction=nonstopmode -halt-on-error -output-directory="$OutputDirectory" $warmupName
    if ($LASTEXITCODE -ne 0) {
      throw "Font cache refresh failed with exit code $LASTEXITCODE."
    }
  } finally {
    Pop-Location
  }

  $cacheFiles = Get-ChildItem -LiteralPath $cacheDir -File -ErrorAction SilentlyContinue
  if (-not $cacheFiles) {
    throw "Font cache refresh completed but no cache files were written to $cacheDir."
  }
  $warmupXdv = Join-Path $OutputDirectory "warmup.xdv"
  if (-not (Test-Path -LiteralPath $warmupXdv)) {
    throw "Warmup completed but XDV fontdefs cache was not written: $warmupXdv"
  }

  $totalBytes = ($cacheFiles | Measure-Object -Property Length -Sum).Sum
  Write-Host "Font cache refreshed: $cacheDir"
  Write-Host ("Cache files: {0}, bytes: {1}" -f $cacheFiles.Count, $totalBytes)
  Write-Host "XDV fontdefs cache: $warmupXdv"
} finally {
  foreach ($name in $savedEnv.Keys) {
    if ($null -ne $savedEnv[$name]) {
      Set-Item -Path "Env:$name" -Value $savedEnv[$name]
    } else {
      Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
    }
  }
}
'@

  Set-Content -LiteralPath (Join-Path $DestRoot "refresh-font-cache.ps1") -Value $ps1 -Encoding ascii

  $bat = @(
    '@echo off',
    'setlocal',
    'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0refresh-font-cache.ps1" %*'
  )
  Set-Content -LiteralPath (Join-Path $DestRoot "refresh-font-cache.bat") -Value $bat -Encoding ascii
}

function Write-Fontconfig {
  param([string]$DestRoot)

  $rootForXml = $DestRoot.Replace("\", "/")
  $confDir = Join-Path $DestRoot "texmf-var\fonts\conf"
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

function Write-MiniTexmfCnfOverlay {
  param([string]$DestRoot)

  $cnf = Join-Path $DestRoot "texmf-dist\web2c\texmf.cnf"
  if (-not (Test-Path -LiteralPath $cnf)) {
    throw "texmf.cnf not found: $cnf"
  }

  $original = Get-Content -LiteralPath $cnf -Raw
  $overlay = @"
% Mini XeLaTeX runtime overrides.
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
TEXMF = {`$TEXMFVAR,`$TEXMFCONFIG,`$TEXMFDIST}
TEXMFDBS = {`$TEXMFVAR,`$TEXMFCONFIG,`$TEXMFDIST}
SYSTEXMF = `$TEXMFSYSVAR;`$TEXMFLOCAL;`$TEXMFDIST
TEXMFCACHE = `$TEXMFVAR
VARTEXFONTS = `$TEXMFVAR/fonts
TEXFORMATS = `$TEXMFVAR/web2c/`$engine;`$TEXMFVAR/web2c
TEXPOOL = `$TEXMFVAR/web2c/`$engine;`$TEXMFDIST/web2c
OSFONTDIR = C:/Windows/fonts
FC_CACHEDIR = `$TEXMFVAR/fonts/cache
XE_FC_CACHEDIR = `$TEXMFVAR/fonts/cache
FONTCONFIG_PATH = `$TEXMFVAR/fonts/conf
XE_FONTCONFIG_PATH = `$TEXMFVAR/fonts/conf

"@

  Set-Content -LiteralPath $cnf -Value ($overlay + $original) -Encoding utf8
}

function Build-Format {
  param(
    [string]$TlRoot,
    [string]$DestRoot,
    [string]$RepoRoot
  )

  $fmtDir = Join-Path $DestRoot "texmf-var\web2c\xetex"
  New-Item -ItemType Directory -Force -Path $fmtDir | Out-Null
  Get-ChildItem -LiteralPath $fmtDir -File |
    Where-Object { $_.Extension -in @(".fmt", ".log", ".aux") } |
    Remove-Item -Force
  $bin = Join-Path $DestRoot "bin\windows"

  $oldPath = $env:PATH
  $oldTexmfRoot = $env:TEXMFROOT
  $oldTexmfCnf = $env:TEXMFCNF
  $oldTexFormats = $env:TEXFORMATS
  $oldIcuData = $env:ICU_DATA
  $oldXeFcPath = $env:XE_FONTCONFIG_PATH
  $oldFcPath = $env:FONTCONFIG_PATH
  $oldXeFcCache = $env:XE_FC_CACHEDIR
  $oldFcCache = $env:FC_CACHEDIR

  try {
    $env:PATH = "$bin;$(Join-Path $TlRoot 'bin\windows');$env:SystemRoot\System32"
    $env:ICU_DATA = Join-Path $bin "icu-data"
    $env:TEXMFROOT = $TlRoot
    $env:TEXMFCNF = Join-Path $TlRoot "texmf-dist\web2c"
    $env:TEXFORMATS = "$fmtDir;$fmtDir\"
    $env:XE_FONTCONFIG_PATH = Join-Path $DestRoot "texmf-var\fonts\conf"
    $env:FONTCONFIG_PATH = Join-Path $DestRoot "texmf-var\fonts\conf"
    $env:XE_FC_CACHEDIR = Join-Path $DestRoot "texmf-var\fonts\cache"
    $env:FC_CACHEDIR = Join-Path $DestRoot "texmf-var\fonts\cache"

    $fmtEngine = Join-Path $bin "xetexdaemon.exe"
    Push-Location -LiteralPath $fmtDir
    try {
      & $fmtEngine -ini -etex -jobname=xelatexdaemon xelatex.ini
      if ($LASTEXITCODE -ne 0) {
        throw "Failed to build xelatexdaemon.fmt."
      }
    } finally {
      Pop-Location
    }

    Get-ChildItem -LiteralPath $fmtDir -File |
      Where-Object { $_.Extension -in @(".log", ".aux") } |
      Remove-Item -Force
  } finally {
    $env:PATH = $oldPath
    $env:TEXMFROOT = $oldTexmfRoot
    $env:TEXMFCNF = $oldTexmfCnf
    $env:TEXFORMATS = $oldTexFormats
    if ($oldIcuData) { $env:ICU_DATA = $oldIcuData } else { Remove-Item Env:ICU_DATA -ErrorAction SilentlyContinue }
    if ($oldXeFcPath) { $env:XE_FONTCONFIG_PATH = $oldXeFcPath } else { Remove-Item Env:XE_FONTCONFIG_PATH -ErrorAction SilentlyContinue }
    if ($oldFcPath) { $env:FONTCONFIG_PATH = $oldFcPath } else { Remove-Item Env:FONTCONFIG_PATH -ErrorAction SilentlyContinue }
    if ($oldXeFcCache) { $env:XE_FC_CACHEDIR = $oldXeFcCache } else { Remove-Item Env:XE_FC_CACHEDIR -ErrorAction SilentlyContinue }
    if ($oldFcCache) { $env:FC_CACHEDIR = $oldFcCache } else { Remove-Item Env:FC_CACHEDIR -ErrorAction SilentlyContinue }
  }
}

function Invoke-FontCacheWarmup {
  param(
    [string]$DestRoot,
    [string]$WarmupTex
  )

  if (-not (Test-Path -LiteralPath $WarmupTex)) {
    throw "Font cache warmup TeX file not found: $WarmupTex"
  }

  $bin = Join-Path $DestRoot "bin\windows"
  $fmtDir = Join-Path $DestRoot "texmf-var\web2c\xetex"
  $cacheDir = Join-Path $DestRoot "texmf-var\fonts\cache"
  $workDir = Join-Path $DestRoot "texmf-var\cache-warmup"
  New-Item -ItemType Directory -Force -Path $cacheDir, $workDir | Out-Null

  $oldPath = $env:PATH
  $oldTexmfRoot = $env:TEXMFROOT
  $oldTexmfCnf = $env:TEXMFCNF
  $oldTexFormats = $env:TEXFORMATS
  $oldIcuData = $env:ICU_DATA
  $oldXeFcPath = $env:XE_FONTCONFIG_PATH
  $oldFcPath = $env:FONTCONFIG_PATH
  $oldXeFcCache = $env:XE_FC_CACHEDIR
  $oldFcCache = $env:FC_CACHEDIR

  try {
    $env:PATH = "$bin;$env:SystemRoot\System32"
    $env:TEXMFROOT = $DestRoot
    $env:TEXMFCNF = Join-Path $DestRoot "texmf-dist\web2c"
    $env:TEXFORMATS = "$fmtDir;$fmtDir\"
    $env:ICU_DATA = Join-Path $bin "icu-data"
    $env:XE_FONTCONFIG_PATH = Join-Path $DestRoot "texmf-var\fonts\conf"
    $env:FONTCONFIG_PATH = Join-Path $DestRoot "texmf-var\fonts\conf"
    $env:XE_FC_CACHEDIR = $cacheDir
    $env:FC_CACHEDIR = $cacheDir

    $resolvedWarmupTex = (Resolve-Path -LiteralPath $WarmupTex).Path
    $warmupTexDir = Split-Path -Parent $resolvedWarmupTex
    $warmupTexName = Split-Path -Leaf $resolvedWarmupTex
    Push-Location -LiteralPath $warmupTexDir
    try {
      & (Join-Path $bin "xetexdaemon.exe") -fmt=xelatexdaemon -no-pdf -interaction=nonstopmode -halt-on-error -output-directory="$workDir" $warmupTexName
      if ($LASTEXITCODE -ne 0) {
        throw "Font cache warmup failed with exit code $LASTEXITCODE."
      }
    } finally {
      Pop-Location
    }

    $cacheFiles = Get-ChildItem -LiteralPath $cacheDir -File -ErrorAction SilentlyContinue
    if (-not $cacheFiles) {
      throw "Font cache warmup completed but no cache files were written to $cacheDir."
    }
  } finally {
    $env:PATH = $oldPath
    $env:TEXMFROOT = $oldTexmfRoot
    $env:TEXMFCNF = $oldTexmfCnf
    $env:TEXFORMATS = $oldTexFormats
    if ($oldIcuData) { $env:ICU_DATA = $oldIcuData } else { Remove-Item Env:ICU_DATA -ErrorAction SilentlyContinue }
    if ($oldXeFcPath) { $env:XE_FONTCONFIG_PATH = $oldXeFcPath } else { Remove-Item Env:XE_FONTCONFIG_PATH -ErrorAction SilentlyContinue }
    if ($oldFcPath) { $env:FONTCONFIG_PATH = $oldFcPath } else { Remove-Item Env:FONTCONFIG_PATH -ErrorAction SilentlyContinue }
    if ($oldXeFcCache) { $env:XE_FC_CACHEDIR = $oldXeFcCache } else { Remove-Item Env:XE_FC_CACHEDIR -ErrorAction SilentlyContinue }
    if ($oldFcCache) { $env:FC_CACHEDIR = $oldFcCache } else { Remove-Item Env:FC_CACHEDIR -ErrorAction SilentlyContinue }
  }
}

$repoRoot = Resolve-ExistingPath -Path $SourceRoot -Name "SourceRoot"
$tlRoot = Find-TeXLiveRoot
$destRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
if (-not $CacheWarmupTex) {
  $CacheWarmupTex = Join-Path (Split-Path -Parent $PSScriptRoot) "test\test_5.tex"
}

if ($Clean -and (Test-Path -LiteralPath $destRoot)) {
  Remove-Item -LiteralPath $destRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $destRoot | Out-Null

$tlBinRoot = Join-Path $tlRoot "bin\windows"
$destBinRoot = Join-Path $destRoot "bin\windows"
New-Item -ItemType Directory -Force -Path $destBinRoot | Out-Null

$binFiles = @(
  "dvipdfmxdaemon.dll",
  "kpathsealibw64.dll",
  "kpsewhich.exe",
  "msvcp140.dll",
  "ucrtbase.dll",
  "vcruntime140.dll",
  "vcruntime140_1.dll",
  "xdvipdfmxdaemon.exe"
)

foreach ($file in $binFiles) {
  Copy-One -FromRoot $tlBinRoot -ToRoot $destBinRoot -RelativePath $file
}

Copy-Item -LiteralPath (Join-Path $repoRoot "ptx\texk\web2c\xetex.dll") -Destination (Join-Path $destBinRoot "xetexdaemon.dll") -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "ktx\texk\calldll\xetexdaemon.exe") -Destination (Join-Path $destBinRoot "xetexdaemon.exe") -Force
$daemonBat = @(
  '@echo off',
  '"%~dp0xetexdaemon.exe" -fmt=xelatexdaemon %*'
)
Set-Content -LiteralPath (Join-Path $destBinRoot "xelatexdaemon.bat") -Value $daemonBat -Encoding ascii
New-Item -ItemType Directory -Force -Path (Join-Path $destBinRoot "icu-data") | Out-Null
$icuDataSource = Join-Path $repoRoot "ptx\libs\icu-src\source\data\in"
$icuDataFile = Get-ChildItem -LiteralPath $icuDataSource -Filter "icudt*l.dat" -File -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $icuDataFile) {
  throw "ICU data file not found under $icuDataSource"
}
Copy-Item -LiteralPath $icuDataFile.FullName -Destination (Join-Path $destBinRoot "icu-data") -Force
$icuDllSourceRoots = @(
  (Join-Path $repoRoot "ptx\libs\icu-src\bin64"),
  (Join-Path $repoRoot "ptx\libs\icu-src\bin")
)
$icuDllFile = $null
foreach ($icuDllSourceRoot in $icuDllSourceRoots) {
  if (Test-Path -LiteralPath $icuDllSourceRoot) {
    $icuDllFile = Get-ChildItem -LiteralPath $icuDllSourceRoot -Filter "icudt*.dll" -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($icuDllFile) {
      break
    }
  }
}
if (-not $icuDllFile) {
  throw "ICU stub DLL not found under ptx\libs\icu-src\bin64 or ptx\libs\icu-src\bin"
}
Copy-Item -LiteralPath $icuDllFile.FullName -Destination $destBinRoot -Force

$texmfDist = Join-Path $tlRoot "texmf-dist"
$destTexmfDist = Join-Path $destRoot "texmf-dist"
$texmfFiles = @(
  "dvipdfmx\dvipdfmx.cfg",
  "fonts\misc\xetex\fontmapping\base\tex-text.tec",
  "fonts\map\fontname\texfonts.map",
  "tex\latex\ctex\ctexhook.sty",
  "web2c\fmtutil.cnf",
  "web2c\texmf.cnf"
)
foreach ($file in $texmfFiles) {
  Copy-One -FromRoot $texmfDist -ToRoot $destTexmfDist -RelativePath $file
}
Write-MiniTexmfCnfOverlay -DestRoot $destRoot

$texmfDirs = @(
  "fonts\afm\public\amsfonts",
  "fonts\afm\public\lm",
  "fonts\enc\dvips\lm",
  "fonts\map\dvipdfm\lm",
  "fonts\map\dvips\amsfonts",
  "fonts\map\dvips\lm",
  "fonts\opentype\public\lm",
  "fonts\opentype\public\xits",
  "fonts\source\public\amsfonts",
  "fonts\source\public\cm",
  "fonts\source\public\latex-fonts",
  "fonts\tfm\public\amsfonts",
  "fonts\tfm\public\cm",
  "fonts\tfm\public\latex-fonts",
  "fonts\tfm\public\lm",
  "fonts\type1\public\amsfonts",
  "fonts\type1\public\lm",
  "tex\latex\amsmath",
  "tex\latex\amsfonts",
  "tex\latex\base",
  "tex\latex\cancel",
  "tex\latex\tex-ini-files",
  "tex\latex\firstaid",
  "tex\latex\fontspec",
  "tex\latex\chemgreek",
  "tex\generic\babel",
  "tex\generic\config",
  "tex\generic\hyph-utf8",
  "tex\generic\hyphen",
  "tex\generic\iftex",
  "tex\generic\unicode-data",
  "tex\latex\graphics",
  "tex\latex\graphics-cfg",
  "tex\latex\graphics-def",
  "tex\latex\l3backend",
  "tex\latex\l3kernel",
  "tex\latex\l3packages\l3keys2e",
  "tex\latex\l3packages\xparse",
  "tex\latex\l3packages\xtemplate",
  "tex\latex\mathtools",
  "tex\latex\mhchem",
  "tex\latex\preview",
  "tex\latex\siunitx",
  "tex\latex\physics",
  "tex\latex\tools",
  "tex\latex\unicode-math",
  "tex\latex\xcolor",
  "tex\xelatex\xecjk",
  "tex\latex\lm",
  "tex\plain\amsfonts"
)
foreach ($dir in $texmfDirs) {
  Copy-Tree -FromRoot $texmfDist -ToRoot $destTexmfDist -RelativePath $dir
}

$languageConfigDir = Join-Path $destTexmfDist "tex\generic\config"
New-Item -ItemType Directory -Force -Path $languageConfigDir | Out-Null
$languageDat = @(
  "% StemTeX intentionally keeps format hyphenation minimal.",
  "english hyphen.tex",
  "=usenglish",
  "=USenglish",
  "=american",
  "nohyphenation zerohyph.tex",
  "dumylang dumyhyph.tex"
)
Set-Content -LiteralPath (Join-Path $languageConfigDir "language.dat") -Value $languageDat -Encoding ascii

New-Item -ItemType Directory -Force -Path (Join-Path $destRoot "texmf-var\fonts\cache") | Out-Null
Set-Content -LiteralPath (Join-Path $destRoot "VERSION") -Value $stemTeXVersion -Encoding ascii
Write-Fontconfig -DestRoot $destRoot
Build-Format -TlRoot $tlRoot -DestRoot $destRoot -RepoRoot $repoRoot
Write-RunScript -DestRoot $destRoot
Write-CacheWarmupTemplate -DestRoot $destRoot
Write-RefreshCacheScript -DestRoot $destRoot
if (-not $SkipFontCacheWarmup) {
  Invoke-FontCacheWarmup -DestRoot $destRoot -WarmupTex $CacheWarmupTex
}

Write-Host "Built StemTeX runtime $stemTeXVersion`: $destRoot"
