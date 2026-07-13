param(
  [string]$OutDir = "",
  [string]$VSWhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
)

$ErrorActionPreference = "Stop"

function Resolve-FullPath([string]$Path) {
  $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Quote-Arg([string]$Value) {
  '"' + $Value + '"'
}

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) {
  $OutDir = Join-Path $Root "out\standalone-msvc"
}
$OutDir = Resolve-FullPath $OutDir
$DvisvgmRoot = Join-Path $Root "src\dvisvgm"
$DvisvgmSrc = Join-Path $DvisvgmRoot "src"
$Prebuilt = Join-Path $Root "prebuilt-msvc"
$ObjDir = Join-Path $OutDir "obj\dvisvgm"

foreach ($path in @(
    (Join-Path $Prebuilt "lib\kpathsea.lib"),
    (Join-Path $Prebuilt "lib\freetype.lib"),
    (Join-Path $Prebuilt "lib\libpng16_static.lib"),
    (Join-Path $Prebuilt "lib\zs.lib"),
    (Join-Path $Prebuilt "lib\expat.lib"),
    (Join-Path $Root "src\windows_mingw_wrapper\calldll.c"),
    (Join-Path $DvisvgmRoot "msvc\dvisvgmdaemon.cpp")
  )) {
  if (-not (Test-Path -LiteralPath $path)) {
    throw "missing file: $path"
  }
}

if (-not (Test-Path -LiteralPath $VSWhere)) {
  throw "vswhere not found: $VSWhere"
}
$VsInstall = & $VSWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsInstall) {
  throw "Visual Studio with VC tools was not found"
}
$VcVars = Join-Path $VsInstall "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $VcVars)) {
  throw "vcvars64.bat not found: $VcVars"
}

New-Item -ItemType Directory -Force $OutDir, $ObjDir | Out-Null
Remove-Item -Force (Join-Path $ObjDir "*.obj") -ErrorAction SilentlyContinue

$Exclude = @(
  "dvisvgm.cpp",
  "CLCommandLine.cpp",
  "MiKTeXCom.cpp",
  "FontWriter.cpp",
  "PSInterpreter.cpp",
  "PSPattern.cpp",
  "PSPreviewHandler.cpp",
  "PsSpecialHandler.cpp",
  "ShadingPatch.cpp",
  "TensorProductPatch.cpp",
  "TriangularPatch.cpp",
  "ImageToSVG.cpp",
  "PDFToSVG.cpp",
  "PDFHandler.cpp",
  "EPSFile.cpp",
  "Ghostscript.cpp"
)

$CppSources = @()
$CppSources += Get-ChildItem $DvisvgmSrc -Filter *.cpp |
  Where-Object { $_.Name -notin $Exclude } |
  Sort-Object Name |
  ForEach-Object FullName
$CppSources += Get-ChildItem (Join-Path $DvisvgmSrc "fonts") -Filter *.cpp |
  Sort-Object Name |
  ForEach-Object FullName
$CppSources += Get-ChildItem (Join-Path $DvisvgmSrc "optimizer") -Filter *.cpp |
  Sort-Object Name |
  ForEach-Object FullName
$CppSources += Join-Path $DvisvgmRoot "libs\clipper\clipper.cpp"
$CppSources += Join-Path $DvisvgmRoot "msvc\font_writer_stub.cpp"
$CppSources += Join-Path $DvisvgmRoot "msvc\dvisvgmdaemon.cpp"

$CSources = @(
  (Join-Path $DvisvgmRoot "libs\md5\md5.c"),
  (Join-Path $DvisvgmRoot "libs\potrace\curve.c"),
  (Join-Path $DvisvgmRoot "libs\potrace\decompose.c"),
  (Join-Path $DvisvgmRoot "libs\potrace\potracelib.c"),
  (Join-Path $DvisvgmRoot "libs\potrace\trace.c"),
  (Join-Path $DvisvgmRoot "libs\xxHash\xxhash.c")
)

$IncludeDirs = @(
  (Join-Path $DvisvgmRoot "msvc"),
  $DvisvgmSrc,
  (Join-Path $DvisvgmSrc "fonts"),
  (Join-Path $DvisvgmSrc "optimizer"),
  (Join-Path $DvisvgmRoot "libs\boost"),
  (Join-Path $DvisvgmRoot "libs\clipper"),
  (Join-Path $DvisvgmRoot "libs\variant\include"),
  (Join-Path $DvisvgmRoot "libs\potrace"),
  (Join-Path $DvisvgmRoot "libs\md5"),
  (Join-Path $DvisvgmRoot "libs\xxHash"),
  (Join-Path $Prebuilt "include"),
  (Join-Path $Prebuilt "include\freetype2"),
  (Join-Path $Prebuilt "texk")
)

$CompileRsp = Join-Path $OutDir "compile-dvisvgmdaemon.rsp"
$CompileLines = @(
  "/nologo",
  "/O2",
  "/MT",
  "/EHsc",
  "/std:c++17",
  "/utf-8",
  "/bigobj",
  "/wd4244",
  "/wd4267",
  "/wd4305",
  "/wd4819",
  "/wd4828",
  "/wd4996",
  "/DWIN32=1",
  "/D_WIN32=1",
  "/DHAVE_CONFIG_H=1",
  "/DNO_KPSE_DLL=1",
  "/DXML_STATIC=1",
  "/DTEXLIVEWIN32=1",
  "/DNOMINMAX",
  "/D_CRT_SECURE_NO_WARNINGS",
  "/D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING",
  "/Foobj\dvisvgm\"
)
foreach ($dir in $IncludeDirs) {
  $CompileLines += "/I" + (Quote-Arg $dir)
}
foreach ($src in ($CppSources + $CSources)) {
  $CompileLines += Quote-Arg $src
}
Set-Content -LiteralPath $CompileRsp -Value $CompileLines -Encoding ASCII

$CompileCmd = 'call "' + $VcVars + '" >nul && cd /d "' + $OutDir + '" && cl /c @compile-dvisvgmdaemon.rsp'
cmd.exe /D /S /C $CompileCmd
if ($LASTEXITCODE -ne 0) {
  throw "dvisvgmdaemon compile failed with exit code $LASTEXITCODE"
}

$DllPath = Join-Path $OutDir "dvisvgmdaemon.dll"
$ImportLib = Join-Path $OutDir "dvisvgmdaemon.lib"
$Objects = Get-ChildItem $ObjDir -Filter *.obj | Sort-Object Name | ForEach-Object FullName
$LinkRsp = Join-Path $OutDir "link-dvisvgmdaemon.rsp"
$LinkLines = @(
  "/nologo",
  "/DLL",
  ("/OUT:" + (Quote-Arg $DllPath)),
  ("/IMPLIB:" + (Quote-Arg $ImportLib))
)
foreach ($obj in $Objects) {
  $LinkLines += Quote-Arg $obj
}
foreach ($lib in @("kpathsea.lib", "freetype.lib", "libpng16_static.lib", "zs.lib", "expat.lib")) {
  $LinkLines += Quote-Arg (Join-Path $Prebuilt ("lib\" + $lib))
}
$LinkLines += @("ws2_32.lib", "user32.lib", "advapi32.lib", "shell32.lib", "gdi32.lib", "ole32.lib", "uuid.lib", "shlwapi.lib")
Set-Content -LiteralPath $LinkRsp -Value $LinkLines -Encoding ASCII

$LinkCmd = 'call "' + $VcVars + '" >nul && cd /d "' + $OutDir + '" && link @link-dvisvgmdaemon.rsp'
cmd.exe /D /S /C $LinkCmd
if ($LASTEXITCODE -ne 0) {
  throw "dvisvgmdaemon link failed with exit code $LASTEXITCODE"
}

$CallDll = Join-Path $Root "src\windows_mingw_wrapper\calldll.c"
$ExePath = Join-Path $OutDir "dvisvgmdaemon.exe"
$ExeCmd = 'call "' + $VcVars + '" >nul && cd /d "' + $OutDir + '" && cl /nologo /O2 /MT /utf-8 /DDLLPROC=dlldvisvgmmain "' + $CallDll + '" dvisvgmdaemon.lib /link /OUT:"' + $ExePath + '"'
cmd.exe /D /S /C $ExeCmd
if ($LASTEXITCODE -ne 0) {
  throw "dvisvgmdaemon exe build failed with exit code $LASTEXITCODE"
}

Write-Host "built: $DllPath"
Write-Host "built: $ExePath"
