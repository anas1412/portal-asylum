#!/bin/sh
# Launch Outlast with the mod preloaded, windowed. Steam must be running.
# It runs in its own systemd scope capped at 6 GB RAM + 1 GB swap: if it ever runs away, only Outlast is stopped,
# never the desktop or whatever launched it. Refuses to start a second copy.
if pgrep -x OLGame.x86_64 >/dev/null; then echo "Outlast is already running (pid $(pgrep -x OLGame.x86_64))"; exit 1; fi
G="$HOME/.local/share/Steam/steamapps/common/Outlast/Binaries/Linux"
cd "$G" && SteamAppId=238320 SteamGameId=238320 LD_PRELOAD="$HOME/outlast-portal-gun/build/libolportal.so" \
  exec systemd-run --user --scope --quiet --unit="outlast-portal-$$" -p MemoryMax=6G -p MemorySwapMax=1G \
  ./OLGame.x86_64 -windowed -ResX=1280 -ResY=720 -nostartupmovies "$@"
