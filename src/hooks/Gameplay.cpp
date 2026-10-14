#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include "../bot/Bot.hpp"
#include "../bot/FakeSim.hpp"
#include "../ui/PathViewer.hpp"

#include <algorithm>

using namespace geode::prelude;

namespace {
    bool isCurrentPlayLayer(GJBaseGameLayer* layer) {
        auto* playLayer = PlayLayer::get();
        return playLayer && static_cast<GJBaseGameLayer*>(playLayer) == layer;
    }

    bool simulating() {
        return ac::fakeContext().active;
    }

    bool isHiddenPlayer(PlayerObject* player) {
        return player && player == ac::FakeWorld::get().player();
    }

    // What the hidden player collides with: terrain, hazards, orbs and pads. Portals,
    // triggers, coins and everything else are left to the real game.
    bool simulatedType(GameObjectType type, bool& activatable) {
        activatable = false;
        switch (type) {
            case GameObjectType::Solid:
            case GameObjectType::Hazard:
            case GameObjectType::AnimatedHazard:
            case GameObjectType::Slope:
                return true;
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::GravityPad:
            case GameObjectType::RedJumpPad:
            case GameObjectType::SpiderPad:
            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
            case GameObjectType::DropRing:
            case GameObjectType::RedJumpRing:
            case GameObjectType::CustomRing:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::SpiderOrb:
                activatable = true;
                return true;
            default:
                return false;
        }
    }
}

namespace {
    // What killed the real player, for the log.
    std::string describeKiller(GameObject* object) {
        if (!object) return "no object (crushed or out of bounds)";
        bool activatable = false;
        bool const simulated = simulatedType(object->m_objectType, activatable);
        auto const rect = object->getObjectRect();
        auto const last = object->getLastPosition();
        auto const now = object->getRealPosition();
        bool const moving = last.x != now.x || last.y != now.y;
        return fmt::format(
            "obj {} type {} at ({:.1f},{:.1f}) size {:.0f}x{:.0f}{}{}{}{}{}{}{}",
            object->m_objectID, static_cast<int>(object->m_objectType), object->getPositionX(), object->getPositionY(),
            rect.size.width, rect.size.height,
            simulated ? "" : " [type not simulated]",
            moving ? " moving" : "",
            object->m_isNoTouch ? " noTouch" : "",
            object->m_isPassable ? " passable" : "",
            object->m_isHide ? " hidden" : "",
            object->m_isGroupDisabled ? " groupDisabled" : "",
            object->m_isDisabled ? " disabled" : ""
        );
    }
}

class $modify(ACPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        // Run before noclip style hooks from other mods, so the hidden player's deaths are
        // always seen and the real player's deaths are always counted.
        if (!self.setHookPriorityPre("PlayLayer::destroyPlayer", Priority::First)) {
            log::warn("Failed to set destroyPlayer hook priority");
        }
        if (!self.setHookPriorityPre("PlayLayer::levelComplete", Priority::First)) {
            log::warn("Failed to set levelComplete hook priority");
        }
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        if (!ac::FakeWorld::get().attach(this)) {
            log::error("Could not create the hidden simulation player");
        }
        ac::Bot::get().onLevelInit(this);
        ac::PathViewer::get().attach(this);
        return true;
    }

    void onQuit() {
        ac::Bot::get().onLevelQuit(this);
        ac::PathViewer::get().detach(this);
        ac::FakeWorld::get().detach(this);
        PlayLayer::onQuit();
    }

    void onExit() {
        // Covers leaving the level without onQuit (e.g. another mod switching scenes).
        ac::Bot::get().onLevelQuit(this);
        ac::PathViewer::get().detach(this);
        ac::FakeWorld::get().detach(this);
        PlayLayer::onExit();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        ac::Bot::get().onReset(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& ctx = ac::fakeContext();
        if (ctx.active) {
            // The anticheat spike isn't physics; it must never end a simulation.
            if (player == ctx.player && object != m_anticheatSpike) {
                ctx.died = true;
                ctx.killer = object;
            }
            return;
        }
        if (isHiddenPlayer(player)) return;
        if (object == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }

        auto& bot = ac::Bot::get();
        bool const testMode = m_isTestMode;
        if (bot.safeModeActive()) {
            m_isTestMode = true;
        }
        PlayLayer::destroyPlayer(player, object);
        m_isTestMode = testMode;

        if ((m_player1 && m_player1->m_isDead) || (m_player2 && m_player2->m_isDead)) {
            bot.onRealDeath(this, describeKiller(object));
        }
    }

    void levelComplete() {
        if (simulating()) return;

        auto& bot = ac::Bot::get();
        bool const testMode = m_isTestMode;
        if (bot.safeModeActive()) {
            // Same flag start positions use: no progress, stars, coins or verification.
            m_isTestMode = true;
        }
        PlayLayer::levelComplete();
        m_isTestMode = testMode;
        bot.onRealComplete(this);
    }

    void playEndAnimationToPos(CCPoint position) {
        if (simulating()) return;
        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(ACBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        if (!self.setHookPriorityPre("GJBaseGameLayer::handleButton", Priority::First)) {
            log::warn("Failed to set handleButton hook priority");
        }
    }

    void update(float dt) {
        if (simulating() || !isCurrentPlayLayer(this)) {
            GJBaseGameLayer::update(dt);
            return;
        }
        auto* playLayer = PlayLayer::get();
        auto& recorder = ac::TickRecorder::get();
        auto& bot = ac::Bot::get();
        bot.onFrame(playLayer);
        recorder.beginFrame(m_player1, m_gameState.m_currentProgress);
        GJBaseGameLayer::update(dt);
        recorder.endFrame(m_gameState.m_currentProgress);
        // The frame's last physics step has run by now.
        bot.endFrame(playLayer);
        ac::PathViewer::get().update(playLayer);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (simulating() || !isCurrentPlayLayer(this)) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }
        // Each step starts here; GD may move the player inside processCommands or right
        // after it, so a step is only known to be finished when the next one starts.
        ac::TickRecorder::get().stepStarted(m_gameState.m_currentProgress);
        ac::Bot::get().beforeTick(PlayLayer::get());
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!ac::botInputFlag() && isCurrentPlayLayer(this) && ac::Bot::get().shouldBlockUserInput()) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        auto& recorder = ac::TickRecorder::get();
        recorder.enter(player, ac::TickOp::Kind::Collide, dt, false);
        int const result = GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
        recorder.leave();
        return result;
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        auto& ctx = ac::fakeContext();
        if (!ctx.active || player != ctx.player || !objects) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        gd::vector<GameObject*> kept;
        int const count = std::min<int>(objectCount, static_cast<int>(objects->size()));
        for (int i = 0; i < count; ++i) {
            GameObject* object = (*objects)[i];
            if (!object) continue;
            bool activatable = false;
            if (!simulatedType(object->m_objectType, activatable)) {
                if (ctx.sim) ctx.sim->notePortal(object);
                continue;
            }
            if (activatable && ctx.sim) {
                ctx.sim->noteObject(object);
            }
            kept.push_back(object);
        }
        GJBaseGameLayer::collisionCheckObjects(player, &kept, static_cast<int>(kept.size()), dt);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        auto& ctx = ac::fakeContext();
        if (ctx.active && player == ctx.player) {
            // Only the hidden player flips; no level wide gravity effect.
            player->flipGravity(flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (simulating()) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void destroyObject(GameObject* object) {
        if (simulating()) return;
        GJBaseGameLayer::destroyObject(object);
    }

    void pickupItem(EffectGameObject* object) {
        if (simulating()) return;
        GJBaseGameLayer::pickupItem(object);
    }

    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (simulating()) return;
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }
};

class $modify(ACPlayerObject, PlayerObject) {
    void resetTouchedRings(bool removeAll) {
        auto& recorder = ac::TickRecorder::get();
        recorder.enter(this, ac::TickOp::Kind::ResetRings, 0.f, removeAll);
        PlayerObject::resetTouchedRings(removeAll);
        recorder.leave();
    }

    void updateInternalActions(float dt) {
        auto& recorder = ac::TickRecorder::get();
        recorder.enter(this, ac::TickOp::Kind::Internal, dt, false);
        PlayerObject::updateInternalActions(dt);
        recorder.leave();
    }

    void updateRotation(float dt) {
        auto& recorder = ac::TickRecorder::get();
        recorder.enter(this, ac::TickOp::Kind::Rotate, dt, false);
        PlayerObject::updateRotation(dt);
        recorder.leave();
    }

    void update(float dt) {
        auto& recorder = ac::TickRecorder::get();
        recorder.enter(this, ac::TickOp::Kind::Update, dt, false);
        PlayerObject::update(dt);
        recorder.leave();
    }

    void postCollision(float dt, bool betweenSteps) {
        auto& recorder = ac::TickRecorder::get();
        recorder.enter(this, ac::TickOp::Kind::Post, dt, betweenSteps);
        PlayerObject::postCollision(dt, betweenSteps);
        recorder.leave();
    }

    void incrementJumps() {
        if (simulating() || isHiddenPlayer(this) || ac::Bot::get().safeModeActive()) return;
        PlayerObject::incrementJumps();
    }

    void spawnCircle() {
        if (isHiddenPlayer(this)) return;
        PlayerObject::spawnCircle();
    }

    void spawnPortalCircle(ccColor3B color, float startRadius) {
        if (isHiddenPlayer(this)) return;
        PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (isHiddenPlayer(this)) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }
};

class $modify(ACGameObject, GameObject) {
    void playShineEffect() {
        if (simulating()) return;
        GameObject::playShineEffect();
    }
};

class $modify(ACEnhancedGameObject, EnhancedGameObject) {
    void powerOnObject(int state) {
        if (simulating()) return;
        EnhancedGameObject::powerOnObject(state);
    }
};

class $modify(ACEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (simulating()) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};
