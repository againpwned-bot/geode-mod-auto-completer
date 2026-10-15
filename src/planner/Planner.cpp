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
        m_seedPlan[tick] = decision;

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

bool Planner::extendFloor(int tick) {
    if (tick <= m_floor) return true;
    int const seedSize = static_cast<int>(m_seed.size());
    if (tick > seedSize) {
        // Past the seed the real game presses nothing: the current path mustn't either.
        if (m_frontier < tick || static_cast<int>(m_bits.size()) < tick) return false;
        for (int t = std::max(m_floor, seedSize); t < tick; ++t) {
            if (m_bits[t]) return false;
        }
    }
    // Every decision in [floor, tick) must still be exactly the seed's: that is what the
    // real game is playing meanwhile.
    auto a = m_plan.lower_bound(m_floor);
    auto b = m_seedPlan.lower_bound(m_floor);
    while (true) {
        bool const aIn = a != m_plan.end() && a->first < tick;
        bool const bIn = b != m_seedPlan.end() && b->first < tick;
        if (!aIn && !bIn) break;
        if (aIn != bIn || a->first != b->first || a->second.state != b->second.state ||
            a->second.tapLen != b->second.tapLen) {
            return false;
        }
        ++a;
        ++b;
    }
    m_floor = tick;
    m_forcedEnd = std::max(m_forcedEnd, std::min(tick, seedSize));
    return true;
}

bool Planner::commitFloor(int tick) {
    if (tick <= m_floor) return true;
    if (m_frontier < tick) return false;
    m_floor = tick;
    // The best path so far may have been made of other early decisions; from now on only
    // paths through the committed ones count.
    m_maxFrontier = m_frontier;
    m_bestEnd = 0;
    m_dirtyFrom = 0;
    this->syncBest();
    return true;
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
    // The first death is the seed's own: every change comes after it.
    if (m_seedDeath < 0) m_seedDeath = deathTick;
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

    bool windowDone = fix.firstValid >= 0 &&
        (fix.firstValid - fix.next > m_cfg.windowCap || fix.invalidStreak >= 2);
    // Only marginal fixes so far (a late orb click that dies a few ticks later): keep
    // scanning while the player is on an orb, an earlier click on it may be the real way
    // through (and becomes an orb click to explore).
    if (windowDone && m_cfg.exploreDepth > 0 && fix.bestSurvival < std::min(m_cfg.clearTicks, m_cfg.horizonMax) &&
        this->ringAt(fix.next)) {
        windowDone = false;
    }
    // The floor may have moved up while the search ran (live use).
    if (fix.next < std::max(fix.lo, this->lockTick()) || windowDone) {
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
    int chances = 0;
    int survival = this->evaluate(tick, decision, fix.deathTick, &chances);
    this->note(TraceEntry::Kind::Eval, tick, survival);

    if (survival >= m_cfg.horizonMin) {
        fix.valid.push_back(Candidate{ tick, decision, survival, false, chances });
        fix.bestSurvival = std::max(fix.bestSurvival, survival);
        if (fix.firstValid < 0) {
            fix.firstValid = tick;
        }
        fix.invalidStreak = 0;
        return;
    }
    if (fix.firstValid >= 0) {
        fix.invalidStreak += 1;
    }
    // An orb click that ends differently, with room left for another click before that end,
    // and somewhere on the way where that click could act (an orb that only sends the player
    // into the next hazard with nothing to click on is a dead end however late it dies).
    if (m_cfg.exploreDepth > 0 && survival != 0 && fix.deathTick + survival - tick >= m_cfg.exploreRoom &&
        chances > 0 && this->ringAt(tick)) {
        fix.explore.push_back(Candidate{ tick, decision, survival, true, chances });
    }
}

bool Planner::ringAt(int tick) const {
    auto touching = [this](int t) {
        return t >= 0 && t < static_cast<int>(m_samples.size()) && m_samples[t].ring;
    };
    return touching(tick) || touching(tick - 1);
}

bool Planner::robustAt(Candidate const& c, bool explore) {
    // A neighbour has to do about as well as the candidate itself: just outliving the old
    // death by a few ticks could still mean landing on the next spike.
    int const need = std::max(m_cfg.horizonMin, std::min(c.survival, m_cfg.horizonMax) - 3);
    bool const current = c.tick < static_cast<int>(m_bits.size()) && m_bits[c.tick];
    int const lo = std::max(m_fix.lo, this->lockTick());
    for (int neighbour : { c.tick - 1, c.tick + 1 }) {
        if (neighbour < lo || neighbour > m_fix.deathTick) return false;
        bool const held = neighbour < static_cast<int>(m_bits.size()) && m_bits[neighbour];
        if (held != current) continue;
        int const survival = this->evaluate(neighbour, c.decision, m_fix.deathTick);
        this->note(TraceEntry::Kind::Eval, neighbour, survival);
        if (explore ? std::abs(survival - c.survival) > 2 : survival < need) return false;
    }
    return true;
}

std::vector<Planner::Candidate> Planner::orderExplore(std::vector<Candidate> explore) const {
    if (explore.empty()) return explore;
    // Clicks that end the same way form windows; the middle of the longest one is the
    // click that still ends that way a tick earlier or later.
    std::sort(explore.begin(), explore.end(), [](Candidate const& a, Candidate const& b) {
        return a.tick < b.tick;
    });
    int const gap = std::max(m_cfg.scanStride, 1);
    size_t bestFrom = 0;
    size_t bestTo = 0;
    size_t runFrom = 0;
    for (size_t i = 1; i <= explore.size(); ++i) {
        bool const broken = i == explore.size() || explore[i].tick - explore[i - 1].tick > gap ||
            std::abs(explore[i].survival - explore[runFrom].survival) > 2;
        if (!broken) continue;
        size_t const runTo = i - 1;
        int const length = explore[runTo].tick - explore[runFrom].tick;
        int const bestLength = explore[bestTo].tick - explore[bestFrom].tick;
        if (length > bestLength ||
            (length == bestLength && explore[runFrom].survival > explore[bestFrom].survival)) {
            bestFrom = runFrom;
            bestTo = runTo;
        }
        runFrom = i;
    }
    double const target = (explore[bestFrom].tick + explore[bestTo].tick) / 2.0;

    std::vector<Candidate> ordered(explore.begin() + bestFrom, explore.begin() + bestTo + 1);
    std::vector<Candidate> rest(explore.begin(), explore.begin() + bestFrom);
    rest.insert(rest.end(), explore.begin() + bestTo + 1, explore.end());
    std::sort(ordered.begin(), ordered.end(), [target](Candidate const& a, Candidate const& b) {
        return std::abs(a.tick - target) < std::abs(b.tick - target);
    });
    std::sort(rest.begin(), rest.end(), [target](Candidate const& a, Candidate const& b) {
        if (a.survival != b.survival) return a.survival > b.survival;
        return std::abs(a.tick - target) < std::abs(b.tick - target);
    });
    ordered.insert(ordered.end(), rest.begin(), rest.end());
    for (auto& c : ordered) c.explore = true;
    return ordered;
}

int Planner::exploreDepth(bool skipTop) const {
    int depth = 0;
    size_t const count = skipTop && !m_stack.empty() ? m_stack.size() - 1 : m_stack.size();
    for (size_t i = 0; i < count; ++i) {
        if (m_stack[i].explore) depth += 1;
    }
    return depth;
}

void Planner::chooseExplore(std::vector<Candidate> explore) {
    std::vector<Candidate> ordered = this->orderExplore(std::move(explore));

    auto pick = ordered.begin();
    if (m_cfg.robust) {
        pick = std::find_if(ordered.begin(), ordered.end(), [this](Candidate const& c) {
            return this->robustAt(c, true);
        });
        if (pick == ordered.end()) {
            this->backtrack();
            return;
        }
    }
    Candidate chosen = *pick;
    ordered.erase(pick);

    this->applyDecision(chosen.tick, chosen.decision);
    this->note(TraceEntry::Kind::Explore, chosen.tick);

    StackEntry entry;
    entry.tick = chosen.tick;
    entry.decision = chosen.decision;
    entry.alternatives = std::move(ordered);
    entry.deathTick = m_fix.deathTick;
    entry.lo = m_fix.lo;
    entry.scanNext = m_fix.next;
    entry.explore = true;
    m_stack.push_back(std::move(entry));
}

bool Planner::canPressAt(int tick) const {
    int const start = m_snaps.empty() ? 0 : m_snaps.begin()->first;
    if (tick <= start || tick - 1 >= static_cast<int>(m_samples.size())) return true;

    Sample const& before = m_samples[tick - 1];
    bool const groundOnly = before.mode == Mode::Cube || before.mode == Mode::Robot ||
        before.mode == Mode::Ball || before.mode == Mode::Spider;
    if (!groundOnly || before.grounded || before.ring) return true;
    // In the air only an orb answers a click: one being touched, or one reached while the
    // button is still held (GD activates an orb the player holds into).
    int const reach = std::max(1, m_cfg.tapLen);
    for (int t = tick; t < tick + reach && t < static_cast<int>(m_samples.size()); ++t) {
        if (m_samples[t].ring) return true;
    }
    return false;
}

int Planner::evaluate(int tick, Decision const& decision, int deathTick, int* chances) {
    m_regionEvals += 1;
    m_totalEvals += 1;

    auto const& snap = m_snaps.at(this->snapAtOrBefore(tick));
    m_sim.load(snap.handle);
    m_cursorValid = false;

    Policy policy = snap.policy;
    InputFn input = [&](int t, Mode mode) {
        return this->policyInput(policy, t, mode, &decision, tick);
    };
    int count = 0;
    bool leftOrb = false; // left the orb the click was on (it is used up)
    SampleFn sample = [&](int t, Sample const& s) {
        if (!chances || t <= tick) return;
        bool const groundOnly = s.mode == Mode::Cube || s.mode == Mode::Robot || s.mode == Mode::Ball ||
            s.mode == Mode::Spider;
        if (!s.ring) leftOrb = true;
        if (!groundOnly || s.grounded || (s.ring && leftOrb)) count += 1;
    };

    RunResult result = m_sim.run(deathTick + m_cfg.horizonMax + 1, input, sample);
    if (chances) *chances = count;
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
    auto explore = std::move(m_fix.explore);
    m_fix.valid.clear();
    m_fix.explore.clear();
    // Clicks the floor has passed meanwhile can't be made any more.
    int const lock = this->lockTick();
    auto passed = [lock](Candidate const& c) { return c.tick < lock; };
    valid.erase(std::remove_if(valid.begin(), valid.end(), passed), valid.end());
    explore.erase(std::remove_if(explore.begin(), explore.end(), passed), explore.end());

    bool const canExplore = !explore.empty() && this->exploreDepth(false) < m_cfg.exploreDepth;
    if (valid.empty()) {
        if (canExplore) this->chooseExplore(std::move(explore));
        else this->backtrack();
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

    if (canExplore) {
        // Orb clicks that change how the player dies are worth a try too: first when every
        // real fix only buys a few ticks (likely a dead end, like jumping into the next
        // hazard), otherwise once the real fixes have failed.
        auto extra = this->orderExplore(std::move(explore));
        bool const marginal = best < std::min(m_cfg.clearTicks, m_cfg.horizonMax);
        ordered.insert(marginal ? ordered.begin() : ordered.end(), extra.begin(), extra.end());
    }

    auto pick = ordered.begin();
    if (m_cfg.robust) {
        // The click must also work a tick earlier and later, so a small timing error in
        // the real game can't kill and the next obstacle keeps some room. Only the best
        // candidates are checked, in order, to keep this cheap.
        pick = std::find_if(ordered.begin(), ordered.end(), [this](Candidate const& c) {
            return this->robustAt(c, c.explore);
        });
        if (pick == ordered.end()) {
            this->backtrack();
            return;
        }
    }
    Candidate chosen = *pick;
    ordered.erase(pick);

    this->applyDecision(chosen.tick, chosen.decision);
    this->note(chosen.explore ? TraceEntry::Kind::Explore : TraceEntry::Kind::Choose, chosen.tick);

    StackEntry entry;
    entry.tick = chosen.tick;
    entry.decision = chosen.decision;
    entry.alternatives = std::move(ordered);
    entry.deathTick = m_fix.deathTick;
    entry.lo = m_fix.lo;
    entry.scanNext = m_fix.next;
    entry.explore = chosen.explore;
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

        // Exploring alternatives only while the chain of them stays short.
        if (this->exploreDepth(true) >= m_cfg.exploreDepth) {
            auto& alternatives = top.alternatives;
            alternatives.erase(
                std::remove_if(
                    alternatives.begin(), alternatives.end(), [](Candidate const& c) { return c.explore; }
                ),
                alternatives.end()
            );
        }
        // Nor clicks the floor has passed (live use).
        top.alternatives.erase(
            std::remove_if(
                top.alternatives.begin(), top.alternatives.end(), [lock](Candidate const& c) { return c.tick < lock; }
            ),
            top.alternatives.end()
        );
        if (!top.alternatives.empty()) {
            Candidate alt = top.alternatives.front();
            top.alternatives.erase(top.alternatives.begin());
            top.tick = alt.tick;
            top.decision = alt.decision;
            top.explore = alt.explore;
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
