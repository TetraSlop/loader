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
| graphics | D3D8 windowed device, first-frame activation, close/short-click messages, optional captures |
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
  path boundaries. Error handling rejects invalid paths before passing them on.
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
- The local xiloader checkout provided module/build/API style reference only;
  no xiloader implementation files or proprietary runtime files were copied.
  `source_provenance.json` records hashes of the supplied harness sources.
