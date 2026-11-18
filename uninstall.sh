#!/bin/sh
# Remove every p2paint file from the local CS:S install (take libp2paint.so out of your launch options
# too). Pictures made in game (materials/paintpics) stay.
cd "$(dirname "$0")"
CSS="${CSS:-$HOME/.local/share/Steam/steamapps/common/Counter-Strike Source/cstrike}"
rm -rf install
rm -f "$CSS/bin/linux64/game_shader_generic_p2paint.so"
rm -rf "$CSS/custom/p2paint" "$CSS/materials/p2paint" "$CSS/resource/p2paint"
echo "p2paint removed"
