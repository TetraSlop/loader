# Texture load path

The game DLL uploads PNG textures through one creator function, called from
seven sites. The loader hooks those calls to substitute HD textures without
changing the native texture objects. Offsets are RVAs in the game DLL, image
base `0x10000000`, and come from static analysis of the native code.

## Approach

`initializeTextureReplacements` redirects the seven CALLs below to a
`WINAPI` `HRESULT` wrapper and keeps `+1B9AA4` as the original creator.
`redirectCall` patches a site only if it is a relative CALL to that creator.

1. Create the native texture unchanged.
2. Hash the original PNG through `IEND` (archive payloads can include up to 15
   alignment bytes after it) and look it up in the texture pack manifest.
3. On a match, create a larger texture through the original creator and attach
   it to the native texture as `IUnknown` private data.
4. Draw hooks bind the HD texture temporarily and restore the original binding
   afterwards.

Native texture identity, metadata, lifetime, `GetTexture`, copies and font
registrations are unchanged. Any failure leaves the original texture in place.

## Creator ABI

```c
HRESULT WINAPI creator(Device*, const void* src, UINT bytes, UINT width,
                       UINT height, UINT levels, DWORD usage, DWORD format,
                       DWORD pool, DWORD filter, DWORD mipFilter,
                       DWORD colorKey, void* srcInfo, void* palette,
                       Texture** output);
```

Fifteen arguments, `__stdcall`, callee cleanup `0x3C`. The prologue at
`+1B9AA4` is `55 8B EC FF 75 40 6A 03`, and the terminal `RET 3C` is at
`+1B9AE1`. It calls an internal common creator at `+1B9431` with 17 cdecl
arguments. All seven callers pass:

| Argument | Value |
|---|---|
| device | `[+2FF20C]` |
| src, bytes | PNG pointer and record size (lazy entries at `+0`/`+8`) |
| width, height | `0xFFFFFFFF` (`D3DX_DEFAULT`) |
| levels, usage | 1, 0 |
| format | `[+2FF200]` |
| pool | 0 (`D3DPOOL_DEFAULT`) |
| filter, mipFilter | `0x80001` (NONE \| DITHER), `0xFFFFFFFF` |
| colorKey, srcInfo, palette | 0, `nullptr`, `nullptr` |

## Call sites

| RVA | Bytes | Matching GetLevelDesc |
|---|---|---|
| `+17BA39` | `E8 66 E0 03 00` | `+17BA8B` |
| `+17C061` | `E8 3E DA 03 00` | `+17C0B7` |
| `+17C1F5` | `E8 AA D8 03 00` | `+17C242` |
| `+17C393` | `E8 0C D7 03 00` | `+17C3DF` |
| `+17C528` | `E8 77 D5 03 00` | `+17C576` |
| `+17CCFA` | `E8 A5 CD 03 00` | `+17CD47` |
| `+17CE91` | `E8 0E CC 03 00` | `+17CEDD` |

`+17BA39` is immediate texture registration, including fonts. The others load
entries whose PNG was registered earlier. All seven must be hooked.

## Logical and physical layout

Texture entries are 20 bytes: `+0` original PNG pointer, `+4` texture COM
pointer, `+8` input byte count, `+C` width (float), `+10` height (float).

Every upload path calls `GetLevelDesc(0)` and stores the width and height at
`+C`/`+10`. The register function `+17B9F0` recognizes `font_XX` at `+17BC34`,
stores the texture pointer at `[+2FC0B8 + index*4]` and the dimensions at
`+2369C4`/`+2369C8`. Font rendering binds these pointers directly at `+184A56`;
there is no atlas recomposition or per-glyph upload.

`D3DX_FILTER_NONE` copies to the top left without scaling and leaves the rest
transparent. The HD texture must therefore be the original `GetLevelDesc`
size times the replacement scale, not the replacement PNG size, because the
native allocation may be padded to a power of two. A 72x96 PNG can occupy
128x128; its 4x replacement is 288x384 inside a 512x512 texture. The same
normalized UVs then address the same visible content.

## Ownership and state

Entry cleanup at `+17C6C6`/`+17C6D5` releases the texture through vtable slot
2; state 3 also frees the source PNG at `+17C6E2` and the entry at `+17C6ED`.
The private-data `IUnknown` therefore lives exactly as long as the native
texture, with no separate pointer map.

- Only stage 0 texture bindings are used.
- `+2FE8F0` caches the last bound texture, so every temporary substitution
  must restore the original binding before returning, even when the draw
  fails.
- Device `GetTexture` and `GetPrivateData` both AddRef; release both
  temporary references.
- `SetPrivateData` (slot 4) receives the interface pointer itself, size
  `sizeof(IUnknown*)`, flag `D3DSPD_IUNKNOWN` (1). `GetPrivateData` is slot 5
  and `FreePrivateData` slot 6.

A scan through `+1B0000` found no texture `LockRect`/`UnlockRect` or device
`CopyRects`/`UpdateTexture`/`GetTexture` calls. The indirect `+40` calls in
`+1Axxxx` take two arguments and belong to audio. If texture writes are found
later, their HD companions would need to be rebuilt.

## References

- [D3DX_FILTER semantics](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dx-filter)
- [Private data AddRef behavior](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dresource9-getprivatedata)
  (D3D9 documentation; the contract is inherited from D3D8)
