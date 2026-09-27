# HD presentation: prepared setup and remaining review

The complete local pack covers all 503 exported images (443 unique originals).
It combines D-upscaled card art, portraits and backgrounds with vector Latin
text, eight main-menu labels, large yellow card digits and direction arrows.
Other assets retain their original shapes through conservative filtering.
No game archives, native DLLs or saved-game data are rewritten.

The prepared account uses **2880x2160 rendering**, HD textures enabled and
original **4:3 proportions**. When maximized or fullscreen, this fills a 2160p
display's height without widening cards or letters. The existing window size
and window/fullscreen choice are preserved. The F10 selector also offers 1920x1440 and
3840x2880 supersampling. Its active resolution is reported separately from
the next-launch preference. The INI selects `textures-hd/pack.json` relative
to the runtime; the earlier `textures/` pack remains available.

## Review when convenient

Launch the normal sibling `tmserver/Play.cmd`. No launch or desktop control
was performed while preparing these changes.

1. Compare the main menu, collection and shop with **F10 > Use HD textures**
   enabled and disabled. This takes effect immediately. The panel should show
   images loaded, and the log should identify successful HD uploads.
2. Start a CPU game and check card art, direction arrows, yellow scores, text
   colors/shadows, hover alignment, cursor poses and scene transitions.
3. Review the replacement Latin face and gold menu labels for style preference.
   They retain original placement and spacing, but are newly rasterized faces.
   The conservative original-glyph font pack remains in the texture lab.

## Known visual limits

- Handwritten score digits, custom title/event/result wordmarks, the hand cursor
  and several decorative symbols still show source pixel contours at large sizes.
  Aggressive filters that changed their shapes were rejected.
- Japanese and other unmapped font glyphs receive cell-isolated filtering, not
  guessed character substitutions. Additional character mapping is separate work.
- AI illustration detail is reconstructed; it is not recovered original art.

Headless Direct3D8 checks verify actual render dimensions, geometry, HD texture
binding, alpha, font UV sampling, tint/shadow composition, reset and fallback.
Those checks and offline asset reviews pass; full native appearance remains
for the user-run check above. See [validation](VALIDATION.md) for the evidence.
