Tetra Master - portable client

This package contains only the client. Your server operator deploys tmserver
separately. There is no bundled server, Python runtime or account database, and
Play never starts a server. Windows PowerShell is already part of Windows.

1. Extract the entire TetraMaster folder to a short, writable location such as
   C:\Games\TetraMaster. Do not run files inside the ZIP.
2. Run Import Game Files.cmd if original files were not bundled. Supply an
   original Tetra Master folder (TM.dll and data/) or a prepared runtime
   (TM_native.dll, polcore_native.dll and data/). Setup also uses the installed
   PlayOnline Viewer's factory defaults, not your current Viewer profile.
3. Run Connection Settings.cmd. Enter your server operator's numeric IPv4
   address, game/profile TCP ports, POL ID and password in client/tmloader.ini.
   The supplied loopback endpoint and synthetic credentials are test examples.
4. Run Check Setup.cmd to validate files without opening the game or connecting.
5. Run Play.cmd to open the client and connect to the configured server.

Advanced import, in a terminal in this folder (put on one line):
  "Import Game Files.cmd" -Source "C:\MyTMRuntime"
    -ViewerDefaults "C:\MyViewer\default\pub\home\option.bin"
    -TexturePack "C:\MyHDPack\pack.json"
The HD argument is optional. For an original installation, use -Source with
its Tetra Master folder and -ViewerDir with the PlayOnlineViewer folder.
Only supported, hash-pinned DLLs and factory defaults are accepted. Importing
copies into this folder; it never changes the source installation. Existing
changed files are preserved: use a fresh extraction to replace an asset set.

F10: wrapper settings. F12: screenshot. Alt+Enter: fullscreen.
HD replacements are enabled by texture-pack=textures-hd/pack.json in the INI;
a missing optional pack uses original art. Settings/screenshots/logs are under
client/runtime/. Account progress is stored on your separately deployed server.

Contents:
  client/       tmloader, connection settings and optional client assets
  app/          Windows setup/import/launch helpers and asset selection policy
  licenses/     third-party library notices
  package-manifest.json  file hashes and selected optional components

The default ZIP contains no SE client DLLs, original media or HD replacement
images. Optional bundles can include locally supplied DLLs/originals/HD art;
check package-manifest.json before sharing. Packaging does not grant rights
to redistribute those optional files. Existing profiles, credentials, logs,
authoring models and account saves are never copied from the build machine.

No administrator access is requested. The launch helper only prepares this
app's runtime folder for the legacy client's writes. Its PowerShell execution
policy override is process-local; it does not change your machine/user policy.
To update, extract a new client and reimport assets, then re-enter connection
settings. Your server deployment and its saved progress remain separate.
