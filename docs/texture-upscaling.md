# Texture upscaling

The HD texture pack is produced offline with Python tools and the
[chaiNNer](https://github.com/chaiNNer-org/chaiNNer) inference backend. The
loader only reads the finished pack; it has no Python or model dependency.
Working files live in the ignored `out/` directory, and the original game data
is only read.

The pack covers all 503 exported images: 250 cards, 30 portraits, 42 font
atlases and 181 UI and background images. Illustrations use **D**, the general
`RealESRGAN_x4plus` model. Verified Latin characters, main-menu labels, large
card digits and direction arrows have vector replacements, and remaining
lettering and symbols are conservatively filtered. See
[UI asset plan](ui-asset-plan.md), [UI texture upscaling](ui-texture-upscaling.md)
and [HD review](hd-review.md).

## Game texture archives

The seven `data/gW*.dat` archives contain **503 PNG images**. Archive records
are 32 bytes:

| Offset | Field |
|---|---|
| `+0` | Type: `0x8000` directory, `0x4000` PNG |
| `+4` | 12-byte NUL-terminated name |
| `+16` | Payload offset, relative to the current directory |
| `+20` | Length |
| `+24` | Flags (2 for textures) |
| `+28` | Unpacked length (equal to length for textures) |

The first entry's payload offset is also the directory table length. PNG
payloads include up to 15 alignment bytes after `IEND`.

`tools/extract_textures.py` checks bounds, names, overlap, PNG chunk CRCs and
decoding before exporting. It trims only the alignment padding, so original
PNG bytes and palette transparency are preserved. Its manifest records
archive and PNG SHA256s, dimensions, offsets and alpha range. Texture IDs
include archive, group and name, such as `gW001/1000/cad00000`, because names
like `chat1` repeat across archives with different art.

## Setup

The tested environment is Python 3.13 on Windows x64, in a repository-local
virtual environment:

```powershell
python -m venv out/hd-work/venv
git clone https://github.com/chaiNNer-org/chaiNNer.git out/hd-work/chaiNNer
git -C out/hd-work/chaiNNer checkout b3ca7ff586d4071dbe8601bc4000493560503f21
out/hd-work/venv/Scripts/python.exe -m pip install torch==2.7.0 torchvision==0.22.0 --index-url https://download.pytorch.org/whl/cpu
out/hd-work/venv/Scripts/python.exe -m pip install -r tools/texture_requirements.txt
```

Download [RealESRGAN_x4plus.pth](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth)
to `out/hd-work/models/` and check it with `Get-FileHash -Algorithm SHA256`:

```text
4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1
```

## Rebuilding the pack

The committed pack in `assets/runtime/textures/` is rebuilt from the original
archives in `assets/runtime/data/`. Working files go to `out/texture-lab/`:

```powershell
$py = 'out/hd-work/venv/Scripts/python.exe'
& $py tools/extract_textures.py         # original/ exports and manifest.json
& $py tools/upscale_textures.py         # 250 card illustrations -> cards-real-esrgan/
& $py tools/build_ui_pack.py            # UI and portraits -> variants/ui-complete/
& $py tools/build_font_pack.py --output variants/fonts-4x
& $py tools/build_vector_font_pack.py   # -> variants/fonts-vector-4x-v2/
Remove-Item -Recurse assets/runtime/textures
& $py tools/assemble_texture_pack.py    # -> assets/runtime/textures/
```

With the pinned environment, fonts and model above, the steps reproduce the
committed pack byte for byte. Each builder writes its own `pack.json` with
source and output hashes, model and font hashes and per-image methods, plus
review sheets. The assembler verifies originals, replacement hashes and
dimensions, refuses an existing destination, and writes only the fields the
loader reads.

`upscale_textures.py` calls chaiNNer's own
`nodes.impl.pytorch.auto_split.pytorch_auto_split`, as used by its PyTorch
Upscale Image node, with 128-pixel tiles and FP32. chaiNNer is pinned to the
revision above because this Python API is internal. Using the backend
directly avoids installing or launching the Electron app.

- RGB is extended into fully transparent areas before inference to avoid black
  fringes. The original alpha is enlarged bilinearly and reattached, so the
  model never changes a silhouette.
- Every image keeps its layout and exactly 4x dimensions; atlases are never
  repacked.
- `--device cuda` works with a CUDA build of PyTorch; CPU is the default.
- Pass texture IDs to process other images, and `--model` and `--output`
  (relative to `out/texture-lab`) to try another model without touching the
  card pack. `comparisons/` in the output shows each result beside a bilinear
  enlargement of the original.

## Model choice

| Model | Release | SHA256 |
|---|---|---|
| [RealESRGAN_x4plus.pth](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth) (D, used) | v0.1.0 | `4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1` |
| [RealESRNet_x4plus.pth](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.1/RealESRNet_x4plus.pth) | v0.1.1 | `a820b9bde89a874d7599d545567308ce6c128fc8754a53208eda016d40aa81df` |
| [RealESRGAN_x4plus_anime_6B.pth](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth) | v0.2.2.4 | `f872d837d3c90ed2e05227bed711af5671a6fd1c9f7d7e91c911a61f155e99da` |

The older releases publish no asset digests; the hashes were computed locally
and the file sizes match the release assets. Side-by-side sheets of bilinear,
sharpened Lanczos, the anime model, D, ESRNet and a 25% D blend showed that the
general GAN keeps more surface variation than the anime model and that ESRNet
is smoother, so D was selected. All models interpret ambiguous pixels; compare
outputs rather than assuming an improvement. See
[FFIX upscaling research](ffix-upscaling-research.md) for a comparison with an
FFIX-trained model.

## Using a pack

Set `texture-pack=textures/pack.json` (relative to the runtime directory)
to use the pack; the default is `textures/pack.json`. `tmloader --check`
verifies every listed image, and at runtime the loader logs indexed images,
uploads and fallback reasons.

## Loader contract

- A replacement is selected by the original PNG's hash; identical PNGs under
  different IDs share one replacement. Missing, mismatched or invalid
  replacements fall back to the original.
- The game reads texture dimensions with `GetLevelDesc(0)` at
  `+17C568..+17C5AB` and stores them in its texture entry at `+0xC`/`+0x10`.
  Named fonts are registered at `+17BC34..+17BC7E` with the same dimensions.
  The game must keep seeing the **original logical dimensions** while the GPU
  texture is larger; normalized UVs then address the same regions at 4x.
- The loader hooks the seven native upload call sites, always returns the
  original texture and attaches the HD texture as private data. Draws bind it
  temporarily and restore the original. HD dimensions include the scaled
  power-of-two padding of the original allocation.

See [texture load path](texture-load-path.md) for the ABI and offsets.

## Limits

- Model output smooths some textures and can turn small details into flat
  shapes. It is reconstructed detail, not original art. The alpha mask stays
  closer to the original and can look softer than the RGB edges.
- Backgrounds are split into tiles (`menu_bg0`, `menu_bg1`, ...). They must be
  joined, processed together and split again to avoid seams; `build_ui_pack.py`
  does this.
- Font atlases `gW000/0090/font_00` to `font_41` are never sent through a
  model. They have their own workflow; see
  [UI texture upscaling](ui-texture-upscaling.md).

## Provenance and licensing

- [chaiNNer](https://github.com/chaiNNer-org/chaiNNer) revision
  `b3ca7ff586d4071dbe8601bc4000493560503f21` (GPL-3.0) is a development tool
  and is not linked into or shipped with the loader.
- [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) weights are
  [BSD-3-Clause](https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE).
  No model is committed.
- Spandrel loads the models and PyTorch runs inference. Exact versions are
  written into every generated pack manifest.
- Game artwork is exported from existing game files. Processing it does not
  grant any right to the artwork.
