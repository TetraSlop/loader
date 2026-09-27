# UI and portrait texture pack

`tools/build_ui_pack.py` handles the **181 UI/background textures and 30
portraits** in the original exports. Card illustrations and the 42 font
atlases have separate workflows. Every output retains its original image
coordinates, animation-frame placement and exactly four times its width and
height. The original game archives are read only.

The [second UI pass](title-ui-refinement.md) supersedes the first-pass treatment
below for the title background/wordmark, version/copyright, PlayOnline controls,
status icons/counters and glove cursor. Those now receive the reviewed complete
D title pass or explicit vector replacements. The original descriptions below
remain useful for the other assets and the initial comparison history.

Run with the prepared local chaiNNer environment:

```powershell
out/hd-work/venv/Scripts/python.exe -m pip install chainner_ext==0.3.10
out/hd-work/venv/Scripts/python.exe tools/build_ui_pack.py
```

The extension is chaiNNer's own standard resize backend, pinned to the version
requested by this checkout. It stays in the offline environment. Python 3.13
may build its wheel locally if no compatible published wheel is available.

The separate output is `out/texture-lab/variants/ui-complete/`. Its `pack.json`
uses the runtime replacement manifest format: source and output SHA256 hashes,
image ID, relative output path, original dimensions and physical dimensions.
Per-image method descriptions distinguish reconstructed illustrations from
faithfully resized lettering. Duplicate originals receive identical outputs,
which is required by the runtime's source-image hash matching.

## Treatment by asset

| Content | Treatment |
| --- | --- |
| All 30 illustrated portraits | User-selected D: RealESRGAN x4plus through chaiNNer's inference backend. |
| Card frames, parchment panels, decorative arrows, moogle, illustrated effects | D for RGB; original alpha coverage enlarged separately. |
| Stat digits, P/M/X/A, control symbols, cursor poses, results/event lettering | chaiNNer Lanczos RGB with mild 20% sharpening; Catmull-Rom enlargement of the source alpha. No neural interpretation of characters. |
| Eight main-menu labels | Recreated with locally installed Arial Bold Italic, fitted to original ink bounds, with a gold gradient and shadow. Exact original wording and atlas positions retained. |
| Eight direction arrows and yellow 0–9 stat digits | Reviewed vector triangles and Arial Black digits, fitted to original ink bounds and palette inside native sprite cells. |
| Title-menu horse illustration and portrait-frame decoration | D on the explicitly audited decorative rectangles; deterministic treatment elsewhere. |
| Main-menu room background | Join all six original tiles, apply D once, split back into original tile positions. |
| Title, game background, four/five-row game boards | Same joined processing, with original baked-in logos/lettering protected from D. |

The deterministic treatment preserves existing glyph shapes and wording. It
does not recover vector outlines or claim new font detail. The main-menu
labels are a separately reviewed exception: Play, VS. COM, Card Shop, Auction,
Player Data, Rankings, Options and Back use vector glyphs. Their label and
shadow bounds are checked, and the builder verifies that no pixels outside
the eight audited rectangles change. The system font is read locally and its
hash is recorded; no font file is copied into the pack. `--menu-font` selects
an explicit alternative, although its appearance would need separate review.
The three cursor poses retain their coordinates and original silhouettes; the noisy AI cursor
sample is not selected. Partial-opacity symbols retain their original alpha
range, including the unusual status symbols whose maximum coverage is below
255. Alpha is processed separately from RGB to avoid dark transparent fringes.

## Main-menu native rectangles

The main-menu constructor at `+14899D..+148B69` initializes seven 20-byte
records on the stack. `+148E14..+148EA0` passes each to `+18EB40`, which copies
the source X/Y from record offsets `+10/+12` into object fields `+E0/+E2`.
Both native label draws (`+18EDD4` and `+18EEF2`) use the same source rectangle
of 120 by 24 pixels. The CPU menu title at `+BD25D..+BD286` samples VS. COM
from X=0, Y=72, with the same dimensions.

| Label | Native source rectangle (left, top, right, bottom) |
| --- | --- |
| Play | 120, 48, 240, 72 |
| VS. COM | 0, 72, 120, 96 |
| Card Shop | 120, 72, 240, 96 |
| Auction | 120, 96, 240, 120 |
| Player Data | 120, 144, 240, 168 |
| Rankings | 120, 168, 240, 192 |
| Options | 120, 192, 240, 216 |
| Back | 120, 216, 240, 240 |

Reconstruction changes only those rectangles. The original gold ink bounds
determine each new label's size and position; its one-source-pixel shadow must
fit with at least a one-source-pixel gutter inside the native rectangle.
The native code still controls input, destination geometry, enabled state,
color modulation and hover animation. In the alternate draw branch it uses
the same UVs but reduces the destination and RGB multiplier from 128 to 80;
no separately baked hover/disabled glyphs exist in these rectangles.

## Stat digit and direction-arrow rectangles

`tools/ui_glyphs.py` contains both reviewed vector passes. In `on_card1`,
the eight arrows are N, NE, E, SE, S, SW, W, NW. Native code at
`+B4F58..+B4FC0` samples `(16*direction + 1, 89)` with dimensions 16 × 16.
That one-pixel origin offset is intentional. Native yellow digits use
`(24*digit, 104)` with dimensions 24 × 24, confirmed by the hundreds and
tens draws at `+B5CEA..+B5D3C` and `+B5E67..+B5EB9`.

The builder uses exact original palette colors: arrows RGB(232,159,25),
bevel (192,95,1), outline (11,11,11); yellow digits (246,217,57), dark
outline (20,10,3). All 18 reconstructed sprites retain their original ink
positions. Visible outlines stay inside each native rectangle with a gutter;
the audit lists their bounds. Pixels outside the 18 rectangles are checked
against the baseline, and regeneration was verified to be idempotent. The
small white stat characters, handwritten score digits, and COMBO mark are
not modified by this pass.

The yellow digits use the locally installed `ariblk.ttf` (Arial Black), with
its SHA256 recorded as `stat_font_sha256`; `--stat-font` can explicitly select
another font. No font files are bundled. The original ink coordinates and
individual digit widths remain authoritative, so font advances cannot alter
TM's numerical layout.

## Verified background arrangement

The menu, title and game backgrounds form a **640 × 448** image: columns of
256, 256 and 128 pixels; rows of 256 and 192 pixels. Their names end in 0–5
in row order. Native `back0` drawing begins at TM `+D3DD4`; the rectangles at
`+D3DF9`, `+D3E5D`, `+D3EC3` and following calls confirm this arrangement.
`menu_bg0` starts the corresponding sequence at `+D419C`.

The two boards form **360 × 448** images, with 256/104-pixel columns and
256/192-pixel rows. Native table `+233A74` selects `masu_4a` through `masu_4d`
or `masu_5a` through `masu_5d`; drawing at `+D44A0..+D4752` confirms the grid.
Their on-screen origin is X=171. Native quirks are retained: the top-right
board tile starts sampling at source X=1, and some title/menu draws sample
194 rows from a 192-row PNG plus the original GPU padding. Replacement loading
must continue scaling the entire original GPU allocation, including padding.

Tile files are split from one processed image, not upscaled independently.
The tool reconstructs every group and checks byte-for-byte equality with that
single result, including the split boundaries. The game/title/board logos use
audited source rectangles and an outside feather; the protected lettering
itself uses only the deterministic method. Native atlas rectangles are unchanged.

## Review and remaining limits

`review/` contains before/after sheets for the five joined backgrounds,
representative portraits, title-menu labels, stat digits, cursor, control
atlases and copyright lettering. `audit.json` records coverage and review
items. The originals were visually audited as contact sheets under
`out/texture-lab/ui-audit/`.

Actual in-game appearance at 1440p and 4K still needs user inspection. These
are offline texture comparisons; no game window, mouse or keyboard was
controlled. The font-atlas work is separate. Handwritten scores, custom title
and event wordmarks, the hand cursor and the three copper combat symbols
still retain filtered original contours. They are covered by the pack but
can remain visibly pixelated at large sizes. Experimental copper-symbol
tracing was rejected because it distorted shapes or retained wobbly edges.
Those assets would need a separate matched reconstruction pass to become
fully smooth; the installed pack does not claim that work is finished.
