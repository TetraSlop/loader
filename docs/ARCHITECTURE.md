# Architecture and behavior ledger

The old online build rewrote `tm_wrapper.cpp` with Python, removed an offline
title substitution and injected `startup_trace.h`. The new build compiles
ordinary translation units and an explicit online bootstrap. No source
generation, hidden environment navigation switches or probe header is needed.

| Module | Responsibility |
|---|---|
| main/config/validation | CLI and config precedence, file checks, pinned DLL SHA256, headless --check |
| loader/abi | Direct COM factories, original GameStart descriptor, Viewer setup/order |
| network/endpoints | Account identity, profile cipher lifetime, initial profile requests, DNS/endpoint routing |
| files | Virtual registry, correct empty LOADDIR, runtime write boundary |
| graphics/render_scale | D3D8 device/reset, optional higher-resolution rasterization, scaled presentation, captures |
| screenshot | User-requested backbuffer capture, WIC PNG encoding, unique output files and status |
| window/window_settings | Desktop system commands, fullscreen/resize, shared viewport, account-scoped preferences, short-click latch |
| settings_ui | Dear ImGui panel using official Win32/DX9 backends on an owned UI thread |
| audio | DirectSound 8 factory and diagnostics; game retains music/effect mixing |
| desktop | Disable native global shortcut interception, cursor warping and forced activation |
| input/input_bridge | Native DirectInput factory plus existing keyboard/mouse fallback |
| patch | Process-local pointer/import replacement |
| log/runtime | Logging, exception capture, reduced-integrity token, shared native context |

Normal loader fixes preserved from the working harness:

1. Common854 encodes the configured POL ID; Common851 initializes the Viewer
   account before TM caches its NN. GameIRC credentials alone are insufficient.
2. Core INET/file/profile setup ordering remains intact. On first successful
   GameIRC open, both the 0x68-byte cipher context and its separate0x1000-byte
   matrix are copied into stable loader storage. Profile bootstrap calls
   Common171/172,175/176,193/194 retain the original bounded polling behavior.
3. The profile endpoint cache and ppNNN hostname aliases remain configured.
   This includes pp001 used after a match. Its fix was built after the earlier
   native match; it still needs a later native validation.
4. Empty LOADDIR avoids the legacy archive-name parser's first-dot behavior.
5. Graphics window flag+2FF248, windowed creation, and first-Present activation
   remain. Probe-only room/zone/title/confirmation events are omitted; they
   never belonged to an interactive launch without TM_PROBE flags.
6. Mouse samples reset on focus/acquisition/window-size transitions; physical
   movement/new clicks map into640x480 and use the original virtual cursor
   integration. Keyboard focus animation remains while the mouse is stationary.
   A short WM_L/RBUTTONDOWN press is latched for one input sample.

Changes deliberately made during extraction:

- Configurable external paths and numeric service endpoints replace hardcoded
  scratch paths. Defaults retain the existing local account and ports.
- SHA256 and live ABI checks precede compatibility patching. x86 is enforced
  by CMake and the launch descriptor's static size assertion.
- Diagnostic frame capture is opt-in. Automatic UI events, watchdog thread
  suspension, timed window closure and bounded probe termination are omitted.
- File mutation checks now include create/truncate dispositions and resolved
  path boundaries. Log/capture destinations are confined to the dedicated
  runtime/logs tree and reject data/pub/DLL aliases and hard-linked files.
  Capture destinations are checked again immediately before writing. Error
  handling rejects invalid paths before passing them on.
- COM factory references are released after creating their objects; unused
  generated COM declarations are replaced by a minimal independently declared
  ABI with the observed method slots.

Remaining debt, not hidden by the module split:

- Fixed RVAs and broad process-local pointer scanning remain tied to the two
  pinned DLLs. They should eventually become a reviewed per-version patch map.
- Legacy native state is shared at module scope; this host supports one game
  session per process. There is no generalized reconnect/account manager.
- Startup profile polling remains synchronous for up to5seconds per request,
  preserving behavior. Error propagation needs a future native-tested design.
- Closing after GameStart returns still terminates **this loader process** as
  the old host did; safe full COM/DLL teardown remains unverified. No watchdog
  or code for terminating other processes is present.
- The input fallback implements the exact tested immediate keyboard/mouse
  path, not general DirectInput: joystick, buffering, force feedback and many
  properties are unimplemented. The new short-click latch has headless evidence
  but no native click confirmation yet.
- Native message boxes retain the harness's log-and-IDOK compatibility hook.
  The full Viewer UI/status/options format and runtime ACL provisioning are
  outside this refactor. Server protocol implementation stays separate.

## Desktop controls and audio

The startup harness deliberately returned SOUND=0. TM's +187149 registry read
stores this at +2369CC; +180E40 skips all sound initialization when it is zero.
The virtual registry now supplies SOUND=1. The native +20401C import is
DirectSoundCreate8 (dsound ordinal 11), which the audio adapter must preserve.
Both music (+1AAFCF) and effect (+1AA933) paths require IDirectSoundBuffer8;
substituting DirectSoundCreate initializes successfully but its buffers reject
that interface with E_NOINTERFACE, leaving playback silent. The factory now
uses the SDK's exact DirectSoundCreate8 signature. The game's decoding, mixing
and volume handling remain intact. Factory results and native sound-engine
state are logged, but initialization alone does not establish audible output.

The audio regression check creates and locks both native buffer configurations
on the Windows audio device, under the same low-integrity setting as the loader.
It requires IDirectSoundBuffer8 and never starts playback or creates a window.
CTest reports it as skipped (77) only when Windows has no audio driver.
See Microsoft's [buffer API](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418039(v=vs.85))
and [DirectSoundCreate8](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416762(v=vs.85)).

The original window procedure at +1863D0 discards WM_SYSCOMMAND at +186838.
That includes SC_SIZE, SC_MOVE, SC_MAXIMIZE and SC_CLOSE. The wrapper routes
desktop commands to DefWindowProc, handles close/Alt+F4 directly, and posts
WM_QUIT to the native main loop (+18778E/+18798C). Shutdown is not a synthetic
game result or an in-game save command; the server handles disconnected sessions.

D3D8 uses a back buffer selected at startup (640x480 times 1, 1.5, 2, 2.25, 3, 4, 4.5 or 6), with
D3DSWAPEFFECT_COPY allowing an explicit presentation rectangle. Device Reset
keeps that resolution and recreates the scale shader. Resizing the window only
changes presentation. The shared content rectangle drives presentation and input. The input
fallback requires the game window itself to have focus and is suspended while
settings are open; settings clicks cannot become game actions.

Automatic resolution reads physical display dimensions for the game's monitor,
falling back to the adapter's display mode. It rounds the fitted 4:3 area up to
the next supported preset and caps automatic selection at 4.5x. The 6x setting
remains a manual supersampling option. Account preferences v5 preserve v1-v4
manual scales and migrate the old HD toggle to both artwork and fonts. Each
game/settings thread enables DPI awareness through the official ImGui helper
before creating its windows. Automatic selection uses the monitor hosting the
window at startup; moving the window later does not trigger a device reset.

F12 is handled by the game window and excluded from fallback keyboard samples.
A queued screenshot is consumed on the render thread before Present. It copies
the actual 32-bit backbuffer, respecting row pitch, then unlocks before WIC PNG
encoding. Only opaque RGB is saved because render-target alpha is unreliable.
Output remains inside the existing logs boundary; CREATE_NEW makes filename
collisions non-destructive. No request means no readback, encoding or disk I/O.
The settings panel displays success/failure and can queue a capture before closing.

HD companions carry artwork/font metadata in the same resource-owned COM
attachment as the retained texture. Draws select a companion according to its
category and restore the original binding afterward. Only exact verified font
atlas IDs are classified as fonts; mixed lettering and unknown assets remain
artwork. Conflicting categories for the same source image hash are rejected.

The supported game's renderer sets FVF 0x1C4 at +18297F and creates a dynamic
32-byte-stride vertex buffer at +1829C0. All fourteen draw sites in its rendering
module use DrawPrimitive with this pretransformed XYZRHW/color/specular/UV
layout. Simply enlarging the buffer would leave these coordinates unscaled.
Above 1x, the wrapper substitutes a small vs.1.1 shader at SetVertexShader (slot
76). It converts the existing screen coordinates to clip space, preserves depth
and reciprocal W, and passes colors/UVs through. Pixel edges follow
`(coordinate + 0.5) * factor - 0.5`; no game coordinates, input or assets change.

CreateVertexBuffer (slot 23) clears only the native 0x1C4 FVF tag at higher
scales, using an explicit shader declaration instead. D3D8's software path
bypasses shader input preparation for an XYZRHW-tagged buffer; leaving the tag
intact crashes on draw. Other buffer formats pass through. At 1x both hooks
delegate unchanged. The shader bytecode is our own short program, documented
instruction by instruction; there is no runtime shader compiler dependency.

Resolution is a next-launch preference to avoid resetting the device while
native default-pool resources and an online match are active. Failure to create
the larger device or shader at startup retries at 1x and logs the fallback.
Window settings v5 stores a fractional manual scale, automatic selection and
separate artwork/font toggles; v1-v4 preferences remain readable. Only the eight
advertised manual scales are accepted.
The 4.5x preset is 2880x2160: exact-height 4:3 content on a 4K display. Present
source rectangles use consistently rounded edges at fractional scales. The
ImGui panel reports the actual active resolution separately. Texture replacement
dimensions are independent of the render scale; native UVs remain unchanged.

Native initialization at +1828AB/+1828BF selects linear minification and
magnification; +1828D3/+1828E7 selects clamp addressing. The native code scan
through +1B0000 found no indirect SetViewport (slot40) calls that would restore
a 640x480 viewport after CreateDevice. These are static observations; full
native visual validation remains separate from the headless pixel checks.

ABI references: [Direct3D 8 interfaces](https://github.com/wine-mirror/wine/blob/master/include/d3d8.h).
Pixel alignment follows the [Direct3D rasterization convention](https://learn.microsoft.com/en-us/windows/win32/direct3d9/directly-mapping-texels-to-pixels).

The settings UI uses Dear ImGui's supported Win32/DX9 backends in a separate
owned window. Only that UI thread touches ImGui or its DX9 device. Changes are
posted to the game's window thread; a mutex protects the small settings snapshot.
The panel does not replace the game's graphics device or require an unofficial
D3D8 ImGui backend. Preferences use a dedicated runtime/settings tree with the
same canonical path and hard-link protections as logs.

The initial desktop update missed additional fullscreen behavior. GameStart
installs a global WH_KEYBOARD_LL callback (+8030) that suppresses Windows keys,
Alt+Tab/Alt+Escape and other shortcuts without testing which app has focus.
The desktop adapter suppresses that installation at import +2041E4. It also
virtualizes the game's ShowCursor loop, AttachThreadInput, SetForegroundWindow
and SetActiveWindow imports and its animation/foreground-lock/screensaver
setting writes. Those replacements affect only the game DLL's imports.

The native renderer at +185753..+185830 warps the OS pointer and clamps the
software cursor to unscaled client coordinates every frame. A guarded five-byte
branch skips that block; both the DLL hash and relocated original instruction
must match. The game file on disk is unchanged. The wrapper owns cursor shape
through WM_SETCURSOR and hides it only within focused game content. ImGui retains
normal cursor handling. Message 0x64, posted by the native loss-of-activation
handler to force the game back to the foreground, is consumed by the wrapper.
- The local xiloader checkout provided module/build/API style reference only;
  no xiloader implementation files or proprietary runtime files were copied.
  `source_provenance.json` records hashes of the supplied harness sources.

## COM ABI audit after review

The first extraction accidentally omitted the two SquareEnix registry-name
getters when counting the area setter. This was caught before any native run.
The corrected mappings were checked against both C and C++ declarations in
the supplied `polcore_interface.h` and the named calls in `tm_wrapper.cpp`:

| Call | Slot | ABI arguments after this |
|---|---:|---|
| SetParamInit |5| HINSTANCE, mutable LPSTR |
| GetCommonFunctionTable |7| unsigned long** |
| SetAreaCode |27| signed long |
| TM GameStart |9| core IUnknown*, 28-byte LaunchDescriptor* |

Slots23/24 are PlayOnline registry-name getters and25/26 are SquareEnix
registry-name getters; neither pair may be skipped. All calls above return
HRESULT and use stdcall. COM factories use the Windows SDK IClassFactory ABI.
The existing D3D8 slots/signatures were compared with the original wrapper
without changes: factory CreateDevice15, display mode8, device Present15,
back-buffer16, texture20, draw70..73; surface description8, lock9, unlock10.
Common-function-table calls remain the original cdecl variadic ABI, separate
from COM vtable indices.

The headless mock declares the complete named Viewer method sequence through
SetAreaCode and verifies actual dispatch, input arguments and output-table
pointer. It explicitly demonstrates that slot25 reaches a different method;
reintroducing the original mistaken index fails the regression check.
