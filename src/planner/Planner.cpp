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
            config.horizonMax = 90;
            config.tapLen = 3;
            config.regionBudget = 8000;
            break;
        case PlayStyle::Human:
            config.tapLen = 14;
            config.tapLenJitter = 6;
            config.timingJitter = 0.22;
            break;
        case PlayStyle::Minimal:
            // Latest safe tick: fewest, latest clicks.
            config.windowCap = 12;
            config.tapLen = 2;
            break;
    }
    return config;
}

Planner::Planner(Simulator& sim, PlannerConfig config, bool initialHold)
    : m_sim(sim), m_cfg(config), m_rng(config.seed) {
    int const start = m_sim.tick();

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
    return std::max(0, m_maxFrontier - m_cfg.lockTicks);
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
    Decision decision = this->makeDecision(!current);
    int survival = this->evaluate(tick, decision, fix.deathTick);

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

    std::vector<Candidate> top;
    std::vector<Candidate> rest;
    for (auto const& c : valid) {
        (capped(c) >= best ? top : rest).push_back(c);
    }

    int early = top.front().tick;
    int late = top.front().tick;
    for (auto const& c : top) {
        early = std::min(early, c.tick);
        late = std::max(late, c.tick);
    }

    double target = (early + late) / 2.0;
    if (m_cfg.style == PlayStyle::Minimal) {
        target = late;
    }
    else if (m_cfg.timingJitter > 0.0 && late > early) {
        std::normal_distribution<double> dist(0.0, m_cfg.timingJitter * (late - early));
        target = std::clamp(target + dist(m_rng), static_cast<double>(early), static_cast<double>(late));
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

    Candidate chosen = ordered.front();
    ordered.erase(ordered.begin());

    this->applyDecision(chosen.tick, chosen.decision);

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
            return;
        }
    }
    this->giveUp(m_maxFrontier);
}

} // namespace ac
