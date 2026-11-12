[CmdletBinding()]
param(
    [string]$TexLiveRoot,
    [string]$EnginePath,
    [string]$StageRoot
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $StageRoot) { $StageRoot = Join-Path $repoRoot 'staging' }
$StageRoot = [IO.Path]::GetFullPath($StageRoot)
$gui = Join-Path $StageRoot 'gui/stemtex-renderer-gui.exe'
$runtime = Join-Path $StageRoot 'runtime'
$profile = Join-Path $StageRoot 'gui/profiles/unicodemath'
if (-not $EnginePath) {
    $EnginePath = Join-Path $runtime 'bin/luatex/luahbtex.exe'
    if (-not (Test-Path -LiteralPath $EnginePath -PathType Leaf)) {
        $EnginePath = Join-Path $repoRoot 'build/luahbtex-mingw/bin/luahbtex.exe'
    }
}
$EnginePath = [IO.Path]::GetFullPath($EnginePath)
foreach ($file in @($gui, $EnginePath, ($EnginePath + '.stemtex-checkpoint'), (Join-Path $profile 'preamble.tex'),
                    (Join-Path $runtime 'bin/windows/dvipdfmxdaemon.dll'),
                    (Join-Path $runtime 'bin/windows/dvisvgmdaemon.dll'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required file missing: $file. See docs/BUILDING_AND_PACKAGING.md (experimental LuaHBTeX GUI)."
    }
}
# Start-Process joins ArgumentList into a Windows command line. Quote each path;
# Windows filenames cannot contain a quote, and these are file/directory paths.
$guiArguments = @('--engine', 'luatex', '--luatex-engine', ('"' + $EnginePath + '"'))
$roots = @($repoRoot, $runtime, $profile)
if ($TexLiveRoot) { $roots += [IO.Path]::GetFullPath($TexLiveRoot) }
$guiArguments += $roots | ForEach-Object { '"' + $_.TrimEnd('\', '/') + '"' }
Start-Process -FilePath $gui -ArgumentList $guiArguments -WorkingDirectory $StageRoot
