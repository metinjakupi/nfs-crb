# Changelog

Game features, performance improvements, and bug fixes for NFS Carbon in the browser.

## Unreleased

- Play the story movies and start-up movies in the browser. They were skipped on the web; the game's own VP6 decoder plays them there. Skip intro movies is now on by default and skips only the EA logo and the street-racing warning.
- Limit how many connections one internet address can hold on the online server, and slow connection floods at the web server, without counting addresses behind a proxy.
- Add a Content Security Policy to the site, so an injected script cannot run or send data out, and stop other sites showing it in a frame.
- Treat lookalike letters as the real ones when checking names, so "Admin" or a claimed name cannot be copied with Cyrillic or Greek letters.
- Keep impossible times and scores off the leaderboards.
- Import a career from the PC game version 1.4, with the game's own checks and signature run first, and Undo if you change your mind.
- Load careers made on another copy of the PC game: the import clears the save's serial number, which made the game refuse them.
- Keep the cloud save keyphrase in plain view: a large panel at the top of Cloud save, with whole words, Copy and Save as text file, and the section opens by itself until it is written down.
- Redesign the start page: Home, Saves, Settings and Help tabs, a clear career card, the live player count at the top, and plainer wording.
- Give the loading screens the Carbon look: the same picture and logo as the start page, slanted progress bars, and a percentage in place of byte counts.
- Lay out the start page for phones: Play first and pinned while scrolling, a smaller logo, and no keyboard-only items.
- Fit the F9 menu to phone screens: a slim bar with categories at the side, leaving nearly the whole screen for settings.
- Add a compact Online panel on phones to hide the badge and chat and turn race invitations on or off, and a Camera button while driving.
- Give the phone controls drawn icons, with a NOS + Gas button above Gas: a thumb slides up to it and the throttle never lifts.
- Add a Crew button to the phone controls, beside Speedbreaker (G) on the left, and My crew to the Online panel.
- Move and resize the phone's driving buttons, each one or all together: F9 menu, Controls, Move and resize touch buttons.
- Keep the chat message line in view above the keyboard on a phone held sideways.
- Hide the phone controls while a controller is in use. Touching the screen or unplugging it brings them back.
- Use a new picture on the start page and loading screens.
- Rebuild the phone controls: a D-pad with OK and Back in menus and on the pause screen, steering and pedals only while driving.
- Add proportional steering: drag a thumb anywhere on the left, tilt the phone, or use the buttons. The choice is remembered.
- Let a thumb slide from Gas to Brake without lifting, and make the pedals larger.
- Keep the gas held when Safari's toolbars resize the page during a race.
- Stop pinch zoom, the long-press menu and page bounce from interrupting play on phones.
- Stop the game writing to its whole 2 GB memory area at start, which crashed iPhones and used about 1.8 GB more memory on every device.
- Render phones at a lower resolution and start fewer background workers on touch devices, to use less memory.

- Remove traffic from every race together and from all Drift and Canyon Drift races: each player's traffic exists in their own game alone.
- Keep other players' cars hidden until everyone has loaded, so nobody sits on the grid looking like a bot.
- Bring your car to a stop after you finish while the results wait for the others, instead of the game's AI driving it on.
- Start a host's own race after the Automatic or Manual question, which left it behind so the race was cancelled for everyone.
- Show the host's real car to the other players after the real Start, and let a player join from any menu screen.
- Allow Canyon Duel from the world map, and send players left in a canyon after a world-map race to the safehouse instead of stranding them.
- Let a host start again after a cancelled race, which opened no lobby and sent no invite.
- Require two players for a Canyon Duel: alone it was won at once.
- Tell players when someone in view goes to the safehouse or leaves, and say who cancelled a world-map race and why.
- Redesign the Race Together card in the chat and F9 menu style: about half the size, with a Minimize button.
- Give two-player races the full race intro with the starter and burnouts: a hidden third racer fills the grid until go, then leaves.
- Start a shared race for players on the Wingman screen too.
- Take a player's car off the road when they leave a race, instead of leaving it parked until the race ends.
- Stop one slow player cancelling a race: the host can start without them after 15 s, and the loading timeout starts the race for those ready.
- Show other players' cars in shared Canyon Drift: the race's scripted start switched them off, leaving only their names.
- Stop other players' cars counting towards your drift score and running TOO SLOW at double speed in shared Drift and Canyon Drift.
- Let cars pass through each other in shared Canyon Drift, so another player cannot end your run.
- Start a shared race for players on the Quick Race list, the screen every race returns to, so a second race no longer hangs for them.
- Repeat the race start, ready, go, finish and cancel messages until they arrive, so one lost message cannot stall a race.
- Stop waiting on the results screen for a player who left or whose game hung; a real finish that arrives late still counts.
- Add a Race invites switch, hide race invites during any race, shared or solo, and keep "Not now" dismissed.

- Stop police copies colliding with the player they chase, other copies and local traffic, which their owners' games have already settled, so chases seen from outside no longer shudder.
- Give a friend shown in the wrong car their real model as soon as it can be built, instead of leaving them in the substitute until they leave.
- Make police cars in other players' chases look driven: they now steer, brake and shift, and update more often.
- Prepare police car models ahead like other cars, build arriving cops one at a time, and add no police cars behind a paused game.
- Let a far-away car wait longer for its files, and keep a friend moving in their old car while a new model downloads.
- Keep shared effect values in their effect pool, as Direct3D does, instead of copying each per-object matrix into every other effect of the pool, cutting about 1.5% of the game thread's CPU time in races.
- Remember Direct3D interface pointers between calls instead of reading them back from guest memory and the object table each time.
- Reduce the cost of every Direct3D and Windows call from the game: the scheduler's call counter is no longer atomic, and the clock is read less often in the browser.

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
