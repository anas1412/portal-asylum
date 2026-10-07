#!/bin/sh
# Build the loader (LD_PRELOAD) and the reloadable mod. src/syms.h holds the engine addresses for Outlast's Linux
# Steam build 576074; regenerate it from a different binary with: OUTLAST_BIN=/path/OLGame.x86_64 python3 tools/gensyms.py
set -e
cd "$(dirname "$0")"
mkdir -p build
g++ -std=c++17 -O2 -fPIC -shared -Wall -o build/libolportal.so src/loader.cpp -ldl
g++ -std=c++17 -O2 -fPIC -shared -Wall -fno-gnu-unique -o build/libolportal_mod.so src/mod.cpp -ldl
echo built
