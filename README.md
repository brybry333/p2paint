# p2paint

Paint for Counter-Strike: Source on Linux. Any colour, 8 sizes, pictures and GIFs, your sprays, on any surface
(walls, props, displacements, clips, triggers). Paint is saved on the server per map.

Two parts:

- **client mod** (`src/`): draws the paint and the `!paint` menu (colours, custom colour, images, backups,
  copy paint from another player, clear). Linux 64-bit CS:S.
- **server plugin** (`plugin/paint_3.sp`): saves paint, sends it to players. Players without the client mod
  still see classic paint decals and get the plugin's numbered menu.

The name is historical: it started as Portal 2 gel.

## Use

- `!paint` or `p2paint_menu`: the menu
- hold `+paint` (e.g. `bind v +paint`) and left click / drag to paint, mouse wheel = size
- hold `+erase` to erase, or set PAINT KEY to RECOLOR / ERASE in the menu
- IMAGES > ADD A FILE: turn a picture or GIF into paint (needs [MareTF](https://github.com/craftablescience/MareTF) installed)

## Build the client

```sh
git clone --depth 1 https://github.com/ValveSoftware/source-sdk-2013 third_party/source-sdk-2013
make
./install.sh
```

Steam launch options (preloads need `-insecure`):

```
LD_PRELOAD="/path/to/p2paint/install/libp2paint.so:$LD_PRELOAD" %command% -insecure
```

Binaries built on a rolling distro need that distro's glibc. For binaries that run everywhere, build inside the
Steam Runtime (sniper) SDK.

At load the mod patches two spots of `engine.so` in memory, nothing on disk: the decal pool goes from 2048 to
32768, and paint decals keep their size (the engine forces spray decals to one size). If a game update moves
that code the console log says so and paint still works, in one size.

## Server plugin

SourceMod 1.12, `clientprefs`. Compile `plugin/paint_3.sp`, put `paint_3.smx` in `addons/sourcemod/plugins`.
Paint is saved in SQLite (`data/sqlite/superpaint.sq3`).

Players without the client mod need the classic decal materials (`materials/decals/paint/*`) from the original
super paint plugin on your FastDL. Optional: the `ShowTriggers_ClientSees` / `ShowPlayerClips_ClientSees`
natives (paint on triggers / clips while a player sees them).

## Credits

- super paint by gangy: the plugin this one grew from
- Source SDK 2013 by Valve
- Roboto Mono (Apache 2.0, `game/resource/p2paint/RobotoMono-LICENSE.txt`)

## License

GPL-3.0
