#!/bin/sh
# Dev launch: windowed, outside Steam's launch options, with the mod preloaded. Steam must be running.
# Refuses to start a second copy (pgrep -x matches the process name only, never this shell's command line).
if pgrep -x OLGame.x86_64 >/dev/null; then echo "Outlast is already running (pid $(pgrep -x OLGame.x86_64))"; exit 1; fi
G="$HOME/.local/share/Steam/steamapps/common/Outlast/Binaries/Linux"
cd "$G" && SteamAppId=238320 SteamGameId=238320 LD_PRELOAD="$HOME/outlast-portal-gun/build/libolportal.so" \
  exec ./OLGame.x86_64 -windowed -ResX=1280 -ResY=720 -nostartupmovies "$@"
