# Title and UI refinement

A second UI pass replaces the elements that remained blurry in the first HD
pack: the title artwork, version and copyright text, PlayOnline controls,
status icons and counters, and the glove cursor. It uses the existing 4x
replacement path; native sprite coordinates and original archives are
unchanged.

## Title artwork

The six tiles `gW201/1001/title_0` to `title_5` are joined into their native
640x448 composition and processed once with D (`RealESRGAN_x4plus.pth`). The
full pass includes the title, its reflection and the Final Fantasy IX
subtitle; an earlier approach that restored the original filtered logo over
the model output left the title visibly pixelated. The result is AI
reconstruction, not recovered original art.

The image is split at the original tile boundaries, and the builder checks
that the tiles reassemble exactly. Its cache checks the composed-image recipe
as well as individual tile hashes, so a changed protected region cannot keep
a stale result.

## Small text and icons

The version is drawn from `on_title`, not the font atlases, so it follows the
**Enhanced cards and artwork** toggle like the other assets below.

| Asset | Treatment |
|---|---|
| `on_title` | Copyright, `Ver.`, the decimal point and individual version digits rasterized from Georgia Bold in their original rectangles. The game still assembles the version number; unused beta and suffix artwork is intact. |
| `friend4` | PlayOnline and Friend List labels in Arial Bold with F1/F2/F3 keycaps, keeping their native state variants and colors. |
| `friend2` | Smooth reconstructed chat and mail icons in their original cells. |
| `friend3` | Status counters in Arial digits; the unknown-count cell is intact. |
| `cursor` | Three glove poses drawn as cubic curves with a light white bevel, keeping the 40x40 cells, pose order, ink extents, transparent gutters and pointer coordinates. |

These are deliberate substitutes, not the original typefaces. Fonts are read
from Windows only during the offline build; their hashes are recorded and the
font files are not bundled. `title_glyphs.py` rebuilds 35 audited regions;
the version sprite layout comes from executing the original constructor
`+1480F0` through its sprite registrations. Duplicate source PNGs receive
identical replacements, and the builders assert that unaudited atlas pixels
are unchanged.

## Building

With an extracted texture lab and the offline environment from
[texture upscaling](texture-upscaling.md):

```powershell
out/hd-work/venv/Scripts/python.exe tools/build_ui_pack.py
out/hd-work/venv/Scripts/python.exe tools/assemble_texture_pack.py --output out/textures-hd
```

The assembler needs a new output directory (without `--output` it rebuilds
`assets/runtime/textures`) and checks every source and output hash and dimension. Point `texture-pack` at the new `pack.json` and relaunch;
a running client keeps its current pack. The artwork toggle still compares
against the originals, and the font toggle is independent.
