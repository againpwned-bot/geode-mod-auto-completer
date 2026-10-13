# Auto Completer (Geode mod)

A pathfinding bot for **Geometry Dash 2.2081** on **Geode v5**. It plays levels for you in real time: no waiting, no freezing. It has play styles, a path viewer and a Mega Hack style ImGui menu (**Tab**).

## How it works
- **Hidden player.** The bot keeps an invisible copy of your player inside the level. Every 16 ticks (GD runs 240 per second) it copies the real player onto it and simulates about 0.8 s ahead against the level's blocks, spikes, slopes, orbs and pads. The real game is never paused, rewound or touched.
- **Exact physics calls.** On the first tick the bot records which physics calls GD makes on your player (update / collision / post-collision), and the hidden player replays exactly that sequence. It also measures whether a click takes effect on the same tick or the next by comparing its predictions with what the real player does. It keeps whichever model predicts better; the accuracy shows in the Stats window.
- **Search only when needed.** If the predicted path dies, the planner looks back from the death for ticks where pressing or releasing avoids it. Those ticks form a safe timing window. The play style picks a tick inside it:
  - **Default:** the middle of the window.
  - **Perfect:** the middle, scanned frame by frame.
  - **Legit Human:** natural jitter in timing and tap length.
  - **Minimal:** the latest safe tick.

  If nothing in the window works, the planner backtracks to earlier decisions. The next 12 ticks are always fixed, so the bot never changes an input it is about to play.
- **Portals.** Portals and triggers are left to the real game. The plan is re-checked from the real player's state, so the bot adapts after each portal.

The planner (`src/planner`) has no Geode dependencies. It is unit tested against a toy physics model in `tests/planner_test.cpp`, including the exact look-ahead loop the game uses. CI runs those tests with sanitizers on every push.

## Safe mode
Safe mode is always on while the bot is active. It uses the same flag as start positions, so nothing is saved: no progress, stars, coins or leaderboard submissions. The bot is disabled on editor levels, so it can't verify uploads.

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
```

## Limitations
- Classic levels only (no platformer or 2-player yet).
- Moving objects are predicted where they currently are. Re-checking every 16 ticks keeps this close, but fast movers can still surprise it.
