#!/bin/sh
set -eu
bw_dir=$(CDPATH= cd -- "${1:?Pass the external BoxedWine build cache directory}" && pwd)
if [ -f "$bw_dir/toolchain/emscripten-config.py" ]; then
    export PATH="$bw_dir/toolchain/compiler/emscripten:$bw_dir/toolchain/node/bin:$PATH"
    export EM_CONFIG="$bw_dir/toolchain/emscripten-config.py"
    export EM_CACHE="$bw_dir/toolchain/cache"
    export TMPDIR="$bw_dir/toolchain/tmp"
fi
command -v em++ >/dev/null || { echo 'Activate Emscripten 6.0.10 first.' >&2; exit 1; }
make -j 6 -C "$bw_dir/source/Boxedwine-05e033b115b8c64ae14cc3836d5246ab8d927fbf/project/emscripten" \
    BUILD_DIR=Build/Network \
    EXTRA_CPP_FLAGS="-DBOXEDWINE_WASM_JIT -DBOXEDWINE_BROWSER_NETWORK -msimd128" \
    SHELL_FILE=shell.html \
    ASSET_FILES="boxedwine.css boxedwine-shell.js" \
    EXTRA_LD_FLAGS="-msimd128 -sALLOW_TABLE_GROWTH -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=3221225472 -sEXPORTED_RUNTIME_METHODS=addFunction,removeFunction,wasmTable,HEAPU32 -sEXPORTED_FUNCTIONS=_main,_wasm_jit_import_from_file,_wasm_jit_set_persistence_active,_wasm_jit_set_record_active,_malloc,_free"
