# UI, icons and fonts

The selected D model is useful for illustrated UI assets, but font glyphs and
small symbolic controls need separate treatment. The original 503 textures
contain 250 `cad*` creature images, 30 `fac*` portraits, 42 `font_*` atlases and
181 other UI/background images. Many names repeat across themes or screens.
This assessment uses lossless exports and original decoded client code; it
does not launch the game or modify runtime assets.

## UI sample pack

`out/texture-lab/variants/ui-real-esrgan/pack.json` describes seven exact 4x
outputs made with the same general RealESRGAN model as D, through chaiNNer.
Model/source/output hashes and logical/physical sizes are recorded. Original
alpha is bilinearly enlarged separately, exactly as in the card samples.
None of the seven sources contains a font alphabet or a text label.

| Asset ID | Original size | Assessment |
| --- | --- | --- |
| `gW000/0090/base_r` | 88x104 | Textured card frame; useful D candidate. |
| `gW000/0090/arrow` | 224x64 | Animated decorative arrows; keep all frame positions and alpha. |
| `gW000/0090/mes_type` | 256x128 | Message/status pictograms; evaluate at actual displayed size. |
| `gW101/1021/badge` | 160x32 | Small achievement/category symbols; visibly cleaner candidate. |
| `gW201/1000/hn` | 179x61 | Text-free parchment/header strip; useful D candidate. |
| `gW201/1005/mog` | 72x72 | Moogle illustration; useful D candidate. |
| `gW000/0090/cursor` | 120x40 | Three glove poses; D adds noisy detail. Hold for redraw, rather than installing automatically. |

The first four `gW000/0090` sample PNGs (`base_r`, `cursor`, `arrow`,
`mes_type`) are byte-identical to their `gW000/0092` counterparts. A
source-PNG-hash replacement can therefore cover both appearances without
guessing a theme. Other names should still be checked individually.

Reproduce the samples using the prepared local environment:

```powershell
out/hd-work/venv/Scripts/python.exe tools/upscale_textures.py --model out/hd-work/models/RealESRGAN_x4plus.pth --output variants/ui-real-esrgan gW000/0090/base_r gW000/0090/cursor gW000/0090/arrow gW000/0090/mes_type gW101/1021/badge gW201/1000/hn gW201/1005/mog
```

The pack's `comparisons/` directory contains correctly alpha-composited
before/after previews. `out/texture-lab/ui-review/original-atlases.png` shows
representative originals. Seven sample output hashes, exact dimensions and
original-alpha equality were verified headlessly.

## Assets to split or recreate

- **Font atlases:** all `gW000/0090/font_00` through `font_41`. Do not send them
  through a general illustration model.
- **Mixed text/control atlases:** `window`, `window2`, `on_card1`, `on_card2`,
  `point`, `get_card`, `menu`, `head`, `ttl_menu`, `game_st`, result banners and
  event atlases contain digits, words or stat symbols beside artwork. Whole
  atlas AI processing can silently change text. Preserve those regions or
  regenerate them separately using verified source rectangles.
- **Simple functional symbols:** arrows, crosses, button markers, card stat
  digits and P/M/X/A marks benefit from matching vector shapes or deliberate
  pixel reconstruction. Their exact silhouette matters more than inferred
  surface texture. Cursor geometry must retain its hotspot and frame layout.
- **Backgrounds and board tiles:** combine neighboring pieces before
  upscaling, then split at scaled original boundaries to avoid seams.

Keep original logical coordinates, frame order and dimensions. Do not pack
icons more tightly or move them into a new atlas. Maintain empty gutters;
filter individual components with padding when neighbors would contaminate
each other. The same source-alpha policy also preserves unusual partial
opacity, such as `onstatus` (maximum alpha 237).

## Verified font layout

Every font PNG is 256x256. RGB is white; glyph coverage is stored in alpha.
The background alpha is **1**, not zero. For example, `font_01` contains
52,775 white pixels with alpha 1, and other pixels use quantized coverage
such as 24, 64, 128, 194 and 255. Preserve this convention until native
blending/alpha-test behavior has been accounted for.

The grid is **14 columns by 14 rows**, containing 196 glyph slots per atlas,
with an **18-pixel pitch**. It is not a 16x16-cell atlas. Original renderer
instructions at TM `+184872..+184893` divide the glyph index by 196; its
remainder selects the slot. At `+184A68..+184AE9`:

```
atlas = glyph_index // 196
slot = glyph_index % 196
column = slot % 14
row = slot // 14
source_x = 18 * column + 2
source_y = 18 * row + 1
sampled UV extent = 17 x 17 pixels
```

The sampled coordinates are divided by the shared original atlas dimensions
at globals `102369C4` and `102369C8`. The constant at `1020873C` is 17.0.
Texture registration `+17BC34..+17BC7E` parses `font_XX` and fills the font
texture table at `102FC0B8 + XX*4`. Replacement texture objects must preserve
the native 256x256 logical dimensions even when their GPU images are
1024x1024. Keep the normalized original UVs and the scaled source layout.

## Verified printable ASCII mapping and spacing

The renderer uses an eight-byte metrics record at
`10236F28 + character_code*8`. For its proportional ASCII path:

| Offset | Observed use |
| --- | --- |
| +0 signed byte | Post-draw horizontal advance, multiplied by requested font size / 16. |
| +1 signed byte | Pre-draw horizontal offset, multiplied by requested font size / 16. |
| +2 signed byte | Left pair class; tested against the previous right class. |
| +3 signed byte | Right pair class retained for the next glyph. |
| +4/+5 | High/low Shift-JIS bytes for the glyph. |
| +6/+7 | Zero in the printable ASCII entries inspected; meaning not established. |

These are the native operations, not a claim that the first field alone is
the complete conventional text advance. The pre-offset changes the running
position before drawing. Opposite-sign neighboring pair classes subtract
two additional pre-offset units. Evidence is `+1848BE..+184995` and
`+1850CA..+1850DF`.

Examples from the original table:

| Input | Post advance | Pre offset | Left/right class | Encoded glyph |
| --- | --- | --- | --- | --- |
| `A` | 14 | -1 | -1 / -1 | `8260` |
| `W` | 15 | 0 | 1 / 1 | `8276` |
| `i` | 10 | -5 | 0 / 0 | `8289` |
| `0` | 13 | -3 | 0 / 0 | `824F` |

The helper at `+185130` converts the encoded pair to JIS row/column; the
renderer calculates `glyph_index = jis_row*94 + jis_column - 0xC3F`.
All **95 printable ASCII records** were fed into this original helper in
Unicorn, and the outputs matched the derived conversion. The resulting
exact code-to-atlas/cell map and metrics are saved in
`out/texture-lab/ui-review/font-mapping.json`. This does not depend on OCR.

There are additional half-width, double-byte and special `0x85` paths,
including behavior controlled by style bit `0x200`. Those branches require
their own mapping/spacing checks before replacing the full Japanese/symbol
repertoire. Keep original glyphs for unverified mappings.

The decoded analysis image used here has SHA256
`422e46ab2ad6fd02db570a2384424dd1540b197e1ccd7e3da2645ca2b40b1879`.
Addresses above are relative to image base `0x10000000` where a leading `+`
is shown. This decoded image is an analysis artifact, not a loadable DLL.

## Complete conservative font pack

`tools/build_font_pack.py` now builds **all 42 atlases** as a compatible
version-1 texture pack. The prepared result is
`out/texture-lab/variants/fonts-4x/pack.json`. All logical sizes remain
256x256; physical images are 1024x1024. The 42 PNGs occupy 23,814,898 bytes
on disk (168 MiB decoded at four bytes per pixel if all are resident).
The pack's SHA256 is
`5b759eb7fab55cd60b050b39fc9de7616ccf335be80651e39251798183e160e9`.

The selected default filters each original glyph cell independently using
Lanczos. Each disjoint 18x18 region starts at `(1+18*column, 18*row)`;
zero-coverage padding is added before filtering so adjacent glyphs cannot
enter the filter's support. The original one-pixel alpha floor is removed
for calculation and restored afterwards. A small bounded coverage curve
`q + 0.20*q*(1-q)*(2*q-1)` firms the resampled edges without moving cells,
dilating outlines or replacing characters. This curve changes a resampled
alpha value by at most 4.89/255 and fixes coverage 0, 0.5 and 1. RGB stays
white everywhere. Atlas borders outside the grid retain their original
values, enlarged by nearest sampling.

The builder validates original PNG hashes, checks every one of 8,232 cells,
preserves all 822 empty cells (including completely empty `font_40`), and
records source/output SHA256 values and per-atlas coverage statistics.
Lanczos filtering changes antialiasing: total cell coverage ratios range
from 0.9828 to 1.1680. Printable ASCII averages +3.27%; the largest ASCII
increase is the thin underscore at +13.04%. These are coverage changes,
not glyph-width or spacing changes. Keeping tiny punctuation visible is
preferable to discarding it through threshold-based contour tracing.

Representative original PNG hashes are:

| Atlas | Source SHA256 |
| --- | --- |
| `font_00` | `b0cc7427fdde12e82ea56b2a129c181034e56be97bb31508407d211c61729ca6` |
| `font_01` | `e36d24743cd8e2ff3943d529403f2f806097faf9b419046c34569d3de10ced00` |
| `font_40` | `3f3c59e866a82635f11b173483d9ea7e565cf980f9c00810fba9cd08b45997e0` |

Reproduce from the lossless exports, selecting a new destination when the
default already exists:

```powershell
out/hd-work/venv/Scripts/python.exe tools/build_font_pack.py --self-check
out/hd-work/venv/Scripts/python.exe tools/build_font_pack.py --output variants/fonts-4x
```

Only Pillow and NumPy are used. No AI model, installed system font or game
process is needed. The verified 95 ASCII index/metric records are embedded
in the tool, so rendering previews does not depend on an untracked mapping
file. Pack generation preserves all glyphs, including Japanese and symbols
whose character-to-cell mappings have not been independently decoded.

The builder also saves:

- `review/font-text-1440p.png`: original and enhanced sample text at 48px
  glyph quads, corresponding to a 16-unit font in a 1440px-tall 4:3 viewport.
- `review/font-text-2160p.png`: the same comparison at 72px quads for 4K.
- `review/font-atlas-detail.png`: punctuation and Japanese glyph regions,
  displayed at exact 4x scale.
- `review/review.json`: source code-image hash, preview hashes and identical
  before/after glyph placements using the verified metrics and pair rules.

Native instructions `+1849EF..+1849FD` make the quad width equal to the
requested font size while sampling 17 source pixels. The comparisons retain
that distinction; enlarging the UV crop directly into a 17-unit quad would
give misleading text spacing. Previews use a software bilinear sampler and
omit native shadows/outlines; they are not screenshots or a GPU substitute.
Visual inspection shows firmer curved edges and clearer punctuation, with
unchanged glyph identity. This is a moderate improvement to the original
bitmap font, not newly rasterized vector text.

## Selected sharper Latin pack

The prepared HD pack now selects a deliberate Latin typeface substitute:
**MS UI Gothic Regular**, rendered from the installed Windows collection.
It is substantially sharper than enlarging the original small bitmap font.
This choice follows comparison against available Arial, Tahoma, Verdana,
Microsoft Sans Serif, Segoe UI, MS Gothic/MS PGothic and Yu Gothic families.
It is **not an identification of the original typeface**.

`tools/build_vector_font_pack.py` renders each of the 95 verified printable
ASCII mappings from its native JIS character, rather than guessing from
the glyph image. For example, the game's ASCII `A` selects the full-width
JIS `A` glyph. The tool also compares regular ASCII inputs while ranking
candidates. Each vector glyph is drawn at 256px, cropped to its ink, then
fitted into that original glyph's exact occupied box at 4x resolution.
The native 18px cell grid, UVs, baseline placement, advances, pair classes,
blank space and logical 256px texture dimensions remain unchanged.

The ranking compares 62 alphanumeric glyphs after fitting them into the
same boxes, averaging normalized alpha error after downsampling onto the
original pixel grid. Lower scores mean closer raster shapes under this
specific comparison, not proof of font identity:

| Candidate | Input | Shape error |
| --- | --- | --- |
| MS UI Gothic Regular | Native JIS | 0.17893 |
| MS Gothic / MS PGothic Regular | Native JIS | 0.17917 |
| Arial Bold | ASCII | 0.21723 |
| Microsoft Sans Serif Regular | ASCII | 0.23086 |
| Arial Regular | ASCII | 0.23998 |
| Segoe UI Regular | ASCII | 0.27964 |
| Tahoma Regular | ASCII | 0.28146 |
| Verdana Regular | ASCII | 0.29253 |

The selected artifact is
`out/texture-lab/variants/fonts-vector-4x-v2/pack.json`, SHA256
`07233a52d4d5104333e341a1de0e4016af82494fecea538c70e0541ee108889c`.
It contains 42 compatible replacement PNGs totaling 23,754,367 bytes.
Only the 95 mapped cells in `font_00` and `font_01` are substituted; one is
the original blank space. **Every unmapped pixel is identical to the
conservative pack**, and atlas files `font_02` through `font_41` are copied
byte-for-byte. The conservative pack remains available as the alternative
that retains every original glyph shape.

The local font source is `C:/Windows/Fonts/msgothic.ttc`, collection index
1, SHA256
`4bde3e6392b96910fb59094c6c1a4dbfae18fee78d0bf13dc30616837c4f95db`.
The tools do not copy or distribute this font binary. Raster outputs stay
in the ignored local texture lab, and the manifest records font provenance.
A different installed font revision may produce different raster hashes.

```powershell
out/hd-work/venv/Scripts/python.exe tools/build_vector_font_pack.py --output variants/fonts-vector-4x-v2
```

Existing output directories are preserved; choose a new `--output` for
another run. The generator needs the previously built conservative pack
and original lossless exports. It rejects unavailable glyphs and ranks only
candidates capable of drawing every verified character.

The output's `review/font-latin-2160p.png` compares original bilinear,
conservative and vector Latin text at 4K-equivalent size with identical
native layout. `review/installed-font-candidates.png` shows the available
families, and `review/review.json` records rankings and exact placements.
These were visually reviewed; the selected vector column has clean edges
and counters while retaining a similar overall style. The software preview
does not include game shadows, outlines or other text effects.

Independent checks in `review/validation.json` verify all 95 native
Shift-JIS/Unicode mappings against the earlier original-helper results,
all original metrics, exact occupied glyph boxes, unchanged unmapped pixels,
the 40 untouched atlas files, and all 42 PNG hashes. The builder also checks
JIS row-boundary punctuation, which prevents an ASCII-only sample from
hiding conversion errors. No font metrics or native code are patched.

## Remaining font work

The generated pack can be merged with the illustrated texture pack using
`tools/assemble_texture_pack.py`, preserving its original source hashes.
Loader draw-time replacement must retain native 256x256 metadata, original
UVs, all layout metrics, texture-stage state and per-draw text colors. A
font-specific offscreen check now compares the native 18px cell/17-texel UV
layout on 16-unit glyph quads, alpha-floor-1 masks, per-vertex tint/opacity
and a separate shadow pass. At 1440p and 2160p, substitution plus the scale
shader matches independently scaled/directly bound HD rendering exactly.
This is a synthetic drawing-contract check, not a native game screenshot or
validation of every native text effect.

The original typeface remains unidentified. The selected substitute changes
letter design inside verified cells; user preference can be reviewed later
against the conservative fallback. Do not infer unmapped characters or
replace the full Japanese/symbol repertoire based on OCR. The half-width,
double-byte and special `0x85` paths still need their own mapping/spacing
checks before further character substitutions. Bitmap wordmarks and control
labels outside the font atlases remain separate work.
