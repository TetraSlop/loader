# HD review

The HD texture pack covers all 503 texture entries (443 unique original
images). It combines upscaled card art, portraits and backgrounds with vector
replacements for Latin text, the eight main-menu labels, large yellow card
digits, direction arrows, version and copyright text, PlayOnline toolbar icons
and counters, and the three glove cursor poses. Other assets are
conservatively filtered versions of the originals. Game archives, DLLs and
saves are never rewritten.

The pack is selected with `texture-pack=textures/pack.json`. The settings
panel's **Artwork & text** tab toggles enhanced artwork and enhanced fonts
independently and takes effect immediately. Rendering resolution takes effect
at the next launch; keep **original 4:3 proportions** enabled to avoid
stretching cards and text.

## Review checklist

1. Compare the main menu, collection and shop with the artwork and font
   toggles on and off. The panel shows the number of images loaded, and the
   log lists successful HD uploads.
2. Play a CPU game and check card art, direction arrows, yellow scores, text
   colors and shadows, hover alignment, cursor poses and scene transitions.
3. Review the replacement Latin typeface and gold menu labels. They keep the
   original placement and spacing but are newly rasterized. The conservative
   original-glyph font pack remains available as an alternative.
4. Save F12 screenshots at full resolution for comparison, and check resizing
   and pointer alignment with Windows display scaling above 100%.

## Known limits

- Handwritten score digits, custom event and result wordmarks and several
  decorative symbols still show source pixel contours at large sizes.
  Aggressive filters that distorted their shapes were rejected.
- Small title and toolbar lettering, status icons and glove contours are
  reconstructed substitutes, not the original typefaces; see
  [title and UI refinement](title-ui-refinement.md).
- Japanese and other unmapped font glyphs are filtered per cell, not replaced.
  Additional character mapping is separate work.
- AI-upscaled illustration detail is reconstructed, not recovered original
  art.

Headless tests cover render dimensions, geometry, HD binding, alpha, font UV
sampling, tint and shadow composition, reset and fallback; see
[testing](testing.md).
