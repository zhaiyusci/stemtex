param(
  [string]$TeXLiveRoot,
  [string]$Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) "runtime"),
  [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot),
  [string]$CacheWarmupTex,
  [switch]$UseSelfBuiltXeTeX,
  [switch]$SkipFontCacheWarmup,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

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
  param([string]$DestRoot, [bool]$SelfBuilt)

  $icuLines = if ($SelfBuilt) {
    @(
      'if exist "%TLROOT%\bin\windows\icu-data\icudt76l.dat" set "ICU_DATA=%TLROOT%\bin\windows\icu-data"'
    )
  } else {
    @()
  }

  $engineLine = if ($SelfBuilt) {
    'xelatex.exe --no-font-cache-refresh %*'
  } else {
    'xelatex.exe %*'
  }

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
    'set "FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"'
  ) + $icuLines + @($engineLine)

  Set-Content -LiteralPath (Join-Path $DestRoot "run-xelatex.bat") -Value $content -Encoding ascii
}

function Write-CacheWarmupTemplate {
  param([string]$DestRoot)

  $warmupDir = Join-Path $DestRoot "cache-warmup"
  New-Item -ItemType Directory -Force -Path $warmupDir | Out-Null

  $content = @'
\documentclass{article}
\usepackage{unicode-math}
\usepackage{xeCJK}
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
$xelatex = Join-Path $bin "xelatex.exe"
$fmtDir = Join-Path $RuntimeRoot "texmf-var\web2c\xetex"
$cacheDir = Join-Path $RuntimeRoot "texmf-var\fonts\cache"
$confDir = Join-Path $RuntimeRoot "texmf-var\fonts\conf"

if (-not (Test-Path -LiteralPath $xelatex)) {
  throw "xelatex.exe not found under runtime root: $RuntimeRoot"
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
  if (Test-Path -LiteralPath (Join-Path $icuData "icudt76l.dat")) {
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
    & $xelatex -no-pdf -interaction=nonstopmode -halt-on-error -output-directory="$OutputDirectory" $warmupName
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

  $totalBytes = ($cacheFiles | Measure-Object -Property Length -Sum).Sum
  Write-Host "Font cache refreshed: $cacheDir"
  Write-Host ("Cache files: {0}, bytes: {1}" -f $cacheFiles.Count, $totalBytes)
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
    [bool]$SelfBuilt,
    [string]$RepoRoot
  )

  $fmtDir = Join-Path $DestRoot "texmf-var\web2c\xetex"
  New-Item -ItemType Directory -Force -Path $fmtDir | Out-Null
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
    if ($SelfBuilt) {
      $env:ICU_DATA = Join-Path $bin "icu-data"
    }
    $env:TEXMFROOT = $TlRoot
    $env:TEXMFCNF = Join-Path $TlRoot "texmf-dist\web2c"
    $env:TEXFORMATS = "$fmtDir;$fmtDir\"
    $env:XE_FONTCONFIG_PATH = Join-Path $DestRoot "texmf-var\fonts\conf"
    $env:FONTCONFIG_PATH = Join-Path $DestRoot "texmf-var\fonts\conf"
    $env:XE_FC_CACHEDIR = Join-Path $DestRoot "texmf-var\fonts\cache"
    $env:FC_CACHEDIR = Join-Path $DestRoot "texmf-var\fonts\cache"

    Push-Location -LiteralPath $fmtDir
    try {
      & (Join-Path $bin "xelatex.exe") -ini -etex -jobname=xelatex xelatex.ini
      if ($LASTEXITCODE -ne 0) {
        throw "Failed to build xelatex.fmt."
      }
    } finally {
      Pop-Location
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

function Invoke-FontCacheWarmup {
  param(
    [string]$DestRoot,
    [string]$WarmupTex,
    [bool]$SelfBuilt
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
    if ($SelfBuilt) {
      $env:ICU_DATA = Join-Path $bin "icu-data"
    }
    $env:XE_FONTCONFIG_PATH = Join-Path $DestRoot "texmf-var\fonts\conf"
    $env:FONTCONFIG_PATH = Join-Path $DestRoot "texmf-var\fonts\conf"
    $env:XE_FC_CACHEDIR = $cacheDir
    $env:FC_CACHEDIR = $cacheDir

    $resolvedWarmupTex = (Resolve-Path -LiteralPath $WarmupTex).Path
    $warmupTexDir = Split-Path -Parent $resolvedWarmupTex
    $warmupTexName = Split-Path -Leaf $resolvedWarmupTex
    Push-Location -LiteralPath $warmupTexDir
    try {
      & (Join-Path $bin "xelatex.exe") -no-pdf -interaction=nonstopmode -halt-on-error -output-directory="$workDir" $warmupTexName
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
  $CacheWarmupTex = Join-Path $repoRoot "test\test_5.tex"
}

if ($Clean -and (Test-Path -LiteralPath $destRoot)) {
  Remove-Item -LiteralPath $destRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $destRoot | Out-Null

$tlBinRoot = Join-Path $tlRoot "bin\windows"
$destBinRoot = Join-Path $destRoot "bin\windows"
New-Item -ItemType Directory -Force -Path $destBinRoot | Out-Null

$binFiles = @(
  "dvipdfmx.dll",
  "kpathsealibw64.dll",
  "kpsewhich.exe",
  "msvcp140.dll",
  "ucrtbase.dll",
  "vcruntime140.dll",
  "vcruntime140_1.dll",
  "xdvipdfmx.exe",
  "xetex.exe",
  "xelatex.exe"
)

foreach ($file in $binFiles) {
  Copy-One -FromRoot $tlBinRoot -ToRoot $destBinRoot -RelativePath $file
}

if ($UseSelfBuiltXeTeX) {
  Copy-One -FromRoot (Join-Path $repoRoot "ptx\texk\web2c") -ToRoot $destBinRoot -RelativePath "xetex.dll"
  Copy-One -FromRoot (Join-Path $repoRoot "ktx\texk\calldll") -ToRoot $destBinRoot -RelativePath "xetex.exe"
  Copy-One -FromRoot (Join-Path $repoRoot "ktx\texk\calldll") -ToRoot $destBinRoot -RelativePath "xelatex.exe"
  New-Item -ItemType Directory -Force -Path (Join-Path $destBinRoot "icu-data") | Out-Null
  Copy-One -FromRoot (Join-Path $repoRoot "ptx\libs\icu-src\source\data\in") -ToRoot (Join-Path $destBinRoot "icu-data") -RelativePath "icudt76l.dat"
  Copy-One -FromRoot (Join-Path $repoRoot "ptx\libs\icu-src\bin64") -ToRoot $destBinRoot -RelativePath "icudt76.dll"
} else {
  $icu = Get-ChildItem -LiteralPath $tlBinRoot -Filter "icudt*.dll" | Sort-Object Name -Descending | Select-Object -First 1
  if (-not $icu) {
    throw "No icudt*.dll found in $tlBinRoot"
  }
  Copy-Item -LiteralPath $icu.FullName -Destination (Join-Path $destBinRoot $icu.Name) -Force
  Copy-One -FromRoot $tlBinRoot -ToRoot $destBinRoot -RelativePath "xetex.dll"
}

$texmfDist = Join-Path $tlRoot "texmf-dist"
$destTexmfDist = Join-Path $destRoot "texmf-dist"
$texmfFiles = @(
  "dvipdfmx\dvipdfmx.cfg",
  "fonts\map\fontname\texfonts.map",
  "fonts\tfm\public\cm\cmr10.tfm",
  "web2c\fmtutil.cnf",
  "web2c\texmf.cnf"
)
foreach ($file in $texmfFiles) {
  Copy-One -FromRoot $texmfDist -ToRoot $destTexmfDist -RelativePath $file
}
Write-MiniTexmfCnfOverlay -DestRoot $destRoot

$texmfDirs = @(
  "fonts\opentype\public\fandol",
  "fonts\opentype\public\lm",
  "fonts\opentype\public\lm-math",
  "tex\latex\amsmath",
  "tex\latex\base",
  "tex\latex\cjk\texinput",
  "tex\latex\ctex",
  "tex\latex\fontspec",
  "tex\latex\chemgreek",
  "tex\latex\graphics",
  "tex\latex\graphics-cfg",
  "tex\latex\graphics-def",
  "tex\latex\l3backend",
  "tex\latex\l3kernel",
  "tex\latex\l3packages\l3keys2e",
  "tex\latex\l3packages\xparse",
  "tex\latex\l3packages\xtemplate",
  "tex\latex\mhchem",
  "tex\latex\physics",
  "tex\latex\tools",
  "tex\latex\unicode-math",
  "tex\latex\xcolor",
  "tex\xelatex\xecjk"
)
foreach ($dir in $texmfDirs) {
  Copy-Tree -FromRoot $texmfDist -ToRoot $destTexmfDist -RelativePath $dir
}

New-Item -ItemType Directory -Force -Path (Join-Path $destRoot "texmf-var\fonts\cache") | Out-Null
Write-Fontconfig -DestRoot $destRoot
Build-Format -TlRoot $tlRoot -DestRoot $destRoot -SelfBuilt:$UseSelfBuiltXeTeX.IsPresent -RepoRoot $repoRoot
Write-RunScript -DestRoot $destRoot -SelfBuilt:$UseSelfBuiltXeTeX.IsPresent
Write-CacheWarmupTemplate -DestRoot $destRoot
Write-RefreshCacheScript -DestRoot $destRoot
if (-not $SkipFontCacheWarmup) {
  Invoke-FontCacheWarmup -DestRoot $destRoot -WarmupTex $CacheWarmupTex -SelfBuilt:$UseSelfBuiltXeTeX.IsPresent
}

Write-Host "Built mini XeLaTeX tree: $destRoot"
