#!/bin/sh
# Dev launch: Outlast windowed with the mod preloaded (Steam must be running), in its own systemd scope capped at
# 6 GB RAM + 1 GB swap so a runaway game can't take the desktop down. Refuses to start a second copy.
if pgrep -x OLGame.x86_64 >/dev/null; then echo "Outlast is already running (pid $(pgrep -x OLGame.x86_64))"; exit 1; fi
MOD="$(cd "$(dirname "$0")" && pwd)"
G="${OUTLAST_DIR:-$HOME/.local/share/Steam/steamapps/common/Outlast}/Binaries/Linux"
cd "$G" && SteamAppId=238320 SteamGameId=238320 LD_PRELOAD="$MOD/build/libolportal.so" \
  exec systemd-run --user --scope --quiet --unit="outlast-portal-$$" -p MemoryMax=6G -p MemorySwapMax=1G \
  ./OLGame.x86_64 -windowed -ResX=1280 -ResY=720 -nostartupmovies "$@"
