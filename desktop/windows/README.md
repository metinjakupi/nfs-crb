# Carbon Local Launcher preview

Windows x64 and macOS arm64/x64 desktop wrapper around the reviewed Carbon browser runtime.
Choose the directory containing your supported PC v1.4 NFSC.exe and required game assets.
The launcher hashes NFSC.exe, reads an allowlisted subset of the installation, and serves bytes
only on 127.0.0.1. It never starts NFSC.exe or loads ASI plugins.

Original game files and original saves are not changed. Runtime-generated settings, save slots,
verified startup modules, downloaded customization overlays and browser caches live in Electron's
per-user application data. Existing web saves require export/import through the game's save tools;
there is no automatic transfer from Chrome or the original Windows game.

Internet is currently required for customization setup and online services. Base game reads use
the selected local installation. The runtime may cache those reads in application storage.
This is not a native Vulkan/Metal port and does not promise higher frame rates or solve every GPU fault.

## Build

1. Run `npm ci` in this directory.
2. Prepare a reviewed website snapshot with `npm run prepare-runtime -- --site <published-play-folder> --page <published-root-index.html>`.
   The script checks the page's release identity against the JS and WebAssembly before copying runtime files.
   Do not point this at an original game folder or an unfinished shared build.
3. Run `npm test`.
4. Run `npm run dist:win -- -c.win.signAndEditExecutable=false` for the unsigned Windows x64 ZIP.
5. Run `CSC_IDENTITY_AUTO_DISCOVERY=false npm run dist:mac` for unsigned macOS ZIPs, or supply release signing credentials for distribution.

Build output is in `dist/`. Runtime and dependencies are excluded from Git. Packages include the
browser runtime, not the player's game installation. Do not publish development configuration or
private profiles. Signing/notarization and real Windows gameplay verification remain release gates.

## Run

Run `npm start`, choose your game folder, then Play Carbon. On macOS, choose a folder containing
the Windows game files; the launcher still uses the recompiled runtime, not Wine.
Close the game window to return to the launcher. Quit the application before selecting a different
installation. One launcher instance binds port 47738, which preserves the origin of browser-backed
save storage. A busy port causes a clear startup error; it never stops another service.

## Compatibility adapter

The packaged worker allows an HTTP manifest only when it has the exact same local origin.
External manifests still require HTTPS. Every local game request resolves against an indexed
allowlist, supports bounded byte ranges, rejects writes and rejects paths that escape the selected
installation. Links are not imported. The web page has no Node.js access; only the launcher UI has
three narrow IPC calls. The game window has no privileged preload script.

This preview reuses the existing public startup and online screens after Play. A later release can
combine the remaining setup screens and add reviewed runtime updates. It does not silently update
the runtime or change the public site.
