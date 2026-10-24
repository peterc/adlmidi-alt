#!/bin/sh
set -eu
cd "$(dirname "$0")"
python3 prepare.py "$@"
em++ src/engine.cpp src/dbopl.cpp src/adldata.cc -Isrc -std=c++17 -O3 -fexceptions \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=worklet,node \
  -sINCOMING_MODULE_JS_API=wasmBinary,print,printErr -sTEXTDECODER=1 -sFILESYSTEM=1 -sFORCE_FILESYSTEM=1 \
  -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=1048576 \
  -sEXPORTED_RUNTIME_METHODS=FS,UTF8ToString,HEAPF32,HEAP32 -o engine.mjs
