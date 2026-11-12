[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$StageDir,
    [string]$OutputDir,
    [string]$CMake = 'cmake.exe',
    [string]$ISCC = "${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe"
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -Raw -LiteralPath (Join-Path $repo 'VERSION')).Trim()
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build/stemtex-ninja' }
if (-not $StageDir) { $StageDir = Join-Path $repo "dist/package-stage/StemTeX-$version" }
if (-not $OutputDir) { $OutputDir = Join-Path $repo 'dist/installer' }
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$StageDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($StageDir)
$OutputDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
if (Test-Path -LiteralPath $StageDir) { throw "Package stage already exists. Choose a fresh -StageDir: $StageDir" }
$CMake = (Get-Command -CommandType Application $CMake).Source
$ISCC = (Get-Command -CommandType Application $ISCC).Source
New-Item -ItemType Directory -Path $StageDir | Out-Null

& $CMake --install $BuildDir --config Release --prefix $StageDir
if ($LASTEXITCODE -ne 0) { throw "CMake application install failed ($LASTEXITCODE)" }
& $CMake --install $BuildDir --config Release --prefix $StageDir --component LuaHBTeX
if ($LASTEXITCODE -ne 0) { throw "CMake LuaHBTeX component install failed ($LASTEXITCODE)" }

$runtime = Join-Path $StageDir 'runtime'
if ((Get-Content -Raw -LiteralPath (Join-Path $runtime 'VERSION')).Trim() -ne $version) {
    throw "Staged VERSION does not match $version"
}

# Build from the staged macro tree, even when the optional prebuilt format is older.
$bin = Join-Path $runtime 'bin/windows'
$workName = 'installer-format-' + [guid]::NewGuid().ToString('N')
$work = [IO.Path]::GetFullPath((Join-Path $BuildDir $workName))
New-Item -ItemType Directory -Path $work | Out-Null
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.UseShellExecute = $false
$start.FileName = Join-Path $bin 'xetexdaemon.exe'
$start.WorkingDirectory = $work
$start.Arguments = '-ini -etex -jobname=xelatexdaemon -interaction=nonstopmode -halt-on-error -output-directory=. xelatex.ini'
$start.EnvironmentVariables.Clear()
foreach ($name in 'SystemRoot','SystemDrive','WINDIR','TEMP','TMP','USERPROFILE','LOCALAPPDATA','APPDATA') {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ($value) { $start.EnvironmentVariables[$name] = $value }
}
$start.EnvironmentVariables['PATH'] = "$bin;$env:SystemRoot\System32"
$start.EnvironmentVariables['TEXMFROOT'] = $runtime
$start.EnvironmentVariables['TEXMFCNF'] = Join-Path $runtime 'texmf-dist/web2c'
$start.EnvironmentVariables['TEXMFDIST'] = Join-Path $runtime 'texmf-dist'
$start.EnvironmentVariables['ICU_DATA'] = Join-Path $bin 'icu-data'
$start.EnvironmentVariables['FONTCONFIG_PATH'] = Join-Path $runtime 'texmf-var/fonts/conf'
$start.EnvironmentVariables['XE_FONTCONFIG_PATH'] = $start.EnvironmentVariables['FONTCONFIG_PATH']
$process = [Diagnostics.Process]::Start($start)
try {
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "XeTeX format generation failed ($($process.ExitCode)). See $work/xelatexdaemon.log" }
} finally {
    $process.Dispose()
}
$formatDir = Join-Path $runtime 'texmf-var/web2c/xetex'
New-Item -ItemType Directory -Force -Path $formatDir | Out-Null
Copy-Item -LiteralPath (Join-Path $work 'xelatexdaemon.fmt') -Destination $formatDir -Force
if ([IO.Path]::GetDirectoryName($work) -ne $BuildDir.TrimEnd('\','/') -or [IO.Path]::GetFileName($work) -ne $workName) {
    throw "Unexpected temporary format directory: $work"
}
Remove-Item -LiteralPath $work -Recurse -Force

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
& $ISCC "/DSourceDir=$StageDir" "/DOutputDir=$OutputDir" "/DAppVersion=$version" (Join-Path $repo 'installer/stemtex.iss')
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed ($LASTEXITCODE)" }
Write-Host "Installer: $(Join-Path $OutputDir "StemTeX-$version-Setup.exe")"
