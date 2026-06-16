#!/usr/bin/env bash

msvc_root="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"
sdk_root="/c/Program Files (x86)/Windows Kits/10"
sdk_ver="10.0.26100.0"

export PATH="$msvc_root/bin/Hostx64/x64:$sdk_root/bin/$sdk_ver/x64:/ucrt64/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

export INCLUDE="$(cygpath -w "$msvc_root/include");$(cygpath -w "$sdk_root/Include/$sdk_ver/ucrt");$(cygpath -w "$sdk_root/Include/$sdk_ver/shared");$(cygpath -w "$sdk_root/Include/$sdk_ver/um");$(cygpath -w "$sdk_root/Include/$sdk_ver/winrt");$(cygpath -w "$sdk_root/Include/$sdk_ver/cppwinrt")"
export LIB="$(cygpath -w "$msvc_root/lib/x64");$(cygpath -w "$sdk_root/Lib/$sdk_ver/ucrt/x64");$(cygpath -w "$sdk_root/Lib/$sdk_ver/um/x64")"
export LIBPATH="$LIB"

# Keep MSYS2 from rewriting MSVC linker switches such as /MANIFEST into
# C:\msys64\MANIFEST.
export MSYS2_ARG_CONV_EXCL="*"
