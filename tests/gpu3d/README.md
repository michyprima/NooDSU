# 3D replay ROM

Draws one frame's polygons on a real DS with exactly the coordinates a game gave them, and captures what the 3D engine really
outputs, to compare pixel by pixel with NooDS U. The polygons are from Zelda: Spirit Tracks' opening tunnel, where NooDS U draws a
bright line along the ground that a real DS doesn't. Textures aren't replayed, so polygons keep only their vertex colors.

## Running it

Copy `build/noodsu-3d.nds` to a flashcart and start it. After a moment it shows the first scene.

| Button | What it does |
|---|---|
| A | Next scene |
| B | Zoom in 4 times, on the first streak if there is one |

| Scene | Drawn with |
|---|---|
| 1 As in the game | The game's anti-aliasing, edge marking and fog |
| 2 Anti-aliasing off | The same without anti-aliasing |
| 3 Only anti-aliasing | No edge marking or fog |
| 4 Black over white | Only the black lining, in front of a plain white wall, with anti-aliasing |

`STREAKS` counts lines at least 4 pixels long and one pixel high that are bright between dark rows. The third line says where the
images were saved: `NOODSU-3D.BIN` in the root of the card, or the save chip it used. If it says `NOT SAVED`, take photos of each scene, zoomed too.

In NooDS U on the Wii U it runs too, and falls back to the save chip, so the captures end up in `noodsu-3d.sav` next to the ROM, in the same format. It captures once at startup, so choose the renderer before starting it.

## Comparing

`compare.py` needs the same file from NooDS U, which is the captures at `0x02200000` in main RAM after 70 frames, in the same format:

```
tests/gpu3d/compare.py noodsu-3d.bin <from NooDS U> <folder for images>
```

## Building it

```
tests/gpu3d/build.sh <any DS ROM, for the logo> [polygon dump]
```

It needs devkitARM and devkitPro's `libfat-gba`. Without a dump, it keeps the current `scene.h`. A dump comes from the Mac harness,
recording each polygon as `gen_scene.py` describes.

## How it replays them

A vertex at the origin takes the last row of the position matrix as its clip coordinates, unrounded, so each vertex loads a matrix
with the game's X, Y, Z and W there, under an identity projection. Clipping and everything after then run on the DS as usual. Polygons are drawn separately rather than in strips, with culling off, since the list only has ones that weren't culled.
