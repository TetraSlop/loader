# UI texture upscaling

D works well for illustrated UI assets, but font glyphs and small functional
symbols need separate treatment. The 503 original textures are 250 `cad*`
creatures, 30 `fac*` portraits, 42 `font_*` atlases and 181 other UI and
background images; many names repeat across themes and screens. This page
covers UI samples, the verified font atlas layout and the two font packs.

Addresses with a leading `+` are RVAs relative to the game DLL image base
`0x10000000`; bare addresses are absolute.

## UI samples

Seven UI assets without text were upscaled with D:

```powershell
out/hd-work/venv/Scripts/python.exe tools/upscale_textures.py --model out/hd-work/models/RealESRGAN_x4plus.pth --output variants/ui-real-esrgan gW000/0090/base_r gW000/0090/cursor gW000/0090/arrow gW000/0090/mes_type gW101/1021/badge gW201/1000/hn gW201/1005/mog
```

| Asset ID | Size | Assessment |
|---|---|---|
| `gW000/0090/base_r` | 88x104 | Textured card frame; good D candidate |
| `gW000/0090/arrow` | 224x64 | Animated decorative arrows; keep frame positions and alpha |
| `gW000/0090/mes_type` | 256x128 | Message and status pictograms; evaluate at display size |
| `gW101/1021/badge` | 160x32 | Small category symbols; visibly cleaner |
| `gW201/1000/hn` | 179x61 | Text-free parchment header; good D candidate |
| `gW201/1005/mog` | 72x72 | Moogle illustration; good D candidate |
| `gW000/0090/cursor` | 120x40 | Three glove poses; D adds noise, so the cursor is redrawn instead |

`base_r`, `cursor`, `arrow` and `mes_type` in `gW000/0090` are byte-identical
to their `gW000/0092` counterparts, so hash-based replacement covers both
themes. Other names need checking individually.

## Assets to split or recreate

- The font atlases (`gW000/0090/font_00` to `font_41`) never go through an
  illustration model.
- Whole-atlas AI processing can alter text in the mixed text and control
  atlases: `window`, `window2`, `on_card1`, `on_card2`, `point`, `get_card`,
  `menu`, `head`, `ttl_menu`, `game_st`, result banners and event atlases.
  Preserve or regenerate those regions using verified source rectangles.
- For functional symbols (arrows, crosses, button markers, stat digits,
  P/M/X/A) the exact silhouette matters more than texture; use vector shapes
  or pixel reconstruction. Cursors must keep their hotspot and frame layout.
- Join backgrounds and board tiles before upscaling and split them at the
  scaled boundaries to avoid seams.

Keep logical coordinates, frame order, dimensions and empty gutters. Filter
individual components with padding when neighbors would bleed into each other.
Keep unusual partial opacity, such as `onstatus` (maximum alpha 237).

## Font atlas layout

Every font PNG is 256x256 with white RGB and coverage in alpha. The background
alpha is 1, not 0 (`font_01` has 52,775 such pixels); other values are
quantized, such as 24, 64, 128, 194 and 255.

The grid is 14 by 14 glyph slots (196 per atlas) on an 18-pixel pitch, not
16x16. The renderer at `+184872..+184893` divides the glyph index
by 196 and at `+184A68..+184AE9` computes:

```text
atlas     = glyph_index // 196
slot      = glyph_index % 196
column    = slot % 14
row       = slot // 14
source_x  = 18 * column + 2
source_y  = 18 * row + 1
UV extent = 17 x 17 pixels
```

Coordinates are divided by the shared atlas dimensions at `102369C4` and
`102369C8`; the constant at `1020873C` is 17.0. Registration at
`+17BC34..+17BC7E` parses `font_XX` and fills the font table at
`102FC0B8 + XX*4`. Replacements must keep the 256x256 logical size even when
the GPU texture is 1024x1024.

Native instructions `+1849EF..+1849FD` make the quad width equal to the
requested font size while sampling 17 source pixels.

## ASCII mapping and spacing

The renderer reads an eight-byte metrics record at
`10236F28 + character_code*8`:

| Offset | Use |
|---|---|
| +0 (signed) | Post-draw advance, times font size / 16 |
| +1 (signed) | Pre-draw offset, times font size / 16 |
| +2 (signed) | Left pair class, tested against the previous right class |
| +3 (signed) | Right pair class, kept for the next glyph |
| +4/+5 | High and low Shift-JIS bytes |
| +6/+7 | Zero in the printable ASCII entries; meaning unknown |

The pre-offset moves the pen before drawing, and neighboring pair classes of
opposite sign subtract two more pre-offset units (`+1848BE..+184995`,
`+1850CA..+1850DF`).

| Input | Advance | Pre-offset | Classes | Glyph |
|---|---|---|---|---|
| `A` | 14 | -1 | -1 / -1 | `8260` |
| `W` | 15 | 0 | 1 / 1 | `8276` |
| `i` | 10 | -5 | 0 / 0 | `8289` |
| `0` | 13 | -3 | 0 / 0 | `824F` |

The helper at `+185130` converts the Shift-JIS pair to a JIS row and column,
and the renderer computes `glyph_index = jis_row*94 + jis_column - 0xC3F`.
All 95 printable ASCII records were run through that helper under emulation
and matched this conversion, so the code-to-cell map does not rely on OCR. The half-width, double-byte and special `0x85` paths (including style
bit `0x200`) have not been mapped; unmapped glyphs keep their original shapes.

## Conservative font pack

`tools/build_font_pack.py` builds all 42 atlases as a texture pack with
256x256 logical and 1024x1024 physical size.

- Each glyph cell (18x18, starting at `(1 + 18*column, 18*row)`) is filtered
  independently with Lanczos, padded so neighbors cannot bleed in.
- The alpha floor of 1 is removed for filtering and restored afterwards.
- A bounded curve `q + 0.20*q*(1-q)*(2*q-1)` firms edges without moving cells;
  it changes alpha by at most 4.89/255 and keeps 0, 0.5 and 1 fixed.
- RGB stays white. Atlas borders outside the grid are enlarged by nearest
  sampling.

The builder validates source hashes, checks all 8,232 cells and preserves the
822 empty ones (including the empty `font_40`). Filtering changes total cell
coverage by a factor of 0.983 to 1.168; printable ASCII averages +3.3%, the
most being the thin underscore at +13%. Glyph widths and spacing do not
change.

```powershell
out/hd-work/venv/Scripts/python.exe tools/build_font_pack.py --self-check
out/hd-work/venv/Scripts/python.exe tools/build_font_pack.py --output variants/fonts-4x
```

Only Pillow and NumPy are needed. The 95 verified ASCII records are embedded
in the tool. Its `review/` output compares original and enhanced text at 1440p
and 2160p quad sizes, shows punctuation and Japanese detail at 4x, and records
hashes and glyph placements. Previews use a software bilinear sampler without
native shadows or outlines.

## Vector Latin font pack

The HD pack uses MS UI Gothic Regular for the 95 mapped ASCII glyphs. It is a
substitute; the original typeface is unidentified.
`tools/build_vector_font_pack.py` renders each glyph from its native JIS
character (the game's ASCII `A` draws the full-width JIS `A`), rasterizes it at
256px, crops to its ink and fits it into the original glyph's occupied box at
4x. The cell grid, UVs, baseline, advances, pair classes and logical 256px size
are unchanged.

The font was chosen by ranking installed Windows fonts by normalized alpha
error after fitting 62 alphanumeric glyphs into the same boxes and
downsampling to the original grid; the builder now renders only the chosen
font:

| Candidate | Input | Shape error |
|---|---|---|
| MS UI Gothic Regular | Native JIS | 0.17893 |
| MS Gothic / MS PGothic Regular | Native JIS | 0.17917 |
| Arial Bold | ASCII | 0.21723 |
| Microsoft Sans Serif Regular | ASCII | 0.23086 |
| Arial Regular | ASCII | 0.23998 |
| Segoe UI Regular | ASCII | 0.27964 |
| Tahoma Regular | ASCII | 0.28146 |
| Verdana Regular | ASCII | 0.29253 |

```powershell
out/hd-work/venv/Scripts/python.exe tools/build_vector_font_pack.py
```

The tool needs the conservative pack and the original exports. Only the 95
mapped cells in `font_00` and `font_01` change (one is the blank space); every
other pixel matches the conservative pack, and `font_02` to `font_41` are
copied unchanged. The font is read from `C:/Windows/Fonts/msgothic.ttc`
(collection index 1; `--font` for another path) and never distributed; its
hash is recorded, and another font revision may produce different output.

The tool validates the 95 Shift-JIS/Unicode mappings against the helper
results, all metrics, glyph boxes, unchanged pixels, the 40 untouched atlases
and all PNG hashes, including JIS row-boundary punctuation.

## Remaining font work

- The loader draws font replacements with the native 256x256 metadata,
  original UVs, layout metrics, texture-stage state and per-draw colors. A
  synthetic test covers the 18px cell and 17-texel UV layout, alpha floor,
  tint and shadow pass at 1440p and 2160p; it does not cover every native
  text effect.
- The conservative pack remains the fallback if the substitute typeface is
  not wanted.
- Unmapped characters must not be inferred by OCR. The half-width,
  double-byte and `0x85` paths need their own mapping and spacing work.
- Bitmap wordmarks and control labels outside the font atlases are handled by
  the [UI asset plan](ui-asset-plan.md).
