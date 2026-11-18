# p2paint

Paint for Counter-Strike: Source (64-bit, Linux and Windows). Any colour, 8 sizes, pictures and GIFs, your sprays, on any surface
(walls, props, displacements, clips, triggers). Paint is saved on the server per map.

Two parts:

- **client mod** (`src/`): draws the paint and the `!paint` menu (colours, custom colour, images, backups,
  copy paint from another player, clear). `libp2paint.so` (+ the library it loads) on Linux, `p2paint.dll` on
  Windows, same sources.
- **server plugin** (`plugin/paint_3.sp`): saves paint, sends it to players. Players without the client mod
  still see classic paint decals and get the plugin's numbered menu.

The name is historical: it started as Portal 2 gel.

## Use

- `!paint` or `p2paint_menu`: the menu
- hold `+paint` (e.g. `bind v +paint`) and left click / drag to paint, mouse wheel = size
- hold `+erase` to erase, or set PAINT KEY to RECOLOR / ERASE in the menu
- IMAGES > ADD A FILE: turn a picture or GIF into paint (needs [MareTF](https://github.com/craftablescience/MareTF) installed)

## Build the client (Linux)

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

The mod writes its own materials and menu font into `cstrike/materials/p2paint` and `cstrike/resource/p2paint`
when the game starts.

## Build the client (Windows)

Cross-compiled on Linux with clang-cl and [xwin](https://github.com/Jake-Shadle/xwin) (the MSVC CRT / SDK in
`~/.local/share/xwin`, or set `XWIN`), plus mingw-w64 for the small loader:

```sh
git clone --depth 1 https://github.com/ValveSoftware/source-sdk-2013 third_party/source-sdk-2013
win/build.sh          # from a path without spaces -> win/build/out/p2paint.dll + wininject.exe
```

`p2paint.dll` starts itself when anything loads it (LoadLibrary). Without another loader, `wininject.exe` starts
the game and loads it:

```
wininject.exe "C:\...\Counter-Strike Source\cstrike_win64.exe" "C:\...\p2paint.dll" -game cstrike -insecure
```

For IMAGES > ADD A FILE put `maretf.exe` (MareTF's Windows release) next to `p2paint.dll`. Tested under Proton so
far, not on Windows itself.

## Engine patches

At load the mod patches two spots of `engine.so` / `engine.dll` in memory, nothing on disk: the decal pool goes from 2048 to
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
