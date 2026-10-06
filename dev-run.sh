#!/bin/sh
# Dev launch: windowed, outside Steam's launch options, with the mod preloaded. Steam must be running.
G="$HOME/.local/share/Steam/steamapps/common/Outlast/Binaries/Linux"
cd "$G" && SteamAppId=238320 SteamGameId=238320 LD_PRELOAD="$HOME/outlast-portal-gun/build/libolportal.so" \
  exec ./OLGame.x86_64 -windowed -ResX=1280 -ResY=720 -nostartupmovies "$@"
