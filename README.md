# Auto Completer (Geode mod)

A pathfinding bot for **Geometry Dash 2.2081** on **Geode v5**. It plays levels for you in real time: no waiting, no freezing. It has play styles, a path viewer and a Mega Hack style ImGui menu (**Tab**).

## How it works
- **Hidden player.** The bot keeps an invisible copy of your player inside the level. Every 16 ticks (GD runs 240 per second) it copies the real player onto it and simulates about 0.8 s ahead against the level's blocks, spikes, slopes, orbs and pads. The real game is never paused, rewound or touched.
- **Exact physics.** The bot records which physics calls GD makes on your player each tick (touched rings, internal actions, update, collisions, rotation) and the hidden player replays exactly that sequence, plus GD's inlined per-tick housekeeping. It copies every physics field of the real player (171 of them). Every real tick is also replayed on the hidden player from the same state and compared; the match rate shows as **Tick match** in Stats.
- **Search only when needed.** If the predicted path dies, the planner looks back from the death for ticks where pressing or releasing avoids it. Those ticks form a safe timing window. The play style picks a tick inside it, always at least 3 ticks from its edges:
  - **Default:** the middle of the window.
  - **Perfect:** the middle, scanned frame by frame.
  - **Legit Human:** natural jitter in timing and tap length.
  - **Minimal:** the latest tick that still keeps the margin.

  A click only counts if the same click one tick earlier and later also works, so a small timing error can't kill. Cube, robot, ball and spider only press where a press does something (on the ground or on an orb). If nothing in the window works, the planner may take an orb click that only changes how the player would die and look for the follow-up click from there (orb chains, like a gravity orb up and another one back down), at most two in a row. Otherwise it backtracks to earlier decisions; if the previous plan led into a dead end, the stretch is planned again from scratch. The next 12 ticks are always fixed, so the bot never changes an input it is about to play.
- **Moving objects.** Every move trigger GD starts is recorded (offset, duration, easing, lock to player), and the simulation starts the move triggers the player will pass, directly or through a spawn trigger. The hidden player meets moving hazards and platforms where they will be.
- **Portals.** Speed and size portals are applied to the hidden player. Mode, gravity, dual and teleport portals end the look-ahead: the bot plans up to the portal and continues from the real player's new state once it is through.

The planner (`src/planner`) and the move model (`src/bot/Motion.hpp`) have no Geode dependencies. CI runs three native test suites with sanitizers on every push:
- `tests/planner_test.cpp`: the planner against a toy physics model, including the exact look-ahead loop the game uses and orb chains.
- `tests/gd_pilot_test.cpp`: the whole bot loop on a physics model matched to GD 2.2081 measurements (jump arc, speed, spike hitboxes), with inputs sometimes applied a tick late.
- `tests/motion_test.cpp`: GD's easing curves and how move triggers move their groups.

## Safe mode
Safe mode is on by default (switch it in the menu or the mod settings). While the bot plays it uses the same flag as start positions, so nothing is saved: no progress, stars, coins or leaderboard submissions. The bot is always disabled on editor levels, so it can't verify uploads.

## Install (Windows, or Linux through Steam/Proton)
1. Install [Geode](https://geode-sdk.org) for GD 2.2081.
2. Get `againpwned.auto-completer.geode` from `dist/` on this branch, or from the latest run on the **Actions** tab.
3. Copy it to `<GD folder>/geode/mods/`. On Linux with Steam the GD folder is usually `~/.local/share/Steam/steamapps/common/Geometry Dash/`.
4. Open a level and press **Tab** for the menu.

## Building locally
- **Windows:** `geode build`.
- **Linux:** `geode sdk install-linux` once, then `geode build`. This cross-compiles the Windows DLL.

To run the planner tests:

```sh
g++ -std=c++20 -O2 -I src tests/planner_test.cpp src/planner/Planner.cpp -o planner_test && ./planner_test
g++ -std=c++20 -O2 -I src tests/gd_pilot_test.cpp src/planner/Planner.cpp -o gd_pilot_test && ./gd_pilot_test
g++ -std=c++20 -O2 -I src tests/motion_test.cpp -o motion_test && ./motion_test
```

## Limitations
- Classic levels only (no platformer or 2-player yet).
- After a mode portal the first plan only starts once the real player is through it, and flying modes use the game's current floor and ceiling.
- Moving objects: groups moved by move triggers are placed exactly (offset, duration, easing, lock to player), including move triggers the player starts by passing them, directly or through a spawn trigger. Anything else that moves (follow, rotate, keyframe and area triggers, move-to-target, chained or remapped spawns, touch or collision triggers) is extrapolated from its current speed, and re-checking every 16 ticks corrects most of the rest.
