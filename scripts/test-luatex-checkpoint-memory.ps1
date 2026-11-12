[CmdletBinding()]
param(
    [string]$MingwBin = '',
    [string]$BuildRoot = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildRoot) { $BuildRoot = Join-Path $repoRoot 'build/checkpoint-memory-test' }
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if (-not $MingwBin) {
    if (Test-Path -LiteralPath 'C:/Qt/Tools/mingw1310_64/bin/gcc.exe') {
        $MingwBin = 'C:/Qt/Tools/mingw1310_64/bin'
    } else {
        $gccCommand = Get-Command gcc.exe -ErrorAction SilentlyContinue
        if ($gccCommand) { $MingwBin = Split-Path -Parent $gccCommand.Source }
    }
}
$compiler = if ($MingwBin) { Join-Path $MingwBin 'gcc.exe' } else { '' }
if (-not $compiler -or -not (Test-Path -LiteralPath $compiler)) {
    throw 'MinGW GCC was not found. Supply -MingwBin pointing to its bin directory.'
}

$patchRoot = Join-Path $repoRoot 'patches/luatex-checkpoint'
$symbols = @(Get-Content -LiteralPath (Join-Path $patchRoot 'wrapped-symbols.txt') |
    ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') })
if ($symbols.Count -eq 0) { throw 'The checkpoint linker wrap list is empty.' }
$wrapArguments = @($symbols | ForEach-Object { '-Wl,--wrap=' + $_ })
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$executable = Join-Path $BuildRoot 'checkpoint-memory-smoke.exe'
$compilerArguments = @(
    '-std=gnu11', '-O2', '-fno-builtin', '-Wall', '-Wextra', '-Werror',
    '-I', $patchRoot,
    (Join-Path $repoRoot 'experiments/luatex-daemon/checkpoint-memory-smoke.c'),
    (Join-Path $patchRoot 'stemtex_checkpoint.c')
) + $wrapArguments + @('-o', $executable)
$cases = @(
    'memory', 'plain-file', 'VirtualAlloc', 'VirtualFree', 'MapViewOfFile',
    'UnmapViewOfFile', 'LoadLibraryA', 'LoadLibraryW', 'LoadLibraryExA',
    'LoadLibraryExW', 'FreeLibrary', 'CreateFileMappingA', 'CreateFileMappingW'
)

$previousPath = $env:PATH
$previousCheckpoint = $env:STEMTEX_LUA_CHECKPOINT
$previousDebug = $env:STEMTEX_LUA_CHECKPOINT_DEBUG
try {
    $env:PATH = $MingwBin + ';' + $env:PATH
    $env:STEMTEX_LUA_CHECKPOINT = '1'
    $env:STEMTEX_LUA_CHECKPOINT_DEBUG = '0'
    & $compiler @compilerArguments
    if ($LASTEXITCODE -ne 0) { throw "Checkpoint memory smoke compilation failed: $LASTEXITCODE" }
    Push-Location -LiteralPath $BuildRoot
    try {
        foreach ($scenario in $cases) {
            & $executable $scenario
            if ($LASTEXITCODE -ne 0) { throw "Checkpoint memory scenario '$scenario' failed: $LASTEXITCODE" }
        }
    } finally {
        Pop-Location
    }
    Write-Host "Checkpoint memory smoke passed: $($cases.Count) scenarios. Executable: $executable"
} finally {
    $env:PATH = $previousPath
    $env:STEMTEX_LUA_CHECKPOINT = $previousCheckpoint
    $env:STEMTEX_LUA_CHECKPOINT_DEBUG = $previousDebug
}
