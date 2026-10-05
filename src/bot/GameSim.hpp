#pragma once

#include <Geode/Geode.hpp>

#include "../planner/Planner.hpp"

#include <vector>

namespace ac {

// State shared between GameSim::run and the gameplay hooks while a simulation is running.
struct SimContext {
    bool active = false;
    InputFn const* input = nullptr;
    SampleFn const* sample = nullptr;
    unsigned baseProgress = 0;
    bool appliedHold = false;
    int currentTick = 0;
    bool died = false;
    bool completed = false;
    int eventTick = -1;
};

SimContext& simContext();

// True while the bot itself is calling handleButton, so the input blocker lets it through.
bool& botInputFlag();

// Presses or releases the jump button through the game's normal input path.
void applyJump(GJBaseGameLayer* layer, bool down);

Mode modeOf(PlayerObject* player);

// Physics values of a player that GD's checkpoints are known to not always restore
// exactly. They are captured together with every snapshot and written back after a load.
struct PlayerFix {
    bool valid = false;
    cocos2d::CCPoint nodePosition;
    float rotation = 0.f;
    cocos2d::CCPoint position;
    double yVelocity = 0.0;
    double fallSpeed = 0.0;
    double yVelocityBeforeSlope = 0.0;
    double groundYVelocity = 0.0;
    double yVelocityRelated = 0.0;
    float yVelocityRelated3 = 0.f;
    float slopeVelocity = 0.f;
    double currentSlopeYVelocity = 0.0;
    double platformerXVelocity = 0.0;
    bool isOnGround = false;
    bool isOnGround2 = false;
    bool isOnGround3 = false;
    bool isOnGround4 = false;
    bool isOnSlope = false;
    bool wasOnSlope = false;
    double lastJumpTime = 0.0;
    double lastLandTime = 0.0;
    double lastFlipTime = 0.0;
    double lastSpiderFlipTime = 0.0;
    double totalTime = 0.0;
    bool jumpBuffered = false;
    bool stateRingJump = false;
    bool wasJumpBuffered = false;
    bool wasRobotJump = false;
    unsigned char stateJumpBuffered = 0;
    bool stateRingJump2 = false;
    bool touchedRing = false;
    bool touchedCustomRing = false;
    bool touchedGravityPortal = false;
    bool touchedPad = false;
    bool ringJumpRelated = false;
    bool padRingRelated = false;
    bool hasEverJumped = false;
    bool hasEverHitRing = false;
    bool fixRobotJump = false;
    int stateNoAutoJump = 0;
    int stateOnGround = 0;
    int stateHitHead = 0;
    int stateFlipGravity = 0;
    double collidedTopMinY = 0.0;
    double collidedBottomMaxY = 0.0;
    double collidedLeftMaxX = 0.0;
    double collidedRightMinX = 0.0;
    gd::map<int, bool> holdingButtons;
    gd::map<int, bool> jumpPadRelated;
    gd::unordered_set<int> touchedRings;

    void capture(PlayerObject* player);
    void apply(PlayerObject* player) const;
};

// Simulator backed by the live PlayLayer: snapshots are GD checkpoints and ticks are run
// by calling the layer's own update, so the planner sees the real physics, triggers and
// collisions of the level.
class GameSim final : public Simulator {
public:
    GameSim(PlayLayer* layer, bool appliedHold);
    ~GameSim() override;

    GameSim(GameSim const&) = delete;
    GameSim& operator=(GameSim const&) = delete;

    int tick() const override;
    int save() override;
    void load(int handle) override;
    void release(int handle) override;
    RunResult run(int untilTick, InputFn const& input, SampleFn const& sample) override;

    unsigned baseProgress() const { return m_base; }
    bool appliedHold() const { return m_hold; }
    int liveSnapshots() const { return m_live; }

    // Runs the same ticks twice from one snapshot and returns the largest position
    // difference between the two runs (0 means restores are exact).
    float selfTest(int ticks);

    // Pretend to be in practice mode while creating and loading checkpoints, in case the
    // game only saves some state for practice checkpoints.
    void setPracticeWrap(bool wrap) { m_practiceWrap = wrap; }
    bool practiceWrap() const { return m_practiceWrap; }

private:
    struct Slot {
        CheckpointObject* checkpoint = nullptr;
        PlayerFix player1;
        PlayerFix player2;
        bool hold = false;
        unsigned progress = 0;
        bool used = false;
    };

    PlayLayer* m_layer = nullptr;
    unsigned m_base = 0;
    bool m_hold = false;
    bool m_practiceWrap = false;
    // Level end flags from before planning; a simulated finish must not leak into the real run.
    bool m_endAnimationStarted = false;
    bool m_endChecked = false;
    int m_live = 0;
    std::vector<Slot> m_slots;
    std::vector<int> m_free;
};

} // namespace ac
