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

    char const* modeName(Mode mode) {
        switch (mode) {
            case Mode::Cube: return "cube";
            case Mode::Ship: return "ship";
            case Mode::Ball: return "ball";
            case Mode::Ufo: return "ufo";
            case Mode::Wave: return "wave";
            case Mode::Robot: return "robot";
            case Mode::Spider: return "spider";
            case Mode::Swing: return "swing";
        }
        return "?";
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
    return m_stepIndex;
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
    if (!settings::safeMode()) return false;
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
    m_tickOpen = false;
    m_stepIndex = 0;
    m_seenStep = false;
    m_movementKnown = false;
    ObjectTracker::get().resetAttempt();
    m_shadowValid = false;
    m_deathPending = false;
    m_selfTestDone = false;
    m_usedThisAttempt = false;
    m_stats.clicks = 0;
    m_stats.plannedAhead = 0;
    m_attempt += 1;
}

void Bot::onLevelInit(PlayLayer* layer) {
    this->resetAttempt();
    ObjectTracker::get().resetLevel();
    m_layer = layer;
    m_appliedHold = false;
    m_requestRestart = false;
    m_stats = BotStats{};
    m_variantError.fill(-1.0);
    m_model = PhysicsModel{};
    m_probe = InlineProbe{};
    m_shadowLogged = 0;
    m_surprisesLogged = 0;
    m_orbLogs = 0;
    m_shadowInputLogged = 0;
    m_shadowInputTicks = 0;
    m_shadowInputMismatches = 0;
    m_selfTests = 0;
    m_tracesLogged = 0;
    m_diagnoses = 0;
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
    ObjectTracker::get().resetLevel();
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

    PlayerObject* const hidden = FakeWorld::get().player();
    if (!FakeWorld::get().ensure(layer)) {
        m_state = BotState::Unsupported;
        this->setStatus("Could not create the simulation player");
        return;
    }
    if (FakeWorld::get().player() != hidden) {
        // Anything still simulating used the old hidden player.
        m_check.reset();
        m_calibration = Calibration{};
    }
    // The first real ticks tell us which physics calls GD makes per tick.
    auto& recorder = TickRecorder::get();
    if (!recorder.ready()) return;
    if (!m_probe.done || m_probeRevision != recorder.revision()) {
        this->runProbe(layer);
    }
    if (!m_selfTestDone && m_selfTests < 3 && modeOf(layer->m_player1) == Mode::Cube &&
        layer->m_player1->m_isOnGround && layer->m_player1->getPositionX() > 0.f) {
        this->runSelfTest(layer);
    }

    // Index of the physics step that runs next (see beforeTick).
    unsigned const now = m_stepIndex;

    // The real player went through something the plan didn't see coming (a gravity, dual or
    // teleport portal, or a mode change the simulation missed): plan again right away from
    // its new state, free to change even the next tick, and within this frame.
    bool const urgent = this->movementSurprise(layer, now);
    int64_t const budgetMs = std::clamp<int64_t>(settings::liveBudget(), 1, 30) * (urgent ? 3 : 1);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);

    if (m_check && (urgent || now >= m_check->t0 + m_check->floor)) {
        // Planning from an outdated state, or too slow to finish before its fixed part ran
        // out; start over from here.
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
        this->startCheck(layer, now, urgent ? 0 : kFloor);
    }

    if (m_check) {
        m_check->sim->beginSlice();
        while (true) {
            Planner::Status status;
            do {
                status = m_check->planner->step();
            } while (status == Planner::Status::Running && std::chrono::steady_clock::now() < deadline);

            if (status == Planner::Status::Running) {
                m_check->sim->endSlice();
                break;
            }
            bool const freshPlan = status == Planner::Status::Stuck && m_check->seeded && !m_check->retried;
            bool const exactPlan = status == Planner::Status::Stuck && !freshPlan && m_check->config.robust;
            if (!freshPlan && !exactPlan) {
                this->finishCheck();
                break;
            }
            // The search can only move the previous plan's jumps earlier, so first plan this
            // stretch again from scratch; if even that needs frame-perfect timing, allow it.
            this->retryCheck(!exactPlan);
            if (std::chrono::steady_clock::now() >= deadline) {
                m_check->sim->endSlice();
                break;
            }
        }
    }

    m_stats.plannedAhead = std::max(0, m_planEnd - static_cast<int>(now));
}

bool Bot::movementSurprise(PlayLayer* layer, unsigned now) {
    auto* player = layer->m_player1;
    Mode const mode = modeOf(player);
    bool const upsideDown = player->m_isUpsideDown;
    bool const dual = layer->m_gameState.m_isDualMode;
    int const signature = static_cast<int>(mode) | (upsideDown ? 16 : 0) | (dual ? 32 : 0);
    bool const changed = m_movementKnown && signature != m_movement;
    int const previous = m_movement;
    m_movement = signature;
    m_movementKnown = true;
    if (!changed || now == 0) return false;

    // Did the plan see it coming? Gravity orbs and pads are GD's own physics, so a predicted
    // flip is exact; a new game mode comes from the bot's portal handling, so the plan is
    // redone from the real state even when it expected it.
    int const last = static_cast<int>(now) - 1;
    bool predicted = false;
    if (last < m_planEnd && last < static_cast<int>(m_samples.size())) {
        auto const& sample = m_samples[last];
        predicted = sample.mode == mode && sample.upsideDown == upsideDown && !dual;
    }
    bool const modeChanged = (previous & 15) != static_cast<int>(mode);
    if (predicted && !modeChanged) return false;
    if (m_surprisesLogged < 6) {
        m_surprisesLogged += 1;
        log::info(
            "Tick {}: player is now {}{}{}; planning again right away", now, modeName(mode),
            upsideDown ? ", upside down" : "", dual ? ", dual" : ""
        );
    }
    return true;
}

void Bot::startCheck(PlayLayer* layer, unsigned now, int floor) {
    auto check = std::make_unique<Check>();
    check->t0 = now;
    check->floor = floor;

    std::vector<uint8_t> seed;
    int const end = std::min<int>(m_planEnd, static_cast<int>(m_inputs.size()));
    for (int t = static_cast<int>(now); t < end && t < static_cast<int>(now) + kHorizon; ++t) {
        seed.push_back(m_inputs[t]);
    }
    check->seedSize = static_cast<int>(seed.size());
    check->initialHold = m_appliedHold;
    for (int t = std::min(floor, check->seedSize); t < check->seedSize; ++t) {
        bool const previous = t == 0 ? m_appliedHold : seed[t - 1] != 0;
        if ((seed[t] != 0) != previous) {
            check->seeded = true;
            break;
        }
    }
    check->seed = seed;
    check->startFix.capture(layer->m_player1);

    check->sim = this->makeSim(layer, m_model, static_cast<int>(now));

    uint32_t const seedValue = static_cast<uint32_t>(layer->m_level->m_levelID.value()) * 2654435761u ^
        (m_attempt * 7919u) ^ (now * 40503u);
    PlannerConfig config = PlannerConfig::forStyle(settings::playStyle(), seedValue);
    config.goalTicks = kHorizon;
    config.robust = true;
    check->config = config;
    check->planner = std::make_unique<Planner>(*check->sim, config, m_appliedHold, std::move(seed), floor);

    m_check = std::move(check);
    m_lastCheck = static_cast<int>(now);
    m_forceCheck = false;
    m_desyncFlagged = false;
    m_stats.checks += 1;
}

void Bot::retryCheck(bool robust) {
    auto& check = *m_check;
    if (m_tracesLogged < 6) {
        m_tracesLogged += 1;
        log::info(
            "Plan from tick {} stuck ({}); planning again from scratch{}", check.t0,
            check.retried ? "fresh plan" : "following the previous plan", robust ? "" : " allowing exact timings"
        );
        this->logTrace(*check.planner, check.t0);
    }
    if (!check.retried) {
        this->diagnoseStuck(check);
        this->logOrbsNearPath(check);
    }
    m_stats.evaluations += check.planner->evaluations();

    // Back to the state the check started from, then forget the old planner (its
    // snapshots are released, the hidden player keeps the loaded state).
    check.sim->load(check.planner->startHandle());
    check.planner.reset();

    // Only the fixed part (the floor) of the previous plan is kept.
    std::vector<uint8_t> seed(
        check.seed.begin(), check.seed.begin() + std::min<size_t>(static_cast<size_t>(check.floor), check.seed.size())
    );
    check.config.robust = robust;
    check.planner = std::make_unique<Planner>(*check.sim, check.config, check.initialHold, std::move(seed), check.floor);
    check.retried = true;
}

void Bot::diagnoseStuck(Check& check) {
    if (m_diagnoses >= 4 || !check.planner || !check.sim) return;

    int death = -1;
    for (auto const& e : check.planner->trace()) {
        if (e.kind == Planner::TraceEntry::Kind::Fix) {
            death = e.a;
            break;
        }
    }
    if (death < 30) return;
    m_diagnoses += 1;

    struct Outcome {
        float rise = 0.f;
        int died = -1;
        float diedY = 0.f;
    };
    auto runWith = [&](FakeSim& sim, int press) {
        Outcome o;
        float const y0 = check.startFix.nodePosition.y;
        InputFn input = [press](int t, Mode) { return t >= press && t < press + 6; };
        SampleFn sample = [&](int, Sample const& s) { o.rise = std::max(o.rise, s.y - y0); };
        auto const result = sim.run(death + 40, input, sample);
        if (result.event == Event::Died) {
            o.died = result.eventTick;
            o.diedY = sim.lastDeath().y;
        }
        return o;
    };
    auto show = [](int press, Outcome const& o) {
        if (o.died < 0) return fmt::format("press {}: rise {:.1f} survived", press, o.rise);
        return fmt::format("press {}: rise {:.1f} died at {} (y {:.1f})", press, o.rise, o.died, o.diedY);
    };

    // Same start, same inputs: once through the planner's snapshot restore, once through
    // a freshly synced simulation.
    std::string restored;
    std::string fresh;
    bool restoreBroken = false;
    for (int offset : { 20, 35, 50 }) {
        int const press = death - offset;
        if (press < 0) continue;

        check.sim->load(check.planner->startHandle());
        Outcome const a = runWith(*check.sim, press);

        auto const& recorder = TickRecorder::get();
        FakeSim sim(
            m_layer, FakeWorld::get().player(), recorder.ops(), recorder.timeStep(), m_model, check.initialHold,
            static_cast<int>(check.t0), &check.startFix
        );
        Outcome const b = runWith(sim, press);

        restored += " | " + show(press, a);
        fresh += " | " + show(press, b);
        if (a.died >= 0 && b.died < 0) restoreBroken = true;
    }
    check.sim->load(check.planner->startHandle());

    log::info("Stuck at death tick {}. Restored:{} || Fresh:{}", death, restored, fresh);
    if (restoreBroken && !FakeSim::reattributeOnLoad()) {
        FakeSim::reattributeOnLoad() = true;
        log::info("Snapshot restore loses state; re-copying player attributes on every restore from now on");
    }
}

void Bot::logTrace(Planner const& planner, unsigned t0) {
    using Kind = Planner::TraceEntry::Kind;
    std::string out;
    for (auto const& e : planner.trace()) {
        if (out.size() > 3000) {
            out += " ...";
            break;
        }
        switch (e.kind) {
            case Kind::Fix: out += fmt::format(" | death {} scan to {}:", e.a, e.b); break;
            case Kind::Eval: out += fmt::format(" {}:{}", e.a, e.b); break;
            case Kind::Choose: out += fmt::format(" -> press {}", e.a); break;
            case Kind::Explore: out += fmt::format(" -> try {}", e.a); break;
            case Kind::Backtrack: out += fmt::format(" | undo {}", e.a); break;
            case Kind::GiveUp: out += fmt::format(" | give up at {}", e.a); break;
        }
    }
    log::info("Search from tick {} (tick:survival):{}", t0, out.empty() ? " none" : out);
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
    int const from = std::min(check.floor, check.seedSize);
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
        this->logDeath(*check.sim);
        this->logOrbsNearPath(check);
        if (m_tracesLogged < 6) {
            m_tracesLogged += 1;
            this->logTrace(planner, check.t0);
        }
        this->diagnoseStuck(check);
    }
    else {
        this->setStatus("Playing");
    }

    m_check.reset();
}

std::unique_ptr<FakeSim> Bot::makeSim(PlayLayer* layer, PhysicsModel const& model, int baseStep) const {
    auto const& recorder = TickRecorder::get();
    return std::make_unique<FakeSim>(
        layer, FakeWorld::get().player(), recorder.ops(), recorder.timeStep(), model, m_appliedHold, baseStep
    );
}

void Bot::runProbe(PlayLayer* layer) {
    using Place = PhysicsModel::Place;
    auto const& recorder = TickRecorder::get();
    m_check.reset();
    m_calibration = Calibration{};

    auto sim = this->makeSim(layer, m_model, static_cast<int>(m_stepIndex));
    m_probe = sim->probeInlines();
    m_probe.done = true;
    m_probeRevision = recorder.revision();
    this->addSimCost(*sim);

    // Run each inlined function only where the recorded calls don't already contain it.
    m_model.preCollision = !m_probe.preCollision;
    m_model.collisionLog = !m_probe.collisionLog;
    m_model.jumpVars = m_probe.jumpVars ? Place::Inside : Place::Start;
    m_model.stateVars = m_probe.stateVars ? Place::Inside : Place::Start;
    m_variantError.fill(-1.0);

    m_stats.model = m_model.describe();
    m_stats.tickCalls = recorder.describe();
    m_stats.inputDelay = m_model.inputDelay;
    log::info("Tick calls: {} | step {:.6f} s | model: {}", m_stats.tickCalls, recorder.timeStep(), m_stats.model);
}

void Bot::startCalibration(PlayLayer* layer, unsigned now) {
    auto& cal = m_calibration;
    cal = Calibration{};
    cal.active = true;
    cal.t0 = now;

    InputFn input = [this, now](int t, Mode) {
        int const tick = static_cast<int>(now) + t;
        return tick < m_planEnd && tick < static_cast<int>(m_inputs.size()) && m_inputs[tick] != 0;
    };
    for (int v = 0; v < PhysicsModel::kVariants; ++v) {
        auto sim = this->makeSim(layer, PhysicsModel::variant(v, m_model), static_cast<int>(now));
        auto& out = cal.predicted[v];
        out.reserve(kCalibrationTicks);
        SampleFn sample = [&out](int, Sample const& s) { out.push_back(s); };
        sim->run(kCalibrationTicks, input, sample);
        this->addSimCost(*sim);
    }
}

void Bot::finishCalibration() {
    auto& cal = m_calibration;
    cal.active = false;

    for (int v = 0; v < PhysicsModel::kVariants; ++v) {
        if (cal.count[v] == 0) continue;
        double const average = cal.error[v] / cal.count[v];
        m_variantError[v] = m_variantError[v] < 0.0 ? average : m_variantError[v] * 0.85 + average * 0.15;
    }

    int const current = m_model.variantIndex();
    int best = current;
    for (int v = 0; v < PhysicsModel::kVariants; ++v) {
        if (m_variantError[v] >= 0.0 && (m_variantError[best] < 0.0 || m_variantError[v] < m_variantError[best])) {
            best = v;
        }
    }
    // Switch only for a clear improvement, so ties keep the current model.
    if (best != current && m_variantError[current] >= 0.0 &&
        m_variantError[best] + std::max(0.02, m_variantError[current] * 0.1) < m_variantError[current]) {
        m_model = PhysicsModel::variant(best, m_model);
        m_stats.model = m_model.describe();
        log::info("Physics model switched to: {}", m_stats.model);
    }
    m_stats.inputDelay = m_model.inputDelay;
    m_stats.predictionError = m_variantError[m_model.variantIndex()];
}

void Bot::addSimCost(FakeSim const& sim) {
    m_simTicks += sim.ticksRun();
    m_simSeconds += sim.secondsRun();
    if (m_simTicks > 0) {
        m_stats.microsPerTick = m_simSeconds * 1e6 / static_cast<double>(m_simTicks);
    }
}

void Bot::beforeTick(PlayLayer* layer) {
    // GD 2.2081 advances m_currentProgress by 2 per physics step, so the bot counts steps
    // itself: plans, samples and simulations are all indexed by physics step. A second
    // processCommands call for the same step (a half step) isn't a new step.
    unsigned const progress = layer->m_gameState.m_currentProgress;
    if (m_seenStep && progress == m_lastProgress) return;
    if (m_seenStep && !m_strideLogged && progress > m_lastProgress) {
        m_strideLogged = true;
        log::info("GD tick counter advances by {} per physics step", progress - m_lastProgress);
    }
    m_seenStep = true;
    m_lastProgress = progress;

    // A new step starting means the previous one is fully done.
    this->closeTick(layer);
    m_tickBefore = m_stepIndex;
    m_stepIndex += 1;
    // GD moves objects right after this, so from here on they show this step.
    ObjectTracker::get().setWorldStep(static_cast<int>(m_tickBefore));
    if (m_state != BotState::Playing) return;
    m_tickOpen = true;

    bool const want = static_cast<int>(m_tickBefore) < m_planEnd &&
        m_tickBefore < m_inputs.size() && m_inputs[m_tickBefore] != 0;
    if (m_probe.done && TickRecorder::get().ready() && layer->m_player1) {
        // Remember the state before this tick so it can be replayed on the hidden player.
        m_shadowPre.capture(layer->m_player1);
        m_shadowHeld = m_appliedHold;
        m_shadowWant = want;
        m_shadowValid = true;
    }
    if (want != m_appliedHold) {
        if (want) m_stats.clicks += 1;
        applyJump(layer, want);
        m_appliedHold = want;
    }
    m_usedThisAttempt = true;
}

void Bot::endFrame(PlayLayer* layer) {
    this->closeTick(layer);
    if (m_state == BotState::Playing && layer) {
        auto& tracker = ObjectTracker::get();
        if (!tracker.levelScanned()) tracker.scanLevel(layer);
        tracker.observeVisible(layer);
    }
}

void Bot::closeTick(PlayLayer* layer) {
    if (!m_tickOpen) return;
    m_tickOpen = false;
    if (m_deathPending) {
        m_deathPending = false;
        this->checkDeathTick(layer);
    }
    if (m_state != BotState::Playing || !layer || !layer->m_player1) return;
    auto const actual = layer->m_player1->getPosition();
    unsigned const tick = m_tickBefore;

    auto& cal = m_calibration;
    if (cal.active) {
        int const rel = static_cast<int>(tick) - static_cast<int>(cal.t0);
        if (rel >= 0 && rel < kCalibrationTicks) {
            for (int v = 0; v < PhysicsModel::kVariants; ++v) {
                if (rel < static_cast<int>(cal.predicted[v].size())) {
                    auto const& p = cal.predicted[v][rel];
                    cal.error[v] += std::abs(p.x - actual.x) + std::abs(p.y - actual.y);
                    cal.count[v] += 1;
                }
            }
        }
        if (rel >= kCalibrationTicks - 1 || rel < 0) {
            this->finishCalibration();
        }
    }

    if (m_shadowValid) {
        this->runShadow(layer);
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

void Bot::runShadow(PlayLayer* layer) {
    m_shadowValid = false;
    auto* real = layer->m_player1;
    auto* fake = FakeWorld::get().player();
    if (!real || !fake || real->m_isDead) return;
    auto const& recorder = TickRecorder::get();

    PlayerFix after;
    after.capture(real);

    // Same tick, same input, same starting state: the hidden player must end up identical.
    PhysicsModel model = m_model;
    model.inputDelay = 0;
    FakeSim sim(
        layer, fake, recorder.ops(), recorder.timeStep(), model, m_shadowHeld, static_cast<int>(m_tickBefore), &m_shadowPre
    );
    // GD sets the player's clock just before each step, so the state captured at the step
    // start already has this tick's time.
    if (recorder.timeStep() > 0.0) {
        sim.setClock(m_shadowPre.m_totalTime - recorder.timeStep());
    }
    bool const want = m_shadowWant;
    sim.run(1, [want](int, Mode) { return want; }, [](int, Sample const&) {});
    PlayerFix simulated;
    sim.captureState(simulated);
    // Written by GD once per frame, not per tick, and the real clock may already be set
    // for the next step when this tick is closed.
    simulated.m_position = after.m_position;
    simulated.m_totalTime = after.m_totalTime;

    size_t count = 0;
    std::string const fields = after.diff(simulated, 10, &count);
    bool const inputChanged = m_shadowHeld != want;
    m_stats.shadowTicks += 1;
    if (inputChanged) m_shadowInputTicks += 1;
    if (count > 0) {
        m_stats.shadowMismatches += 1;
        if (inputChanged) m_shadowInputMismatches += 1;
        // Ticks where the bot pressed or released are the interesting ones.
        bool const shouldLog = inputChanged ? m_shadowInputLogged < 40 : m_shadowLogged < 20;
        if (shouldLog) {
            (inputChanged ? m_shadowInputLogged : m_shadowLogged) += 1;
            log::info(
                "Tick check {} (input {}->{}, {}): {} field(s) differ (real/sim): {}",
                m_tickBefore, m_shadowHeld ? 1 : 0, want ? 1 : 0, sim.lastDeath().valid ? "sim died" : "alive",
                count, fields
            );
        }
    }
    if (m_stats.shadowTicks % 1200 == 0) {
        log::info(
            "Tick checks: {} ticks, {} differ; press/release ticks: {}, {} differ; moving objects: {} of {}, "
            "trigger moves: {}, group toggles: {}, passed triggers that didn't fire: {} ({} switched off)",
            m_stats.shadowTicks, m_stats.shadowMismatches, m_shadowInputTicks, m_shadowInputMismatches,
            ObjectTracker::get().movingCount(), ObjectTracker::get().size(), ObjectTracker::get().commands().size(),
            ObjectTracker::get().toggleCount(), ObjectTracker::get().unfiredTriggers(),
            ObjectTracker::get().unfiredSwitchedOff()
        );
    }
}

void Bot::checkDeathTick(PlayLayer* layer) {
    // Replay the tick the real player died on, from the exact state it started in. If the
    // hidden player survives, the simulation is missing whatever killed the real one; if
    // it dies too, the plan went wrong earlier (for example an object moved meanwhile).
    if (!m_shadowValid || !layer) return;
    m_shadowValid = false;
    auto* fake = FakeWorld::get().player();
    if (!fake) return;
    auto const& recorder = TickRecorder::get();
    PhysicsModel model = m_model;
    model.inputDelay = 0;
    FakeSim sim(
        layer, fake, recorder.ops(), recorder.timeStep(), model, m_shadowHeld, static_cast<int>(m_tickBefore), &m_shadowPre
    );
    if (recorder.timeStep() > 0.0) {
        sim.setClock(m_shadowPre.m_totalTime - recorder.timeStep());
    }
    bool const want = m_shadowWant;
    Sample last;
    sim.run(1, [want](int, Mode) { return want; }, [&last](int, Sample const& s) { last = s; });

    std::string outcome;
    auto const& d = sim.lastDeath();
    if (d.valid) {
        outcome = fmt::format(
            "died too, by obj {} type {} at ({:.1f},{:.1f})", d.objectId, d.objectType, d.objectX, d.objectY
        );
    }
    else {
        outcome = fmt::format("SURVIVED at ({:.1f},{:.1f})", last.x, last.y);
    }
    log::info(
        "Death tick check: real player killed by {}; hidden player from the same state: {}",
        m_deathKiller, outcome
    );
}

void Bot::runSelfTest(PlayLayer* layer) {
    m_selfTestDone = true;
    m_selfTests += 1;
    auto* real = layer->m_player1;

    struct Result {
        float rise = 0.f;
        int airTicks = 0;
        float dx = 0.f;
        int died = -1;
        std::string killer;
    };
    auto test = [&](int pressAt) {
        Result r;
        PhysicsModel model = m_model;
        auto sim = this->makeSim(layer, model, static_cast<int>(m_stepIndex));
        float const x0 = real->getPositionX();
        float const y0 = real->getPositionY();
        int lastAir = -1;
        int firstAir = -1;
        InputFn input = [pressAt](int t, Mode) { return pressAt >= 0 && t >= pressAt && t < pressAt + 6; };
        SampleFn sample = [&](int t, Sample const& s) {
            r.rise = std::max(r.rise, s.y - y0);
            r.dx = s.x - x0;
            if (std::abs(s.y - y0) > 0.5f) {
                if (firstAir < 0) firstAir = t;
                lastAir = t;
            }
        };
        auto const result = sim->run(90, input, sample);
        if (result.event == Event::Died) {
            r.died = result.eventTick;
            auto const& d = sim->lastDeath();
            r.killer = fmt::format("obj {} type {} at ({:.0f},{:.0f})", d.objectId, d.objectType, d.objectX, d.objectY);
        }
        r.airTicks = firstAir >= 0 ? lastAir - firstAir + 1 : 0;
        return r;
    };
    auto show = [](Result const& r) {
        std::string out = fmt::format("rise {:.1f}, air {} ticks, dx {:.1f}", r.rise, r.airTicks, r.dx);
        if (r.died >= 0) out += fmt::format(", died at tick {} ({})", r.died, r.killer);
        return out;
    };

    Result const now = test(0);
    Result const later = test(12);
    Result const none = test(-1);
    log::info(
        "Self test {}: real at ({:.1f},{:.1f}) vy {:.3f} ground {}/{}/{} buffered {} 22changes {} impulsefix {} speed {:.3f} | press now: {} | press at 12: {} | no press: {}",
        m_selfTests, real->getPositionX(), real->getPositionY(), real->m_yVelocity,
        real->m_isOnGround ? 1 : 0, real->m_isOnGround2 ? 1 : 0, real->m_isOnGround3 ? 1 : 0,
        real->m_jumpBuffered ? 1 : 0, real->m_enable22Changes ? 1 : 0, real->m_enableImpulseFix ? 1 : 0,
        real->m_playerSpeed, show(now), show(later), show(none)
    );
}

void Bot::logDeath(FakeSim const& sim) {
    auto const& d = sim.lastDeath();
    if (!d.valid) return;
    float const realX = m_layer && m_layer->m_player1 ? m_layer->m_player1->getPositionX() : 0.f;
    // Why the killer was there: where it is now, and what moves or switches it.
    std::string why;
    if (auto* object = d.object) {
        why = fmt::format(
            " (now at ({:.1f},{:.1f}){}; {})", object->m_positionX, object->m_positionY,
            d.switchedOn ? ", switched on by a toggle the simulation expects" : "", ObjectTracker::get().describe(object)
        );
    }
    log::info(
        "Simulation died {} ticks ahead at ({:.1f},{:.1f}) by obj {} type {} at ({:.1f},{:.1f}){}; real player at x {:.1f}",
        d.tick, d.x, d.y, d.objectId, d.objectType, d.objectX, d.objectY, why, realX
    );
}

void Bot::logOrbsNearPath(Check const& check) {
    if (m_orbLogs >= 6 || !check.planner) return;
    auto const& samples = check.planner->samples();
    int const end = std::min<int>(static_cast<int>(samples.size()), check.planner->bestFrontier() + 1);
    int const from = std::min(check.floor, end);
    if (end - from < 2) return;
    m_orbLogs += 1;

    float minX = samples[from].x;
    float maxX = samples[from].x;
    for (int t = from; t < end; ++t) {
        minX = std::min(minX, samples[t].x);
        maxX = std::max(maxX, samples[t].x);
    }
    // Orbs and pads the planned path comes close to, closest first.
    struct Near {
        GameObject* object;
        float distance;
        int tick;
    };
    std::vector<Near> found;
    auto& tracker = ObjectTracker::get();
    auto* real = m_layer ? m_layer->m_player1 : nullptr;
    float const realX = real ? real->getPositionX() : samples[from].x;
    float const realY = real ? real->getPositionY() : samples[from].y;
    for (auto* orb : tracker.orbs()) {
        // Moving orbs can come from far away; anything within a few hundred units counts.
        if (orb->m_positionX < minX - 600.0 || orb->m_positionX > maxX + 600.0) continue;
        float best = 1e9f;
        int bestTick = -1;
        for (int t = from; t < end; ++t) {
            // Where the orb is expected to be at that tick, not where it is now.
            auto const at = tracker.estimate(orb, static_cast<int>(check.t0) + t, samples[t].x - realX, samples[t].y - realY);
            float const d = std::max(std::abs(samples[t].x - at.x), std::abs(samples[t].y - at.y));
            if (d < best) {
                best = d;
                bestTick = t;
            }
        }
        if (best < 120.f) found.push_back({ orb, best, bestTick });
    }
    std::sort(found.begin(), found.end(), [](Near const& a, Near const& b) { return a.distance < b.distance; });

    std::string out;
    for (size_t i = 0; i < found.size() && i < 5; ++i) {
        auto* orb = found[i].object;
        out += fmt::format(
            " | obj {} type {} at ({:.1f},{:.1f}){}{}, path {:.0f} away at tick {}; {}", orb->m_objectID,
            static_cast<int>(orb->m_objectType), orb->m_positionX, orb->m_positionY,
            orb->m_isGroupDisabled ? " switched off" : "", orb->m_isDisabled ? " disabled" : "", found[i].distance,
            check.t0 + found[i].tick, ObjectTracker::get().describe(orb)
        );
    }
    log::info("Orbs and pads near the path from tick {}:{}", check.t0, out.empty() ? " none within 120" : out);
}

void Bot::onRealDeath(PlayLayer* layer, std::string const& killer) {
    if (m_state != BotState::Playing) return;
    m_deathKiller = killer;
    m_deathPending = true;
    float const at = progressOf(layer, layer->m_player1 ? layer->m_player1->getPositionX() : 0.f);

    // What the plan expected around the death, for the log.
    if (layer->m_player1) {
        unsigned const tick = m_tickBefore;
        auto const pos = layer->m_player1->getPosition();
        std::string presses;
        int const from = std::max(0, static_cast<int>(tick) - 240);
        for (int t = from; t <= static_cast<int>(tick) && t < static_cast<int>(m_inputs.size()); ++t) {
            bool const now = m_inputs[t] != 0;
            bool const before = t > 0 && m_inputs[t - 1] != 0;
            if (now != before) presses += fmt::format(" {}{}", now ? "+" : "-", t);
        }
        std::string expected = "none";
        if (tick < m_samples.size() && static_cast<int>(tick) < m_planEnd) {
            expected = fmt::format("({:.1f},{:.1f})", m_samples[tick].x, m_samples[tick].y);
        }
        log::info(
            "Real player died at tick {} at ({:.1f},{:.1f}) by {}; plan expected {}; plan ends at {}; inputs before:{}",
            tick, pos.x, pos.y, killer, expected, m_planEnd, presses.empty() ? " none" : presses
        );
    }
    m_stats.deaths += 1;
    m_state = BotState::Died;
    this->setStatus(fmt::format("Died at {}", percent(at)));
}

void Bot::onRealComplete(PlayLayer*) {
    if (m_state != BotState::Playing && !m_usedThisAttempt) return;
    m_check.reset();
    m_state = BotState::Finished;
    this->setStatus(settings::safeMode() ? "Level completed (safe mode: nothing was saved)" : "Level completed");
}

} // namespace ac
