#!/usr/bin/env bash
set -e

# Initialize submodules on first run + run safetyhook's amalgamator if
# the dist artifacts aren't there yet. Amalgamated-dist is gitignored
# inside the safetyhook submodule so we regenerate it locally per clone.
if [ ! -f deps/safetyhook/amalgamated-dist/safetyhook.cpp ]; then
  git submodule update --init --recursive --depth 1
  (cd deps/safetyhook && python3 amalgamate.py)
fi

# Regenerate embedded .nut headers (dev branch dropped this step).
# Without it, edits to src/Netcode/embed/*.nut never reach the binary.
MAKE_EMBED=./tools/make_embed_linux.run
if [ -x "$MAKE_EMBED" ]; then
  shopt -s globstar nullglob
  for nut in src/Netcode/embed/**/*.nut src/Netcode/embed/*.nut; do
    [ -f "$nut" ] || continue
    "$MAKE_EMBED" "$nut" "$nut.h"
  done
  shopt -u globstar nullglob
fi

llvm-rc-19 src/th155r/th155r.rc

PREFIX="$HOME/.xwin-cache/splat"
INCLUDES="/imsvc$PREFIX/crt/include /imsvc$PREFIX/sdk/include/shared /imsvc$PREFIX/sdk/include/ucrt /imsvc$PREFIX/sdk/include/um"
LIBPATHS="/LIBPATH:$PREFIX/crt/lib/x86 /LIBPATH:$PREFIX/sdk/lib/ucrt/x86 /LIBPATH:$PREFIX/sdk/lib/um/x86"
DEFINES="-D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS -DNOMINMAX -D_WINSOCKAPI_ -D_CRT_SECURE_NO_DEPRECATE -D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_DECLARE_NONSTDC_NAMES"
WARNINGS="-Wno-cpp -Wno-narrowing -Wno-c99-designator -Wno-c23-extensions"
FLAGS="/Gs- /GS- /clang:-fwrapv /Zc:threadSafeInit- -mfpmath=sse -msse2 -msse -mstack-probe-size=1024 -flto=full -mstack-alignment=4 -mno-stackrealign /clang:-fomit-frame-pointer"

# --- safetyhook + Zydis -----------------------------------------------------
# safetyhook provides InlineHook (entry hook with proper trampoline +
# Zydis-based length disassembly) and MidHook (register-preserving mid-
# function patches). Compiled in via the amalgamated single-file build so
# we don't need to drag CMake into our build pipeline. Zydis + zycore C
# sources are compiled inline; bumps build time by a few seconds but
# avoids managing a separate .lib artifact.
SH_INCLUDES="/I deps/safetyhook/amalgamated-dist /I deps/zydis/include /I deps/zydis/src /I deps/zydis/dependencies/zycore/include /I deps/zydis/dependencies/zycore/src"
SH_DEFINES="-DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD"
SH_C_SRCS="deps/zydis/src/*.c deps/zydis/dependencies/zycore/src/*.c"
SH_CPP_SRCS="deps/safetyhook/amalgamated-dist/safetyhook.cpp"

clang-cl-19 -m32 -fuse-ld=lld /EHsc $WARNINGS $DEFINES $INCLUDES $FLAGS /Isrc/shared src/th155r/main.cpp src/th155r/th155r.res -O2 /link $LIBPATHS /OUT:th155r.exe
clang-cl-19 -m32 -fuse-ld=lld /EHsc $WARNINGS $DEFINES $SH_DEFINES $INCLUDES $SH_INCLUDES $FLAGS /Isrc/shared /Isrc/Netcode/include src/Netcode/*.cpp $SH_CPP_SRCS $SH_C_SRCS /std:c++latest -O2 /link /DLL $LIBPATHS user32.lib WS2_32.lib dbghelp.lib winmm.lib -exclude-all-symbols -kill-at /DEF:Netcode.def /OUT:Netcode.dll

# Auto-deploy to the AoCF game folder so iteration doesn't need a manual cp.
DEPLOY_DIR="/mnt/c/dev/aocf/th155"
if [ -d "$DEPLOY_DIR" ]; then
  cp -f Netcode.dll th155r.exe "$DEPLOY_DIR/"
  echo "Deployed Netcode.dll + th155r.exe to $DEPLOY_DIR"
fi
