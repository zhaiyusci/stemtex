param(
  [ValidateSet("Fontconfig", "XeTeX", "Launchers", "All")]
  [string]$Target = "All",

  [ValidateSet("x64", "x86")]
  [string]$Arch = "x64",

  [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
  param([string]$Path)
  return (Resolve-Path -LiteralPath $Path).Path
}

function Find-VsDevCmd {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path -LiteralPath $vswhere) {
    $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($installPath) {
      $candidate = Join-Path $installPath "Common7\Tools\VsDevCmd.bat"
      if (Test-Path -LiteralPath $candidate) {
        return $candidate
      }
    }
  }

  $roots = @(${env:ProgramFiles(x86)}, $env:ProgramFiles) | Where-Object { $_ }
  foreach ($root in $roots) {
    $base = Join-Path $root "Microsoft Visual Studio"
    if (-not (Test-Path -LiteralPath $base)) {
      continue
    }
    $candidate = Get-ChildItem -LiteralPath $base -Recurse -Filter VsDevCmd.bat -ErrorAction SilentlyContinue |
      Select-Object -First 1 -ExpandProperty FullName
    if ($candidate) {
      return $candidate
    }
  }

  throw "Could not find VsDevCmd.bat. Install Visual Studio Build Tools with the C++ toolchain, or run this from an initialized VS Developer shell."
}

function Import-VsEnvironment {
  param(
    [string]$VsDevCmd,
    [string]$Arch
  )

  $cmd = "`"$VsDevCmd`" -no_logo -arch=$Arch >nul && set"
  $after = & cmd.exe /d /s /c $cmd
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to initialize Visual Studio environment for $Arch."
  }

  foreach ($line in $after) {
    $idx = $line.IndexOf("=")
    if ($idx -le 0) {
      continue
    }
    Set-Item -Path ("Env:" + $line.Substring(0, $idx)) -Value $line.Substring($idx + 1)
  }
}

function Assert-Command {
  param([string]$Name)
  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Required command '$Name' was not found in PATH."
  }
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

function Ensure-GnuMake {
  param([string]$Root)

  $make = Get-Command make.exe -ErrorAction SilentlyContinue
  if ($make) {
    return $make.Source
  }

  $tools = Join-Path $Root ".build-tools"
  $makeExe = Join-Path $tools "make\bin\make.exe"
  if (Test-Path -LiteralPath $makeExe) {
    $env:PATH = "$(Split-Path -Parent $makeExe);$env:PATH"
    return $makeExe
  }

  New-Item -ItemType Directory -Force -Path $tools | Out-Null
  $zip = Join-Path $tools "make.zip"
  $url = "https://sourceforge.net/projects/ezwinports/files/make-4.4.1-without-guile-w32-bin.zip/download"
  Write-Host "Downloading GNU make..."
  Invoke-WebRequest -Uri $url -OutFile $zip
  Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $tools "make") -Force
  if (-not (Test-Path -LiteralPath $makeExe)) {
    throw "GNU make download completed but make.exe was not found: $makeExe"
  }
  $env:PATH = "$(Split-Path -Parent $makeExe);$env:PATH"
  return $makeExe
}

function Find-GitUsrBin {
  $sh = Get-Command sh.exe -ErrorAction SilentlyContinue
  if ($sh -and (Split-Path -Leaf (Split-Path -Parent $sh.Source)) -ieq "bin") {
    $usr = Split-Path -Parent $sh.Source
    if (Test-Path -LiteralPath (Join-Path $usr "rm.exe")) {
      return $usr
    }
  }

  $candidates = @(
    (Join-Path $env:ProgramFiles "Git\usr\bin"),
    (Join-Path ${env:ProgramFiles(x86)} "Git\usr\bin"),
    (Join-Path $env:LOCALAPPDATA "Programs\Git\usr\bin")
  ) | Where-Object { $_ }

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath (Join-Path $candidate "sh.exe")) {
      return $candidate
    }
  }

  throw "Could not find Git for Windows usr\bin\sh.exe. Install Git for Windows or add usr\bin to PATH."
}

function Ensure-GitUsrJunction {
  param([string]$Root)

  $tools = Join-Path $Root ".build-tools"
  New-Item -ItemType Directory -Force -Path $tools | Out-Null
  $link = Join-Path $tools "gitusr"
  $sh = Join-Path $link "sh.exe"
  if (Test-Path -LiteralPath $sh) {
    return $link
  }

  $gitUsr = Find-GitUsrBin
  if (Test-Path -LiteralPath $link) {
    throw "$link exists but does not contain sh.exe. Remove it or point PATH at Git usr\bin."
  }

  & cmd.exe /d /c "mklink /J `"$link`" `"$gitUsr`"" | Out-Host
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $sh)) {
    throw "Failed to create Git usr\bin junction at $link."
  }
  return $link
}

function Initialize-BuildTools {
  param([string]$Root)

  $make = Ensure-GnuMake -Root $Root
  $gitUsr = Ensure-GitUsrJunction -Root $Root
  $env:PATH = "$(Split-Path -Parent $make);$gitUsr;$env:PATH"
  return (Join-Path $gitUsr "sh.exe").Replace("\", "/")
}

function Invoke-GnuMake {
  param(
    [string]$Root,
    [string]$Directory,
    [string[]]$Targets = @()
  )

  $shell = Initialize-BuildTools -Root $Root
  Invoke-Native -FilePath "make.exe" -Arguments (@("SHELL=$shell") + $Targets) -WorkingDirectory $Directory
}

function Build-Fontconfig {
  param([string]$Root)

  $dir = Join-Path $Root "ptx\libs\fontconfig\src"
  Assert-Command "cl.exe"
  Assert-Command "lib.exe"
  Assert-Command "nmake.exe"
  Invoke-Native -FilePath "nmake.exe" -Arguments @("/f", "Makefile", "libfontconfig.lib") -WorkingDirectory $dir
}

function Build-Zlib {
  param([string]$Root)

  $out = Join-Path $Root "ptx\libs\zlib\libz.lib"
  if (Test-Path -LiteralPath $out) {
    return
  }
  $dir = Join-Path $Root "ptx\libs\zlib"
  Invoke-Native -FilePath "nmake.exe" -Arguments @("/f", "win32\Makefile.msc", "libz.lib", "CFLAGS=-nologo -MT -W3 -O2 -Oy- -Zi -Fdzlib") -WorkingDirectory $dir
}

function Build-Libpng {
  param([string]$Root)

  Build-Zlib -Root $Root
  $out = Join-Path $Root "ptx\libs\libpng\libpng.lib"
  if (Test-Path -LiteralPath $out) {
    return
  }
  Invoke-Native -FilePath "nmake.exe" -Arguments @("/f", "Makefile", "libpng.lib") -WorkingDirectory (Join-Path $Root "ptx\libs\libpng")
}

function Build-Expat {
  param([string]$Root)

  $out = Join-Path $Root "ptx\libs\expat\lib\libexpat.lib"
  if (Test-Path -LiteralPath $out) {
    return
  }
  Invoke-Native -FilePath "nmake.exe" -Arguments @("/f", "Makefile") -WorkingDirectory (Join-Path $Root "ptx\libs\expat\lib")
}

function Build-StaticDeps {
  param([string]$Root)

  Build-Fontconfig -Root $Root
  Build-Expat -Root $Root
  Build-Zlib -Root $Root
  Build-Libpng -Root $Root
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\libs\teckit")
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\libs\freetype")
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\libs\pplib\src") -Targets @("libpplib.lib")
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\libs\graphite2-src\src")
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\libs\icu-src\source\stubdata")
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\libs\icu-src\source\stubdata") -Targets @("install")
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\libs\icu-src\source\common")
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\libs\icu-src\source\common") -Targets @("install")
}

function Ensure-Web2cPathsMk {
  param([string]$Root)

  $path = Join-Path $Root "ptx\texk\make\paths.mk"
  if (Test-Path -LiteralPath $path) {
    return
  }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
  @"
# Placeholder included by texk/web2c/web2c/Makefile in this Windows source tree.
# The XeTeX build path does not require variables from the TeX Live top-level tree.
"@ | Set-Content -LiteralPath $path -Encoding ascii
}

function Build-XeTeX {
  param([string]$Root)

  Ensure-Web2cPathsMk -Root $Root
  Build-StaticDeps -Root $Root
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\texk\kpathsea")
  $env:TEXMFCNF = Join-Path $Root "ptx\texk\kpathsea"
  Invoke-GnuMake -Root $Root -Directory (Join-Path $Root "ptx\texk\web2c") -Targets @("xetex.dll")
  $out = Join-Path $Root "ptx\texk\web2c\xetex.dll"
  if (-not (Test-Path -LiteralPath $out)) {
    throw "XeTeX build completed but output was not found: $out"
  }
  Write-Host "Built $out"
}

function Build-Launchers {
  param([string]$Root)

  $dir = Join-Path $Root "ktx\texk\calldll"
  Invoke-GnuMake -Root $Root -Directory $dir -Targets @("xetexdaemon.exe")
  $launcher = Join-Path $dir "xetexdaemon.exe"
  if (-not (Test-Path -LiteralPath $launcher)) {
    throw "Launcher build completed but output was not found: $launcher"
  }
  Write-Host "Built $launcher"
}

$root = Resolve-RepoPath $SourceRoot
if (-not (Test-Path -LiteralPath (Join-Path $root "ptx"))) {
  throw "SourceRoot does not contain ptx: $root"
}

if (-not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
  Import-VsEnvironment -VsDevCmd (Find-VsDevCmd) -Arch $Arch
}

Assert-Command "cl.exe"
Assert-Command "lib.exe"
Assert-Command "nmake.exe"

switch ($Target) {
  "Fontconfig" { Build-Fontconfig -Root $root }
  "XeTeX" { Build-XeTeX -Root $root }
  "Launchers" { Build-Launchers -Root $root }
  "All" {
    Build-XeTeX -Root $root
    Build-Launchers -Root $root
  }
}
