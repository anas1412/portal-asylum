#!/bin/sh
# Build the loader (LD_PRELOAD) and the reloadable mod. src/syms.h holds the engine addresses for Outlast's Linux
# Steam build 576074; regenerate it from a different binary with: OUTLAST_BIN=/path/OLGame.x86_64 python3 tools/gensyms.py
# Portable flags: baseline x86-64, old glibc symbol versions (src/compat.h), libdl kept for older glibc.
set -e
cd "$(dirname "$0")"
mkdir -p build
FLAGS="-std=c++17 -O2 -march=x86-64 -mtune=generic -fPIC -shared -Wall -include src/compat.h"
LIBS="-Wl,--no-as-needed -ldl -lpthread"
g++ $FLAGS -o build/libolportal.so src/loader.cpp $LIBS
g++ $FLAGS -fno-gnu-unique -o build/libolportal_mod.so src/mod.cpp $LIBS
echo built
