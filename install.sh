#!/bin/sh
# Install the built p2paint files for the local CS:S (Linux).
#   install/libp2paint.so + install/game_shader_generic_p2paint.so  (LD_PRELOAD the first)
# The mod writes its own materials + menu font into cstrike/materials/p2paint and cstrike/resource/p2paint when the
# game starts. Pictures made in game live in $CSS/materials/paintpics (never touched here).
set -e
cd "$(dirname "$0")"
CSS="${CSS:-$HOME/.local/share/Steam/steamapps/common/Counter-Strike Source/cstrike}"
mkdir -p install
for f in libp2paint.so game_shader_generic_p2paint.so; do   # atomic: a running game keeps its old copy
  cp "build/$f" "install/.$f.tmp" && mv -f "install/.$f.tmp" "install/$f"
done
rm -f "$CSS/bin/linux64/game_shader_generic_p2paint.so"     # old location; CS:S never loads it from there
rm -rf "$CSS/custom/p2paint"                                 # (versions before 2026-11-17 installed the game files there)
echo "installed: $(pwd)/install"
echo "preload:   $(pwd)/install/libp2paint.so  (add it to LD_PRELOAD in your launch options)"
