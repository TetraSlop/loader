# HD presentation: prepared setup and remaining review

The complete local pack covers all 503 exported images (443 unique originals).
It combines D-upscaled card art, portraits and backgrounds with vector Latin
text, eight main-menu labels, large yellow card digits and direction arrows.
The second pass adds clean version/copyright glyphs, PlayOnline toolbar/status
icons and counters, three smooth glove poses, and a sharper complete title image.
Other assets retain their original shapes through conservative filtering.
No game archives, native DLLs or saved-game data are rewritten.

The F10 selector offers **1920x1440**, **2880x2160** and **3840x2880** rendering.
Keeping original 4:3 proportions avoids widening cards and letters. Its active
resolution is reported separately from the next-launch preference. The second
asset pass preserves current window/rendering preferences and selects
`textures-hd-v2/pack.json` relative to the runtime in the local INI. Earlier packs
remain available. Relaunch to load the newly selected pack.

## Review when convenient

Launch the normal sibling `tmserver/Play.cmd`. No launch or desktop control
was performed while preparing these changes.

1. Compare the main menu, collection and shop with **F10 > Artwork & text**
   artwork/font toggles enabled and disabled independently. This takes effect immediately. The panel should show
   images loaded, and the log should identify successful HD uploads.
2. Start a CPU game and check card art, direction arrows, yellow scores, text
   colors/shadows, hover alignment, cursor poses and scene transitions.
3. Review the replacement Latin face and gold menu labels for style preference.
   They retain original placement and spacing, but are newly rasterized faces.
   The conservative original-glyph font pack remains in the texture lab.
4. Use F12 to save full-resolution PNGs for comparison; inspect the Screenshots
   tab for the saved path or any failure. Automatic resolution and manual choices
   take effect on relaunch. Check resizing/pointer alignment with Windows display
   scaling enabled, since the game and settings threads now opt into DPI awareness.

## Known visual limits

- Handwritten score digits, custom event/result wordmarks and several decorative
  symbols still show source pixel contours at large sizes.
  Aggressive filters that changed their shapes were rejected.
- Title lettering now uses the reviewed D output. Small title/toolbar lettering,
  status icons and glove contours are reconstructed substitutes; their new style
  should be checked in the native UI. See [the refinement notes](title-ui-refinement.md).
- Japanese and other unmapped font glyphs receive cell-isolated filtering, not
  guessed character substitutions. Additional character mapping is separate work.
- AI illustration detail is reconstructed; it is not recovered original art.

Headless Direct3D8 checks verify actual render dimensions, geometry, HD texture
binding, alpha, font UV sampling, tint/shadow composition, reset and fallback.
Those checks and offline asset reviews pass; full native appearance remains
for the user-run check above. See [validation](VALIDATION.md) for the evidence.
