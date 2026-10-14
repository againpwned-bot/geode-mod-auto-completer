#include "Planner.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <iterator>

namespace ac {

namespace {
    constexpr int kCompletedSurvival = INT_MAX / 2;
}

PlannerConfig PlannerConfig::forStyle(PlayStyle style, uint32_t seed) {
    PlannerConfig config;
    config.style = style;
    config.seed = seed;
    switch (style) {
        case PlayStyle::Default:
            break;
        case PlayStyle::Perfect:
            // Frame precise scan, wider window and a longer look past each obstacle,
            // so the chosen tick sits in the exact middle of the safe window.
            config.scanStride = 1;
            config.windowCap = 32;
            config.horizonMax = 120;
            config.tapLen = 3;
            config.regionBudget = 8000;
            break;
        case PlayStyle::Human:
            config.tapLen = 14;
            config.tapLenJitter = 6;
            config.timingJitter = 0.15;
            break;
        case PlayStyle::Minimal:
            // Latest safe tick: fewest, latest clicks.
            config.windowCap = 12;
            config.tapLen = 2;
            break;
    }
    return config;
}

Planner::Planner(Simulator& sim, PlannerConfig config, bool initialHold, std::vector<uint8_t> seed, int floor)
    : m_sim(sim), m_cfg(config), m_rng(config.seed), m_seed(std::move(seed)) {
    int const start = m_sim.tick();
    m_floor = std::max(floor, 0);
    m_forcedEnd = std::min(m_floor, static_cast<int>(m_seed.size()));

    // Presses and releases of the seed after the floor become revisable decisions.
    int lastDecision = m_floor - 1;
    for (int tick = m_floor; tick < static_cast<int>(m_seed.size()); ++tick) {
        bool const previous = tick == 0 ? initialHold : m_seed[tick - 1] != 0;
        bool const current = m_seed[tick] != 0;
        if (current == previous) continue;

        int run = 1;
        while (tick + run < static_cast<int>(m_seed.size()) && (m_seed[tick + run] != 0) == current) {
            run += 1;
        }
        Decision decision;
        decision.state = current;
        decision.tapLen = current ? run : 0;
        m_plan[tick] = decision;

        StackEntry entry;
        entry.tick = tick;
        entry.decision = decision;
        entry.deathTick = tick + m_cfg.horizonMin + 8;
        entry.lo = std::max(lastDecision + 1, m_floor);
        entry.scanNext = tick - 1;
        m_stack.push_back(std::move(entry));
        lastDecision = tick;
    }

    Policy policy;
    policy.held = initialHold;
    if (initialHold) {
        policy.pressTick = start;
        policy.tapLen = m_cfg.tapLen;
    }

    Snap snap;
    snap.handle = m_sim.save();
    snap.policy = policy;
    if (snap.handle < 0) {
        m_status = Status::Stuck;
        m_endTick = start;
        return;
    }

    m_startHandle = snap.handle;
    m_snaps[start] = snap;
    m_frontier = start;
    m_maxFrontier = start;
    m_cursorValid = true;
    m_cursorTick = start;
    m_cursorPolicy = policy;
}

Planner::~Planner() {
    for (auto const& [tick, snap] : m_snaps) {
        m_sim.release(snap.handle);
    }
}

int Planner::startHandle() const {
    return m_startHandle;
}

Planner::Status Planner::step() {
    if (m_status != Status::Running) {
        return m_status;
    }
    if (m_fix.active) {
        this->stepFix();
    }
    else {
        this->advance();
    }
    return m_status;
}

bool Planner::policyInput(Policy& policy, int tick, Mode mode, Decision const* extra, int extraTick) const {
    if (tick >= 0 && tick < m_forcedEnd) {
        // Before the floor the seed is played exactly.
        bool const held = m_seed[tick] != 0;
        if (held && !policy.held) {
            int run = 1;
            while (tick + run < static_cast<int>(m_seed.size()) && m_seed[tick + run]) run += 1;
            policy.pressTick = tick;
            policy.tapLen = run;
        }
        policy.held = held;
        return held;
    }

    Decision const* decision = nullptr;
    if (extra && tick == extraTick) {
        decision = extra;
    }
    else {
        auto it = m_plan.find(tick);
        if (it != m_plan.end()) {
            decision = &it->second;
        }
    }

    if (decision) {
        policy.held = decision->state;
        if (decision->state) {
            policy.pressTick = tick;
            policy.tapLen = decision->tapLen;
        }
    }
    else if (policy.held && !isHoldMode(mode) && tick - policy.pressTick >= policy.tapLen) {
        policy.held = false;
    }
    return policy.held;
}

Planner::Decision Planner::makeDecision(bool state) {
    Decision decision;
    decision.state = state;
    decision.tapLen = m_cfg.tapLen;
    if (state && m_cfg.tapLenJitter > 0) {
        std::uniform_int_distribution<int> dist(-m_cfg.tapLenJitter, m_cfg.tapLenJitter);
        decision.tapLen += dist(m_rng);
    }
    decision.tapLen = std::max(decision.tapLen, 1);
    return decision;
}

void Planner::recordBit(int tick, bool held) {
    if (tick < 0) return;
    if (tick >= static_cast<int>(m_bits.size())) {
        m_bits.resize(tick + 1, 0);
    }
    m_bits[tick] = held ? 1 : 0;
}

void Planner::recordSample(int tick, Sample const& sample) {
    if (tick < 0) return;
    if (tick >= static_cast<int>(m_samples.size())) {
        m_samples.resize(tick + 1);
    }
    m_samples[tick] = sample;
}

int Planner::lockTick() const {
    return std::max({ 0, m_maxFrontier - m_cfg.lockTicks, m_floor });
}

int Planner::snapAtOrBefore(int tick) const {
    auto it = m_snaps.upper_bound(tick);
    if (it == m_snaps.begin()) {
        return it->first;
    }
    return std::prev(it)->first;
}

void Planner::loadSnap(int tick) {
    auto const& snap = m_snaps.at(tick);
    m_sim.load(snap.handle);
    m_cursorPolicy = snap.policy;
    m_cursorTick = tick;
    m_cursorValid = true;
}

void Planner::pushSnap() {
    int handle = m_sim.save();
    if (handle < 0) return;
    auto it = m_snaps.find(m_cursorTick);
    if (it != m_snaps.end()) {
        m_sim.release(it->second.handle);
    }
    m_snaps[m_cursorTick] = Snap{ handle, m_cursorPolicy };
}

void Planner::invalidateAfter(int tick) {
    for (auto it = m_snaps.upper_bound(tick); it != m_snaps.end();) {
        if (it->second.handle == m_startHandle) {
            ++it;
            continue;
        }
        m_sim.release(it->second.handle);
        it = m_snaps.erase(it);
    }
}

void Planner::prune() {
    int const lock = this->lockTick();
    if (lock <= 0) return;

    int const keep = this->snapAtOrBefore(lock);
    for (auto it = m_snaps.begin(); it != m_snaps.end();) {
        if (it->first < keep && it->second.handle != m_startHandle) {
            m_sim.release(it->second.handle);
            it = m_snaps.erase(it);
        }
        else {
            ++it;
        }
    }

    // Decisions behind the lock are final; forget their alternatives.
    auto firstLive = std::find_if(m_stack.begin(), m_stack.end(), [lock](StackEntry const& entry) {
        return entry.tick >= lock;
    });
    m_stack.erase(m_stack.begin(), firstLive);
}

void Planner::markDirty(int tick) {
    m_dirtyFrom = std::min(m_dirtyFrom, std::max(tick, 0));
}

void Planner::syncBest() {
    // Everything from the earliest change up to the frontier was re-simulated with the
    // current plan, so it can be copied over the previous best path.
    int const from = std::min(m_dirtyFrom, m_bestEnd);
    int const to = std::min<int>(m_frontier, static_cast<int>(std::min(m_bits.size(), m_samples.size())));
    if (to > static_cast<int>(m_bestBits.size())) {
        m_bestBits.resize(to, 0);
        m_bestSamples.resize(to);
    }
    for (int tick = from; tick < to; ++tick) {
        m_bestBits[tick] = m_bits[tick];
        m_bestSamples[tick] = m_samples[tick];
    }
    m_bestEnd = std::max(m_bestEnd, to);
    m_dirtyFrom = INT_MAX;
}

void Planner::giveUp(int tick) {
    this->note(TraceEntry::Kind::GiveUp, tick);
    m_fix.active = false;
    m_status = Status::Stuck;
    m_endTick = tick;
}

void Planner::advance() {
    if (!m_cursorValid || m_cursorTick != m_frontier) {
        this->loadSnap(m_frontier);
    }

    Policy& policy = m_cursorPolicy;
    InputFn input = [&](int tick, Mode mode) {
        bool held = this->policyInput(policy, tick, mode, nullptr, 0);
        this->recordBit(tick, held);
        return held;
    };
    SampleFn sample = [&](int tick, Sample const& s) {
        this->recordSample(tick, s);
    };

    RunResult result = m_sim.run(m_frontier + m_cfg.snapshotInterval, input, sample);

    if (result.event == Event::Completed) {
        m_cursorValid = false;
        m_endTick = result.eventTick + 1;
        m_frontier = std::max(m_frontier, m_endTick);
        m_maxFrontier = std::max(m_maxFrontier, m_frontier);
        this->syncBest();
        m_status = Status::Completed;
        return;
    }
    if (result.event == Event::Died) {
        m_cursorValid = false;
        this->beginFix(result.eventTick);
        return;
    }

    if (result.endTick <= m_frontier) {
        // The simulator made no progress; nothing sensible left to do.
        this->giveUp(m_frontier);
        return;
    }

    m_cursorTick = result.endTick;
    m_frontier = result.endTick;
    this->pushSnap();

    if (m_cfg.goalTicks > 0 && m_frontier >= m_cfg.goalTicks) {
        m_maxFrontier = std::max(m_maxFrontier, m_frontier);
        this->syncBest();
        m_endTick = m_frontier;
        m_status = Status::Completed;
        return;
    }

    if (m_frontier > m_maxFrontier) {
        m_maxFrontier = m_frontier;
        m_regionEvals = 0;
        this->syncBest();
        this->prune();
    }
}

void Planner::beginFix(int deathTick) {
    int lastDecision = -1;
    auto it = m_plan.lower_bound(deathTick + 1);
    if (it != m_plan.begin()) {
        lastDecision = std::prev(it)->first;
    }

    m_fix = FixSearch{};
    m_fix.active = true;
    m_fix.deathTick = deathTick;
    m_fix.lo = std::max({ lastDecision + 1, deathTick - m_cfg.maxLookback, this->lockTick(), 0 });
    m_fix.next = deathTick;
    this->note(TraceEntry::Kind::Fix, deathTick, m_fix.lo);

    if (m_fix.lo > m_fix.next) {
        m_fix.active = false;
        this->backtrack();
    }
}

void Planner::stepFix() {
    auto& fix = m_fix;

    bool const windowDone = fix.firstValid >= 0 &&
        (fix.firstValid - fix.next > m_cfg.windowCap || fix.invalidStreak >= 2);
    if (fix.next < fix.lo || windowDone) {
        this->finishFix();
        return;
    }
    if (m_regionEvals >= m_cfg.regionBudget) {
        this->giveUp(m_maxFrontier);
        return;
    }

    int const tick = fix.next;
    fix.next -= std::max(m_cfg.scanStride, 1);

    bool const current = tick < static_cast<int>(m_bits.size()) && m_bits[tick];
    if (!current && !this->canPressAt(tick)) {
        return;
    }
    Decision decision = this->makeDecision(!current);
    int survival = this->evaluate(tick, decision, fix.deathTick);
    this->note(TraceEntry::Kind::Eval, tick, survival);

    if (survival >= m_cfg.horizonMin) {
        fix.valid.push_back(Candidate{ tick, decision, survival });
        if (fix.firstValid < 0) {
            fix.firstValid = tick;
        }
        fix.invalidStreak = 0;
    }
    else if (fix.firstValid >= 0) {
        fix.invalidStreak += 1;
    }
}

bool Planner::robustAt(Candidate const& c) {
    // A neighbour has to do about as well as the candidate itself: just outliving the old
    // death by a few ticks could still mean landing on the next spike.
    int const need = std::max(m_cfg.horizonMin, std::min(c.survival, m_cfg.horizonMax) - 3);
    bool const current = c.tick < static_cast<int>(m_bits.size()) && m_bits[c.tick];
    for (int neighbour : { c.tick - 1, c.tick + 1 }) {
        if (neighbour < m_fix.lo || neighbour > m_fix.deathTick) return false;
        bool const held = neighbour < static_cast<int>(m_bits.size()) && m_bits[neighbour];
        if (held != current) continue;
        int const survival = this->evaluate(neighbour, c.decision, m_fix.deathTick);
        this->note(TraceEntry::Kind::Eval, neighbour, survival);
        if (survival < need) return false;
    }
    return true;
}

bool Planner::canPressAt(int tick) const {
    int const start = m_snaps.empty() ? 0 : m_snaps.begin()->first;
    if (tick <= start || tick - 1 >= static_cast<int>(m_samples.size())) return true;

    Sample const& before = m_samples[tick - 1];
    bool const groundOnly = before.mode == Mode::Cube || before.mode == Mode::Robot ||
        before.mode == Mode::Ball || before.mode == Mode::Spider;
    if (!groundOnly || before.grounded || before.ring) return true;
    return tick < static_cast<int>(m_samples.size()) && m_samples[tick].ring;
}

int Planner::evaluate(int tick, Decision const& decision, int deathTick) {
    m_regionEvals += 1;
    m_totalEvals += 1;

    auto const& snap = m_snaps.at(this->snapAtOrBefore(tick));
    m_sim.load(snap.handle);
    m_cursorValid = false;

    Policy policy = snap.policy;
    InputFn input = [&](int t, Mode mode) {
        return this->policyInput(policy, t, mode, &decision, tick);
    };
    SampleFn sample = [](int, Sample const&) {};

    RunResult result = m_sim.run(deathTick + m_cfg.horizonMax + 1, input, sample);
    switch (result.event) {
        case Event::Completed: return kCompletedSurvival;
        case Event::Died: return result.eventTick - deathTick;
        case Event::None: break;
    }
    return m_cfg.horizonMax;
}

void Planner::finishFix() {
    m_fix.active = false;
    auto valid = std::move(m_fix.valid);
    m_fix.valid.clear();

    if (valid.empty()) {
        this->backtrack();
        return;
    }

    auto capped = [this](Candidate const& c) {
        return std::min(c.survival, m_cfg.horizonMax);
    };

    int best = 0;
    for (auto const& c : valid) {
        best = std::max(best, capped(c));
    }

    // Every candidate that clears the obstacle comfortably is equally good; preferring
    // the one that survives longest would favour the edge of the window.
    int const clear = best >= m_cfg.horizonMax ? best : std::min(best, std::max(m_cfg.clearTicks, m_cfg.horizonMin));
    std::vector<Candidate> top;
    std::vector<Candidate> rest;
    for (auto const& c : valid) {
        (capped(c) >= clear ? top : rest).push_back(c);
    }

    // The window is the longest unbroken run of good ticks (the latest one for Minimal).
    std::vector<int> ticks;
    for (auto const& c : top) ticks.push_back(c.tick);
    std::sort(ticks.begin(), ticks.end());
    int const gap = std::max(m_cfg.scanStride, 1);
    int early = ticks.front();
    int late = ticks.front();
    int runStart = ticks.front();
    for (size_t i = 1; i <= ticks.size(); ++i) {
        bool const broken = i == ticks.size() || ticks[i] - ticks[i - 1] > gap;
        if (!broken) continue;
        int const runEnd = ticks[i - 1];
        bool const longer = runEnd - runStart > late - early;
        bool const sameLater = runEnd - runStart == late - early && m_cfg.style == PlayStyle::Minimal;
        if (longer || sameLater) {
            early = runStart;
            late = runEnd;
        }
        if (i < ticks.size()) runStart = ticks[i];
    }

    // Stay a few ticks inside the window so a tick of input or physics error can't turn
    // a safe click into a death.
    double const margin = std::min(static_cast<double>(std::max(m_cfg.safetyMargin, 0)), (late - early) / 2.0);
    double const lo = early + margin;
    double const hi = late - margin;
    double target = (early + late) / 2.0;
    if (m_cfg.style == PlayStyle::Minimal) {
        target = hi;
    }
    else if (m_cfg.timingJitter > 0.0 && late > early) {
        std::normal_distribution<double> dist(0.0, m_cfg.timingJitter * (late - early));
        target = std::clamp(target + dist(m_rng), lo, hi);
    }

    auto byTarget = [target](Candidate const& a, Candidate const& b) {
        double da = std::abs(a.tick - target);
        double db = std::abs(b.tick - target);
        if (da != db) return da < db;
        return a.tick > b.tick;
    };
    std::sort(top.begin(), top.end(), byTarget);
    std::sort(rest.begin(), rest.end(), [&](Candidate const& a, Candidate const& b) {
        if (a.survival != b.survival) return a.survival > b.survival;
        return byTarget(a, b);
    });

    std::vector<Candidate> ordered = std::move(top);
    ordered.insert(ordered.end(), rest.begin(), rest.end());

    auto pick = ordered.begin();
    if (m_cfg.robust) {
        // The click must also work a tick earlier and later, so a small timing error in
        // the real game can't kill and the next obstacle keeps some room. Only the best
        // candidates are checked, in order, to keep this cheap.
        pick = std::find_if(ordered.begin(), ordered.end(), [this](Candidate const& c) {
            return this->robustAt(c);
        });
        if (pick == ordered.end()) {
            this->backtrack();
            return;
        }
    }
    Candidate chosen = *pick;
    ordered.erase(pick);

    this->applyDecision(chosen.tick, chosen.decision);
    this->note(TraceEntry::Kind::Choose, chosen.tick);

    StackEntry entry;
    entry.tick = chosen.tick;
    entry.decision = chosen.decision;
    entry.alternatives = std::move(ordered);
    entry.deathTick = m_fix.deathTick;
    entry.lo = m_fix.lo;
    entry.scanNext = m_fix.next;
    m_stack.push_back(std::move(entry));
}

void Planner::applyDecision(int tick, Decision const& decision) {
    m_plan[tick] = decision;
    this->markDirty(tick);
    this->invalidateAfter(tick);
    m_frontier = this->snapAtOrBefore(tick);
    m_cursorValid = false;
}

void Planner::backtrack() {
    int const lock = this->lockTick();
    while (!m_stack.empty()) {
        StackEntry& top = m_stack.back();
        if (top.tick < lock) break;

        int const oldTick = top.tick;
        m_plan.erase(oldTick);
        this->note(TraceEntry::Kind::Backtrack, oldTick);

        if (!top.alternatives.empty()) {
            Candidate alt = top.alternatives.front();
            top.alternatives.erase(top.alternatives.begin());
            top.tick = alt.tick;
            top.decision = alt.decision;
            m_plan[alt.tick] = alt.decision;

            int const from = std::min(oldTick, alt.tick);
            this->markDirty(from);
            this->invalidateAfter(from);
            m_frontier = this->snapAtOrBefore(from);
            m_cursorValid = false;
            return;
        }

        this->markDirty(oldTick);
        this->invalidateAfter(oldTick);
        m_frontier = this->snapAtOrBefore(oldTick);
        m_cursorValid = false;

        int const deathTick = top.deathTick;
        int const lo = std::max(top.lo, lock);
        int const scanNext = top.scanNext;
        m_stack.pop_back();

        if (scanNext >= lo) {
            // Keep scanning below the old window for this death.
            m_fix = FixSearch{};
            m_fix.active = true;
            m_fix.deathTick = deathTick;
            m_fix.lo = lo;
            m_fix.next = scanNext;
            this->note(TraceEntry::Kind::Fix, deathTick, lo);
            return;
        }
    }
    this->giveUp(m_maxFrontier);
}

} // namespace ac
