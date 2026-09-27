# Title, version, PlayOnline and cursor refinement

The second UI pass targets the elements the user found blurry in the first HD
pack. It uses the existing 4x PNG replacement path; no additional graphics hook
or loader rebuild is needed. Original archives and native sprite coordinates
remain unchanged.

## Title artwork

The six `gW201/1001/title_0` through `title_5` images are joined into their native
640x448 composition before processing with the selected D model,
`RealESRGAN_x4plus.pth`. The first pack restored the original filtered logo over
the model output, which left the large title visibly pixelated. A reviewed full
D pass now includes the title, its reflection and the Final Fantasy IX subtitle.
The wording and composition were inspected against the original; this is AI
reconstruction, not recovered higher-resolution original art.

The result is split at the same original tile boundaries. The builder checks
exact reassembly against the single processed image. Its background cache now
checks the composed-image recipe even when individual tile hashes are cached,
so a changed protected region cannot silently keep an old result.

## Small text and icons

The version is drawn from `on_title`, not the regular font atlases. It therefore
follows **Enhanced cards and artwork**, as do the other assets below.

- `on_title`: copyright, `Ver.`, the decimal point and individual version digits
  are rasterized from Georgia Bold inside their original sampled rectangles.
  The version number remains assembled by the game; it is not baked into one
  new image. Unused beta and suffix artwork stays intact.
- `friend4`: PlayOnline and Friend List labels retain their native state variants
  and colors, with clean Arial Bold lettering and F1/F2/F3 keycaps.
- `friend2`: the bottom-right chat and mail icons use smooth reconstructed shapes
  inside their original cells.
- `friend3`: the status counters use clean Arial digits. The native unknown-count
  cell stays intact.
- `cursor`: three separately drawn glove poses use editable cubic curves and a
  restrained white bevel. The original 40x40 cells, pose order, ink extents and
  transparent gutters stay in place. The native pointer coordinates are unchanged.

The lettering and icon shapes are deliberate substitutions, not claims about
the original typefaces. Font files are read from Windows only during the offline
build; their hashes are recorded and the files themselves are not bundled.
Duplicate source PNGs receive identical replacements. The builders assert that
unaudited atlas pixels remain unchanged.

## Reproduce and use

With the existing extracted texture lab and offline environment:

```powershell
out/hd-work/venv/Scripts/python.exe tools/build_ui_pack.py
out/hd-work/venv/Scripts/python.exe tools/assemble_texture_pack.py out/texture-lab/cards-real-esrgan out/texture-lab/variants/ui-complete out/texture-lab/variants/fonts-vector-4x-v2 --output runtime/textures-hd-v2
```

The assembler requires a new output directory and checks every source/output
hash and dimension. Select `texture-pack=textures-hd-v2/pack.json` in the local
`tmloader.ini`, then relaunch through the normal `tmserver/Play.cmd`. An already
running client keeps its existing pack. F10's artwork toggle still compares
against the original assets; font-atlas settings are independent.

Offline comparisons and actual pack validation are recorded in
[VALIDATION.md](VALIDATION.md). Full native appearance remains a user-run check;
this work does not launch the game or control the desktop.
