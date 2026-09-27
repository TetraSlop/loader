# FFIX fan-upscaling references

Research date: 2026-09-27. The user preferred **D**, the general
`RealESRGAN_x4plus` output, over the other five card treatments. Keep that
output as the comparison baseline; it is not installed in the native game.

## What Moguri actually used

[Moguri's official site](https://sites.google.com/view/moguri-mod/) describes
ESRGAN and Stable Diffusion, followed by manual polishing. It also documents
manual redrawing of layer boundaries. This is an edited asset pipeline, not
one model applied uniformly to every image.

The developer posts distinguish several generations of the workflow:

- In [Snouz's 2019 world-map thread](https://forums.qhimm.com/index.php?topic=19178.0),
  the named tools/models include manga109, zePilot's FF9 model and waifu2x,
  plus manual texturing. He describes rendering every available model,
  selecting the useful results and combining them manually.
- In a [November 2019 progress update, reply 33](https://forums.qhimm.com/index.php?topic=19225.25),
  Snouz describes a de-dither pass followed by BOX. The additional texture
  detail also introduced color noise, so he retained existing colors and
  selectively blended detail into zePilot's renders. This was particularly
  useful for cave and dark background surfaces, not a card-specific recipe.
- In [March 2024 development notes](https://www.reddit.com/r/FinalFantasyIX/comments/1b7tcq0/),
  Snouz describes experimenting with RealisticVision 6.0 (SD 1.5), blended
  lineart ControlNets, FFIX-10/dreamshaper/more_details LoRAs, a fixed seed,
  zePilot's upscaler, and Ultimate SD Upscale. Reported denoise was 0.15-0.25,
  with partial restoration of original colors afterwards. These were settings
  for an evolving background workflow, not a released universal preset.
- His [May 2024 preview](https://www.reddit.com/r/FinalFantasyIX/comments/1chjulc/)
  confirms the broader approach: several ESRGAN results, de-dithering,
  Stable Diffusion in ComfyUI, and manual composition/retouching.

The original author also published
[FFIX_Upscale_Scripts](https://github.com/ZePilOt/FFIX_Upscale_Scripts),
including `GAN_Trained/ff9_trained.pth`. The repository's
[upscale script](https://github.com/ZePilOt/FFIX_Upscale_Scripts/blob/master/4-Upscale.py)
explicitly uses that checkpoint with a 4x, 23-block RRDB network. This gives
us an author-published model to test, rather than an unidentified mirror or
an approximation based on the name. It is the historical background model,
not the complete Moguri 9 pipeline.

## Local FFIX-model trial

Downloaded the author's checkpoint from commit
`a02c7992cfe0f2421f271634777bd13869da6d36`. The file is 66,900,514 bytes;
its Git blob identity matches the author's repository. Local SHA256:
`c1540cfa1ef9a51b1be7e38cdb9f38c20844f64bebc390d72d7ec79f0aa80e0a`.
The repository carries an MIT license; its local copy and full provenance
are stored beside the model in ignored `out/hd-work/models/`.

Both card samples run through the same chaiNNer backend as D. Reproduce with:

```powershell
out/hd-work/venv/Scripts/python.exe tools/upscale_textures.py --model out/hd-work/models/zepilot_ff9_trained.pth --output variants/zepilot-ff9 gW001/1000/cad00000 gW001/1002/cad00020
```

Results and source/output hashes are in
`out/texture-lab/variants/zepilot-ff9/pack.json`. This is a direct model
comparison with equal source images and alpha, without the historical
de-dither/preprocessing or manual edits. It does not reproduce the appearance
of a complete Moguri release.

The composited comparison is
`out/texture-lab/variant-review/ffix-vs-d.png` (D left, FFIX model right).
Both samples passed source/output hash, exact 4x size and identical-alpha
checks. Visual inspection shows more hard outlines and segmented shading
from the FFIX model, especially on the green creature. It makes some details
clearer but is not an obvious improvement over D. Preserve the user's D
preference pending a broader review.

## Application to this project

Start with D and compare any available FFIX-specific model against the same
two 72x96 card sources, at the same 4x dimensions and with identical alpha.
The useful lesson from Moguri is to select/blend by asset type and inspect
the result. A model tuned for backgrounds is not automatically better at
small transparent creature illustrations.

Only add de-dithering when an original asset actually shows dithering. A
blanket cleanup pass can erase the very small details we are trying to keep.
Do not assume the March 2024 diffusion settings are appropriate for cards:
card silhouettes, eyes, weapons and symbols need a stricter fidelity review
than decorative background detail.

Fonts remain a separate task. Moguri's
[official credits](https://sites.google.com/view/moguri-mod/about) identify
Teaito's Alexandria font, now in Memoria. That is a lead for font research,
not evidence that it matches the PlayOnline game's font or glyph atlas.

No drop-in PlayOnline texture pack or exact card-specific Moguri model was
established by these sources. Use our own extracted images for comparison;
the shared FFIX subject matter alone does not establish matching texture
layouts, source resolutions or coverage.
