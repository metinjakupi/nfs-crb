# Changelog

Game features, performance improvements, and bug fixes for NFS Carbon in the browser.

## Unreleased

- Reduce CPU overhead before interpreted audio callbacks by replacing repeated diagnostic byte formatting, preserving callback execution and sound.

- Reduce browser CPU overhead when binding shader resources by using the typed-array WebGPU call, retaining the Firefox memory workaround.

- Avoid repeated peer-value comparisons after changed numeric shader parameters while preserving shared values and texture references.
- Pack compatible float shader constants directly, retaining matrix layout, numeric conversion and car-shine scaling.

- Filter plate and wheel callbacks at their specific draw calls to avoid unnecessary CPU state copies.
- Reduce audio sample-scaling bookkeeping while preserving sample order and floating-point results.

- Fix an unsafe cross-thread read when host events reach the game scheduler.
- Skip repeated mip-level scans for textures whose CPU contents have not changed.
- Reduce audio thread handoffs by keeping mixer setup and timing serialized while the inner sample mix runs in parallel.

## 2026-11-01

- Reject software graphics adapters before starting the renderer. A lost graphics device now unloads the failed game and opens a recovery screen instead of retrying the dead device.
- Notify native online rooms when players leave, disconnect, time out, or reconnect. Clear stale player session state to prevent ghost lobby entries.
- Add bounded automatic performance summaries for sustained low FPS and a sample of normal sessions. Measure new gameplay frames, drawing work, thread waits and optional GPU render-pass timestamps without player-submitted logs.

- Let AI racers consider undiscovered shortcuts while keeping the native difficulty and shortcut chance rules.
- Enable the native reverse controller for police patrols.
- Stop busted AI racers through native goal and brake inputs.

## 2026-10-31

- Reduce CPU work for repeated material and matrix updates, including shared effects, to improve rendering frame times.

- Autosculpt editors reopen fitted parts with the selected car's saved slider settings and preserve pending shopping-cart edits.

- Online players, crew tags and race invitations have a separate F8 panel, also opened from the online status button. F9 keeps an Online link. Player search, filters and 50-row batches support larger rosters.

- Request fullscreen keyboard lock so short Esc presses remain available to the game on supported browsers; preserve browser emergency exit and permission-denied fallback.

- Cancelled online Quick Races always offer Exit to start page, including during loading. Dismiss appears only after returning to native menus.

- Hide racing players and their map markers from free-roam observers; show Racing in the player list.
- Remove spectator copies of another player's AI race rivals.
- Keep same-session multiplayer opponents visible to one another.
- Restore free-roam cars only from a fresh position after a race; keep normal paused-car behavior.
- Fix an opponent car disappearing when a Crew Challenge starts near a third free-roam player.

## 2026-10-30

### Controls
- HyperX Cloud headset media controls no longer occupy the game controller slot.
- Controller check confirms the displayed device matches the controller used by the game.

### Chat
- Removed the preset quick-chat menu; Y opens typed chat directly.
- Added scrollable session chat history and recent messages.

### Startup and reports
- Retry startup once when the browser changes its downloaded-game storage handles.
- Give save-preserving recovery instructions when startup storage remains unavailable.
- Explain unsupported mobile graphics and in-app browser limits more clearly.
- Redirect site addresses with an extra trailing dot to the normal address.
- Include recent race roster changes and failed game-file details in reports, retaining race history across a same-tab reload.

### Online play
- Keep paused players' cars visible and non-colliding, with a paused status.
- Smooth other players' recovery after interrupted position updates.
- Protect claimed names with cloud-save identity and preserve existing careers.
- Nearby players from different crews now receive Carbon's original Crew Challenge prompt automatically.
- Both players accept before choosing a finish on the original map and racing with the game's countdown, standings and results.
- Dismissed requests clear for both players and wait two minutes before repeating for that pair.
- Fixed a progress update writing to the wrong memory address and corrupting online race rankings.
- Crew Challenges end for both players when the first player finishes, with one agreed winner.
- Cancelled Crew Challenges return players to free roam, including when a player disconnects during loading.
- Fixed Crew Challenge failing to load through the normal Online Play button.
- Fixed a Rust relay coordinate-rounding difference that could leave one player waiting after accepting a finish.

## 2026-10-28

### Fixes
- Fixed a race timer crash after returning to the menus.
- Fixed conflicting position corrections when friends accelerate away from the same spawn.
- Paused players and hidden browser tabs now remove their remote car until play resumes.
- Fixed a Firefox graphics crash when the game uses more than 2 GB of memory.
- Fixed an Android graphics error during screen resizing and rotation.
- Phones in portrait ask the player to rotate before starting.
- Interrupted game-file downloads retry without caching incomplete data.
- Startup tries the default graphics adapter if the preferred adapter fails.
- Storage and graphics errors give clearer recovery instructions and explain that saves are kept.
- Problem reports identify the loaded game version.

## 2026-10-27

### Online play
- Added typed chat: Shift+Y opens chat, Enter sends, and Esc cancels.
- Added nearby rival-car sharing so friends can see a player's rival encounter while remaining in free roam.
- Fixed false crashes caused by remote cars hitting traffic that exists only in the receiving player's game.
- Players leaving the same safehouse can drive apart without overlapping cars trapping each other.
- Fixed repeated backward movement after gaps in online position updates.
- Added headlight challenges for free-roam races.

## 2026-10-26

- Friends' cars show steering, wheel spin, braking, nitrous, gear changes, and engine revs.
- Added shared police pursuits and synchronized vehicle lights.
- Refreshing during free roam restores the saved career and recent position, paused.
- Reduced pauses caused by game-file downloads during play.
- Added render-resolution controls and improved controller support.
- Reduced support-script download sizes.

## 2026-10-25

- Restored Custom Match races, including Sprint, Circuit, Speedtrap, Pursuit Tag, Pursuit Knockout, and Canyon Duel.
- Fixed Canyon Duel stopping after the first heat.
- Fixed online disconnections after several minutes of racing.
- Prevented duplicate free-roam cars during online races.
- Restored friend requests, invitations, blocking, and private chat in the game's online menus.
- Improved crowded-race performance and reduced online bandwidth use.
- Added streamed loading, refresh-safe downloads, and problem reporting.

## 2026-10-24

- Added free-roam multiplayer with player names and customized cars.
- Fixed freezing when another player joined.
- Added races with friends, map markers, quick chat, horns, and crews.
- Added cloud saves and in-game settings.
- Reduced recurring frame stalls.

## Still under investigation

- Some NVIDIA graphics-device failures and hardware-specific low frame rates remain unresolved.
- Earlier online movement fixes address reproduced cases; they do not establish that every shaking or collision difference is fixed.
