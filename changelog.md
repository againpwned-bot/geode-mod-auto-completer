# v1.8.1
- Fix: after the game started playing a running search's path (new in 1.8.0), the search could still drop one of those clicks later and keep going as if the game wouldn't press there, while the game did. It now checks against what the game really plays.
- Fix: a search that started while the button was already held assumed the hold would last its usual tap length, but the game keeps holding as long as the plan it is playing says to (Legit Human taps vary in length). Also, when a plan ends mid-hold in ship/wave-style modes the planner now knows the game lets go there.
- New test: 150 random orb chains, each in every play style at 144 and 60 FPS with a realistic planning budget and a starved one (2400 runs). Every path the bot plans must match what the game really played, and with the realistic budget every chain the planner can beat at all is beaten live. Before these fixes some of those runs planned from a state the game wasn't in.

# v1.8.0
- Fix (the actual I Spy killer): every log showed the bot's player physics matching the game exactly, every tick, yet it always played the same plan into the eye. The plan it played was made around tick 1840, before the death at 2048 was inside its 200 tick look-ahead. Every later search did see the death and went for the second orb early, but as soon as that click came close, the search was thrown away, because it differed from the old plan the game was playing. So the old plan kept playing until only late, useless clicks were left. Now, when a search's click comes due, the game plays the search's path from there on (the old plan dies anyway) and the search carries on to find the next click. Searches are also kept going whenever they are still consistent with what is being played, instead of being restarted.
- Fix: at the start of a level (or after a short plan), no search could be kept going at all, because there was no old plan to compare with; the game then presses nothing, and a search that presses nothing there can now carry on.
- A search may run for 180 ticks (was 120) while it is still valid.
- New test: the orb chains are now beaten with as little as 4 planner steps per frame in every play style. With the old rules every one of them died.

# v1.7.0
- Fix (I Spy second orb): GD 2.2081 stamps every click with the game layer's input clock and lets a buffered click run out by that clock. The bot's simulations run hundreds of physics steps inside one frame while that clock stood still, so in the bot's head a click never ran out: it believed its click at tick 1946 would still take the second orb when the player reached it 27 ticks later, saw no reason to click the orb itself, and the real game (where that click had long expired) flew the player into the eye. Simulations now move the game's clocks (input clock, step and tick counters, level time) along with every simulated step and put them back afterwards, so buffered clicks run out exactly like in the game.
- When the real player leaves the planned path, the bot now plans again right away with nothing fixed (like after a mode change), instead of keeping the next 12 ticks of the broken plan. It is logged when it happens.
- "Touching an orb" for the planner now means really inside an orb after that tick's collisions. GD's own "touched a ring" flag can stay set long after an orb, which made clicks in mid-air look like orb clicks.
- Logs: how far the input clock moves per physics step (once per level).

# v1.6.0
- Fix (the big one this time): hard searches never finished. Each plan check had about 3 frames before its fixed part ran out, and was then thrown away and started over. On an orb chain like the second orb on "I Spy" the search needs more than that, so it restarted forever (hundreds of checks, "planned 0.07 s ahead") while the bot followed an old plan that missed the orb, and by the time anything finished the moment to click had passed. Now a search keeps going while the game plays on, as long as it hasn't changed what is being played: its fixed part just grows with the game. A search that already took more than a frame also gets twice the time per frame. In a new test that runs the planner a few steps per frame like the game, every orb chain is now beaten; with the old behaviour every one of them died.
- Smarter orb chains: an orb click is only explored if there is still something to click on afterwards (ground or another orb). A late click on the second orb that just drops the player into the next hazard is no longer tried before the early click that lands on a ledge.
- Fix: when the only fixes found were "marginal" (a late orb click that dies a few ticks later), the planner stopped scanning and never looked at the earlier clicks on the same orb, the real way through. It now keeps scanning while the player is on an orb.

# v1.5.0
- Fix (the big one): the bot "ignored" orbs. When you click while already inside an orb, GD uses the list of orbs the player touched on the step before. The bot's hidden player never got that list from the real player, and every time a simulation was restored it kept whatever list an earlier run had left. So a click on an orb the player was inside of often did nothing in the bot's head, even though the real game took it: the bot thought the orb didn't help and planned around it (the second orb on "I Spy", the yellow orbs on "polarcris", where it then jumped too late). The list is now copied and restored with everything else, and the logs showed this was what broke every one of those orb clicks.
- Fix: GD's click buffer for orbs (a hidden anti-cheat counter) wasn't copied either, so after a restore the bot could believe an old click was still buffered and would still take the next orb. It is copied now.
- Fix: the hidden player could plan to use an orb or pad again that the real player had already used (GD keeps "used" per player). Orbs and pads the real player used are now spent for the simulation too. Anything the hidden player activates is always put back afterwards, however GD got there.
- Fix: portal timing. Straight portals switch the player the moment the hitboxes touch (measured on polarcris); only turned portals (like on "I Spy") wait for the player's center. v1.4.0 used the center rule for every portal, which switched the bot's prediction several ticks late at normal portals. The bot also no longer mistakes jump pads for portals when it checks portal timing.
- Rescue planning: when the bot finds no safe path, it now plans again at once, free to change even the very next tick (with extra time that frame), instead of keeping the next 12 ticks fixed. A click that was still possible a few ticks before a death is no longer missed.
- The planner also considers clicking just before an orb and holding into it, which GD activates too.

# v1.4.0
- Fix: the hidden player switched mode at portals too early. GD only switches the player once its center reaches the portal, not as soon as the hitboxes touch, so the bot believed it was a ship or ball a few ticks (on big or turned portals, dozens of ticks) before it really was, planned the tight part after the portal for the wrong mode and then had to scramble right after it ("panicking"). The simulation now uses GD's rule, and the bot checks every portal the real player goes through and adjusts if the game disagrees (logged).
- Fix: in tight spots the planner could settle for a click that only bought a few ticks (like jumping into the next hazard) and never try the orb click that leads to the real way through. When every plain fix is that short, orb clicks that change how the player would die are now tried first; otherwise they are kept as fallbacks when the plain fixes fail.
- Fix: the measured move scale could flip between 1x, 0.5x and 0.33x on levels where several things move the same groups, throwing moving hazards and orbs off. Moves are now always applied 1:1 (the measurement is only logged).
- Fix: move triggers placed before the level start were reported as "didn't fire" on every attempt; GD fires those while the level resets.
- Logs: the detailed logs (plans, stuck checks, orbs near the path, look-ahead stops, mode changes) now restart every attempt instead of running out after the first few attempts.

# v1.3.9
- Fix: switched-off portals (toggled off, disabled or "no touch") still counted in the simulation. A gravity, dual or teleport portal ends the look-ahead, so on "I Spy" a fake portal made the bot stop looking ahead before the hazard, believe the path was fine and never plan the orb click. GD ignores such portals and now so does the bot.
- Logs: when the look-ahead ends at a portal, the log says which portal and where.

# v1.3.8
- Fix: the simulation fired move, toggle and spawn triggers even when the trigger itself sat in a group that was switched off, which GD never does. On levels that steer their traps with toggles (like "I Spy") the bot then believed in moves or toggles that never happen: it planned as if a hazard would get out of the way, skipped the orb, and saw the real danger only when it was too late. Switched-off triggers are now skipped, counting the toggles expected before the trigger is reached.
- Faster toggle checks on levels with thousands of toggle triggers.
- Logs: triggers the real player passed that the game didn't fire are listed (and counted in the 5 second summary), so a wrong guess about triggers shows up right away.

# v1.3.7
- Fix: the bot didn't see orbs and pads that move along with the player (like the pink orb on "I Spy"). The hidden player only checks objects filed near its own spot in the level, and while it looks a second or two ahead, an orb travelling with the player is still filed back where the player is now. Moving hazards were already handled this way; orbs and pads now are too, including ones a toggle trigger is about to switch on.
- Logs: when the bot gets stuck, it lists the orbs and pads the planned path passes near (type, where they are expected to be at that moment, whether they are moving or switched off).

# v1.3.6
- Portals: the hidden player now goes through mode portals (ship, ball, UFO, wave, robot, spider, swing, cube) instead of stopping the look-ahead there, so the bot plans the tight part right after a portal ahead of time.
- Right after the real player changes mode, or goes through a gravity, dual or teleport portal the plan didn't expect, the bot plans again from its real state at once, free to change even the very next tick (it used to keep the next 12 ticks fixed).
- Toggle triggers: traps that are switched on or off by toggle triggers the player passes (directly or through a spawn trigger) are simulated, so a hazard that is about to appear is seen in time. Toggle orbs the hidden player touches no longer switch groups in the real level, and spawn orbs no longer fire for real.
- Fix: after leaving and re-entering a level the bot could stop with "Could not create the simulation player". The hidden player is now re-created whenever it goes missing.
- Logs: a simulated death says why the killer was there (its groups, the moves and toggles on them, and where it really is now); the level summary lists trigger types the bot doesn't simulate yet; tick checks also compare the game mode.

# v1.3.5
- Fix: the bot skipped the second orb in orb chains (like the gravity orbs on "I Spy"). It only took a click that kept the player alive longer, but there the first orb on its own makes things worse and only the second orb saves you. The planner now tries an orb click that changes how the player would die and looks for the follow-up click from there (at most two in a row, and it backs out if no follow-up works).
- The measured move scale ignores noise: GD moves groups by exactly their trigger offset, so small wobbles in the measurement no longer stretch predictions. The log line no longer repeats.
- New tests: orb chains for every play style, through the bot's own planning loop.

# v1.3.4
- Moving objects follow their move triggers: the bot reads each move as GD starts it (offset, duration, easing, lock to player or camera), so it knows where a moving hazard or platform stops instead of assuming it keeps going.
- Traps that haven't started moving yet: the simulation starts the move triggers the player will pass, directly or through a spawn trigger with its delay, so the bot sees them coming.
- Hazards moved by triggers are checked even while they are still far from the player.
- The bot measures how far groups really move per unit of trigger offset and when position triggers fire, and corrects itself (both are logged).
- Fix: the simulation applied speed portals one step before the real game.
- Safe mode is now a switch (menu and mod settings). It stays on by default.
- Death logs list the killer's groups and the moves running on them.

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
