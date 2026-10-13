#pragma once

#include <Geode/Geode.hpp>

#include "../planner/Planner.hpp"
#include "PlayerState.hpp"

#include <unordered_set>
#include <vector>

namespace ac {

// One physics call the real game makes on player 1 during a tick.
struct TickOp {
    enum class Kind : uint8_t { Update, Collide, Post };
    Kind kind = Kind::Update;
    float dt = 0.f;
    bool flag = false;
};

// Records the sequence of player physics calls (update / checkCollisions / postCollision)
// the real game makes in one tick, so the hidden player can replay exactly the same steps
// instead of guessing GD's internal order.
class TickRecorder {
public:
    static TickRecorder& get();

    // Frame level fallback, in case GD moves the player outside processCommands.
    void beginFrame(PlayerObject* player);
    void endFrame();
    void beginTick(PlayerObject* player);
    void endTick();
    // Called by the hooks around each physics call; nested calls are ignored.
    void enter(PlayerObject* player, TickOp::Kind kind, float dt, bool flag);
    void leave();

    bool ready() const { return !m_template.empty(); }
    std::vector<TickOp> const& ops() const { return m_template; }
    void reset();

private:
    PlayerObject* m_player = nullptr;
    bool m_active = false;
    PlayerObject* m_framePlayer = nullptr;
    bool m_frameActive = false;
    int m_frameTicks = 0;
    bool m_tickBased = false;
    int m_depth = 0;
    std::vector<TickOp> m_current;
    std::vector<TickOp> m_frameOps;
    std::vector<TickOp> m_template;
};

class FakeSim;

// What the hooks need to know while the hidden player is being simulated.
struct FakeContext {
    bool active = false;
    PlayerObject* player = nullptr;
    FakeSim* sim = nullptr;
    bool died = false;
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
    // Copies the real player 1 onto the hidden player; tick 0 is the real game's current tick.
    FakeSim(PlayLayer* layer, PlayerObject* fake, std::vector<TickOp> ops, int inputDelay, bool hold);
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
        bool used = false;
    };

    static Flags read(EnhancedGameObject* object);
    static void write(EnhancedGameObject* object, Flags const& flags);
    void restoreBaseline();
    void stepPhysics();

    PlayLayer* m_layer = nullptr;
    PlayerObject* m_fake = nullptr;
    std::vector<TickOp> m_ops;
    int m_delay = 0;
    bool m_held = false;
    bool m_previousWant = false;
    int m_tick = 0;
    bool m_inSlice = true;
    int m_cursor = -1;

    std::vector<LogEntry> m_log;
    std::unordered_set<EnhancedGameObject*> m_logged;
    std::vector<Slot> m_slots;
    std::vector<int> m_free;

    long m_ticksRun = 0;
    double m_secondsRun = 0.0;
};

} // namespace ac
