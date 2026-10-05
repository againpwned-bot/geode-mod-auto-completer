# Auto Completer (Geode mod)

A pathfinding bot for **Geometry Dash 2.2081** on **Geode v5**. It plans a safe route through a level with the game's own physics, then plays it automatically. It has play styles, a path viewer and a Mega Hack style ImGui menu (**Tab**).

## How it works
1. When a level starts, the bot freezes the game and plans. It runs the real game forward tick by tick (240 ticks/s) using GD's checkpoint system to rewind.
2. With nobody pressing anything, the player eventually dies at some tick. The planner then looks back from that tick for every tick where flipping the input avoids the death. Those ticks form a safe timing window.
3. The play style picks a tick inside the window:
   - **Default**: middle of the window.
   - **Perfect**: frame precise scan and the middle of the widest window.
   - **Legit Human**: jittered timing and tap length.
   - **Minimal**: the latest safe tick.
4. If no tick works, it backtracks to the previous decision and tries its next best option (depth first search with a work budget).
5. Once the path reaches the end, the level restarts and the bot plays the recorded inputs. With **Auto correct** on, any drift between the real run and the plan triggers a re-plan from that point.

The planner lives in `src/planner` with no Geode dependencies. It's unit tested against a toy physics model in `tests/planner_test.cpp`, and CI runs that test with sanitizers on every push.

## Safe mode
Safe mode is always on while the bot is active. It uses the same flag as start positions, so nothing is saved: no progress, stars, coins or leaderboard submissions. The bot is disabled on editor levels, so it can't verify uploads.

## Install (Windows or Linux through Steam/Proton)
1. Install [Geode](https://geode-sdk.org) for GD 2.2081.
2. Download the `.geode` file from the latest successful run on the repo's **Actions** tab (artifact "Auto Completer (Windows)").
3. Copy it to `<GD folder>/geode/mods/`. On Linux with Steam the GD folder is usually `~/.local/share/Steam/steamapps/common/Geometry Dash/`.
4. Start the game, open a level and press **Tab** for the menu.

## Building locally
- **Windows**: install the Geode CLI and SDK, then run `geode build`.
- **Linux**: run `geode sdk install-linux` once, then `geode build`. This cross-compiles the Windows DLL.

To run the planner tests:

```sh
g++ -std=c++20 -O2 -I src tests/planner_test.cpp src/planner/Planner.cpp -o planner_test && ./planner_test
```

## Limitations
- Classic levels only (no platformer or 2-player yet).
- Planning time grows with level length. The *Perfect* style is the slowest.
