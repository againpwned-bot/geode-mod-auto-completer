# Auto Completer

A pathfinding bot for Geometry Dash. Open a level and it plans a safe route with the game's own physics, then plays it for you. You don't need to touch anything.

## Features
- **Real pathfinding**: the bot rewinds with GD checkpoints and tests jump timings against the level's actual physics, triggers and hitboxes.
- **Play styles**: *Default*, *Perfect* (frame precise, widest margins), *Legit Human* (natural timing and tap lengths) and *Minimal* (fewest, latest clicks).
- **Path viewer**: draws the planned route in the level, green while holding and white while released, with markers where each click starts and ends.
- **Auto correct**: re-plans from the current point if the run ever drifts from the plan.
- **Ignore inputs**: blocks your own clicks while the bot plays.
- **Mega Hack style menu**: press **Tab** (configurable in the mod settings).

## Safe mode
While the bot is active, safe mode is always on. No progress, stars, coins or leaderboard submissions are saved, and the bot is disabled on editor levels, so it can't be used to verify a level.

## Limitations
- Classic levels only. Platformer and 2-player levels are not supported yet.
- The bot pauses in practice mode.

Menu font: Roboto (Apache License 2.0).
