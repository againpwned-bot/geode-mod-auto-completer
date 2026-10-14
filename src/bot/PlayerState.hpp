#pragma once

#include <Geode/Geode.hpp>

#include "../planner/Planner.hpp"

#include <string>
#include <type_traits>
#include <utility>

namespace ac {

// True while the bot itself is calling handleButton, so the input blocker lets it through.
bool& botInputFlag();

// Presses or releases the jump button of the real game through its normal input path.
void applyJump(GJBaseGameLayer* layer, bool down);

Mode modeOf(PlayerObject* player);

// Every PlayerObject member that physics reads or writes (2.2081 bindings). Sprites,
// particles, layers and the action manager are left out: they belong to each player.
#define AC_PLAYER_FIELDS(X) \
    X(m_wasTeleported) X(m_fixGravityBug) X(m_reverseSync) X(m_yVelocityBeforeSlope) \
    X(m_dashX) X(m_dashY) X(m_dashAngle) X(m_dashStartTime) \
    X(m_dashRing) X(m_slopeStartTime) X(m_maybeLastGroundObject) X(m_lastCollisionBottom) \
    X(m_lastCollisionTop) X(m_lastCollisionLeft) X(m_lastCollisionRight) X(m_unk50C) \
    X(m_unk510) X(m_currentSlope2) X(m_preLastGroundObject) X(m_slopeAngle) \
    X(m_slopeSlidingMaybeRotated) X(m_collidedObject) X(m_lastGroundObject) X(m_collidingWithLeft) \
    X(m_collidingWithRight) X(m_scaleXRelated2) X(m_groundYVelocity) X(m_yVelocityRelated) \
    X(m_scaleXRelated3) X(m_scaleXRelated4) X(m_scaleXRelated5) \
    X(m_isCollidingWithSlope) X(m_isBallRotating) X(m_unk669) X(m_currentPotentialSlope) \
    X(m_currentSlope) X(m_collidingWithSlopeId) X(m_slopeFlipGravityRelated) X(m_slopeAngleRadians) \
    X(m_rotateObjectsRelated) X(m_potentialSlopeMap) X(m_rotationSpeed) X(m_rotateSpeed) \
    X(m_isRotating) X(m_isBallRotating2) X(m_speedMultiplier) X(m_yStart) \
    X(m_gravity) X(m_gameModeChangedTime) X(m_padRingRelated) X(m_maybeIsFalling) \
    X(m_maybeCanRunIntoBlocks) X(m_isOnGround3) X(m_lastJumpTime) X(m_lastFlipTime) \
    X(m_lastSpiderFlipTime) X(m_accelerationOrSpeed) X(m_snapDistance) X(m_ringJumpRelated) \
    X(m_ringRelatedSet) X(m_objectSnappedTo) X(m_slopeRotation) X(m_currentSlopeYVelocity) \
    X(m_unk3d0) X(m_blackOrbRelated) X(m_unk3e0) X(m_unk3e1) \
    X(m_isAccelerating) X(m_isCurrentSlopeTop) X(m_collidedTopMinY) X(m_collidedBottomMaxY) \
    X(m_collidedLeftMaxX) X(m_collidedRightMinX) X(m_maybeIsColliding) X(m_jumpBuffered) \
    X(m_stateRingJump) X(m_wasJumpBuffered) X(m_wasRobotJump) X(m_stateJumpBuffered) \
    X(m_stateRingJump2) X(m_touchedRing) X(m_touchedCustomRing) X(m_touchedGravityPortal) \
    X(m_maybeTouchedBreakableBlock) X(m_touchedPad) X(m_yVelocity) X(m_fallSpeed) \
    X(m_isOnSlope) X(m_wasOnSlope) X(m_slopeVelocity) X(m_maybeUpsideDownSlope) \
    X(m_isUpsideDown) X(m_isOnGround) X(m_isGoingLeft) X(m_isSideways) \
    X(m_reverseRelated) X(m_maybeReverseSpeed) X(m_maybeReverseAcceleration) X(m_xVelocityRelated2) \
    X(m_isDashing) X(m_dashFireFrame) X(m_groundObjectMaterial) X(m_vehicleSize) \
    X(m_playerSpeed) X(m_shipRotation) X(m_lastPortalPos) X(m_isOnGround2) \
    X(m_lastLandTime) X(m_platformerVelocityRelated) X(m_maybeIsBoosted) X(m_scaleXRelatedTime) \
    X(m_decreaseBoostSlide) X(m_unkA29) X(m_isLocked) X(m_controlsDisabled) \
    X(m_lastGroundedPos) X(m_touchedRings) X(m_lastActivatedPortal) X(m_hasEverJumped) \
    X(m_hasEverHitRing) X(m_position) X(m_totalTime) X(m_unkAngle1) \
    X(m_yVelocityRelated3) X(m_stateOnGround) X(m_stateUnk) X(m_stateNoStickX) \
    X(m_stateNoStickY) X(m_stateUnk2) X(m_stateBoostX) X(m_stateBoostY) \
    X(m_maybeStateForce2) X(m_stateScale) X(m_platformerXVelocity) X(m_holdingRight) \
    X(m_holdingLeft) X(m_leftPressedFirst) X(m_scaleXRelated) X(m_maybeHasStopped) \
    X(m_xVelocityRelated) X(m_maybeGoingCorrectSlopeDirection) X(m_isSliding) X(m_maybeSlopeForce) \
    X(m_isOnIce) X(m_physDeltaRelated) X(m_isOnGround4) X(m_maybeSlidingTime) \
    X(m_maybeSlidingStartTime) X(m_changedDirectionsTime) X(m_slopeEndTime) X(m_isMoving) \
    X(m_platformerMovingLeft) X(m_platformerMovingRight) X(m_isSlidingRight) X(m_maybeChangedDirectionAngle) \
    X(m_isPlatformer) X(m_stateNoAutoJump) X(m_stateDartSlide) X(m_stateHitHead) \
    X(m_stateFlipGravity) X(m_gravityMod) X(m_stateForce) X(m_stateForceVector) \
    X(m_affectedByForces) X(m_jumpPadRelated) X(m_playerSpeedAC) X(m_fixRobotJump) \
    X(m_holdingButtons) X(m_inputsLocked) X(m_gv0123) X(m_isOutOfBounds) \
    X(m_fallStartY) X(m_disablePlayerSqueeze) X(m_enable22Changes) X(m_enableImpulseFix)

// Physics state of a player: copied from the real player onto the hidden simulation
// player, and saved/restored for the planner's snapshots.
struct PlayerFix {
    bool valid = false;
    cocos2d::CCPoint nodePosition;
    float rotation = 0.f;
    // Game mode; apply() leaves it alone, switching modes takes GD's toggles.
    Mode mode = Mode::Cube;

#define AC_DECLARE_FIELD(name) std::remove_cvref_t<decltype(std::declval<PlayerObject&>().name)> name {};
    AC_PLAYER_FIELDS(AC_DECLARE_FIELD)
#undef AC_DECLARE_FIELD

    void capture(PlayerObject* player);
    void apply(PlayerObject* player) const;
    // Fields that differ from `other` ("name real/other"), at most `limit` of them.
    std::string diff(PlayerFix const& other, size_t limit, size_t* count = nullptr) const;
};

} // namespace ac
