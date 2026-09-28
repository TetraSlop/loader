# FFIX upscaling research

The HD pack's illustrations use **D**, the general `RealESRGAN_x4plus` model
(see [texture upscaling](texture-upscaling.md)). This page summarizes how the
Final Fantasy IX Moguri mod approached upscaling, and how its author-published
model compares with D on Tetra Master cards.

## Moguri's workflow

[Moguri's official site](https://sites.google.com/view/moguri-mod/) describes
ESRGAN and Stable Diffusion followed by manual polishing, including redrawing
layer boundaries. It is an edited asset pipeline, not one model applied
uniformly.

The developers' posts describe several generations of the workflow:

- [2019 world-map thread](https://forums.qhimm.com/index.php?topic=19178.0):
  manga109, zePilot's FF9 model and waifu2x, plus manual texturing. Every
  available model was rendered and useful results were combined by hand.
- [November 2019 update, reply 33](https://forums.qhimm.com/index.php?topic=19225.25):
  a de-dither pass followed by BOX. The extra detail also added color noise,
  so original colors were retained and detail was blended selectively into
  zePilot's renders, mainly for caves and dark backgrounds.
- [March 2024 notes](https://www.reddit.com/r/FinalFantasyIX/comments/1b7tcq0/):
  RealisticVision 6.0 (SD 1.5), blended lineart ControlNets, FFIX-10,
  dreamshaper and more_details LoRAs, a fixed seed, zePilot's upscaler and
  Ultimate SD Upscale, with denoise 0.15-0.25 and partial restoration of the
  original colors. These were settings for backgrounds, not a universal preset.
- [May 2024 preview](https://www.reddit.com/r/FinalFantasyIX/comments/1chjulc/):
  several ESRGAN results, de-dithering, Stable Diffusion in ComfyUI, and manual
  composition and retouching.

zePilot published [FFIX_Upscale_Scripts](https://github.com/ZePilOt/FFIX_Upscale_Scripts)
(MIT), including `GAN_Trained/ff9_trained.pth`. Its
[upscale script](https://github.com/ZePilOt/FFIX_Upscale_Scripts/blob/master/4-Upscale.py)
uses that checkpoint with a 4x, 23-block RRDB network. It is the historical
background model, not the complete Moguri pipeline.

## FFIX model comparison

The checkpoint from commit `a02c7992cfe0f2421f271634777bd13869da6d36` is
66,900,514 bytes with SHA256
`c1540cfa1ef9a51b1be7e38cdb9f38c20844f64bebc390d72d7ec79f0aa80e0a`. Both card
samples were run through the same chaiNNer backend as D:

```powershell
out/hd-work/venv/Scripts/python.exe tools/upscale_textures.py --model out/hd-work/models/zepilot_ff9_trained.pth --output variants/zepilot-ff9 gW001/1000/cad00000 gW001/1002/cad00020
```

This is a direct model comparison with the same sources and alpha, without
Moguri's de-dithering or manual edits. The FFIX model produces harder outlines
and segmented shading, especially on the green creature. Some details are
clearer, but it is not a clear improvement over D, so D remains the baseline.

## Takeaways

- Compare any candidate model against D on the same 72x96 card sources, at the
  same 4x size and with identical alpha.
- Select and blend by asset type. A model tuned for backgrounds is not
  automatically better for small transparent creature illustrations.
- De-dither only assets that are actually dithered; a blanket pass erases the
  small details worth keeping.
- The March 2024 diffusion settings are not suitable for cards as-is: card
  silhouettes, eyes, weapons and symbols need stricter fidelity than
  decorative backgrounds.
- Moguri's [credits](https://sites.google.com/view/moguri-mod/about) name
  Teaito's Alexandria font, now in Memoria. That is a lead for font research,
  not evidence that it matches the PlayOnline game's font.

No drop-in PlayOnline texture pack or card-specific Moguri model exists. The
shared subject matter does not imply matching texture layouts, resolutions or
coverage.
