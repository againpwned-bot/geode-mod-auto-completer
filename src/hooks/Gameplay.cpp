#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include "../bot/Bot.hpp"
#include "../bot/GameSim.hpp"
#include "../ui/PathViewer.hpp"

using namespace geode::prelude;

namespace {
    bool isCurrentPlayLayer(GJBaseGameLayer* layer) {
        auto* playLayer = PlayLayer::get();
        return playLayer && static_cast<GJBaseGameLayer*>(playLayer) == layer;
    }
}

class $modify(ACPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        // Run before noclip style hooks from other mods, so a simulated death is always
        // seen and the planner never mistakes a deadly path for a safe one.
        if (!self.setHookPriorityPre("PlayLayer::destroyPlayer", Priority::First)) {
            log::warn("Failed to set destroyPlayer hook priority");
        }
        if (!self.setHookPriorityPre("PlayLayer::levelComplete", Priority::First)) {
            log::warn("Failed to set levelComplete hook priority");
        }
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        ac::Bot::get().onLevelInit(this);
        ac::PathViewer::get().attach(this);
        return true;
    }

    void onQuit() {
        ac::Bot::get().onLevelQuit(this);
        ac::PathViewer::get().detach(this);
        PlayLayer::onQuit();
    }

    void onExit() {
        // Covers leaving the level without onQuit (e.g. another mod switching scenes).
        ac::Bot::get().onLevelQuit(this);
        ac::PathViewer::get().detach(this);
        PlayLayer::onExit();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (ac::simContext().active) return;
        ac::Bot::get().onReset(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& ctx = ac::simContext();
        if (ctx.active) {
            if (object != m_anticheatSpike && !ctx.died) {
                ctx.died = true;
                ctx.eventTick = ctx.currentTick;
            }
            return;
        }
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
            bot.onRealDeath(this);
        }
    }

    void levelComplete() {
        auto& ctx = ac::simContext();
        if (ctx.active) {
            if (!ctx.completed) {
                ctx.completed = true;
                ctx.eventTick = ctx.currentTick;
            }
            return;
        }

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
        auto& ctx = ac::simContext();
        if (ctx.active) {
            if (!ctx.completed) {
                ctx.completed = true;
                ctx.eventTick = ctx.currentTick;
            }
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }

    void updateVisibility(float dt) {
        // While simulating every tick is its own update; refresh visibility at the rate a
        // 60 fps frame would instead of on every tick.
        auto& ctx = ac::simContext();
        if (ctx.active && (ctx.currentTick & 3) != 3) return;
        PlayLayer::updateVisibility(dt);
    }
};

class $modify(ACBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        if (!self.setHookPriorityPre("GJBaseGameLayer::handleButton", Priority::First)) {
            log::warn("Failed to set handleButton hook priority");
        }
    }

    void update(float dt) {
        if (ac::simContext().active || !isCurrentPlayLayer(this)) {
            GJBaseGameLayer::update(dt);
            return;
        }

        auto* playLayer = PlayLayer::get();
        if (ac::Bot::get().onFrame(playLayer, dt)) {
            ac::PathViewer::get().update(playLayer);
            return;
        }
        GJBaseGameLayer::update(dt);
        ac::Bot::get().afterFrame(playLayer);
        ac::PathViewer::get().update(playLayer);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isCurrentPlayLayer(this)) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        auto& ctx = ac::simContext();
        if (ctx.active) {
            int const tick = static_cast<int>(m_gameState.m_currentProgress) - static_cast<int>(ctx.baseProgress);
            ac::Mode const mode = ac::modeOf(m_player1);
            bool const want = ctx.input ? (*ctx.input)(tick, mode) : false;
            if (want != ctx.appliedHold) {
                ac::applyJump(this, want);
                ctx.appliedHold = want;
            }
            ctx.currentTick = tick;

            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

            if (ctx.sample && m_player1) {
                ac::Sample sample;
                sample.x = m_player1->getPositionX();
                sample.y = m_player1->getPositionY();
                sample.hold = want;
                sample.mode = mode;
                (*ctx.sample)(tick, sample);
            }
            return;
        }

        auto* playLayer = PlayLayer::get();
        auto& bot = ac::Bot::get();
        bot.beforeTick(playLayer);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        bot.afterTick(playLayer);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!ac::botInputFlag() && isCurrentPlayLayer(this)) {
            if (ac::simContext().active || ac::Bot::get().shouldBlockUserInput()) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void updateParticles(float dt) {
        if (ac::simContext().active) return;
        GJBaseGameLayer::updateParticles(dt);
    }
};

class $modify(ACPlayerObject, PlayerObject) {
    void incrementJumps() {
        if (ac::simContext().active || ac::Bot::get().safeModeActive()) return;
        PlayerObject::incrementJumps();
    }
};

class $modify(ACHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        // Keep the wave trail clean while the planner tries out inputs.
        if (ac::simContext().active) return;
        HardStreak::addPoint(point);
    }
};
