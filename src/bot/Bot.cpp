#include "Bot.hpp"

#include "../Settings.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

using namespace geode::prelude;

namespace ac {

namespace {
    constexpr int kMaxReplans = 3;
    constexpr int kMaxFailures = 2;
    constexpr float kDivergenceTolerance = 2.f;
    constexpr int kSelfTestTicks = 240;
    constexpr float kExactRestore = 0.01f;

    // Live mode, in physics ticks (240 per second).
    constexpr int kLiveMinLead = 60;    // never let the player run closer than this to the end of the plan
    constexpr int kLiveLow = 480;       // start planning again when the lead drops below this
    constexpr int kLiveHigh = 960;      // keep planning until the lead reaches this
    constexpr int kLiveSnapshotInterval = 32;
    constexpr int kMaxLiveReplans = 30;

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
        case BotState::Live: return "Live";
        case BotState::Playing: return "Playing";
        case BotState::Finished: return "Finished";
        case BotState::Failed: return "Failed";
        case BotState::Unsupported: return "Unsupported";
    }
    return "?";
}

std::string Bot::statusText() const {
    if (m_state == BotState::Live && m_planner) {
        if (m_planner->status() == Planner::Status::Running) {
            return fmt::format("Planning {:.1f}s ahead", this->liveLeadSeconds());
        }
        if (m_planner->status() == Planner::Status::Completed) {
            return "Path found all the way to the end";
        }
        return fmt::format("No safe path past {}; playing on", percent(m_planProgress));
    }
    return m_status;
}

float Bot::liveLeadSeconds() const {
    if (!m_planner || !m_sim) return 0.f;
    return std::max(0, m_planner->frontier() - m_sim->tick()) / 240.f;
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
    return m_state == BotState::Playing || m_state == BotState::Waiting || m_state == BotState::Live;
}

bool Bot::safeModeActive() const {
    if (m_usedThisAttempt) return true;
    if (!m_layer || !settings::enabled()) return false;
    return m_state == BotState::Waiting || m_state == BotState::Computing || m_state == BotState::Live ||
        m_state == BotState::Playing || m_state == BotState::Finished || m_state == BotState::Failed;
}

void Bot::requestRecompute() {
    this->invalidatePlan();
    m_failures = 0;
    m_selfTested = false;
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
    m_practiceWrap = false;
    m_selfTested = false;
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
    if (m_state == BotState::Live) {
        this->stopLive();
    }
    m_stats.clicks = 0;

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
    if (settings::liveMode()) {
        this->invalidatePlan();
        m_state = BotState::Waiting;
        this->setStatus("Waiting for the level to start");
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
        if (m_state == BotState::Live) this->stopLive();
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
        if (m_state == BotState::Live) this->stopLive();
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
        if (m_state == BotState::Live) this->stopLive();
        layer->resetLevel();
        return true;
    }

    switch (m_state) {
        case BotState::Waiting:
            if (!this->readyToCompute(layer)) return false;
            if (settings::liveMode()) {
                this->startLive(layer);
                return m_state == BotState::Live ? this->liveFrame(layer) : false;
            }
            this->startCompute(layer, layer->m_gameState.m_currentProgress != 0);
            if (m_state == BotState::Computing) this->runCompute(layer);
            return true;

        case BotState::Computing:
            this->runCompute(layer);
            return true;

        case BotState::Live:
            if (m_divergence && settings::autoCorrect() && m_replans < kMaxLiveReplans && this->readyToCompute(layer)) {
                m_divergence = false;
                m_replans += 1;
                this->stopLive();
                this->startLive(layer);
                if (m_state != BotState::Live) return false;
            }
            return this->liveFrame(layer);

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

void Bot::runSelfTest(PlayLayer*) {
    // Check once per level that checkpoints restore the game exactly. If they don't,
    // see whether practice style checkpoints do better and keep whichever is closer.
    m_selfTested = true;
    float deviation = m_sim->selfTest(kSelfTestTicks);
    if (deviation > kExactRestore || deviation < 0.f) {
        m_sim->setPracticeWrap(true);
        float const wrapped = m_sim->selfTest(kSelfTestTicks);
        log::info("Checkpoint self test: normal {:.4f}, practice style {:.4f}", deviation, wrapped);
        if (wrapped >= 0.f && (deviation < 0.f || wrapped < deviation)) {
            deviation = wrapped;
            m_practiceWrap = true;
        }
        else {
            m_sim->setPracticeWrap(false);
        }
    }
    m_stats.selfTestDeviation = deviation;
    if (deviation > kExactRestore) {
        log::warn("Checkpoint restores are off by up to {:.3f} units; relying on auto correct", deviation);
    }
    else {
        log::info("Checkpoint self test passed (deviation {:.4f})", deviation);
    }
    m_sim->setPracticeWrap(m_practiceWrap);
}

void Bot::startCompute(PlayLayer* layer, bool replan) {
    m_replanning = replan;
    m_computeStart = layer->m_gameState.m_currentProgress;
    this->pauseAudio();

    bool const hold = replan ? m_appliedHold : false;
    m_sim = std::make_unique<GameSim>(layer, hold);

    if (!replan && !m_selfTested) {
        this->runSelfTest(layer);
    }
    m_sim->setPracticeWrap(m_practiceWrap);

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

void Bot::startLive(PlayLayer* layer) {
    bool const hold = m_appliedHold;
    m_sim = std::make_unique<GameSim>(layer, hold);
    if (!m_selfTested) {
        this->runSelfTest(layer);
    }
    m_sim->setPracticeWrap(m_practiceWrap);

    m_planStyle = settings::playStyle();
    uint32_t seed = static_cast<uint32_t>(layer->m_level->m_levelID.value()) * 2654435761u;
    seed ^= m_seedSalt + layer->m_gameState.m_currentProgress * 40503u;

    PlannerConfig config = PlannerConfig::forStyle(m_planStyle, seed);
    config.snapshotInterval = kLiveSnapshotInterval;
    m_planner = std::make_unique<Planner>(*m_sim, config, hold);
    if (m_planner->status() != Planner::Status::Running) {
        // Checkpoints are unavailable in this level; there is nothing to plan with.
        this->stopLive();
        m_state = BotState::Failed;
        this->setStatus("Could not create checkpoints in this level");
        return;
    }

    this->invalidatePlan();
    m_state = BotState::Live;
    m_livePlanning = true;
    m_liveFrames = 0;
    m_divergence = false;
    this->setStatus("Live: planning ahead while you play");
}

void Bot::stopLive() {
    m_planner.reset();
    m_sim.reset();
    m_livePlanning = true;
}

void Bot::syncLiveView() {
    if (!m_planner || !m_sim) return;

    auto const& bits = m_planner->pathInputs();
    auto const& samples = m_planner->pathSamples();
    int const front = std::min<int>(m_planner->frontier(), static_cast<int>(std::min(bits.size(), samples.size())));
    int const base = static_cast<int>(m_sim->baseProgress());
    int const tick = m_sim->tick();

    size_t const need = static_cast<size_t>(base + std::max(front, 0));
    if (m_inputs.size() < need) {
        m_inputs.resize(need, 0);
        m_samples.resize(need);
    }
    // Only the part around and ahead of the player is ever drawn, so only that is copied.
    for (int rel = std::max(0, tick - 64); rel < front; ++rel) {
        m_inputs[base + rel] = bits[rel];
        m_samples[base + rel] = samples[rel];
    }

    m_planBegin = static_cast<unsigned>(base);
    m_planEnd = base + std::max(front, 0);
    m_planValid = front > 0;
    m_planComplete = m_planner->status() == Planner::Status::Completed;
    m_planRevision += 1;
    if (m_planEnd > 0 && m_planEnd <= static_cast<int>(m_samples.size())) {
        m_planProgress = m_planComplete ? 1.f : progressOf(m_layer, m_samples[m_planEnd - 1].x);
    }
    m_stats.decisions = m_planner->decisions();
    m_stats.evaluations = m_planner->evaluations();
    m_stats.plannedTicks = front;
    m_stats.complete = m_planComplete;
}

bool Bot::liveFrame(PlayLayer*) {
    if (!m_planner || !m_sim) {
        m_state = BotState::Waiting;
        return false;
    }

    auto& planner = *m_planner;
    int const tick = m_sim->tick();
    bool changed = false;

    if (planner.status() == Planner::Status::Running) {
        // Decisions at or before the tick being played now are final.
        planner.setFloor(tick);

        int lead = planner.frontier() - tick;
        if (lead < kLiveLow) m_livePlanning = true;
        if (lead >= kLiveHigh) m_livePlanning = false;

        if (m_livePlanning || lead < kLiveMinLead) {
            auto const begin = std::chrono::steady_clock::now();
            auto const budget = std::chrono::milliseconds(std::clamp<int64_t>(settings::liveBudget(), 1, 50));
            auto const deadline = begin + budget;

            // Simulating moves the real game around, so remember where it is and put it back.
            int const here = m_sim->save();
            if (here >= 0) {
                Planner::Status status;
                do {
                    status = planner.step();
                } while (status == Planner::Status::Running &&
                         planner.frontier() - tick < kLiveHigh &&
                         std::chrono::steady_clock::now() < deadline);
                m_sim->load(here);
                m_sim->release(here);
                changed = true;
            }
            else {
                // Checkpoints stopped working mid-level, so nothing more can be planned.
                this->stopLive();
                m_state = BotState::Failed;
                this->setStatus("Checkpoints stopped working; bot stopped");
                return false;
            }
            m_stats.computeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        }
    }

    if (changed || (++m_liveFrames % 30) == 0) {
        this->syncLiveView();
    }

    int const lead = planner.frontier() - tick;
    bool const running = planner.status() == Planner::Status::Running;
    if (running && lead < kLiveMinLead) {
        // The plan is too short to play safely; hold the game for a frame so it can catch up.
        return true;
    }
    return false;
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

    if (m_state == BotState::Live && m_planner && m_sim) {
        int const rel = static_cast<int>(m_tickBefore) - static_cast<int>(m_sim->baseProgress());
        auto const& bits = m_planner->pathInputs();
        bool want = false;
        if (rel >= 0 && rel < m_planner->frontier() && rel < static_cast<int>(bits.size())) {
            want = bits[rel] != 0;
        }
        if (want != m_appliedHold) {
            if (want) m_stats.clicks += 1;
            applyJump(layer, want);
            m_appliedHold = want;
        }
        m_usedThisAttempt = true;
        return;
    }

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
    if (m_state == BotState::Live && m_planner && m_sim && !m_divergence && layer->m_player1) {
        int const rel = static_cast<int>(m_tickBefore) - static_cast<int>(m_sim->baseProgress());
        auto const& samples = m_planner->pathSamples();
        if (rel >= 0 && rel < m_planner->frontier() && rel < static_cast<int>(samples.size())) {
            auto const& expected = samples[rel];
            auto const actual = layer->m_player1->getPosition();
            if (std::abs(actual.x - expected.x) > kDivergenceTolerance || std::abs(actual.y - expected.y) > kDivergenceTolerance) {
                m_divergence = true;
                log::warn(
                    "Desync at tick {}: expected ({:.2f}, {:.2f}), got ({:.2f}, {:.2f})",
                    m_tickBefore, expected.x, expected.y, actual.x, actual.y
                );
            }
        }
        return;
    }
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
    if (m_state == BotState::Live) {
        float const at = progressOf(layer, layer->m_player1 ? layer->m_player1->getPositionX() : 0.f);
        bool const stuck = m_planner && m_planner->status() == Planner::Status::Stuck;
        m_appliedHold = false;
        m_failures += 1;
        m_seedSalt += 7919;
        this->stopLive();
        m_state = BotState::Failed;
        if (m_failures > kMaxFailures) {
            this->setStatus(fmt::format("Died at {} again; bot stopped. Try another play style.", percent(at)));
        }
        else if (stuck) {
            this->setStatus(fmt::format("Died at {}: no safe path was found past this point", percent(at)));
        }
        else {
            this->setStatus(fmt::format("Died at {} (the run drifted from the plan), trying again", percent(at)));
        }
        return;
    }
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
    if (m_state != BotState::Playing && m_state != BotState::Live && !m_usedThisAttempt) return;
    this->stopLive();
    m_state = BotState::Finished;
    m_failures = 0;
    this->setStatus("Level completed (safe mode: nothing was saved)");
}

} // namespace ac
