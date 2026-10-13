# v1.2.1
- Fix: the bot jumped too early and glitched on blocks. The hidden player now runs GD's per-tick housekeeping (collision limits, collision log, jump and state timers, player clock) like the real one.
- Fix: predictions were compared against the real player one tick too early, which made the bot pick the wrong input timing.
- The bot detects which of GD's inlined per-tick functions it has to run itself, and calibrates where to run them against the real player.
- The hidden player can no longer be "killed" by the anticheat spike.
- Stats window shows the physics model and the per-tick calls it replays.

# v1.2.0
- Rebuilt the bot around a hidden simulation player: it plays in real time, never freezes and never rewinds the real game.
- The hidden player replays the exact physics calls GD makes each tick, and the bot measures its own input timing.
- Orbs and pads touched by the simulation are restored, so the real player can always use them.
- Removed precompute mode and checkpoint based simulation (slow, and glitched the player on blocks).

# v1.1.0
- Live mode: play at normal speed while the bot plans ahead in small slices each frame (default).
- Music and sound effects are muted for the bot's look-ahead only.
- Fix: simulated ticks could stall if the game skipped a physics step.

# v1.0.0
- Initial release: checkpoint based pathfinder, 4 play styles, path viewer, auto correct, ImGui menu.
