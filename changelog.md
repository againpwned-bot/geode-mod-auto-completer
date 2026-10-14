# v1.3.3
- Fix: moving objects (move triggers) were frozen in place in the simulation, so on harder levels the bot only noticed a moving spike or block a few ticks before hitting it, too late to react ("give up", then death). The bot now measures how fast every nearby and on-screen object moves and the hidden player sees each one where it will be at that moment, including moving hazards that are still far away.
- The log's 5 second summary shows how many moving objects the bot is tracking.

# v1.3.2
- Diagnostics for harder levels: when the real player dies, the log names the object that killed it (id, type, size, whether it was moving, hidden, disabled or of a type the simulation skips) and replays that exact tick on the hidden player to show whether the simulation agrees.

# v1.3.1
- Fix: the bot jumped too early because it indexed its plan by GD's tick counter, which in 2.2081 advances by 2 per physics step. Plans were played back at double speed, so every click came early and the real player constantly drifted from the prediction. The bot now counts physics steps itself.

# v1.3.0
- Fix: the bot clicked at the very edge of the safe timing window (Minimal had zero margin, and every style preferred jumps that survived a few ticks longer), so a single tick of error killed it on spikes and double spikes. Clicks now sit at least 3 ticks inside the window and must also work a tick earlier and later.
- Fix: cube, robot, ball and spider no longer press in mid-air, where a press either does nothing or depends on exact landing timing.
- Fix: jumps are judged until they land, not just 60 ticks past the obstacle.
- If the previous plan leads into a dead end, the stretch is planned again from scratch before giving up.
- Speed and size portals are simulated; mode and gravity portals end the look-ahead instead of predicting the wrong mode or gravity.
- Diagnostics: the log reports tick checks on press/release ticks, a summary every 5 seconds and what the plan expected when the real player died.
- New end-to-end test of the bot loop on GD-accurate physics with input timing errors (288 of 288 runs complete).

# v1.2.3
- Fix: a check that kept a jump from the previous plan could only move that jump earlier, never later, so the bot locked in early jumps and gave up ("No safe path found"). Stuck checks now plan the stretch again from scratch.
- Fix: plans are indexed by the tick the game is about to run, even if GD advances its tick counter before the physics step.
- The hidden player now updates its last position every tick like the real one; the tick check ignores once-per-frame bookkeeping.
- Diagnostics: when a check gets stuck, the log shows the planner's search and compares snapshot restores against fresh simulations (and repairs restores automatically if they lose state).

# v1.2.2
- Fix: the hidden player copied only part of the real player's state. It now copies every physics field GD uses (171 of them), including the level's 2.2 physics flags, the boost/fall state and the slope state, so it jumps exactly like the real one.
- Diagnostics: every real tick is replayed on the hidden player and compared ("Tick match" in Stats). Differences, a jump self test and what killed the simulation are written to the Geode log.

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
