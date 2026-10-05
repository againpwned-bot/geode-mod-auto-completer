#include "Bot.hpp"

#include "../Settings.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace ac {

namespace {
    constexpr int kMaxReplans = 3;
    constexpr int kMaxFailures = 2;
    constexpr float kDivergenceTolerance = 2.f;
    constexpr int kSelfTestTicks = 240;

    float progressOf(PlayLayer* layer, float x) {
        if (!layer || layer->m_levelLength <= 0.f) return 0.f;
        return std::clamp(x / layer->m_levelLength, 0.f, 1.f);
    }

    std::string percent(float value) {
        return fmt::format("{:.0f}%", value * 100.f);
    }
}

Bot& Bot::get() {
    static Bot bot;
    return bot;
}

char const* Bot::stateName() const {
    switch (m_state) {
        case BotState::Off: return "Off";
        case BotState::Waiting: return "Waiting";
        case BotState::Computing: return "Computing";
        case BotState::Playing: return "Playing";
        case BotState::Finished: return "Finished";
        case BotState::Failed: return "Failed";
        case BotState::Unsupported: return "Unsupported";
    }
    return "?";
}

unsigned Bot::currentTick() const {
    return m_layer ? m_layer->m_gameState.m_currentProgress : 0;
}

void Bot::setStatus(std::string status) {
    if (status != m_status) {
        log::info("{}", status);
        m_status = std::move(status);
    }
}

bool Bot::supported(PlayLayer* layer, std::string& reason) const {
    if (!layer || !layer->m_level || !layer->m_player1) {
        reason = "Level not ready";
        return false;
    }
    if (layer->m_isPlatformer) {
        reason = "Platformer levels are not supported yet";
        return false;
    }
    if (layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode) {
        reason = "2-player levels are not supported yet";
        return false;
    }
    if (layer->m_level->m_levelType == GJLevelType::Editor) {
        reason = "Disabled on editor levels (the bot can't be used to verify)";
        return false;
    }
    return true;
}

bool Bot::readyToCompute(PlayLayer* layer) const {
    return layer->m_started &&
        !layer->m_isPaused &&
        !layer->m_isPracticeMode &&
        !layer->m_levelEndAnimationStarted &&
        layer->m_player1 && !layer->m_player1->m_isDead &&
        (!layer->m_player2 || !layer->m_player2->m_isDead);
}

bool Bot::shouldBlockUserInput() const {
    if (m_state == BotState::Computing) return true;
    if (!settings::enabled() || !settings::ignoreInputs()) return false;
    return m_state == BotState::Playing || m_state == BotState::Waiting;
}

bool Bot::safeModeActive() const {
    if (m_usedThisAttempt) return true;
    if (!m_layer || !settings::enabled()) return false;
    return m_state == BotState::Waiting || m_state == BotState::Computing ||
        m_state == BotState::Playing || m_state == BotState::Finished || m_state == BotState::Failed;
}

void Bot::requestRecompute() {
    this->invalidatePlan();
    m_failures = 0;
    m_stats = BotStats{};
    if (m_layer) {
        m_requestRestart = true;
    }
}

void Bot::invalidatePlan() {
    m_planValid = false;
    m_planComplete = false;
    m_inputs.clear();
    m_samples.clear();
    m_planEnd = 0;
    m_planBegin = 0;
    m_planProgress = 0.f;
    m_planRevision += 1;
}

void Bot::updatePlanStats() {
    int clicks = 0;
    int const end = std::min<int>(m_planEnd, static_cast<int>(m_inputs.size()));
    for (int i = 1; i < end; ++i) {
        if (m_inputs[i] && !m_inputs[i - 1]) clicks += 1;
    }
    m_stats.clicks = clicks;
    m_stats.plannedTicks = m_planEnd;
    m_stats.complete = m_planComplete;
    m_planProgress = (m_planEnd > 0 && m_planEnd <= static_cast<int>(m_samples.size()))
        ? progressOf(m_layer, m_samples[m_planEnd - 1].x)
        : 0.f;
    if (m_planComplete) m_planProgress = 1.f;
}

void Bot::pauseAudio() {
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;
    // GD may resume audio on its own (e.g. after unpausing), so pause again every frame.
    engine->m_allAudioPaused = false;
    engine->pauseAllAudio();
    engine->pauseAllMusic(true);
    m_audioPaused = true;
}

void Bot::resumeAudio() {
    if (!m_audioPaused) return;
    m_audioPaused = false;
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;
    engine->stopAllEffects();
    engine->resumeAllMusic();
    engine->resumeAllAudio();
}

void Bot::onLevelInit(PlayLayer* layer) {
    this->cancelCompute(false);
    m_layer = layer;
    this->invalidatePlan();
    m_stats = BotStats{};
    m_failures = 0;
    m_replans = 0;
    m_seedSalt = 0;
    m_usedThisAttempt = false;
    m_divergence = false;
    m_appliedHold = false;
    m_requestRestart = false;

    std::string reason;
    if (!settings::enabled()) {
        m_state = BotState::Off;
        this->setStatus("Disabled");
    }
    else if (!this->supported(layer, reason)) {
        m_state = BotState::Unsupported;
        this->setStatus(reason);
    }
    else {
        m_state = BotState::Waiting;
        this->setStatus("Waiting for the level to start");
    }
}

void Bot::onLevelQuit(PlayLayer* layer) {
    if (layer != m_layer) return;
    this->cancelCompute(false);
    this->invalidatePlan();
    m_layer = nullptr;
    m_state = BotState::Off;
    m_usedThisAttempt = false;
    this->setStatus("Not in a level");
}

void Bot::onReset(PlayLayer* layer) {
    m_layer = layer;
    if (m_state == BotState::Computing) {
        // The level was restarted by the player mid-plan; the game already reset itself.
        this->cancelCompute(false);
    }

    m_usedThisAttempt = false;
    m_divergence = false;
    m_replans = 0;
    if (m_appliedHold) {
        applyJump(layer, false);
    }
    m_appliedHold = false;

    if (!settings::enabled()) {
        m_state = BotState::Off;
        this->setStatus("Disabled");
        return;
    }
    if (layer->m_isPracticeMode) {
        m_state = BotState::Off;
        this->setStatus("Paused in practice mode");
        return;
    }
    std::string reason;
    if (!this->supported(layer, reason)) {
        m_state = BotState::Unsupported;
        this->setStatus(reason);
        return;
    }

    if (m_failures > kMaxFailures) {
        m_state = BotState::Failed;
        return;
    }
    if (m_planValid && m_planStyle == settings::playStyle()) {
        m_state = BotState::Playing;
        this->setStatus(m_planComplete ? "Playing" : fmt::format("Playing (path only reaches {})", percent(m_planProgress)));
        return;
    }

    this->invalidatePlan();
    m_state = BotState::Waiting;
    this->setStatus("Waiting for the level to start");
}

bool Bot::onFrame(PlayLayer* layer, float) {
    m_layer = layer;

    if (!settings::enabled()) {
        if (m_state == BotState::Computing) this->cancelCompute(true);
        if (m_state != BotState::Off) {
            if (m_appliedHold) {
                applyJump(layer, false);
                m_appliedHold = false;
            }
            m_state = BotState::Off;
            this->setStatus("Disabled");
        }
        return false;
    }

    if (layer->m_isPracticeMode) {
        if (m_state == BotState::Computing) this->cancelCompute(true);
        if (m_state != BotState::Off) {
            m_state = BotState::Off;
            this->setStatus("Paused in practice mode");
        }
        return false;
    }

    if (m_state == BotState::Off) {
        // Turned on (or practice mode left) in the middle of an attempt.
        std::string reason;
        if (!this->supported(layer, reason)) {
            m_state = BotState::Unsupported;
            this->setStatus(reason);
            return false;
        }
        m_state = BotState::Waiting;
    }

    if (m_requestRestart) {
        m_requestRestart = false;
        if (m_state == BotState::Computing) this->cancelCompute(true);
        layer->resetLevel();
        return true;
    }

    switch (m_state) {
        case BotState::Waiting:
            if (!this->readyToCompute(layer)) return false;
            this->startCompute(layer, layer->m_gameState.m_currentProgress != 0);
            if (m_state == BotState::Computing) this->runCompute(layer);
            return true;

        case BotState::Computing:
            this->runCompute(layer);
            return true;

        case BotState::Playing:
            if (m_divergence && settings::autoCorrect() && m_replans < kMaxReplans && this->readyToCompute(layer)) {
                m_divergence = false;
                m_replans += 1;
                this->startCompute(layer, true);
                if (m_state == BotState::Computing) this->runCompute(layer);
                return true;
            }
            return false;

        default:
            return false;
    }
}

void Bot::afterFrame(PlayLayer*) {}

void Bot::startCompute(PlayLayer* layer, bool replan) {
    m_replanning = replan;
    m_computeStart = layer->m_gameState.m_currentProgress;
    this->pauseAudio();

    bool const hold = replan ? m_appliedHold : false;
    m_sim = std::make_unique<GameSim>(layer, hold);

    if (!replan && m_stats.selfTestDeviation < 0.f) {
        m_stats.selfTestDeviation = m_sim->selfTest(kSelfTestTicks);
        if (m_stats.selfTestDeviation > 0.01f) {
            log::warn("Checkpoint self test: restores are off by up to {:.3f} units", m_stats.selfTestDeviation);
        }
        else {
            log::info("Checkpoint self test passed (deviation {:.4f})", m_stats.selfTestDeviation);
        }
    }

    if (!replan || !m_planValid) {
        m_planStyle = settings::playStyle();
    }

    uint32_t seed = static_cast<uint32_t>(layer->m_level->m_levelID.value()) * 2654435761u;
    seed ^= m_seedSalt + m_computeStart * 40503u;

    m_planner = std::make_unique<Planner>(*m_sim, PlannerConfig::forStyle(m_planStyle, seed), hold);
    m_state = BotState::Computing;
    m_computeBegan = std::chrono::steady_clock::now();
    m_computeProgress = progressOf(layer, layer->m_player1->getPositionX());
    this->setStatus(replan ? "Re-planning from here..." : "Computing path...");
}

void Bot::runCompute(PlayLayer* layer) {
    if (!m_planner || !m_sim) {
        m_state = BotState::Waiting;
        return;
    }
    this->pauseAudio();

    auto const budget = std::chrono::milliseconds(std::clamp<int64_t>(settings::computeBudget(), 4, 250));
    auto const deadline = std::chrono::steady_clock::now() + budget;

    Planner::Status status;
    do {
        status = m_planner->step();
    } while (status == Planner::Status::Running && std::chrono::steady_clock::now() < deadline);

    int const best = m_planner->bestFrontier();
    auto const& samples = m_planner->samples();
    if (best > 0 && best <= static_cast<int>(samples.size())) {
        m_computeProgress = progressOf(layer, samples[best - 1].x);
    }

    if (status != Planner::Status::Running) {
        this->finishCompute(layer);
    }
    else {
        this->setStatus(fmt::format("Computing path... {}", percent(m_computeProgress)));
    }
}

void Bot::finishCompute(PlayLayer* layer) {
    auto const status = m_planner->status();
    auto const& inputs = m_planner->inputs();
    auto const& samples = m_planner->samples();

    int planned = status == Planner::Status::Completed ? m_planner->endTick() : m_planner->bestFrontier();
    planned = std::min<int>(planned, static_cast<int>(std::min(inputs.size(), samples.size())));
    planned = std::max(planned, 0);

    // Splice the new segment into the plan at the tick planning started from.
    size_t const offset = m_computeStart;
    if (!m_planValid) {
        m_planBegin = m_computeStart;
    }
    m_inputs.resize(offset, 0);
    m_samples.resize(offset);
    m_inputs.insert(m_inputs.end(), inputs.begin(), inputs.begin() + planned);
    m_samples.insert(m_samples.end(), samples.begin(), samples.begin() + planned);
    m_planEnd = static_cast<int>(offset) + planned;
    m_planComplete = status == Planner::Status::Completed;
    m_planValid = true;
    m_planRevision += 1;

    double const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_computeBegan).count();
    m_stats.computeSeconds += seconds;
    m_stats.decisions = m_planner->decisions();
    m_stats.evaluations += m_planner->evaluations();
    this->updatePlanStats();

    log::info(
        "Planning finished: {} after {:.2f}s, {} ticks, {} evaluations, {} decisions",
        m_planComplete ? "complete" : "stuck", seconds, planned, m_planner->evaluations(), m_planner->decisions()
    );

    bool const restartLevel = !m_replanning && m_computeStart == 0;
    if (!restartLevel) {
        // Put the game back where planning started.
        m_sim->load(m_planner->startHandle());
        m_appliedHold = m_sim->appliedHold();
    }
    else if (m_sim->appliedHold()) {
        // Don't carry a held button from the simulation into the fresh attempt.
        applyJump(layer, false);
    }
    m_planner.reset();
    m_sim.reset();
    this->resumeAudio();

    m_state = BotState::Playing;
    if (m_planComplete) {
        this->setStatus(fmt::format("Path found in {:.1f}s", seconds));
    }
    else {
        this->setStatus(fmt::format("No safe path past {}; playing as far as possible", percent(m_planProgress)));
    }

    if (restartLevel) {
        layer->resetLevel();
    }
}

void Bot::cancelCompute(bool restore) {
    if (m_planner && m_sim && restore) {
        m_sim->load(m_planner->startHandle());
        m_appliedHold = m_sim->appliedHold();
    }
    else if (m_sim && m_sim->appliedHold() && m_layer) {
        applyJump(m_layer, false);
    }
    m_planner.reset();
    m_sim.reset();
    this->resumeAudio();
    if (m_state == BotState::Computing) {
        m_state = BotState::Waiting;
    }
}

void Bot::beforeTick(PlayLayer* layer) {
    m_tickBefore = layer->m_gameState.m_currentProgress;
    if (m_state != BotState::Playing) return;

    bool want = false;
    if (m_tickBefore < m_inputs.size() && static_cast<int>(m_tickBefore) < m_planEnd) {
        want = m_inputs[m_tickBefore] != 0;
    }
    if (want != m_appliedHold) {
        applyJump(layer, want);
        m_appliedHold = want;
    }
    m_usedThisAttempt = true;
}

void Bot::afterTick(PlayLayer* layer) {
    if (m_state != BotState::Playing || m_divergence || !layer->m_player1) return;

    unsigned const tick = m_tickBefore;
    if (tick < m_planBegin || tick >= m_samples.size() || static_cast<int>(tick) >= m_planEnd) return;

    auto const& expected = m_samples[tick];
    auto const actual = layer->m_player1->getPosition();
    if (std::abs(actual.x - expected.x) > kDivergenceTolerance || std::abs(actual.y - expected.y) > kDivergenceTolerance) {
        m_divergence = true;
        log::warn(
            "Desync at tick {}: expected ({:.2f}, {:.2f}), got ({:.2f}, {:.2f})",
            tick, expected.x, expected.y, actual.x, actual.y
        );
    }
}

void Bot::onRealDeath(PlayLayer* layer) {
    if (m_state != BotState::Playing) return;

    unsigned const tick = layer->m_gameState.m_currentProgress;
    float const at = progressOf(layer, layer->m_player1 ? layer->m_player1->getPositionX() : 0.f);

    if (m_appliedHold) {
        m_appliedHold = false;
    }

    if (!m_planComplete && static_cast<int>(tick) >= m_planEnd - 4) {
        m_state = BotState::Failed;
        this->setStatus(fmt::format("Stuck at {}: no safe path found past this point", percent(at)));
        return;
    }

    m_failures += 1;
    m_state = BotState::Failed;
    if (m_failures <= kMaxFailures) {
        // The real run did not match the simulation. Plan again with a different seed.
        this->invalidatePlan();
        m_seedSalt += 7919;
        this->setStatus(fmt::format("Died at {} (desync), recomputing next attempt", percent(at)));
    }
    else {
        this->setStatus(fmt::format("Died at {} again; bot stopped. Try another play style.", percent(at)));
    }
}

void Bot::onRealComplete(PlayLayer*) {
    if (m_state != BotState::Playing && !m_usedThisAttempt) return;
    m_state = BotState::Finished;
    m_failures = 0;
    this->setStatus("Level completed (safe mode: nothing was saved)");
}

} // namespace ac
