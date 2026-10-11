# UI asset plan

`tools/build_ui_pack.py` builds replacements for the 181 UI and background
textures and 30 portraits. Card illustrations and the 42 font atlases have
their own workflows. Every output keeps its original image coordinates and
animation-frame placement at exactly 4x size, and the game archives are only
read.

The title background and wordmark, version and copyright text, PlayOnline
controls, status icons and counters, and the glove cursor are handled by a
later pass described in [title and UI refinement](title-ui-refinement.md).

## Building

```powershell
out/hd-work/venv/Scripts/python.exe tools/build_ui_pack.py
```

It needs `chainner_ext`, chaiNNer's resize backend, which
`tools/texture_requirements.txt` pins to the version the checkout requests;
Python 3.13 may build the wheel locally. An interrupted run resumes from the
outputs whose hashes still match.

The output is `out/texture-lab/variants/ui-complete/`. Its `pack.json` uses the
runtime manifest format: source and output SHA256, image ID, output path, and
logical and physical dimensions. Per-image method notes distinguish
reconstructed illustrations from resized lettering. Duplicate originals get
identical outputs, as the runtime's hash matching requires.

## Treatment by asset

| Content | Treatment |
|---|---|
| All 30 portraits | D (RealESRGAN x4plus) through chaiNNer's backend |
| Card frames, parchment panels, decorative arrows, moogle, illustrated effects | D for RGB; original alpha enlarged separately |
| Stat digits, P/M/X/A, control symbols, cursor poses, result and event lettering | chaiNNer Lanczos RGB with 20% sharpening and Catmull-Rom alpha; no neural processing of characters |
| Eight main-menu labels | Arial Bold Italic, fitted to the original ink bounds, with a gold gradient and shadow |
| Eight direction arrows and yellow 0-9 stat digits | Vector triangles and Arial Black digits fitted to the original ink bounds and palette |
| Title-menu horse and portrait-frame decoration | D on audited decorative rectangles; deterministic treatment elsewhere |
| Main-menu room background | All six tiles joined, D applied once, split back into tiles |
| Title, game background, four- and five-row boards | Same joined processing, with baked-in logos and lettering protected from D |

The deterministic treatment keeps the existing glyph shapes and wording and
recovers no vector outlines. The menu labels (Play, VS. COM, Card Shop,
Auction, Player Data, Rankings, Options, Back) are the exception. The builder
checks their label and shadow bounds and verifies that no pixel outside the
eight audited rectangles changes. System fonts are read locally and their
hashes recorded; no font file is copied into the pack. The files are listed
in the builder's `FONTS` table and read from `--fonts` (`C:/Windows/Fonts`).

Partial-opacity symbols keep their original alpha range, including status
symbols whose maximum coverage is below 255. Alpha is processed separately
from RGB to avoid dark fringes.

## Main-menu label rectangles

The main-menu constructor at `+14899D..+148B69` initializes seven 20-byte
records. `+148E14..+148EA0` passes each to `+18EB40`, which copies the source
X/Y from record offsets `+10`/`+12` into object fields `+E0`/`+E2`. Both label
draws (`+18EDD4`, `+18EEF2`) use a 120x24 source rectangle. The CPU menu title
at `+BD25D..+BD286` samples VS. COM from (0, 72) with the same size.

| Label | Source rectangle (left, top, right, bottom) |
|---|---|
| Play | 120, 48, 240, 72 |
| VS. COM | 0, 72, 120, 96 |
| Card Shop | 120, 72, 240, 96 |
| Auction | 120, 96, 240, 120 |
| Player Data | 120, 144, 240, 168 |
| Rankings | 120, 168, 240, 192 |
| Options | 120, 192, 240, 216 |
| Back | 120, 216, 240, 240 |

Only these rectangles change. Each label's size and position follow the
original gold ink bounds, and its one-pixel shadow must leave at least a
one-pixel gutter inside the rectangle. The game still controls input,
destination geometry, enabled state, color modulation and hover animation. Its
alternate draw branch uses the same UVs with a smaller destination and an RGB
multiplier of 80 instead of 128; there are no separate hover or disabled
glyphs.

## Stat digits and direction arrows

`tools/ui_glyphs.py` draws both. In `on_card1`, the arrows are N, NE, E, SE, S,
SW, W, NW. Native code at `+B4F58..+B4FC0` samples `(16*direction + 1, 89)`
at 16x16; the one-pixel X offset is intentional. Yellow digits are sampled at
`(24*digit, 104)` at 24x24, confirmed by the hundreds and tens draws at
`+B5CEA..+B5D3C` and `+B5E67..+B5EB9`.

| Sprite | Fill | Bevel | Outline |
|---|---|---|---|
| Arrows | RGB(232,159,25) | RGB(192,95,1) | RGB(11,11,11) |
| Yellow digits | RGB(246,217,57) | | RGB(20,10,3) |

All 18 sprites keep their original ink positions, visible outlines stay inside
their rectangles with a gutter, pixels outside the 18 rectangles are checked
against the original, and regeneration is idempotent. Small white stat
characters, handwritten score digits and the COMBO mark are not changed.

Digits use the installed `ariblk.ttf` (Arial Black), with its SHA256 recorded
as `stat_font_sha256`. The original ink coordinates and digit widths are
authoritative, so font advances cannot change the game's number layout.

## Background arrangement

Menu, title and game backgrounds form a 640x448 image: columns of 256, 256
and 128 pixels and rows of 256 and 192 pixels, with names ending 0-5 in row
order. `back0` drawing starts at `+D3DD4`; the rectangles at `+D3DF9`,
`+D3E5D`, `+D3EC3` and following calls confirm the arrangement. `menu_bg0`
starts at `+D419C`.

The boards form 360x448 images with 256/104-pixel columns and 256/192-pixel
rows. Table `+233A74` selects `masu_4a` to `masu_4d` or `masu_5a` to `masu_5d`,
and drawing at `+D44A0..+D4752` confirms the grid, with the on-screen origin at
X=171. Two native quirks are kept: the top-right board tile samples from source
X=1, and some title and menu draws sample 194 rows from a 192-row PNG plus GPU
padding. Replacements therefore scale the whole original GPU allocation,
including padding.

Tiles are split from one processed image and the builder checks byte-for-byte
equality with that image, including the split boundaries. Logos use audited
source rectangles with an outside feather, and the protected lettering uses
only the deterministic method.

## Review output and limits

`review/` contains before and after sheets for the joined backgrounds,
representative portraits, menu labels, stat digits, cursor, control atlases
and copyright text, and `audit.json` records coverage and review items.

Handwritten scores, custom title and event wordmarks and the three copper
combat symbols keep filtered original contours and can look pixelated at large
sizes. Tracing the copper symbols distorted their shapes and was rejected;
smoothing them needs a separate reconstruction pass.
