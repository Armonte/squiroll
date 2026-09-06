#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# Squiroll build — incremental, cached.
#
# Layout:
#   build/deps/   one-time .obj for safetyhook + zydis + GekkoNet + asio.
#                 Only recompiled when its dep .cpp/.c is newer than the .obj.
#   build/obj/    per-squiroll-.cpp .obj. Recompiled when the .cpp or any
#                 included header is newer than the .obj (tracked via
#                 /showIncludes-driven .d files).
#   build/pch/    precompiled headers for the heavy hitters (windows.h,
#                 winsock2.h, asio.hpp).
#
# Switches:
#   BUILD_TYPE=dev    (default) -O0 + no LTO, fastest iteration
#   BUILD_TYPE=release           -O2 + LTO, slow, ships
#   FORCE_FULL_REBUILD=1         wipe build/ first
# ============================================================================

BUILD_TYPE="${BUILD_TYPE:-dev}"
BUILD_DIR="build"
DEPS_DIR="$BUILD_DIR/deps"
OBJ_DIR="$BUILD_DIR/obj"
PCH_DIR="$BUILD_DIR/pch"

if [ "${FORCE_FULL_REBUILD:-0}" = "1" ]; then
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$DEPS_DIR" "$OBJ_DIR" "$PCH_DIR"

# --- One-time bootstraps ----------------------------------------------------
if [ ! -f deps/safetyhook/amalgamated-dist/safetyhook.cpp ]; then
  git submodule update --init --recursive --depth 1
  (cd deps/safetyhook && python3 amalgamate.py)
fi

# --- Stage 0: Squirrel pre-flight ------------------------------------------
# Catch reserved-word / syntax errors in .nut files before we waste 10s on
# the Windows compile + a game boot to find them. Uses Squirrel 3.0.6's
# own compiler, built as a small Linux CLI.
SQCHECK_BIN=tools/sqcheck
SQCHECK_SRC=tools/sqcheck.cpp
SQ_DIR=deps/squirrel
if [ -d "$SQ_DIR/squirrel" ] && [ -f "$SQCHECK_SRC" ]; then
  # Rebuild sqcheck only when src or any Squirrel source is newer.
  rebuild_sqcheck=0
  if [ ! -x "$SQCHECK_BIN" ]; then
    rebuild_sqcheck=1
  else
    for f in "$SQCHECK_SRC" "$SQ_DIR"/squirrel/*.cpp "$SQ_DIR"/include/*.h; do
      [ "$f" -nt "$SQCHECK_BIN" ] && { rebuild_sqcheck=1; break; }
    done
  fi
  if [ "$rebuild_sqcheck" = "1" ]; then
    echo "=== Stage 0: build sqcheck ==="
    g++ -O2 -DSQUSEDOUBLE -I "$SQ_DIR/include" -I "$SQ_DIR/squirrel" \
        "$SQCHECK_SRC" "$SQ_DIR"/squirrel/*.cpp \
        -o "$SQCHECK_BIN"
  fi
  # Collect every .nut and run them through. If any fail the build halts.
  shopt -s globstar nullglob
  NUT_FILES=(src/Netcode/embed/**/*.nut src/Netcode/embed/*.nut)
  shopt -u globstar nullglob
  if [ "${#NUT_FILES[@]}" -gt 0 ]; then
    "$SQCHECK_BIN" "${NUT_FILES[@]}"
  fi
fi

# --- Stage 0.5: generate embed includes (PR #28 manifest system) -----------
# generate_embeds.py reads src/Netcode/embed_manifest.txt and writes
# embed_declarations.inc / embed_map.inc, which file_replacement.cpp
# #include's. Both .inc files are .gitignore'd, so this must run every
# build. Fast (pure text), so just always run it.
echo "=== Stage 0.5: generate embeds ==="
python3 generate_embeds.py

# src/Netcode/file_replacement.cpp #include's the generated .inc files,
# which in turn `#embed` every .nut. Our mtime check only sees the .cpp
# itself, so when only a .nut or the manifest changes we'd skip
# rebuilding file_replacement.obj and the embed would be stale. Bump the
# .cpp's mtime when any .nut or the manifest is newer than the .obj.
NUT_NEWEST=src/Netcode/embed_manifest.txt
shopt -s globstar nullglob
for nut in src/Netcode/embed/**/*.nut src/Netcode/embed/*.nut; do
  [ -f "$nut" ] || continue
  if [ -z "$NUT_NEWEST" ] || [ "$nut" -nt "$NUT_NEWEST" ]; then
    NUT_NEWEST="$nut"
  fi
done
shopt -u globstar nullglob
FR_CPP=src/Netcode/file_replacement.cpp
FR_OBJ="$OBJ_DIR/$(echo "${FR_CPP//\//_}" | sed 's/\.[^.]*$//').obj"
if [ -n "$NUT_NEWEST" ] && [ -f "$FR_OBJ" ] && [ "$NUT_NEWEST" -nt "$FR_OBJ" ]; then
  touch "$FR_CPP"
fi

# Legacy make_embed step: generated .nut.h files that nothing
# #include's anymore (file_replacement.cpp moved to C23 #embed). Kept
# for backward-compat in case an out-of-tree consumer still uses it.
MAKE_EMBED=./tools/make_embed_linux.run
if [ -x "$MAKE_EMBED" ]; then
  shopt -s globstar nullglob
  for nut in src/Netcode/embed/**/*.nut src/Netcode/embed/*.nut; do
    [ -f "$nut" ] || continue
    if [ ! -f "$nut.h" ] || [ "$nut" -nt "$nut.h" ]; then
      "$MAKE_EMBED" "$nut" "$nut.h"
    fi
  done
  shopt -u globstar nullglob
fi

# Skip the resource-script rebuild when the .res is fresher than the .rc.
# Without this, every invocation re-generates the .res, which then dirties
# th155r.exe via the next stage's mtime check.
if [ ! -f src/th155r/th155r.res ] || [ src/th155r/th155r.rc -nt src/th155r/th155r.res ]; then
  llvm-rc-19 src/th155r/th155r.rc
fi

# --- Flags ------------------------------------------------------------------
PREFIX="$HOME/.xwin-cache/splat"
INCLUDES="/imsvc$PREFIX/crt/include /imsvc$PREFIX/sdk/include/shared /imsvc$PREFIX/sdk/include/ucrt /imsvc$PREFIX/sdk/include/um"
LIBPATHS="/LIBPATH:$PREFIX/crt/lib/x86 /LIBPATH:$PREFIX/sdk/lib/ucrt/x86 /LIBPATH:$PREFIX/sdk/lib/um/x86"
DEFINES="-D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS -DNOMINMAX -D_WINSOCKAPI_ -D_CRT_SECURE_NO_DEPRECATE -D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_DECLARE_NONSTDC_NAMES"
WARNINGS="-Wno-cpp -Wno-narrowing -Wno-c99-designator -Wno-c23-extensions"
FLAGS_BASE="/Gs- /GS- /clang:-fwrapv /Zc:threadSafeInit- -mfpmath=sse -msse2 -msse -mstack-probe-size=1024 -mstack-alignment=4 -mno-stackrealign /clang:-fomit-frame-pointer"

if [ "$BUILD_TYPE" = "release" ]; then
  OPT_FLAGS="-O2 -flto=full"
else
  # /Od (no optimization) is clang-cl's MSVC-style equivalent of -O0,
  # without the [-Wunused-command-line-argument] spam on TUs that have
  # explicit `-O2` elsewhere on the line.
  OPT_FLAGS="/Od"
fi

SH_INCLUDES="/I deps/safetyhook/amalgamated-dist /I deps/zydis/include /I deps/zydis/src /I deps/zydis/dependencies/zycore/include /I deps/zydis/dependencies/zycore/src"
SH_DEFINES="-DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD"
GK_INCLUDES="/I deps/GekkoNet/GekkoLib/include /I deps/GekkoNet/GekkoLib/thirdparty /I deps/GekkoNet/GekkoLib/thirdparty/asio"
GK_DEFINES="-DGEKKONET_STATIC -DASIO_STANDALONE -D_WIN32_WINNT=0x0601 -D_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING"
GK_FORCE_INCLUDE="/FI winsock2.h"
SQ_INCLUDES="/Isrc/shared /Isrc/Netcode/include"

# Common per-TU flags. Wrap in an array for safe space-handling.
# /std:c++latest is silently ignored for .c inputs with /TC, so it lives
# in DEP_FLAGS too — GekkoNet/safetyhook need it for C++17+ features.
# /std:c++latest goes only on C++ batches (DEP_CPP_FLAGS, SQ_FLAGS) — it
# triggers a [-Wunused-command-line-argument] noise warning on every C
# input otherwise.
DEP_C_FLAGS=($WARNINGS $DEFINES $SH_DEFINES $GK_DEFINES $INCLUDES $SH_INCLUDES $GK_INCLUDES $GK_FORCE_INCLUDE $FLAGS_BASE $OPT_FLAGS)
DEP_CPP_FLAGS=("${DEP_C_FLAGS[@]}" /std:c++latest)
SQ_FLAGS=($WARNINGS $DEFINES $SH_DEFINES $GK_DEFINES $INCLUDES $SH_INCLUDES $GK_INCLUDES $GK_FORCE_INCLUDE $FLAGS_BASE $OPT_FLAGS $SQ_INCLUDES /std:c++latest /Z7)

# --- Helpers ----------------------------------------------------------------
# obj_path src.cpp → build/{deps|obj}/<flattened-relpath>.obj
# Flatten so duplicate basenames (e.g. zydis/src/String.c vs
# zydis/.../zycore/src/String.c) don't collide.
obj_path() {
  local src="$1"
  local out_dir="$2"
  local flat="${src//\//_}"
  flat="${flat%.*}"
  echo "$out_dir/$flat.obj"
}

# True if the .obj is missing or older than the source.
needs_rebuild() {
  local src="$1"
  local obj="$2"
  [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]
}

# Note: we only mtime-check the .cpp itself, not its included headers.
# Header tracking with clang-cl is awkward (`/MD` means runtime-DLL, not
# emit-deps), and edits to shared headers are rare enough that the
# escape hatch `FORCE_FULL_REBUILD=1 ./build.sh` covers it. Touch the
# .cpp or wipe build/ when a header changes that you care about.
compile_one() {
  local src="$1"
  local out_dir="$2"
  local extra_lang_flag="$3"  # /TC for C, /TP for C++; empty for default
  local -n flags_ref="$4"
  local obj
  obj=$(obj_path "$src" "$out_dir")
  if ! needs_rebuild "$src" "$obj"; then
    return 0
  fi
  echo "[cc] $src"
  clang-cl-19 -m32 /EHsc ${extra_lang_flag:-} "${flags_ref[@]}" -c "$src" -o "$obj"
}

# Spawn compile_one in the background up to NPROC jobs, then wait.
compile_batch() {
  local out_dir="$1"
  local lang_flag="$2"
  local flags_name="$3"
  shift 3
  local nproc
  nproc=$(nproc 2>/dev/null || echo 4)
  local running=0
  local failed=0
  for src in "$@"; do
    compile_one "$src" "$out_dir" "$lang_flag" "$flags_name" &
    running=$((running + 1))
    if [ "$running" -ge "$nproc" ]; then
      if ! wait -n; then failed=1; fi
      running=$((running - 1))
    fi
  done
  while [ "$running" -gt 0 ]; do
    if ! wait -n; then failed=1; fi
    running=$((running - 1))
  done
  return $failed
}

# --- Stage 1: deps (one-time, cached) --------------------------------------
echo "=== Stage 1: deps ==="
DEP_C_SRCS=(deps/zydis/src/*.c deps/zydis/dependencies/zycore/src/*.c)
DEP_CPP_SRCS=(deps/safetyhook/amalgamated-dist/safetyhook.cpp deps/GekkoNet/GekkoLib/src/*.cpp)
DEP_OBJS=()
for src in "${DEP_C_SRCS[@]}" "${DEP_CPP_SRCS[@]}"; do
  DEP_OBJS+=("$(obj_path "$src" "$DEPS_DIR")")
done
compile_batch "$DEPS_DIR" "/TC" DEP_C_FLAGS "${DEP_C_SRCS[@]}"
compile_batch "$DEPS_DIR" "/TP" DEP_CPP_FLAGS "${DEP_CPP_SRCS[@]}"

# --- Stage 2: squiroll TUs (incremental) -----------------------------------
echo "=== Stage 2: squiroll ==="
SQ_SRCS=(src/Netcode/*.cpp)
SQ_OBJS=()
for src in "${SQ_SRCS[@]}"; do
  SQ_OBJS+=("$(obj_path "$src" "$OBJ_DIR")")
done
compile_batch "$OBJ_DIR" "/TP" SQ_FLAGS "${SQ_SRCS[@]}"

# --- Stage 3: launcher EXE -------------------------------------------------
# Rebuild only if src/th155r/main.cpp or the .res is newer than th155r.exe.
TH155R_SRC=src/th155r/main.cpp
TH155R_RES=src/th155r/th155r.res
if [ ! -f th155r.exe ] || [ "$TH155R_SRC" -nt th155r.exe ] || [ "$TH155R_RES" -nt th155r.exe ]; then
  echo "=== Stage 3: th155r.exe ==="
  clang-cl-19 -m32 -fuse-ld=lld /EHsc $WARNINGS $DEFINES $INCLUDES $FLAGS_BASE $OPT_FLAGS /Isrc/shared "$TH155R_SRC" "$TH155R_RES" /link $LIBPATHS /OUT:th155r.exe
fi

# --- Stage 4: link DLL ------------------------------------------------------
# Skip link if every .obj is older than Netcode.dll AND the .def is too.
need_link=0
if [ ! -f Netcode.dll ]; then
  need_link=1
elif [ src/Netcode/Netcode.def -nt Netcode.dll ]; then
  need_link=1
else
  for obj in "${SQ_OBJS[@]}" "${DEP_OBJS[@]}"; do
    if [ "$obj" -nt Netcode.dll ]; then
      need_link=1
      break
    fi
  done
fi

if [ "$need_link" = "1" ]; then
  echo "=== Stage 4: link ==="
  clang-cl-19 -m32 -fuse-ld=lld "${SQ_OBJS[@]}" "${DEP_OBJS[@]}" /link /DLL /DEBUG /PDB:Netcode.pdb $LIBPATHS user32.lib WS2_32.lib dbghelp.lib winmm.lib -exclude-all-symbols -kill-at /DEF:Netcode.def /OUT:Netcode.dll
else
  echo "=== Stage 4: link skipped (up to date) ==="
fi

# --- Deploy (skip cp when target is already current) ------------------------
DEPLOY_DIR="/mnt/c/dev/aocf/th155"
if [ -d "$DEPLOY_DIR" ]; then
  deployed=0
  for artifact in Netcode.dll th155r.exe; do
    if [ "$artifact" -nt "$DEPLOY_DIR/$artifact" ]; then
      cp -f "$artifact" "$DEPLOY_DIR/$artifact"
      deployed=1
    fi
  done
  [ "$deployed" = "1" ] && echo "Deployed to $DEPLOY_DIR"
fi

# thcrap/bin is the REAL/normal play chain (th155 (en).lnk -> thcrap loads
# Netcode.dll from here). Must stay in sync with the th155/ harness copy or
# the English/normal launch runs a stale build. See project_aocf_layout memory.
THCRAP_BIN="/mnt/c/dev/aocf/thcrap/bin"
if [ -d "$THCRAP_BIN" ] && [ Netcode.dll -nt "$THCRAP_BIN/Netcode.dll" ]; then
  cp -f Netcode.dll "$THCRAP_BIN/Netcode.dll"
  echo "Deployed Netcode.dll to $THCRAP_BIN"
fi
