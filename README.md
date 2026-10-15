# Auto Completer (Geode mod)

A pathfinding bot for **Geometry Dash 2.2081** on **Geode v5**. It plays levels for you in real time: no waiting, no freezing. It has play styles, a path viewer and a Mega Hack style ImGui menu (**Tab**).

## How it works
- **Hidden player.** The bot keeps an invisible copy of your player inside the level. Every 16 ticks (GD runs 240 per second) it copies the real player onto it and simulates about 1.25 s ahead against the level's blocks, spikes, slopes, orbs and pads. The real game is never paused, rewound or touched.
- **Exact physics.** The bot records which physics calls GD makes on your player each tick (touched rings, internal actions, update, collisions, rotation) and the hidden player replays exactly that sequence, plus GD's inlined per-tick housekeeping. It copies every physics field of the real player (171 of them), plus the orbs it is touching and GD's hidden click buffer, so a click on an orb works in the simulation exactly when it works in the game. Orbs and pads the real player already used are spent for the simulation too. Every real tick is also replayed on the hidden player from the same state and compared (its fields, its hitbox, and whether it dies where the real player doesn't); the match rate shows as **Tick match** in Stats.
- **Search only when needed.** If the predicted path dies, the planner looks back from the death for ticks where pressing or releasing avoids it. Those ticks form a safe timing window. The play style picks a tick inside it, always at least 3 ticks from its edges:
  - **Default:** the middle of the window.
  - **Perfect:** the middle, scanned frame by frame.
  - **Legit Human:** natural jitter in timing and tap length.
  - **Minimal:** the latest tick that still keeps the margin.

  A click only counts if the same click one tick earlier and later also works, so a small timing error can't kill. Cube, robot, ball and spider only press where a press does something (on the ground, on an orb, or just before an orb while still holding). If nothing in the window works, the planner may take an orb click that only changes how the player would die and look for the follow-up click from there (orb chains, like a gravity orb up and another one back down), at most two in a row, and only if there is still something to click on afterwards. Otherwise it searches forward over every sequence of clicks from 60, then 140, then 280 ticks before the death (never inside the part that is fixed): at every tick where a click can do something, orb touches first, right after each click checking whether no further click is needed, and merging paths that reach the same state, until the player survives 40 ticks past every death found so far. That finds the ways through that need several clicks where the first one alone looks worse: jumping into an orb that isn't on the current path and clicking it, an earlier jump that leaves room for the next one, or an earlier click that has to change so a later orb can save the player. If even that fails, the stretch is planned again from scratch. Normally the next 12 ticks stay fixed, so the bot never changes an input it is about to play. A search that needs longer keeps going while the game plays on: its fixed part grows with the game, and when one of its own clicks comes due the game plays the search's path from there on (the old plan dies anyway) while the search carries on (and it gets more time per frame). When no safe path is found it plans again at once with nothing fixed, so a click needed in the very next ticks isn't missed.
- **Moving objects.** Every move trigger GD starts is recorded (offset, duration, easing, lock to player), and the simulation starts the move triggers the player will pass, directly or through a spawn trigger. The hidden player meets moving hazards and platforms where they will be.
- **Toggles.** Toggle triggers the player passes (directly or through a spawn trigger) switch their groups on or off in the simulation too, so a hazard that is about to appear is seen in time.
- **Portals.** Speed, size and mode portals are applied to the hidden player at the step GD applies them (straight portals on first touch, turned portals once the player's center reaches them, checked against every turned portal the real player goes through), so the bot plans through them. Gravity, dual and teleport portals end the look-ahead: the bot plans up to the portal and plans again from the real player's new state the moment it is through (it does the same after every mode change, free to change even the next tick). A hazard that hits the hidden player in the very step it goes through a mode portal ends the look-ahead at that portal the same way: GD's own handling there (the new mode's floor and ceiling, its order of checks) isn't simulated, and the real game has let the player through such spots.

The planner (`src/planner`) and the move model (`src/bot/Motion.hpp`) have no Geode dependencies. CI runs three native test suites with sanitizers on every push:
- `tests/planner_test.cpp`: the planner against a toy physics model, including the exact look-ahead loop the game uses, orb chains, and the same loop sliced into a few planner steps per frame.
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
- Past a mode portal the simulation keeps the game's current floor and ceiling (a new ship or ball corridor only exists once the real player is through), and gravity, dual and teleport portals end the look-ahead; the bot plans again from the real player the moment it is through.
- Toggles: toggle triggers the player passes and toggle orbs are simulated; groups switched by touch, collision, count or item triggers are only seen once they happen.
- Moving objects: groups moved by move triggers are placed exactly (offset, duration, easing, lock to player), including move triggers the player starts by passing them, directly or through a spawn trigger. Anything else that moves (follow, rotate, keyframe and area triggers, move-to-target, chained or remapped spawns, touch or collision triggers) is extrapolated from its current speed, and re-checking every 16 ticks corrects most of the rest.
