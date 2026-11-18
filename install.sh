#!/bin/sh
# Install the built p2paint files for the local CS:S (Linux).
#   install/libp2paint.so + install/game_shader_generic_p2paint.so  (LD_PRELOAD the first)
#   $CSS/custom/p2paint/{materials,resource}                        (paint materials, aim ghost, menu font)
# Pictures made in game live in $CSS/materials/paintpics (never touched here).
set -e
cd "$(dirname "$0")"
CSS="${CSS:-$HOME/.local/share/Steam/steamapps/common/Counter-Strike Source/cstrike}"
mkdir -p install
for f in libp2paint.so game_shader_generic_p2paint.so; do   # atomic: a running game keeps its old copy
  cp "build/$f" "install/.$f.tmp" && mv -f "install/.$f.tmp" "install/$f"
done
rm -f "$CSS/bin/linux64/game_shader_generic_p2paint.so"     # old location; CS:S never loads it from there
rm -rf "$CSS/custom/p2paint"                                 # (also drops files older versions shipped)
mkdir -p "$CSS/custom/p2paint" "$CSS/materials/paintpics"
cp -r game/materials game/resource "$CSS/custom/p2paint/"
echo "installed: $(pwd)/install + $CSS/custom/p2paint"
echo "preload:   $(pwd)/install/libp2paint.so  (add it to LD_PRELOAD in your launch options)"
