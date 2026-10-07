#!/bin/sh
# One-time setup for Portal Asylum (Outlast portal gun):
#   1. checks Outlast (native Linux) and Portal 2 are installed through Steam
#   2. builds the mod (skipped when the prebuilt build/*.so from a release download are present)
#   3. converts the portal gun model and sounds from YOUR Portal 2 install into cache/ (nothing of Valve's is shipped)
#   4. prints the Steam launch option to paste
# Changes no game file. Run it again any time (after updating the mod, or if cache/ was deleted).
set -e
cd "$(dirname "$0")"
MOD="$(pwd)"

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing '$1': $2"; exit 1; }; }
# release downloads ship build/*.so prebuilt; a git clone builds them (or pass --rebuild)
BUILD=0
{ [ -f build/libolportal.so ] && [ -f build/libolportal_mod.so ]; } || BUILD=1
[ "$1" = "--rebuild" ] && BUILD=1
[ "$BUILD" = 1 ] && need g++ "install your distro's C++ compiler (Arch: base-devel, Debian/Ubuntu: build-essential, Fedora: gcc-c++)"
need python3 "install Python 3"
need ffmpeg "install ffmpeg (used once to convert Portal 2's sounds)"

# Steam libraries: the main one plus every extra library folder
STEAM=""
for d in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
  [ -f "$d/steamapps/libraryfolders.vdf" ] && { STEAM="$d"; break; }
done
[ -n "$STEAM" ] || { echo "Steam not found."; exit 1; }
LIBS="$STEAM $(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"/\1/p' "$STEAM/steamapps/libraryfolders.vdf")"
find_game() { for l in $LIBS; do [ -d "$l/steamapps/common/$1" ] && { echo "$l/steamapps/common/$1"; return; }; done; }

OUTLAST="$(find_game Outlast)"
P2="$(find_game 'Portal 2')"
[ -n "$OUTLAST" ] || { echo "Outlast is not installed in any Steam library."; exit 1; }
[ -n "$P2" ] || { echo "Portal 2 is not installed in any Steam library (the gun and sounds are read from it)."; exit 1; }
EXE="$OUTLAST/Binaries/Linux/OLGame.x86_64"
[ -f "$EXE" ] || { echo "Outlast is installed, but not the native Linux version."
                   echo "Steam > Outlast > Properties > Compatibility: untick 'Force the use of a compatibility tool', let it update, run this again."; exit 1; }
SUPPORTED=01d787b8154c7914dedcc85487bc896dbbd13a77e1fc09b400b86cb4fe4d1556  # Steam build 576074
if [ "$(sha256sum "$EXE" | cut -c1-64)" != "$SUPPORTED" ]; then
  echo "Warning: this Outlast build isn't the one the mod was made for (Steam build 576074)."
  echo "The mod checks itself at start and stays off if the engine doesn't match. Continuing."
fi
echo "Outlast:  $OUTLAST"
echo "Portal 2: $P2"

if [ "$BUILD" = 1 ]; then ./build.sh; else echo "Using the prebuilt mod in build/"; fi
P2_DIR="$P2/portal2" python3 tools/p2gun.py
mkdir -p run

cat <<EOF

Done. Last step, once:
  Steam > right-click Outlast > Properties > General > Launch Options, paste:

LD_PRELOAD="\$LD_PRELOAD:$MOD/build/libolportal.so" systemd-run --user --scope --quiet -p MemoryMax=6G -p MemorySwapMax=1G %command%

Then press Play. To remove the mod: clear that box (and delete this folder if you like).
EOF
