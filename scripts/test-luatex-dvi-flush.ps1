param(
  [string]$Engine = '',
  [string]$TexLiveRoot = 'C:/texlive/2026',
  [string]$Python = '',
  [string]$OutputDirectory = '',
  [string[]]$Case = @('plain-small', 'unicode-small', 'plain-boundary', 'unicode-boundary',
                    'plain-alignment', 'unicode-alignment'),
  [switch]$RebuildFormats
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Engine) { $Engine = Join-Path $repo 'build/luahbtex-mingw/bin/luahbtex.exe' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'build/luatex-flush-tests' }
$Engine = (Resolve-Path -LiteralPath $Engine).Path
$TexLiveRoot = (Resolve-Path -LiteralPath $TexLiveRoot).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $Python) {
  $command = Get-Command python.exe -ErrorAction SilentlyContinue
  if ($command -and $command.Source -notmatch 'WindowsApps') { $Python = $command.Source }
  else {
    $Python = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
  }
}
if (-not (Test-Path -LiteralPath $Python)) { throw 'Pass -Python with a working Python 3 executable.' }
$runtime = Join-Path $OutputDirectory 'runtime'
$formats = Join-Path $runtime 'formats'
$cache = Join-Path $runtime 'cache'
New-Item -ItemType Directory -Force -Path $formats, $cache | Out-Null

$updates = @{
  TEXMFROOT = $TexLiveRoot
  TEXMFCNF = (Join-Path $TexLiveRoot 'texmf-dist/web2c')
  TEXMFVAR = $cache
  TEXMFCACHE = $cache
  TEXFORMATS = ($formats.Replace('\', '/') + '//;')
  PATH = ((Join-Path $TexLiveRoot 'bin/windows') + ';' + $env:PATH)
}
$previous = @{}
foreach ($name in $updates.Keys) {
  $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
  [Environment]::SetEnvironmentVariable($name, $updates[$name], 'Process')
}
try {
  foreach ($format in @('luatex', 'lualatex')) {
    $fmt = Join-Path $formats "$format.fmt"
    if ($RebuildFormats -or -not (Test-Path -LiteralPath $fmt) -or
        (Get-Item -LiteralPath $fmt).LastWriteTimeUtc -lt (Get-Item -LiteralPath $Engine).LastWriteTimeUtc) {
      Write-Host "Generating $format format for the experimental engine..."
      $arguments = @('--ini', '--interaction=nonstopmode', '--halt-on-error', '--no-shell-escape',
                     "--progname=$format", "--jobname=$format", "--output-directory=$formats", "$format.ini")
      & $Engine @arguments *> (Join-Path $runtime "$format-format-console.log")
      if ($LASTEXITCODE -ne 0) { throw "Failed to build $format format; see $runtime/$format-format-console.log" }
    }
    if (-not (Test-Path -LiteralPath $fmt)) { throw "Format was not produced: $fmt" }
  }
  $arguments = @((Join-Path $repo 'experiments/luatex-daemon/live-dvi-probe.py'),
                 '--luatex', $Engine, '--lualatex', $Engine, '--flush-output-on-shipout',
                 '--plain-format', (Join-Path $formats 'luatex.fmt'),
                 '--latex-format', (Join-Path $formats 'lualatex.fmt'),
                 '--output-directory', $OutputDirectory)
  foreach ($selected in $Case) { $arguments += @('--case', $selected) }
  & $Python @arguments *> (Join-Path $OutputDirectory 'live-console.log')
  if ($LASTEXITCODE -ne 0) { throw "DVI flush regression failed; see $OutputDirectory/results.json" }
  $results = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'results.json') | ConvertFrom-Json
  foreach ($result in $results) {
    Write-Host ("PASS {0}: {1} live pages" -f $result.case, $result.live.Count)
  }
  $modeOutput = Join-Path $OutputDirectory 'output-modes'
  $arguments = @((Join-Path $repo 'experiments/luatex-daemon/test-luatex-output-modes.py'),
                 '--engine', $Engine, '--plain-format', (Join-Path $formats 'luatex.fmt'),
                 '--output-directory', $modeOutput)
  & $Python @arguments *> (Join-Path $OutputDirectory 'output-modes-console.log')
  if ($LASTEXITCODE -ne 0) { throw "Output-mode regression failed; see $modeOutput/results.json" }
  Write-Host 'PASS default-off DVI and native PDF isolation'
} finally {
  foreach ($name in $updates.Keys) {
    [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
  }
}
