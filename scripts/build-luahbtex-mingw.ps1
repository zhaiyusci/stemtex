param(
  [string]$SourceRoot = '',
  [string]$BuildRoot = '',
  [string]$MingwRoot = 'C:/Qt/Tools/mingw1310_64',
  [string]$Bash = 'C:/Program Files/Git/bin/bash.exe',
  [string]$MakeShell = 'C:/PROGRA~1/Git/usr/bin/sh.exe',
  [ValidateSet('Prepare', 'Configure', 'Build')]
  [string]$Stage = 'Build',
  [int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $SourceRoot) { $SourceRoot = Join-Path $repo '../texlive-source' }
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo 'build/luahbtex-mingw' }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$source = Join-Path $BuildRoot 'source'
$work = Join-Path $BuildRoot 'work'
$bin = Join-Path $BuildRoot 'bin'
$make = Join-Path $repo '.build-tools/make/bin/make.exe'
$patch = Join-Path $repo 'patches/luatex-dvi-flush-on-shipout.patch'
$originPath = Join-Path $BuildRoot 'source-origin.json'
$configureIdentityPath = Join-Path $BuildRoot 'configure-inputs.json'
foreach ($required in @($Bash, $MakeShell, (Join-Path $MingwRoot 'bin/gcc.exe'), $make)) {
  if (-not (Test-Path -LiteralPath $required)) { throw "Missing build tool: $required" }
}
$sourceIdentity = [ordered]@{
  source_root = $SourceRoot
  luatex_c_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $SourceRoot 'texk/web2c/luatexdir/luatex.c')).Hash
  revision_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $SourceRoot 'texk/web2c/luatexdir/luatex_svnversion.h')).Hash
}
if (Test-Path -LiteralPath (Join-Path $source 'configure')) {
  if (-not (Test-Path -LiteralPath $originPath)) {
    throw "Copied source has no origin record. Use a new -BuildRoot: $BuildRoot"
  }
  $recorded = Get-Content -Raw -LiteralPath $originPath | ConvertFrom-Json
  foreach ($key in $sourceIdentity.Keys) {
    if ($recorded.$key -ne $sourceIdentity[$key]) {
      throw "Source identity changed ($key). Use a new -BuildRoot instead of reusing $BuildRoot"
    }
  }
} else {
  if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot 'configure'))) {
    throw "SourceRoot must contain the TeX Live source tree: $SourceRoot"
  }
  New-Item -ItemType Directory -Force -Path $source | Out-Null
  & robocopy $SourceRoot $source /E /XD .git /NFL /NDL /NJH /NJS /NP
  if ($LASTEXITCODE -ge 8) { throw "Copying source failed with robocopy exit $LASTEXITCODE" }
  [IO.File]::WriteAllText($originPath, ($sourceIdentity | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}
New-Item -ItemType Directory -Force -Path $work | Out-Null
Write-Host "Source: $source"
if ($Stage -eq 'Prepare') { exit 0 }
if (-not (Test-Path -LiteralPath $patch)) { throw "Required engine patch is missing: $patch" }
$versionSource = Get-Content -Raw -LiteralPath (Join-Path $source 'texk/web2c/luatexdir/luatex.c')
$revisionSource = Get-Content -Raw -LiteralPath (Join-Path $source 'texk/web2c/luatexdir/luatex_svnversion.h')
if ($versionSource -notmatch 'luatex_version_string\s*=\s*"1\.26\.2"' -or
    $revisionSource -notmatch 'luatex_svn_revision\s+7748\b') {
  throw 'This build recipe is validated for LuaTeX 1.26.2, development revision 7748.'
}
$configureIdentity = [ordered]@{
  mingw_root = [IO.Path]::GetFullPath($MingwRoot)
  gcc_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $MingwRoot 'bin/gcc.exe')).Hash
  configure_recipe = 1
}
if (Test-Path -LiteralPath $configureIdentityPath) {
  $recorded = Get-Content -Raw -LiteralPath $configureIdentityPath | ConvertFrom-Json
  foreach ($key in $configureIdentity.Keys) {
    if ($recorded.$key -ne $configureIdentity[$key]) {
      throw "Configure inputs changed ($key). Use a new -BuildRoot instead of reusing $BuildRoot"
    }
  }
} else {
  if (Test-Path -LiteralPath (Join-Path $work '.configure-complete')) {
    throw "Configured tree has no input record. Use a new -BuildRoot: $BuildRoot"
  }
  [IO.File]::WriteAllText($configureIdentityPath, ($configureIdentity | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}

# Pass paths as positional arguments; the shell script never interpolates them
# into source text. All toolchain environment changes belong to this child.
$bootstrap = @'
#!/usr/bin/env bash
set -euo pipefail
build_root="$(cygpath -u "$1")"
mingw_root="$(cygpath -u "$2")"
make_path="$(cygpath -u "$3")"
stage="$4"
jobs="$5"
repo="$(cygpath -u "$6")"
make_shell="$7"
export PATH="$mingw_root/bin:$(dirname "$make_path"):/usr/bin:/bin:$PATH"
export CC=gcc CXX=g++ AR=ar RANLIB=ranlib STRIP=strip
export CONFIG_SHELL=/usr/bin/bash SHELL=/usr/bin/bash
export MAKE="$make_path"
cd "$build_root/source"
patch_file="$repo/patches/luatex-dvi-flush-on-shipout.patch"
if [[ ! -f "$patch_file" ]]; then
  echo "Required engine patch is missing: $patch_file" >&2
  exit 1
else
  if git apply --check "$patch_file" >/dev/null 2>&1; then
    git apply "$patch_file"
  elif ! git apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    echo "Patch does not match the copied LuaTeX sources: $patch_file" >&2
    exit 1
  fi
fi
cd "$build_root/work"
if [[ ! -f .configure-complete ]]; then
  ../source/configure -C \
    --build=x86_64-w64-mingw32 --host=x86_64-w64-mingw32 \
    --disable-native-texlive-build --disable-shared --disable-all-pkgs \
    --enable-web2c --enable-luahbtex --disable-luatex --disable-luajittex \
    --disable-luajithbtex --disable-xetex --disable-tex --disable-etex \
    --disable-euptex --disable-aleph --disable-hitex --disable-pdftex \
    --disable-mf --disable-mflua --disable-mfluajit --disable-mp \
    --disable-pmp --disable-upmp --disable-texprof --disable-web-progs \
    --disable-synctex --without-x --enable-missing \
    --without-system-zlib --without-system-libpng --without-system-freetype2 \
    --without-system-harfbuzz --without-system-graphite2 --without-system-zziplib \
    --without-system-lua53 --without-system-gmp --without-system-mpfr \
    CFLAGS='-O2' CXXFLAGS='-O2 -Wa,-mbig-obj' \
    LDFLAGS='-static-libgcc -static-libstdc++' >configure.log 2>&1
  touch .configure-complete
fi
if [[ "$stage" == Build ]]; then
  # Upstream also configures every disabled package. For this focused engine
  # build, retain its selected dependency lists and skip only disabled trees.
  # These are generated files in work/, never upstream Makefile.in sources.
  for makefile in Makefile libs/Makefile utils/Makefile texk/Makefile; do
    sed -i 's/^CONF_SUBDIRS =.*/CONF_SUBDIRS = $(MAKE_SUBDIRS)/' "$makefile"
  done
  # The broad dependency selector still lists ICU for HarfBuzz, although
  # m4/kpse-harfbuzz-flags.m4 and libs/harfbuzz/Makefile.am explicitly build
  # LuaHBTeX's HarfBuzz without ICU. Skip that unused library in this recipe.
  sed -i '/^MAKE_SUBDIRS =/s/ icu / /' libs/Makefile
  configure_package() {
    local package="$1" configure_command
    if [[ ! -f "$package/Makefile" ]]; then
      mkdir -p "$package"
      configure_command="$(sed "s,auxdir/auxsub,$package,g" subsubdir-conf.cmd)"
      (cd "$package" && export CONFIG_SHELL="$make_shell" &&
        eval "\"$make_shell\" $configure_command")
    fi
  }
  {
    configure_package texk/kpathsea
    "$make_path" -C texk/kpathsea -j "$jobs" SHELL="$make_shell"
    "$make_path" -C libs -j "$jobs" SHELL="$make_shell"
    configure_package texk/web2c
    # The engine target avoids unrelated WEB programs. Upstream's explicit
    # luahbtex_DEPENDENCIES omits some archives that 'all' normally builds.
    "$make_path" -C texk/web2c -j "$jobs" SHELL="$make_shell" \
      'EXTRA_luahbtex_DEPENDENCIES=libff.a libluamisc.a libluasocket.a libluaffi.a libmplibcore.a libunilib.a' \
      luahbtex.exe
  } >build.log 2>&1
fi
'@
$bootstrapPath = Join-Path $BuildRoot 'build.sh'
[IO.File]::WriteAllText($bootstrapPath, $bootstrap.Replace("`r`n", "`n"), [Text.UTF8Encoding]::new($false))
& $Bash $bootstrapPath $BuildRoot $MingwRoot $make $Stage $Jobs $repo $MakeShell
if ($LASTEXITCODE -ne 0) {
  throw "LuaHBTeX $Stage failed. See $work/configure.log and $work/build.log."
}
Write-Host "LuaHBTeX $Stage completed: $work"
if ($Stage -eq 'Build') {
  $engine = Join-Path $work 'texk/web2c/luahbtex.exe'
  if (-not (Test-Path -LiteralPath $engine)) { throw "Build did not produce $engine" }
  New-Item -ItemType Directory -Force -Path $bin | Out-Null
  Copy-Item -LiteralPath $engine -Destination (Join-Path $bin 'luahbtex.exe') -Force
  $pending = [Collections.Generic.Queue[string]]::new()
  $pending.Enqueue($engine)
  $copied = @{}
  while ($pending.Count) {
    $file = $pending.Dequeue()
    $imports = & (Join-Path $MingwRoot 'bin/objdump.exe') -p $file
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect runtime dependencies: $file" }
    foreach ($line in $imports) {
      if ($line -match 'DLL Name:\s*(.+)$') {
        $name = $Matches[1].Trim()
        $dependency = Join-Path $MingwRoot "bin/$name"
        if ((Test-Path -LiteralPath $dependency) -and -not $copied.ContainsKey($name)) {
          Copy-Item -LiteralPath $dependency -Destination (Join-Path $bin $name) -Force
          $copied[$name] = $true
          $pending.Enqueue($dependency)
        }
      }
    }
  }
  $version = & (Join-Path $bin 'luahbtex.exe') --version
  if ($LASTEXITCODE -ne 0) { throw 'Built LuaHBTeX failed to start' }
  $inputs = @{}
  foreach ($relative in @('texk/web2c/luatexdir/luatex.c', 'texk/web2c/luatexdir/luatex_svnversion.h',
                         'texk/web2c/luatexdir/dvi/dvigen.c', 'texk/web2c/luatexdir/dvi/dvigen.h',
                         'texk/web2c/luatexdir/lua/luainit.c')) {
    $inputs[$relative] = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $source $relative)).Hash
  }
  $manifest = [ordered]@{
    engine_version = @($version)
    source_root = $SourceRoot
    copied_source = $source
    patched_source_sha256 = $inputs
    patch_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $patch).Hash
    binary_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $bin 'luahbtex.exe')).Hash
    mingw_root = $MingwRoot
    compiler_version = @(& (Join-Path $MingwRoot 'bin/gcc.exe') --version)
    make_shell = $MakeShell
    copied_runtime_dlls = @($copied.Keys)
  }
  [IO.File]::WriteAllText((Join-Path $bin 'build-manifest.json'), ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
  Write-Host "Runnable engine: $(Join-Path $bin 'luahbtex.exe')"
}
