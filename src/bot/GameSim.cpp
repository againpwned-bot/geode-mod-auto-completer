#include "GameSim.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace ac {

namespace {
    // One physics step. GD 2.2 runs 240 steps per second.
    constexpr float kTickDt = 1.f / 240.f;
    // Updates in a row without the tick counter moving before a run is aborted.
    constexpr int kMaxStalls = 16;
}

SimContext& simContext() {
    static SimContext context;
    return context;
}

bool& botInputFlag() {
    static bool flag = false;
    return flag;
}

void applyJump(GJBaseGameLayer* layer, bool down) {
    if (!layer) return;
    auto& flag = botInputFlag();
    bool const previous = flag;
    flag = true;
    layer->handleButton(down, static_cast<int>(PlayerButton::Jump), true);
    flag = previous;
}

Mode modeOf(PlayerObject* player) {
    if (!player) return Mode::Cube;
    if (player->m_isShip) return Mode::Ship;
    if (player->m_isDart) return Mode::Wave;
    if (player->m_isBird) return Mode::Ufo;
    if (player->m_isBall) return Mode::Ball;
    if (player->m_isRobot) return Mode::Robot;
    if (player->m_isSpider) return Mode::Spider;
    if (player->m_isSwing) return Mode::Swing;
    return Mode::Cube;
}

void PlayerFix::capture(PlayerObject* player) {
    valid = player != nullptr;
    if (!player) return;

    nodePosition = player->getPosition();
    rotation = player->getRotation();
    position = player->m_position;
    yVelocity = player->m_yVelocity;
    fallSpeed = player->m_fallSpeed;
    yVelocityBeforeSlope = player->m_yVelocityBeforeSlope;
    groundYVelocity = player->m_groundYVelocity;
    yVelocityRelated = player->m_yVelocityRelated;
    yVelocityRelated3 = player->m_yVelocityRelated3;
    slopeVelocity = player->m_slopeVelocity;
    currentSlopeYVelocity = player->m_currentSlopeYVelocity;
    platformerXVelocity = player->m_platformerXVelocity;
    isOnGround = player->m_isOnGround;
    isOnGround2 = player->m_isOnGround2;
    isOnGround3 = player->m_isOnGround3;
    isOnGround4 = player->m_isOnGround4;
    isOnSlope = player->m_isOnSlope;
    wasOnSlope = player->m_wasOnSlope;
    lastJumpTime = player->m_lastJumpTime;
    lastLandTime = player->m_lastLandTime;
    lastFlipTime = player->m_lastFlipTime;
    lastSpiderFlipTime = player->m_lastSpiderFlipTime;
    totalTime = player->m_totalTime;
    jumpBuffered = player->m_jumpBuffered;
    stateRingJump = player->m_stateRingJump;
    wasJumpBuffered = player->m_wasJumpBuffered;
    wasRobotJump = player->m_wasRobotJump;
    stateJumpBuffered = player->m_stateJumpBuffered;
    stateRingJump2 = player->m_stateRingJump2;
    touchedRing = player->m_touchedRing;
    touchedCustomRing = player->m_touchedCustomRing;
    touchedGravityPortal = player->m_touchedGravityPortal;
    touchedPad = player->m_touchedPad;
    ringJumpRelated = player->m_ringJumpRelated;
    padRingRelated = player->m_padRingRelated;
    hasEverJumped = player->m_hasEverJumped;
    hasEverHitRing = player->m_hasEverHitRing;
    fixRobotJump = player->m_fixRobotJump;
    stateNoAutoJump = player->m_stateNoAutoJump;
    stateOnGround = player->m_stateOnGround;
    stateHitHead = player->m_stateHitHead;
    stateFlipGravity = player->m_stateFlipGravity;
    collidedTopMinY = player->m_collidedTopMinY;
    collidedBottomMaxY = player->m_collidedBottomMaxY;
    collidedLeftMaxX = player->m_collidedLeftMaxX;
    collidedRightMinX = player->m_collidedRightMinX;
    holdingButtons = player->m_holdingButtons;
    jumpPadRelated = player->m_jumpPadRelated;
    touchedRings = player->m_touchedRings;
}

void PlayerFix::apply(PlayerObject* player) const {
    if (!valid || !player) return;

    player->setPosition(nodePosition);
    player->setRotation(rotation);
    player->m_position = position;
    player->m_yVelocity = yVelocity;
    player->m_fallSpeed = fallSpeed;
    player->m_yVelocityBeforeSlope = yVelocityBeforeSlope;
    player->m_groundYVelocity = groundYVelocity;
    player->m_yVelocityRelated = yVelocityRelated;
    player->m_yVelocityRelated3 = yVelocityRelated3;
    player->m_slopeVelocity = slopeVelocity;
    player->m_currentSlopeYVelocity = currentSlopeYVelocity;
    player->m_platformerXVelocity = platformerXVelocity;
    player->m_isOnGround = isOnGround;
    player->m_isOnGround2 = isOnGround2;
    player->m_isOnGround3 = isOnGround3;
    player->m_isOnGround4 = isOnGround4;
    player->m_isOnSlope = isOnSlope;
    player->m_wasOnSlope = wasOnSlope;
    player->m_lastJumpTime = lastJumpTime;
    player->m_lastLandTime = lastLandTime;
    player->m_lastFlipTime = lastFlipTime;
    player->m_lastSpiderFlipTime = lastSpiderFlipTime;
    player->m_totalTime = totalTime;
    player->m_jumpBuffered = jumpBuffered;
    player->m_stateRingJump = stateRingJump;
    player->m_wasJumpBuffered = wasJumpBuffered;
    player->m_wasRobotJump = wasRobotJump;
    player->m_stateJumpBuffered = stateJumpBuffered;
    player->m_stateRingJump2 = stateRingJump2;
    player->m_touchedRing = touchedRing;
    player->m_touchedCustomRing = touchedCustomRing;
    player->m_touchedGravityPortal = touchedGravityPortal;
    player->m_touchedPad = touchedPad;
    player->m_ringJumpRelated = ringJumpRelated;
    player->m_padRingRelated = padRingRelated;
    player->m_hasEverJumped = hasEverJumped;
    player->m_hasEverHitRing = hasEverHitRing;
    player->m_fixRobotJump = fixRobotJump;
    player->m_stateNoAutoJump = stateNoAutoJump;
    player->m_stateOnGround = stateOnGround;
    player->m_stateHitHead = stateHitHead;
    player->m_stateFlipGravity = stateFlipGravity;
    player->m_collidedTopMinY = collidedTopMinY;
    player->m_collidedBottomMaxY = collidedBottomMaxY;
    player->m_collidedLeftMaxX = collidedLeftMaxX;
    player->m_collidedRightMinX = collidedRightMinX;
    player->m_holdingButtons = holdingButtons;
    player->m_jumpPadRelated = jumpPadRelated;
    player->m_touchedRings = touchedRings;
}

GameSim::GameSim(PlayLayer* layer, bool appliedHold)
    : m_layer(layer),
      m_base(layer->m_gameState.m_currentProgress),
      m_hold(appliedHold),
      m_endAnimationStarted(layer->m_levelEndAnimationStarted),
      m_endChecked(layer->m_endChecked) {}

GameSim::~GameSim() {
    for (auto& slot : m_slots) {
        if (slot.used && slot.checkpoint) {
            slot.checkpoint->release();
        }
    }
}

int GameSim::tick() const {
    return static_cast<int>(m_layer->m_gameState.m_currentProgress) - static_cast<int>(m_base);
}

int GameSim::save() {
    bool const practice = m_layer->m_isPracticeMode;
    if (m_practiceWrap) m_layer->m_isPracticeMode = true;
    CheckpointObject* checkpoint = m_layer->createCheckpoint();
    m_layer->m_isPracticeMode = practice;
    if (!checkpoint) return -1;
    checkpoint->retain();

    Slot slot;
    slot.checkpoint = checkpoint;
    slot.player1.capture(m_layer->m_player1);
    slot.player2.capture(m_layer->m_player2);
    slot.hold = m_hold;
    slot.progress = m_layer->m_gameState.m_currentProgress;
    slot.used = true;

    int handle;
    if (!m_free.empty()) {
        handle = m_free.back();
        m_free.pop_back();
        m_slots[handle] = std::move(slot);
    }
    else {
        handle = static_cast<int>(m_slots.size());
        m_slots.push_back(std::move(slot));
    }
    m_live += 1;
    return handle;
}

void GameSim::load(int handle) {
    if (handle < 0 || handle >= static_cast<int>(m_slots.size())) return;
    auto const& slot = m_slots[handle];
    if (!slot.used || !slot.checkpoint) return;

    bool const practice = m_layer->m_isPracticeMode;
    if (m_practiceWrap) m_layer->m_isPracticeMode = true;
    m_layer->loadFromCheckpoint(slot.checkpoint);
    m_layer->m_isPracticeMode = practice;
    slot.player1.apply(m_layer->m_player1);
    slot.player2.apply(m_layer->m_player2);
    m_layer->m_gameState.m_currentProgress = slot.progress;
    m_layer->m_levelEndAnimationStarted = m_endAnimationStarted;
    m_layer->m_endChecked = m_endChecked;
    m_hold = slot.hold;
}

void GameSim::release(int handle) {
    if (handle < 0 || handle >= static_cast<int>(m_slots.size())) return;
    auto& slot = m_slots[handle];
    if (!slot.used) return;
    if (slot.checkpoint) {
        slot.checkpoint->release();
    }
    slot = Slot{};
    m_free.push_back(handle);
    m_live -= 1;
}

RunResult GameSim::run(int untilTick, InputFn const& input, SampleFn const& sample) {
    auto& ctx = simContext();
    ctx = SimContext{};
    ctx.active = true;
    ctx.input = &input;
    ctx.sample = &sample;
    ctx.baseProgress = m_base;
    ctx.appliedHold = m_hold;

    RunResult result;
    int stalls = 0;
    float const levelEnd = m_layer->m_levelLength;

    while (true) {
        int const before = this->tick();
        if (before >= untilTick) break;

        ctx.currentTick = before;
        m_layer->m_extraDelta = 0.0;
        m_layer->update(kTickDt);

        if (ctx.died) {
            result.event = Event::Died;
            result.eventTick = ctx.eventTick >= 0 ? ctx.eventTick : before;
            break;
        }
        if (ctx.completed || (levelEnd > 0.f && m_layer->m_player1 && m_layer->m_player1->getPositionX() >= levelEnd)) {
            result.event = Event::Completed;
            result.eventTick = std::max(before, this->tick() - 1);
            break;
        }

        if (this->tick() == before) {
            if (++stalls > kMaxStalls) {
                // The game refuses to advance (paused, ended, ...). Treat as a dead end.
                result.event = Event::Died;
                result.eventTick = before;
                break;
            }
        }
        else {
            stalls = 0;
        }
    }

    m_hold = ctx.appliedHold;
    ctx = SimContext{};
    result.endTick = this->tick();
    return result;
}

float GameSim::selfTest(int ticks) {
    int const handle = this->save();
    if (handle < 0) return -1.f;

    int const start = this->tick();
    std::vector<Sample> first;
    std::vector<Sample> second;
    InputFn none = [](int, Mode) { return false; };
    SampleFn recordFirst = [&](int, Sample const& s) { first.push_back(s); };
    SampleFn recordSecond = [&](int, Sample const& s) { second.push_back(s); };

    this->run(start + ticks, none, recordFirst);
    this->load(handle);
    this->run(start + ticks, none, recordSecond);
    this->load(handle);
    this->release(handle);

    float deviation = 0.f;
    size_t const count = std::min(first.size(), second.size());
    for (size_t i = 0; i < count; ++i) {
        deviation = std::max(deviation, std::abs(first[i].x - second[i].x));
        deviation = std::max(deviation, std::abs(first[i].y - second[i].y));
    }
    if (first.size() != second.size()) {
        deviation = std::max(deviation, 1000.f);
    }
    return deviation;
}

} // namespace ac
