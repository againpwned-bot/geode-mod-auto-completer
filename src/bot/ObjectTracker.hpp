#pragma once

#include <Geode/Geode.hpp>

#include "Motion.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ac {

// Knows where moving objects will be, so the hidden player meets a moving hazard or platform
// where it will be instead of where it is now.
//
// GD moves objects early in each physics step, before the player's collision checks, so
// between steps every object sits where that step's collisions saw it: the "world step".
// Groups a move trigger is moving are placed exactly from the trigger's offset, duration and
// easing. Move triggers the player starts by passing them (directly or through a spawn
// trigger) are started by the simulation when the hidden player passes them. Anything else
// that moves is extrapolated from its measured speed.
class ObjectTracker {
public:
    static constexpr int kMoveTrigger = 901;
    static constexpr int kSpawnTrigger = 1268;
    static constexpr int kToggleTrigger = 1049;

    // A group switched on or off at the end of physics step `step`.
    struct Toggle {
        int group = 0;
        int step = 0;
        bool on = false;
        void const* source = nullptr; // trigger that did it, if known
        bool predicted = false;       // expected from a spawn trigger, not done yet
    };

    static ObjectTracker& get();

    struct Track {
        double x = 0.0;
        double y = 0.0;
        double vx = 0.0;
        double vy = 0.0;
        int step = 0;
        bool listed = false;
        uint64_t placed = 0; // simulated tick that last placed the object
    };

    // Physics step whose object positions the level shows from now on (-1: level start).
    void setWorldStep(int step);
    int worldStep() const { return m_worldStep; }

    // Records where `object` is in the current world step.
    Track& observe(GameObject* object);
    // Observes every object in the sections the level currently shows, so objects still far
    // from the player are already known to move when they matter.
    void observeVisible(GJBaseGameLayer* layer);
    // Objects that moved on the last steps they were seen.
    std::vector<GameObject*> const& movingObjects() const { return m_moving; }

    // A move started in the real game (GJEffectManager::createMoveCommand).
    void onMoveCommand(
        cocos2d::CCPoint offset, int group, float duration, int easing, float rate, bool lockPlayerX,
        bool lockPlayerY, bool lockCameraX, bool lockCameraY, float modX, float modY
    );
    // A group was switched on or off in the real game (GJBaseGameLayer::toggleGroup).
    void onToggle(int group, bool on);
    // Whether `object` is switched on during step `step` according to toggles after the world
    // step (expected ones and `extra`, the simulation's own): 1 on, 0 off, -1 nothing known.
    int enabledAt(GameObject* object, int step, std::vector<Toggle> const* extra) const;
    std::vector<Toggle> const& expectedToggles() const { return m_expectedToggles; }
    size_t toggleCount() const { return m_toggleLog.size(); }
    // A trigger is being activated in the real game; `playerX` is where player 1 is.
    // Returns the trigger that was being activated before, for endTrigger.
    EffectGameObject* beginTrigger(EffectGameObject* trigger, GJBaseGameLayer* layer, double playerX);
    void endTrigger(EffectGameObject* previous) { m_currentTrigger = previous; }
    std::vector<MoveCommand> const& commands() const { return m_commands; }

    // Move, toggle and spawn triggers that fire when the player passes them, ordered by x.
    struct PositionTrigger {
        double x = 0.0;
        EffectGameObject* trigger = nullptr;
    };
    // Finds them; runs once per level, after the level's objects exist.
    void scanLevel(GJBaseGameLayer* layer);
    bool levelScanned() const { return m_scanned; }
    // Moves and toggles started when the player passes from `fromX` to `toX` during step `step`.
    void eventsBetween(
        GJBaseGameLayer* layer, double fromX, double toX, int step, std::vector<MoveCommand>& moves,
        std::vector<Toggle>& toggles
    );
    // Player x where position triggers fire, relative to the trigger.
    double activationLead() const { return m_leadCount > 0 ? m_lead : 0.0; }

    struct Moved {
        GameObject* object = nullptr;
        double positionX = 0.0;
        double positionY = 0.0;
        cocos2d::CCPoint node;
        cocos2d::CCPoint last;
    };
    struct Context {
        int step = 0;                                    // physics step being simulated
        std::vector<MoveCommand> const* extra = nullptr; // moves the simulation started itself
        double playerDx = 0.0;                           // hidden player's travel since the world step
        double playerDy = 0.0;
    };
    // Moves `object` to where it will be during the step being simulated, recording how to
    // undo it. Objects that aren't moving are left alone.
    void predict(GameObject* object, Track const& track, Context const& ctx, std::vector<Moved>& moved) const;
    // Where `object` is expected during `step`, the player having travelled (dx, dy) by then.
    cocos2d::CCPoint estimate(GameObject* object, int step, double playerDx, double playerDy) const;
    static void restore(std::vector<Moved>& moved);
    // Whether `group` is being moved during (world step, step].
    static bool movesDuring(MoveCommand const& command, int worldStep, int step);

    // Hazards, orbs and pads: what the hidden player must meet even while it is still filed
    // under a part of the level its own collision check doesn't look at.
    static bool reachType(GameObject* object);
    // Those objects in a group (cached per level), for moving groups far from the player.
    std::vector<GameObject*> const& groupObjects(GJBaseGameLayer* layer, int group);
    // Every orb and pad in the level (for the log).
    std::vector<GameObject*> const& orbs() const { return m_orbs; }

    // Groups and running moves of an object, for the log.
    std::string describe(GameObject* object) const;
    // How far groups really move per unit of a move's offset: 1 unless measurements clearly
    // show another simple factor.
    double offsetScale() const { return m_offsetScale; }

    void resetAttempt();
    void resetLevel();
    size_t size() const { return m_tracks.size(); }
    size_t movingCount() const { return m_moving.size(); }

private:
    bool commandFor(EffectGameObject* trigger, int start, MoveCommand& out) const;
    // How far `object` moves from the world step to `ctx.step`; false if it doesn't.
    bool offsetAt(GameObject* object, Track const* track, Context const& ctx, double& dx, double& dy) const;
    // Compares a measured move of `object` between two steps with the move the commands say.
    void calibrate(GameObject* object, int from, int to, double dx, double dy);
    // Move and toggle triggers a spawn trigger starts.
    std::vector<EffectGameObject*> const& spawnedTriggers(GJBaseGameLayer* layer, EffectGameObject* spawn);
    static Toggle toggleFor(EffectGameObject* trigger, int step);
    void indexCommands();

    std::unordered_map<GameObject*, Track> m_tracks;
    std::vector<GameObject*> m_moving;
    int m_worldStep = -1;
    int m_lastPrune = 0;

    // Real moves, plus the moves of spawn triggers that fired with a delay (predicted until
    // the spawned move trigger really fires).
    std::vector<MoveCommand> m_commands;
    std::unordered_map<int, std::vector<int>> m_byGroup;
    // Triggers whose moves are known for real; the simulation doesn't start them again.
    std::unordered_set<void const*> m_sources;
    EffectGameObject* m_currentTrigger = nullptr;
    int m_unknownSources = 0;

    // Real toggles (latest first matters for the log) and ones expected from spawn delays.
    std::vector<Toggle> m_toggleLog;
    std::vector<Toggle> m_expectedToggles;
    std::unordered_set<void const*> m_toggleSources;

    std::vector<PositionTrigger> m_triggers;
    std::unordered_map<EffectGameObject*, std::vector<EffectGameObject*>> m_spawned;
    std::unordered_map<int, std::vector<GameObject*>> m_groupObjects;
    std::vector<GameObject*> m_orbs;
    bool m_scanned = false;
    bool m_loggedSections = false;

    // Units of a move trigger's offset in position units, without and with "small step".
    double m_scale[2] = { 0.0, 0.0 };
    bool m_scaleKnown[2] = { false, false };
    double m_lead = 0.0;
    int m_leadCount = 0;
    std::vector<double> m_ratios; // latest measurements, as a ring
    int m_ratioCount = 0;
    double m_offsetScale = 1.0;
};

} // namespace ac
