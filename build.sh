#!/bin/sh
# Build the loader (LD_PRELOAD) and the reloadable mod. touch run/reload to hot-reload the mod in a running game.
set -e
cd "$(dirname "$0")"
python3 tools/gensyms.py >/dev/null
g++ -std=c++17 -O2 -fPIC -shared -Wall -o build/libolportal.so src/loader.cpp -ldl
g++ -std=c++17 -O2 -fPIC -shared -Wall -fno-gnu-unique -o build/libolportal_mod.so src/mod.cpp -ldl
echo built
