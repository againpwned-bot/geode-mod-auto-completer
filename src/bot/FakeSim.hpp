#pragma once

#include <Geode/Geode.hpp>

#include "../planner/Planner.hpp"
#include "PlayerState.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ac {

// One physics call the real game makes on player 1 during a tick.
struct TickOp {
    enum class Kind : uint8_t { ResetRings, Internal, Update, Collide, Post, Rotate };
    Kind kind = Kind::Update;
    float dt = 0.f;
    bool flag = false;
};

// GD also runs a few small per-tick functions that are inlined on Windows, so they can't be
// hooked or recorded: preCollision, resetCollisionLog, updateJumpVariables and
// updateStateVariables. This says where the hidden player runs them.
struct PhysicsModel {
    // Inside: already part of one of the recorded calls, don't run it again.
    // Start / End: run it at the start (after the input) or the end of the tick.
    enum class Place : uint8_t { Inside, Start, End };

    int inputDelay = 0;
    Place jumpVars = Place::Start;
    Place stateVars = Place::Start;
    bool preCollision = true;   // run preCollision before each checkCollisions
    bool collisionLog = true;   // run resetCollisionLog(false) once per tick

    static constexpr int kVariants = 18;
    // Calibration candidates: input delay x jump variables x state variables.
    static PhysicsModel variant(int index, PhysicsModel const& base);
    int variantIndex() const;
    std::string describe() const;
};

// Which inlined functions turned out to already run inside the recorded calls.
struct InlineProbe {
    bool done = false;
    bool preCollision = false;
    bool collisionLog = false;
    bool jumpVars = false;
    bool stateVars = false;
};

// Records the player physics calls GD makes in one tick (one processCommands step), so the
// hidden player replays exactly the same steps instead of guessing GD's internal order.
// The calls are split at each processCommands call, so it works whether GD makes them
// inside processCommands or after it.
class TickRecorder {
public:
    static TickRecorder& get();

    void beginFrame(PlayerObject* player, unsigned progress);
    // Called at each processCommands; a new step only when the tick counter moved.
    void stepStarted(unsigned progress);
    void endFrame(unsigned progress);
    // Called by the hooks around each physics call; nested calls are ignored.
    void enter(PlayerObject* player, TickOp::Kind kind, float dt, bool flag);
    void leave();

    bool ready() const { return !m_template.empty(); }
    std::vector<TickOp> const& ops() const { return m_template; }
    // Player clock advance per tick in seconds, 0 if not measured yet.
    double timeStep() const { return m_timeStep; }
    unsigned revision() const { return m_revision; }
    std::string describe() const;
    void reset();

private:
    PlayerObject* m_player = nullptr;
    bool m_active = false;
    int m_step = 0;
    int m_depth = 0;
    unsigned m_stepProgress = 0;
    bool m_strong = false;
    unsigned m_revision = 0;
    double m_lastTime = -1.0;
    double m_timeStep = 0.0;
    std::vector<TickOp> m_segment;
    std::vector<TickOp> m_template;
};

// Tracks how fast objects move (move triggers, followers, ...), so the hidden player sees
// where a moving hazard or platform will be instead of where it is right now. GD moves
// objects early in each physics step, before the player's collision checks, so between
// steps every object sits where that step's collisions saw it; velocities come from those
// positions on nearby steps and are extrapolated linearly.
class ObjectTracker {
public:
    static ObjectTracker& get();

    // Physics step whose object positions the level shows from now on (-1: level start).
    void setWorldStep(int step);
    int worldStep() const { return m_worldStep; }

    // Records where `object` is in the current world step.
    void observe(GameObject* object);
    // Observes every object in the sections the level currently shows, so objects that are
    // still far from the player are already known to move when they matter.
    void observeVisible(GJBaseGameLayer* layer);
    // Objects that moved on the last steps they were seen.
    std::vector<GameObject*> const& movingObjects() const { return m_moving; }

    struct Moved {
        GameObject* object = nullptr;
        double positionX = 0.0;
        double positionY = 0.0;
        cocos2d::CCPoint node;
        cocos2d::CCPoint last;
    };
    // Moves `object` to where it is predicted to be during physics step `step`, recording how
    // to undo it. Objects that aren't moving are left alone.
    void predict(GameObject* object, int step, std::vector<Moved>& moved) const;
    static void restore(std::vector<Moved>& moved);

    void reset();
    size_t size() const { return m_tracks.size(); }
    // Objects seen moving lately.
    size_t movingCount() const;

private:
    struct Track {
        double x = 0.0;
        double y = 0.0;
        double vx = 0.0;
        double vy = 0.0;
        int step = 0;
        bool listed = false;
    };
    std::unordered_map<GameObject*, Track> m_tracks;
    std::vector<GameObject*> m_moving;
    int m_worldStep = -1;
    int m_lastPrune = 0;
    bool m_loggedSections = false;
};

class FakeSim;

// What the hooks need to know while the hidden player is being simulated.
struct FakeContext {
    bool active = false;
    PlayerObject* player = nullptr;
    FakeSim* sim = nullptr;
    bool died = false;
    GameObject* killer = nullptr;
};

// Where and by what the hidden player last died (diagnostics).
struct DeathInfo {
    bool valid = false;
    int tick = 0;
    float x = 0.f;
    float y = 0.f;
    int objectId = 0;
    int objectType = -1;
    float objectX = 0.f;
    float objectY = 0.f;
};

FakeContext& fakeContext();

// The hidden player that simulations run on. It lives in an invisible container inside the
// level so its particles and trails are never drawn.
class FakeWorld {
public:
    static FakeWorld& get();

    bool attach(PlayLayer* layer);
    void detach(PlayLayer* layer);
    PlayerObject* player() const { return m_player; }

private:
    PlayLayer* m_layer = nullptr;
    cocos2d::CCLayer* m_container = nullptr;
    PlayerObject* m_player = nullptr;
};

// Simulator that runs the hidden player against the level's blocks, hazards, orbs and pads.
// The real game is never touched: snapshots only copy the hidden player's state, and the
// "already used" flags of orbs and pads it touches are restored afterwards.
class FakeSim final : public Simulator {
public:
    // Copies the real player 1 (or `from`, a state captured from it earlier) onto the hidden
    // player; tick 0 is the real game's current tick.
    // `baseStep` is the real physics step that simulated tick 0 corresponds to; it is used
    // to place moving objects.
    FakeSim(
        PlayLayer* layer, PlayerObject* fake, std::vector<TickOp> ops, double timeStep, PhysicsModel model,
        bool hold, int baseStep, PlayerFix const* from = nullptr
    );
    ~FakeSim() override;

    FakeSim(FakeSim const&) = delete;
    FakeSim& operator=(FakeSim const&) = delete;

    // A planner may keep a FakeSim across frames. Between slices the hidden player is
    // free for other uses and every touched object is back to its real state.
    void beginSlice();
    void endSlice();

    int tick() const override { return m_tick; }
    int save() override;
    void load(int handle) override;
    void release(int handle) override;
    RunResult run(int untilTick, InputFn const& input, SampleFn const& sample) override;

    // Remembers an orb or pad before the hidden player may activate it.
    void noteObject(GameObject* object);
    // Portals are applied by hand, never through the game layer: speed and size change
    // the hidden player; mode, gravity, dual and teleport portals end the look-ahead (the
    // real player gets there first and later checks continue from its new state).
    void notePortal(GameObject* object);

    // Runs each recorded call once with marker values to see which inlined functions it
    // already contains. Leaves the hidden player as it was.
    InlineProbe probeInlines();

    void captureState(PlayerFix& out) const { out.capture(m_fake); }
    // When set, restoring a snapshot also re-copies the real player's attributes (mode,
    // size, ...) first, like a fresh simulation does.
    static bool& reattributeOnLoad();
    void setClock(double time) { if (m_fake) m_fake->m_totalTime = time; }
    DeathInfo const& lastDeath() const { return m_lastDeath; }
    // Real physics step the tick being simulated corresponds to.
    int absoluteStep() const { return m_baseStep + m_tick; }
    // Moves `object` to where it will be during the tick being simulated if it is moving;
    // undone when the tick ends.
    void placeObject(GameObject* object);

    long ticksRun() const { return m_ticksRun; }
    double secondsRun() const { return m_secondsRun; }

private:
    struct Flags {
        bool activated = false;
        bool byPlayer1 = false;
        bool byPlayer2 = false;
    };
    struct LogEntry {
        EnhancedGameObject* object = nullptr;
        Flags base;
    };
    struct Slot {
        PlayerFix fix;
        bool held = false;
        bool previousWant = false;
        int tick = 0;
        std::vector<Flags> flags;
        std::vector<GameObject*> portals;
        bool used = false;
    };

    static Flags read(EnhancedGameObject* object);
    static void write(EnhancedGameObject* object, Flags const& flags);
    void restoreBaseline();
    void runOp(TickOp const& op);
    void stepPhysics();
    void stepPlayer();
    // Checks moving hazards that will be near the hidden player but sit in sections its own
    // collision check doesn't look at yet.
    void collideMovingHazards(float dt);

    PlayLayer* m_layer = nullptr;
    PlayerObject* m_fake = nullptr;
    std::vector<TickOp> m_ops;
    double m_timeStep = 0.0;
    int m_baseStep = 0;
    std::vector<ObjectTracker::Moved> m_moved;
    cocos2d::CCPoint m_killerPosition;
    PhysicsModel m_model;
    bool m_hasRingReset = false;
    bool m_held = false;
    bool m_previousWant = false;
    int m_tick = 0;
    bool m_inSlice = true;
    int m_cursor = -1;

    std::vector<GameObject*> m_portalsUsed;
    bool m_reachedGoal = false;
    std::vector<LogEntry> m_log;
    std::unordered_set<EnhancedGameObject*> m_logged;
    std::vector<Slot> m_slots;
    std::vector<int> m_free;

    DeathInfo m_lastDeath;
    long m_ticksRun = 0;
    double m_secondsRun = 0.0;
};

} // namespace ac
