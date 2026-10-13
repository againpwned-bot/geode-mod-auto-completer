#pragma once

#include <Geode/Geode.hpp>

#include "../planner/Planner.hpp"

namespace ac {

// True while the bot itself is calling handleButton, so the input blocker lets it through.
bool& botInputFlag();

// Presses or releases the jump button of the real game through its normal input path.
void applyJump(GJBaseGameLayer* layer, bool down);

Mode modeOf(PlayerObject* player);

// Physics state of a player: copied from the real player onto the hidden simulation
// player, and saved/restored for the planner's snapshots.
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
    bool isUpsideDown = false;
    float playerSpeed = 0.f;
    float vehicleSize = 1.f;
    float gravityMod = 1.f;
    bool isDashing = false;
    double dashX = 0.0;
    double dashY = 0.0;
    double dashAngle = 0.0;
    double dashStartTime = 0.0;
    DashRingObject* dashRing = nullptr;
    int dashFireFrame = 0;
    gd::map<int, bool> holdingButtons;
    gd::map<int, bool> jumpPadRelated;
    gd::unordered_set<int> touchedRings;

    void capture(PlayerObject* player);
    void apply(PlayerObject* player) const;
};

} // namespace ac
