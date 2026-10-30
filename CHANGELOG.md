# Changelog

Game features, performance improvements, and bug fixes for NFS Carbon in the browser.

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
