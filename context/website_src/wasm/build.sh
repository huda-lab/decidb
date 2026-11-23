#!/bin/bash
# Build the DeciDB that runs in the live demo (context/website/demo/): DeciDB compiled to WebAssembly with
# Emscripten, single-threaded, with HiGHS (Gurobi is loaded at run time and is never present in a browser).
#
#   context/website_src/wasm/build.sh                 # from the repository root; builds origin/direct-solve-prototype
#   DECIDB_REF=origin/other-branch WORK=/tmp/w context/website_src/wasm/build.sh
#
# WORK holds the source snapshot, Emscripten and the build (default: build/wasm-demo, which git ignores). The
# outputs are demo/decidb.mjs (the module's JavaScript) and demo/decidb.wasm.gz (the engine, gzipped: the page
# unpacks it in the browser, so it downloads at its compressed size from any host).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
REF="${DECIDB_REF:-origin/direct-solve-prototype}"
WORK="${WORK:-$REPO/build/wasm-demo}"
EMSDK_VERSION=3.1.73
OUT="$REPO/context/website/demo"
mkdir -p "$WORK" "$OUT"
cd "$WORK"

# the source: a snapshot of REF, so the build never touches a checkout
if [ ! -f src/CMakeLists.txt ] || [ "$(cat src/.ref 2>/dev/null)" != "$(git -C "$REPO" rev-parse "$REF")" ]; then
  rm -rf src build && mkdir src
  git -C "$REPO" archive "$REF" | tar -x -C src
  git -C "$REPO" rev-parse "$REF" > src/.ref
fi
DESCRIBE="$(git -C "$REPO" describe --tags --long "$REF" 2>/dev/null || echo v0.0.0)"

if [ ! -d emsdk ]; then git clone -q --depth 1 https://github.com/emscripten-core/emsdk.git; fi
(cd emsdk && ./emsdk install "$EMSDK_VERSION" >/dev/null && ./emsdk activate "$EMSDK_VERSION" >/dev/null)
# shellcheck disable=SC1091
source emsdk/emsdk_env.sh >/dev/null 2>&1

# the library: no shell, no threads, no extension loading; core_functions is the only extension it needs
emcmake cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_SHELL=0 -DBUILD_UNITTESTS=0 -DBUILD_BENCHMARKS=0 -DDISABLE_THREADS=1 -DDISABLE_EXTENSION_LOAD=1 \
  -DBUILD_EXTENSIONS="core_functions" -DSKIP_EXTENSIONS="parquet;tpch;jemalloc" -DENABLE_SANITIZER=0 -DENABLE_UBSAN=0 \
  -DOVERRIDE_GIT_DESCRIBE="$DESCRIBE" -DDUCKDB_EXPLICIT_PLATFORM=wasm_eh \
  -DCMAKE_C_FLAGS="-fwasm-exceptions" -DCMAKE_CXX_FLAGS="-fwasm-exceptions -DWEBDB_FAST_EXCEPTIONS=1" >cmake.log
ninja -C build duckdb_static >ninja.log

# the module: decidb_wasm.cpp's two functions, an ES module the page's worker imports. DuckDB's binder recurses
# deeply, so the stack is 8 MB (Emscripten's default is 64 KB)
em++ -O3 -fwasm-exceptions -std=c++17 -Isrc/src/include "$HERE/decidb_wasm.cpp" $(find build -name '*.a' | sort) \
  -o "$WORK/decidb.mjs" -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createDecidb -sENVIRONMENT=web,worker,node \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sINITIAL_MEMORY=64MB -sSTACK_SIZE=8MB \
  -sEXPORTED_FUNCTIONS=_decidb_open,_decidb_query -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString
cp "$WORK/decidb.mjs" "$OUT/decidb.mjs"
gzip -9 -n -c "$WORK/decidb.wasm" >"$OUT/decidb.wasm.gz"
echo "built $OUT/decidb.mjs and decidb.wasm.gz ($(wc -c <"$OUT/decidb.wasm.gz" | tr -d ' ') bytes) from $REF ($DESCRIBE)"
