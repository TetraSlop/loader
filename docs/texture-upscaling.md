# Offline texture upscaling with chaiNNer

The loader now supports separate HD packs, with an immediate **F10 > Use HD
textures** toggle. The prepared local pack covers all 503 exported images:
250 cards, 30 portraits, 42 font atlases and 181 UI/background images. D is
used for illustrations; verified Latin characters, main-menu labels, large
card digits and direction arrows have vector replacements. Other lettering
and functional symbols are conservatively filtered. Samples and third-party
tools live in ignored `out/`; the installed complete pack lives in ignored
`runtime/textures-hd/`. The original
`runtime/data`, DLLs, and saved game are untouched.
No game, desktop UI, Electron window, or local web server is started by this
workflow.

## Install a reviewed pack

```powershell
out/hd-work/venv/Scripts/python.exe tools/assemble_texture_pack.py out/texture-lab/cards-real-esrgan out/texture-lab/variants/ui-complete out/texture-lab/variants/fonts-vector-4x-v2 --output runtime/textures-hd
build/RelWithDebInfo/tmloader.exe --config tmloader.ini --texture-pack textures-hd/pack.json --check
```

The assembler verifies originals, replacement hashes and dimensions before
creating a new pack directory. It refuses an existing destination.
For a later revision, choose a new directory with `--output` and point the
loader's `texture-pack` setting at its `pack.json`. The default manifest is
`textures/pack.json`, relative to the configured runtime. No Python or models
are required by the native loader after generating the pack. Set
`texture-pack=textures-hd/pack.json` in the local INI to select the complete pack.

Run the usual sibling `tmserver/Play.cmd` yourself, then open the collection or
shop and compare **F10 > Use HD textures** on/off. The loader logs indexed
images, successful uploads and fallback reasons. The native game was not
launched during development; this remains the user-run integration check.
The original archives remain untouched; see the [font findings](ui-texture-upscaling.md),
[UI treatment](ui-asset-plan.md) and [remaining visual review](HD_REVIEW.md).

## What is in the game

The seven `runtime/data/gW*.dat` archives contain **503 ordinary PNG images**,
including 250 card creatures and 42 font atlases. Their archive records are
32 bytes: type at `+0`, a 12-byte NUL-terminated name at `+4`, relative payload
offset at `+16`, length at `+20`, flags at `+24`, and unpacked length at `+28`.
Type `0x8000` contains another directory; type `0x4000` contains a PNG. Texture
records have flags 2 and equal stored/unpacked lengths. PNG data includes up
to 15 alignment bytes after IEND. The first entry's payload offset is also
the directory table length. Offsets are relative to the current directory.

`tools/extract_textures.py` checks bounds, safe names, overlap, PNG chunk CRCs
and image decoding before exporting. It trims only the archive padding;
original PNG bytes and palette transparency are preserved. Its manifest
records archive and PNG SHA256s, original dimensions, offsets and alpha range.
Texture IDs include archive, group and name, e.g. `gW001/1000/cad00000`, because
names such as `chat1` occur in multiple archives/groups with different art.

## Reproduce the sample

Run these commands from the `tmloader` repository in PowerShell. The tested
environment is Python 3.13 on Windows x64. Only a repository-local virtual
environment is changed; the runtime loader remains x86 and gains no Python
dependency.

```powershell
python -m venv out/hd-work/venv
git clone https://github.com/chaiNNer-org/chaiNNer.git out/hd-work/chaiNNer
git -C out/hd-work/chaiNNer checkout b3ca7ff586d4071dbe8601bc4000493560503f21
out/hd-work/venv/Scripts/python.exe -m pip install torch==2.7.0 torchvision==0.22.0 --index-url https://download.pytorch.org/whl/cpu
out/hd-work/venv/Scripts/python.exe -m pip install -r tools/texture_requirements.txt
```

The selected default is **D**, the general 4x model. Download the official
[RealESRGAN_x4plus model](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth)
to `out/hd-work/models/RealESRGAN_x4plus.pth` and check it with
`Get-FileHash -Algorithm SHA256`. Expected hash:

```
4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1
```

Then:

```powershell
out/hd-work/venv/Scripts/python.exe tools/extract_textures.py
out/hd-work/venv/Scripts/python.exe tools/upscale_textures.py
python -m unittest discover -s tests -p test_texture_tools.py -v
```

The upscaler calls the actual upstream
`nodes.impl.pytorch.auto_split.pytorch_auto_split` implementation used by
chaiNNer's PyTorch Upscale Image node, with 128-pixel tiles and FP32. It does
not reproduce the network or copy chaiNNer source into tmloader. Source is
pinned to the tested revision because this Python API is internal. The
separate [chaiNNer CLI](https://github.com/chaiNNer-org/chaiNNer/blob/main/docs/05--CLI.md)
can run saved `.chn` graphs; this project uses the smaller backend adapter
to avoid installing/launching Electron or changing its application settings.

RGB is extended into fully transparent areas before inference to reduce
black fringes. Original alpha is enlarged with bilinear interpolation and
reattached afterwards; the neural model never changes the silhouette mask.
The entire image/atlas keeps its layout and exactly 4x dimensions. No atlas
repacking occurs. `--device cuda` is supported if a compatible CUDA build of
PyTorch is deliberately installed in this environment; CPU is the tested
default and was sufficient for the sample.

Outputs:

- `out/texture-lab/manifest.json`: all original image identities and hashes.
- `out/texture-lab/original/`: 503 lossless PNG exports.
- `out/texture-lab/variants/real-esrgan/`: four enhanced images and `pack.json`, recording
  model/source/output hashes, tool versions, original and physical dimensions.
- `out/texture-lab/variants/real-esrgan/comparisons/`: original bilinear scaling on the left and
  chaiNNer output on the right, at identical displayed dimensions.

Default sample IDs are `gW001/1000/cad00000`, `gW001/1002/cad00020`,
`gW201/1000/menu_bg0`, and `gW000/0090/chat1`. Pass different IDs as positional
arguments to evaluate another small batch. Each run rewrites the sample
manifest to describe that run; old images may remain on disk but are not
listed as part of the new pack.

For separate model trials, use `--output variants/<name>` (relative to
`--lab`). The PNGs, `pack.json`, and that variant's `comparisons/` are written
there, preserving the initial anime sample and other variants. The original export
directory cannot be selected as an output destination.

Two additional official models have been evaluated on both small card samples:

| Variant | Official model | Release | SHA256 |
| --- | --- | --- | --- |
| `real-esrgan` | [RealESRGAN_x4plus.pth](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth) | v0.1.0 | `4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1` |
| `real-esrnet` | [RealESRNet_x4plus.pth](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.1/RealESRNet_x4plus.pth) | v0.1.1 | `a820b9bde89a874d7599d545567308ce6c128fc8754a53208eda016d40aa81df` |

Both are 23-block 4x RGB networks, loaded through the same chaiNNer backend.
The general GAN model retains more surface variation than the anime sample;
ESRNet is smoother and less grainy. Both still interpret ambiguous source
pixels, so inspect the actual comparison rather than assuming either is an
automatic improvement. Alpha handling is identical between all three models.

The downloaded file sizes match the official GitHub release assets; hashes
above were calculated locally. Those older releases do not publish a GitHub
asset digest. Their URLs are also referenced by the upstream
[inference script](https://github.com/xinntao/Real-ESRGAN/blob/master/inference_realesrgan.py).
The local `out/hd-work/models/general-models.provenance.json` records release
URLs, asset IDs, sizes, hashes and the upstream BSD-3-Clause license link.
Each prepared variant's `pack.json` includes that provenance.

Example after downloading a model to `out/hd-work/models/`:

```powershell
out/hd-work/venv/Scripts/python.exe tools/upscale_textures.py --model out/hd-work/models/RealESRGAN_x4plus.pth --output variants/real-esrgan gW001/1000/cad00000 gW001/1002/cad00020
```

The six-way review also uses the earlier anime sample in `sample-4x`. To
reproduce it, download the official
[anime 6B model](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth)
(SHA256 `f872d837d3c90ed2e05227bed711af5671a6fd1c9f7d7e91c911a61f155e99da`)
and pass `--model out/hd-work/models/RealESRGAN_x4plus_anime_6B.pth --output sample-4x`.
After all three models' samples are ready, generate the six-way visual review:

```powershell
out/hd-work/venv/Scripts/python.exe tools/compare_texture_variants.py
```

`out/texture-lab/variant-review/` contains labeled comparison sheets, separate
RGBA outputs and `review.json`. The panels compare bilinear, lightly sharpened
Lanczos, anime AI, general GAN, ESRNet, and a 25% general-GAN blend. The review
tool verifies source/sample hashes, dimensions and matching alpha first.

The user preferred panel **D** (general `RealESRGAN_x4plus`). See
[FFIX upscaling research](ffix-upscaling-research.md) for the Moguri
developers' workflows and a direct comparison with zePilot's original
FFIX-trained model. D remains the preferred offline baseline.

## Assessment and limits

The sample model noticeably cleans card and UI edges, but smooths texture
and sometimes interprets small details as flat painted shapes. It is a
candidate for visual review, not a claim that invented detail is original.
Model selection remains open. The alpha mask deliberately stays closer to
the original and can therefore look softer than the sharpened RGB edges.

Background images are split into adjacent tiles (`menu_bg0`, `menu_bg1`,
etc.). Before a production batch, reconstruct those backgrounds in their
native arrangement, upscale with shared context, then split at the scaled
boundaries. Processing each tile independently can create seams. UI atlases
also need a component-aware padding pass if the model bleeds details between
nearby icons. The single background tile and chat atlas here are evaluation
samples, not a complete polished pack.

Fonts `gW000/0090/font_00` through `font_41` are deliberately excluded from
AI processing. They are 256x256 bitmap atlases, including Latin, Japanese,
symbols and placeholder sheets. A replacement needs the native character
mapping, cell bounds, baseline, advance and coverage convention preserved.
The matching original typeface has not been identified. The font workflow now
filters all 42 atlases cell by cell and substitutes 95 verified ASCII mappings
using fitted MS UI Gothic vector glyphs. Every other cell keeps its original
character. This is a deliberate substitute face, not a claim to have recovered
the original outlines; the conservative all-original-glyph pack is retained.

## Loader integration contract

Use the archive/group/name identity and the original PNG hash, then load a
separate replacement only when the source matches. Do not rewrite archives.
The source PNG hash selects the replacement (identical PNGs shared between
archive identities share the same visual result). Missing, mismatched or
invalid replacements fall back to the original texture.

The native loader obtains texture dimensions using D3D8 `GetLevelDesc(0)` at
TM `+17C568..+17C5AB`, then stores width/height floats in its texture entry at
`+0xC/+0x10`. Named fonts are registered at `+17BC34..+17BC7E`; the same
dimensions become shared font metadata. A simple larger `CreateTexture`
allocation or PNG substitution may therefore change UV/layout calculations.
Preserve the **original logical dimensions** in the game-facing metadata,
while the actual GPU texture has larger physical dimensions. Normalized UVs
can then address the same art regions at 4x detail. Physical padding to a
power of two, edge texels, texture pitch and any native copies into atlases
must be traced before choosing the exact interception point.

The implemented adapter guards seven native PNG-upload call sites, always
returns the original resource, and attaches a managed HD companion through
D3D8 private data. Each draw temporarily binds that companion and restores
the original before the native renderer resumes. Its metadata, texture cache,
font dimensions and normal release/reset paths therefore keep seeing original
resources. HD GPU dimensions include scaled original power-of-two padding;
the native FILTER_NONE upload preserves image placement and transparent borders.
See [the traced load path](texture-load-path.md) for the ABI and offsets.

## Provenance and licensing

- [chaiNNer](https://github.com/chaiNNer-org/chaiNNer) revision
  `b3ca7ff586d4071dbe8601bc4000493560503f21`, GPL-3.0; its complete license
  remains in the external checkout. This development tool is not linked into
  or shipped with the native loader.
- [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN), official v0.2.2.4
  anime 6B weights, with the hash above. The upstream project carries the
  [BSD 3-Clause license](https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE);
  a local copy is beside the downloaded weights. No model is committed.
- Spandrel loads the model; PyTorch performs inference. Exact versions are
  written into every generated pack manifest.
- Game artwork is exported from the user's existing local assets. Exports
  and enhanced artwork are ignored development outputs, not repository or
  redistribution content. Processing them does not grant an artwork license.

Validation on 2026-09-27: 8 synthetic parser/path regression tests pass; all
503 source PNGs pass chunk CRCs and decode; four model runs succeed at exact
4x dimensions. Original archive hashes remain unchanged. No native game
launch or GUI interaction was used.
