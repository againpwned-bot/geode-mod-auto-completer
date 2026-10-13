#include "PlayerState.hpp"

using namespace geode::prelude;

namespace ac {

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
    stateDartSlide = player->m_stateDartSlide;
    stateBoostX = player->m_stateBoostX;
    stateBoostY = player->m_stateBoostY;
    stateForce2 = player->m_maybeStateForce2;
    stateScale = player->m_stateScale;
    stateForce = player->m_stateForce;
    stateForceVector = player->m_stateForceVector;
    wasTeleported = player->m_wasTeleported;
    touchedBreakableBlock = player->m_maybeTouchedBreakableBlock;
    lastCollisionBottom = player->m_lastCollisionBottom;
    lastCollisionTop = player->m_lastCollisionTop;
    lastCollisionLeft = player->m_lastCollisionLeft;
    lastCollisionRight = player->m_lastCollisionRight;
    previousCollisionBottom = player->m_unk50C;
    previousCollisionTop = player->m_unk510;
    slopeStartTime = player->m_slopeStartTime;
    slopeEndTime = player->m_slopeEndTime;
    slidingTime = player->m_maybeSlidingTime;
    slidingStartTime = player->m_maybeSlidingStartTime;
    collidedTopMinY = player->m_collidedTopMinY;
    collidedBottomMaxY = player->m_collidedBottomMaxY;
    collidedLeftMaxX = player->m_collidedLeftMaxX;
    collidedRightMinX = player->m_collidedRightMinX;
    isUpsideDown = player->m_isUpsideDown;
    playerSpeed = player->m_playerSpeed;
    vehicleSize = player->m_vehicleSize;
    gravityMod = player->m_gravityMod;
    isDashing = player->m_isDashing;
    dashX = player->m_dashX;
    dashY = player->m_dashY;
    dashAngle = player->m_dashAngle;
    dashStartTime = player->m_dashStartTime;
    dashRing = player->m_dashRing;
    dashFireFrame = player->m_dashFireFrame;
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
    player->m_stateDartSlide = stateDartSlide;
    player->m_stateBoostX = stateBoostX;
    player->m_stateBoostY = stateBoostY;
    player->m_maybeStateForce2 = stateForce2;
    player->m_stateScale = stateScale;
    player->m_stateForce = stateForce;
    player->m_stateForceVector = stateForceVector;
    player->m_wasTeleported = wasTeleported;
    player->m_maybeTouchedBreakableBlock = touchedBreakableBlock;
    player->m_lastCollisionBottom = lastCollisionBottom;
    player->m_lastCollisionTop = lastCollisionTop;
    player->m_lastCollisionLeft = lastCollisionLeft;
    player->m_lastCollisionRight = lastCollisionRight;
    player->m_unk50C = previousCollisionBottom;
    player->m_unk510 = previousCollisionTop;
    player->m_slopeStartTime = slopeStartTime;
    player->m_slopeEndTime = slopeEndTime;
    player->m_maybeSlidingTime = slidingTime;
    player->m_maybeSlidingStartTime = slidingStartTime;
    player->m_collidedTopMinY = collidedTopMinY;
    player->m_collidedBottomMaxY = collidedBottomMaxY;
    player->m_collidedLeftMaxX = collidedLeftMaxX;
    player->m_collidedRightMinX = collidedRightMinX;
    player->m_isUpsideDown = isUpsideDown;
    player->m_playerSpeed = playerSpeed;
    player->m_vehicleSize = vehicleSize;
    player->m_gravityMod = gravityMod;
    player->m_isDashing = isDashing;
    player->m_dashX = dashX;
    player->m_dashY = dashY;
    player->m_dashAngle = dashAngle;
    player->m_dashStartTime = dashStartTime;
    player->m_dashRing = dashRing;
    player->m_dashFireFrame = dashFireFrame;
    player->m_holdingButtons = holdingButtons;
    player->m_jumpPadRelated = jumpPadRelated;
    player->m_touchedRings = touchedRings;
}

} // namespace ac
