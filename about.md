# Auto Completer

A pathfinding bot for Geometry Dash that plays levels for you in real time, with no waiting or freezing.

## Features
- **Real-time pathfinding**: an invisible copy of your player simulates ~0.8 s ahead against the level's blocks, spikes, orbs and pads, and the bot clicks for you. The real game is never paused or rewound.
- **Play styles**: *Default*, *Perfect* (frame precise, widest margins), *Legit Human* (natural timing and tap lengths) and *Minimal* (fewest, latest clicks).
- **Path viewer**: the planned route is drawn in the level (green holding, white released) with click markers.
- **Ignore inputs**: blocks your own clicks while the bot plays.
- **Mega Hack style menu**: press **Tab** (configurable in the mod settings).

## Safe mode
While the bot is active, safe mode is always on. No progress, stars, coins or leaderboard submissions are saved, and the bot is disabled on editor levels so it can't be used to verify a level.

## Limitations
- Classic levels only. Platformer and 2-player levels are not supported yet.
- The bot pauses in practice mode.

Menu font: Roboto (Apache License 2.0).
