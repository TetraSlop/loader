Tetra Master - portable local app

Windows 10/11 x64. Extract the entire TetraMaster folder to a short, writable
location (for example C:\Games\TetraMaster). Do not run files inside the ZIP.
Python is bundled; no developer tools or system Python installation are needed.

1. Run Import Game Files.cmd if original game files were not bundled.
   Enter the original Tetra Master folder containing TM.dll and data/, or a
   prepared runtime with TM_native.dll, polcore_native.dll and data/.
   Setup uses the installed PlayOnline Viewer's
   factory default/pub/home/option.bin. It never imports your Viewer profile.
   Supported original DLL hashes are checked; unknown versions are rejected.
2. Run Check Setup.cmd. It validates files without opening the game.
3. Run Play.cmd. It starts the local Python server and the game together.
   Close all game windows to stop that server. Saved progress stays in user/.
   CPU play: Local Room > empty table > Computer.

Advanced import (from a terminal in this folder):
  python\python.exe -I -B -m distribution.app import --source C:\MyTMRuntime
    --viewer-defaults "C:\MyViewer\default\pub\home\option.bin"
    --texture-pack "C:\MyHDPack\pack.json"
Put the command on one line. The HD argument is optional. The import copies
files into this folder; it does not edit the source installation. It refuses
to overwrite changed files. Use a fresh extraction for a different asset set.

For an original installation at a custom location, use --source with its
Tetra Master folder and --viewer-dir with the PlayOnlineViewer folder.
The importer copies/renames the two supported DLLs and brings in the required
Viewer environment file. It never edits or registers the installed DLLs.

Client settings: client/tmloader.ini. Press F10 for wrapper settings, F12 for a
screenshot, or Alt+Enter for fullscreen. HD replacements are selected by
texture-pack=textures-hd/pack.json; a missing optional pack uses original art.
Start Server.cmd runs only the server on local ports 51241/51220. Stop it with
Ctrl+C before running Play.cmd against the same save. Additional server
arguments can be passed on the command line. Services listen on loopback.

Play.cmd --clients 2 opens two clients for local versus play.
Play.cmd --rewards enables the documented experimental local reward policy.
This build is for local testing; do not expose these synthetic accounts as a
public service without separate account and deployment work.

Package contents:
  client/      loader, fresh configuration and optional imported client files
  python/      isolated Python runtime and its LICENSE.txt
  server/      Python server code, game/protocol data and room definitions
  app/         portable setup/launch helpers
  licenses/    third-party library notices
  user/        generated save data (absent in a fresh ZIP)
  package-manifest.json  file hashes and explicit optional-component inventory

The default ZIP excludes original client DLLs, original client media and HD
replacement images. Server rule/catalogue/protocol data are still included.
An optional full ZIP contains locally supplied original/derived assets; see
package-manifest.json before sharing it. Packaging does not grant rights to
redistribute those optional files. No existing accounts, saves, logs, local
preferences, authoring models or original installation paths are bundled.

To update: extract into a new folder, import your assets, then copy user/ from
the old app while both servers are stopped. Keep client/runtime/settings if
you want to retain your wrapper preferences. Do not merge a new ZIP over a
running app. This is a portable ZIP, not a signed Windows installer.
