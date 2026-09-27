# Native texture loading and HD draw substitution

All offsets are RVAs in TM_emulated_mapped.dll; base 0x10000000.

## Hook recommendation

`installTextureCallHooks` redirects the seven guarded CALLs below to a WINAPI HRESULT wrapper, retaining
RVA 0x1B9AA4 as the original creator. First create the native texture unchanged.
Hash the original PNG through IEND (archive payload can include alignment bytes)
and match a replacement manifest. Then create a second, higher-resolution texture
through the original creator; attach it as IUNKNOWN private data to the native
texture. Draw hooks temporarily bind it and restore original binding afterwards.
Native texture identity, metadata, lifetime, GetTexture, copies, and font registrations
remain unchanged. Failures should leave the original texture intact.

## Creator ABI

HRESULT WINAPI creator(Device*, const void* src, UINT bytes, UINT width,
 UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool,
 DWORD filter, DWORD mipFilter, DWORD colorKey, void* srcInfo,
 void* palette, Texture** output)

Fifteen arguments, __stdcall callee cleanup 0x3C. Prologue at 0x1B9AA4 is
55 8B EC FF 75 40 6A 03; terminal RET 3C is at 0x1B9AE1.
Calls an internal common texture creator at 0x1B9431 using 17 cdecl args.
All seven native callers use:
- device = [TM+0x2FF20C]
- src/bytes = PNG pointer/record size (lazy entries at +0/+8)
- width = height = 0xFFFFFFFF (D3DX_DEFAULT)
- levels = 1, usage = 0
- format = [TM+0x2FF200]
- pool = 0 (DEFAULT)
- filter = 0x80001 (NONE | DITHER), mipFilter = 0xFFFFFFFF
- colorKey = 0, srcInfo = nullptr, palette = nullptr

## CALL guards

| RVA | Expected 5 bytes | Corresponding GetLevelDesc |
|---|---|---|
| 17BA39 | E8 66 E0 03 00 | 17BA8B |
| 17C061 | E8 3E DA 03 00 | 17C0B7 |
| 17C1F5 | E8 AA D8 03 00 | 17C242 |
| 17C393 | E8 0C D7 03 00 | 17C3DF |
| 17C528 | E8 77 D5 03 00 | 17C576 |
| 17CCFA | E8 A5 CD 03 00 | 17CD47 |
| 17CE91 | E8 0E CC 03 00 | 17CEDD |

17BA39 is immediate texture registration (including fonts); the other paths
load entries whose source PNG was registered earlier. Keep all seven calls.

## Logical and physical layout

Entry structure is 20 bytes:
+0 original PNG pointer; +4 texture COM pointer; +8 input byte count;
+C texture width float; +10 texture height float.

All upload paths query texture GetLevelDesc(0) then store width/height at +C/+10.
Register function 17B9F0 recognizes font_XX at 17BC34 and copies texture pointer
into global[0x2FC0B8 + index*4], width/height to globals 0x2369C4/0x2369C8.
Font rendering binds these pointers directly at 0x184A56; no atlas recomposition
or upload per glyph was found.

FILTER_NONE performs a top-left copy without scaling; destination space outside
source is transparent. Therefore replacement backing texture dimensions must be
original GetLevelDesc.Width/Height times replacement scale. Do not use replacement
PNG dimensions alone: native allocation may be POT padded. A 72x96 PNG might
occupy 128x128; corresponding 4x PNG is 288x384 within 512x512 backing.
Same normalized UVs then address matching visible content and preserve crop/layout.

## Ownership and state

Entry cleanup at 17C6C6 / 17C6D5 calls texture Release via vtable slot2; state3
also frees source PNG at 17C6E2 then entry at 17C6ED. Private-data IUNKNOWN lifetime
follows native texture lifetime without a stale pointer map.

Only native stage0 texture bindings were found. Native global 0x2FE8F0 caches the
last texture bound, so every temporary draw substitution must restore the original
before returning even when draw fails. Device GetTexture itself AddRefs; private-data
GetPrivateData on IUNKNOWN also AddRefs. Release both temporary references.
SetPrivateData(slot4) receives the interface pointer itself, size sizeof(IUnknown*),
flag D3DSPD_IUNKNOWN(1). GetPrivateData is slot5; FreePrivateData slot6.

No texture LockRect/UnlockRect, device CopyRects/UpdateTexture/GetTexture calls
were found in the native game code scan through RVA1B0000. The indirect +40 calls
in 1Axxxx take two arguments and belong to audio, not texture writes. This is static
evidence, not a runtime guarantee. Original resource retention makes future copies
safe but edits would require invalidating/rebuilding HD companions if discovered.

## Documentation

D3DX_FILTER_NONE semantics:
https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dx-filter
Resource private-data AddRef behavior (same inherited contract; modern docs):
https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dresource9-getprivatedata

No game process or GUI was launched. Original files were only read.
