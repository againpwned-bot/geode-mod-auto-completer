#include "Bot.hpp"

#include "../Settings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

namespace ac {

namespace {
    // Ticks are GD physics steps, 240 per second.
    constexpr int kHorizon = 200;          // how far each check looks ahead
    constexpr int kFloor = 12;             // ticks after a check starts that it may not change
    constexpr int kEvery = 16;             // re-check at least this often
    constexpr int kCalibrationTicks = 12;  // prediction window used to measure input timing
    constexpr float kDesyncTolerance = 2.f;

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
        case BotState::Playing: return "Playing";
        case BotState::Finished: return "Finished";
        case BotState::Died: return "Died";
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

bool Bot::readyToPlay(PlayLayer* layer) const {
    return layer->m_started &&
        !layer->m_isPaused &&
        !layer->m_levelEndAnimationStarted &&
        layer->m_player1 && !layer->m_player1->m_isDead;
}

bool Bot::shouldBlockUserInput() const {
    return settings::enabled() && settings::ignoreInputs() && m_state == BotState::Playing;
}

bool Bot::safeModeActive() const {
    if (m_usedThisAttempt) return true;
    if (!m_layer || !settings::enabled()) return false;
    return m_state == BotState::Playing || m_state == BotState::Finished || m_state == BotState::Died;
}

void Bot::requestRestart() {
    if (m_layer) {
        m_requestRestart = true;
    }
}

void Bot::releaseHold(PlayLayer* layer) {
    if (m_appliedHold && layer) {
        applyJump(layer, false);
    }
    m_appliedHold = false;
}

void Bot::resetAttempt() {
    m_check.reset();
    m_calibration = Calibration{};
    m_inputs.clear();
    m_samples.clear();
    m_planEnd = 0;
    m_planRevision += 1;
    m_lastCheck = -1000000;
    m_forceCheck = false;
    m_desyncFlagged = false;
    m_usedThisAttempt = false;
    m_stats.clicks = 0;
    m_stats.plannedAhead = 0;
    m_attempt += 1;
}

void Bot::onLevelInit(PlayLayer* layer) {
    this->resetAttempt();
    m_layer = layer;
    m_appliedHold = false;
    m_requestRestart = false;
    m_stats = BotStats{};
    m_delayError = { -1.0, -1.0 };
    m_inputDelay = 0;
    m_simTicks = 0;
    m_simSeconds = 0.0;
    TickRecorder::get().reset();

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
        m_state = BotState::Playing;
        this->setStatus("Playing");
    }
}

void Bot::onLevelQuit(PlayLayer* layer) {
    if (layer != m_layer) return;
    this->resetAttempt();
    m_layer = nullptr;
    m_appliedHold = false;
    m_state = BotState::Off;
    this->setStatus("Not in a level");
}

void Bot::onReset(PlayLayer* layer) {
    m_layer = layer;
    this->releaseHold(layer);
    this->resetAttempt();

    std::string reason;
    if (!settings::enabled()) {
        m_state = BotState::Off;
        this->setStatus("Disabled");
    }
    else if (layer->m_isPracticeMode) {
        m_state = BotState::Off;
        this->setStatus("Paused in practice mode");
    }
    else if (!this->supported(layer, reason)) {
        m_state = BotState::Unsupported;
        this->setStatus(reason);
    }
    else {
        m_state = BotState::Playing;
        this->setStatus("Playing");
    }
}

void Bot::onFrame(PlayLayer* layer) {
    m_layer = layer;

    if (!settings::enabled() || layer->m_isPracticeMode) {
        if (m_state != BotState::Off) {
            this->releaseHold(layer);
            this->resetAttempt();
            m_state = BotState::Off;
            this->setStatus(layer->m_isPracticeMode ? "Paused in practice mode" : "Disabled");
        }
        return;
    }

    if (m_state == BotState::Off) {
        // Turned on (or practice mode left) in the middle of an attempt.
        std::string reason;
        if (!this->supported(layer, reason)) {
            m_state = BotState::Unsupported;
            this->setStatus(reason);
            return;
        }
        this->resetAttempt();
        m_state = BotState::Playing;
        this->setStatus("Playing");
    }

    if (m_requestRestart) {
        m_requestRestart = false;
        layer->resetLevel();
        return;
    }

    if (m_state != BotState::Playing || !this->readyToPlay(layer)) return;

    if (!FakeWorld::get().player()) {
        m_state = BotState::Unsupported;
        this->setStatus("Could not create the simulation player");
        return;
    }
    // The first real tick tells us which physics calls GD makes per tick.
    if (!TickRecorder::get().ready()) return;

    unsigned const now = layer->m_gameState.m_currentProgress;
    auto const budget = std::chrono::microseconds(std::clamp<int64_t>(settings::liveBudget(), 1, 30) * 1000);
    auto const deadline = std::chrono::steady_clock::now() + budget;

    if (m_check && now >= m_check->t0 + kFloor) {
        // Too slow to finish before its fixed part ran out; start over from here.
        m_check.reset();
        m_forceCheck = true;
    }

    if (!m_check && !m_calibration.active) {
        this->startCalibration(layer, now);
    }

    bool const due = m_forceCheck ||
        static_cast<int>(now) - m_lastCheck >= kEvery ||
        m_planEnd < static_cast<int>(now) + kHorizon / 2;
    if (!m_check && due) {
        this->startCheck(layer, now);
    }

    if (m_check) {
        m_check->sim->beginSlice();
        Planner::Status status;
        do {
            status = m_check->planner->step();
        } while (status == Planner::Status::Running && std::chrono::steady_clock::now() < deadline);

        if (status != Planner::Status::Running) {
            this->finishCheck();
        }
        else {
            m_check->sim->endSlice();
        }
    }

    m_stats.plannedAhead = std::max(0, m_planEnd - static_cast<int>(now));
}

void Bot::startCheck(PlayLayer* layer, unsigned now) {
    auto check = std::make_unique<Check>();
    check->t0 = now;

    std::vector<uint8_t> seed;
    int const end = std::min<int>(m_planEnd, static_cast<int>(m_inputs.size()));
    for (int t = static_cast<int>(now); t < end && t < static_cast<int>(now) + kHorizon; ++t) {
        seed.push_back(m_inputs[t]);
    }
    check->seedSize = static_cast<int>(seed.size());

    check->sim = std::make_unique<FakeSim>(
        layer, FakeWorld::get().player(), TickRecorder::get().ops(), m_inputDelay, m_appliedHold
    );

    uint32_t const seedValue = static_cast<uint32_t>(layer->m_level->m_levelID.value()) * 2654435761u ^
        (m_attempt * 7919u) ^ (now * 40503u);
    PlannerConfig config = PlannerConfig::forStyle(settings::playStyle(), seedValue);
    config.goalTicks = kHorizon;
    check->planner = std::make_unique<Planner>(*check->sim, config, m_appliedHold, std::move(seed), kFloor);

    m_check = std::move(check);
    m_lastCheck = static_cast<int>(now);
    m_forceCheck = false;
    m_desyncFlagged = false;
    m_stats.checks += 1;
}

void Bot::finishCheck() {
    auto& check = *m_check;
    auto& planner = *check.planner;

    auto const status = planner.status();
    auto const& bits = planner.inputs();
    auto const& samples = planner.samples();
    int end = status == Planner::Status::Completed ? planner.endTick() : planner.bestFrontier();
    end = std::min<int>(end, static_cast<int>(std::min(bits.size(), samples.size())));
    end = std::max(end, 0);

    // Ticks before `from` are either already decided or were played exactly as planned.
    int const from = std::min(kFloor, check.seedSize);
    size_t const need = static_cast<size_t>(check.t0) + static_cast<size_t>(end);
    if (m_inputs.size() < need) {
        m_inputs.resize(need, 0);
        m_samples.resize(need);
    }
    for (int rel = 0; rel < end; ++rel) {
        size_t const tick = check.t0 + rel;
        m_samples[tick] = samples[rel];
        if (rel >= from) {
            m_inputs[tick] = bits[rel];
        }
    }
    if (end > from || static_cast<int>(check.t0) + end > m_planEnd) {
        m_planEnd = static_cast<int>(check.t0) + std::max(end, from);
    }
    m_planRevision += 1;

    m_stats.evaluations += planner.evaluations();
    if (planner.evaluations() > 0) m_stats.searches += 1;
    this->addSimCost(*check.sim);

    if (status == Planner::Status::Stuck) {
        float const at = end > 0 ? progressOf(m_layer, samples[end - 1].x) : 0.f;
        this->setStatus(fmt::format("No safe path found ahead (around {})", percent(at)));
    }
    else {
        this->setStatus("Playing");
    }

    m_check.reset();
}

void Bot::startCalibration(PlayLayer* layer, unsigned now) {
    auto& cal = m_calibration;
    cal = Calibration{};
    cal.active = true;
    cal.t0 = now;

    for (int delay = 0; delay < 2; ++delay) {
        FakeSim sim(layer, FakeWorld::get().player(), TickRecorder::get().ops(), delay, m_appliedHold);
        auto& out = cal.predicted[delay];
        out.reserve(kCalibrationTicks);
        InputFn input = [this, now](int t, Mode) {
            int const tick = static_cast<int>(now) + t;
            return tick < m_planEnd && tick < static_cast<int>(m_inputs.size()) && m_inputs[tick] != 0;
        };
        SampleFn sample = [&out](int, Sample const& s) { out.push_back(s); };
        sim.run(kCalibrationTicks, input, sample);
        this->addSimCost(sim);
    }
}

void Bot::finishCalibration() {
    auto& cal = m_calibration;
    cal.active = false;

    for (int delay = 0; delay < 2; ++delay) {
        if (cal.count[delay] == 0) continue;
        double const average = cal.error[delay] / cal.count[delay];
        m_delayError[delay] = m_delayError[delay] < 0.0 ? average : m_delayError[delay] * 0.85 + average * 0.15;
    }
    if (m_delayError[0] >= 0.0 && m_delayError[1] >= 0.0) {
        // Switch only for a clear improvement, so ties keep the current model.
        int const other = 1 - m_inputDelay;
        if (m_delayError[other] + 0.05 < m_delayError[m_inputDelay]) {
            m_inputDelay = other;
            log::info("Input timing model switched to {} tick delay", m_inputDelay);
        }
    }
    m_stats.inputDelay = m_inputDelay;
    m_stats.predictionError = m_delayError[m_inputDelay];
}

void Bot::addSimCost(FakeSim const& sim) {
    m_simTicks += sim.ticksRun();
    m_simSeconds += sim.secondsRun();
    if (m_simTicks > 0) {
        m_stats.microsPerTick = m_simSeconds * 1e6 / static_cast<double>(m_simTicks);
    }
}

void Bot::beforeTick(PlayLayer* layer) {
    m_tickBefore = layer->m_gameState.m_currentProgress;
    if (m_state != BotState::Playing) return;

    bool const want = static_cast<int>(m_tickBefore) < m_planEnd &&
        m_tickBefore < m_inputs.size() && m_inputs[m_tickBefore] != 0;
    if (want != m_appliedHold) {
        if (want) m_stats.clicks += 1;
        applyJump(layer, want);
        m_appliedHold = want;
    }
    m_usedThisAttempt = true;
}

void Bot::afterTick(PlayLayer* layer) {
    if (m_state != BotState::Playing || !layer->m_player1) return;
    auto const actual = layer->m_player1->getPosition();
    unsigned const tick = m_tickBefore;

    auto& cal = m_calibration;
    if (cal.active) {
        int const rel = static_cast<int>(tick) - static_cast<int>(cal.t0);
        if (rel >= 0 && rel < kCalibrationTicks) {
            for (int delay = 0; delay < 2; ++delay) {
                if (rel < static_cast<int>(cal.predicted[delay].size())) {
                    auto const& p = cal.predicted[delay][rel];
                    cal.error[delay] += std::abs(p.x - actual.x) + std::abs(p.y - actual.y);
                    cal.count[delay] += 1;
                }
            }
        }
        if (rel >= kCalibrationTicks - 1 || rel < 0) {
            this->finishCalibration();
        }
    }

    // If reality drifts from the prediction, re-check right away.
    if (static_cast<int>(tick) < m_planEnd && tick < m_samples.size() && !m_desyncFlagged) {
        auto const& expected = m_samples[tick];
        bool const known = expected.x != 0.f || expected.y != 0.f;
        if (known && (std::abs(expected.x - actual.x) > kDesyncTolerance || std::abs(expected.y - actual.y) > kDesyncTolerance)) {
            m_desyncFlagged = true;
            m_stats.desyncs += 1;
            if (settings::autoCorrect()) {
                m_forceCheck = true;
            }
        }
    }
}

void Bot::onRealDeath(PlayLayer* layer) {
    if (m_state != BotState::Playing) return;
    float const at = progressOf(layer, layer->m_player1 ? layer->m_player1->getPositionX() : 0.f);
    m_stats.deaths += 1;
    m_state = BotState::Died;
    this->setStatus(fmt::format("Died at {}", percent(at)));
}

void Bot::onRealComplete(PlayLayer*) {
    if (m_state != BotState::Playing && !m_usedThisAttempt) return;
    m_check.reset();
    m_state = BotState::Finished;
    this->setStatus("Level completed (safe mode: nothing was saved)");
}

} // namespace ac
