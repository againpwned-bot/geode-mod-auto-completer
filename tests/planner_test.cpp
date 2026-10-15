// Native test for the planner, using a small deterministic platformer that loosely
// mimics Geometry Dash (cube, ship, spikes, blocks, yellow orbs, ship portals).
//
// Build & run:  c++ -std=c++20 -O2 -I src tests/planner_test.cpp src/planner/Planner.cpp -o planner_test && ./planner_test

#include "planner/Planner.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <string>
#include <random>
#include <vector>

using namespace ac;

namespace {

struct Rect {
    float x0, y0, x1, y1;
    bool overlaps(Rect const& o) const {
        return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
    }
};

enum class Kind { Spike, Block, Orb, ShipPortal, CubePortal };

struct Object {
    Kind kind;
    Rect rect;
};

struct Level {
    std::string name;
    std::vector<Object> objects;
    float end = 0.f;
    float ceiling = 10000.f;
};

struct State {
    int tick = 0;
    float x = 0.f;
    float y = 0.f;
    float vy = 0.f;
    bool ship = false;
    bool onGround = true;
    bool held = false;
    bool prevHeld = false;
    bool buffered = false;
    std::vector<uint8_t> usedOrbs;
};

constexpr float kSpeed = 1.3f;
constexpr float kSize = 30.f;
constexpr float kJump = 2.4f;
constexpr float kGravity = 0.048f;
constexpr float kShipAccel = 0.03f;
constexpr float kShipMax = 1.4f;

class ToySim final : public Simulator {
public:
    // `restoreNoise` adds a small error to every restored snapshot, imitating a game whose
    // checkpoints do not restore the state bit for bit.
    explicit ToySim(Level const& level, float restoreNoise = 0.f) : m_level(level), m_noise(restoreNoise) {
        m_state.usedOrbs.assign(level.objects.size(), 0);
    }

    int tick() const override { return m_state.tick; }

    int save() override {
        m_snaps.push_back(m_state);
        m_live.push_back(true);
        m_saves += 1;
        return static_cast<int>(m_snaps.size() - 1);
    }

    void load(int handle) override {
        assert(handle >= 0 && handle < static_cast<int>(m_snaps.size()));
        if (!m_live[handle]) {
            std::printf("FATAL: load of released snapshot %d\n", handle);
            std::abort();
        }
        m_state = m_snaps[handle];
        if (m_noise != 0.f) {
            m_state.y += (m_loads % 2 ? m_noise : -m_noise);
            m_state.x += m_noise;
        }
        m_loads += 1;
    }

    void release(int handle) override {
        assert(handle >= 0 && handle < static_cast<int>(m_snaps.size()));
        if (!m_live[handle]) {
            std::printf("FATAL: double release of snapshot %d\n", handle);
            std::abort();
        }
        m_live[handle] = false;
    }

    RunResult run(int untilTick, InputFn const& input, SampleFn const& sample) override {
        RunResult result;
        while (m_state.tick < untilTick) {
            int const t = m_state.tick;
            Mode const mode = m_state.ship ? Mode::Ship : Mode::Cube;
            bool const held = input(t, mode);
            Event ev = this->stepOnce(held);
            m_ticks += 1;
            Sample smp{ m_state.x, m_state.y, held, mode };
            smp.grounded = m_state.onGround;
            smp.ring = m_ring;
            sample(t, smp);
            if (ev != Event::None) {
                result.event = ev;
                result.eventTick = t;
                break;
            }
        }
        result.endTick = m_state.tick;
        return result;
    }

    uint64_t stateKey() const override {
        uint64_t hash = 0xcbf29ce484222325ull;
        auto mix = [&hash](uint64_t v) { hash ^= v + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2); };
        auto q = [](float v) { return static_cast<uint64_t>(static_cast<int64_t>(std::lround(v * 256.f))); };
        auto const& s = m_state;
        mix(static_cast<uint64_t>(s.tick));
        mix(q(s.x));
        mix(q(s.y));
        mix(q(s.vy * 16.f));
        mix((s.ship ? 1u : 0u) | (s.onGround ? 2u : 0u) | (s.held ? 4u : 0u) | (s.prevHeld ? 8u : 0u) |
            (s.buffered ? 16u : 0u));
        for (uint8_t used : s.usedOrbs) mix(used);
        return hash == 0 ? 1 : hash;
    }

    // Start a planning copy from another simulator's current state, at tick 0.
    void copyStateFrom(ToySim const& other) {
        m_state = other.m_state;
        m_state.tick = 0;
    }

    bool held() const { return m_state.held; }

    int liveSnapshots() const {
        int n = 0;
        for (bool b : m_live) n += b ? 1 : 0;
        return n;
    }

    long ticks() const { return m_ticks; }
    long loads() const { return m_loads; }

private:
    Rect playerRect() const {
        return Rect{ m_state.x, m_state.y, m_state.x + kSize, m_state.y + kSize };
    }

    Rect hazardRect() const {
        // GD uses a smaller hitbox for hazards than for solids.
        return Rect{ m_state.x + 9.f, m_state.y + 9.f, m_state.x + kSize - 9.f, m_state.y + kSize - 9.f };
    }

    Event stepOnce(bool held) {
        State& s = m_state;
        m_ring = false;
        for (size_t i = 0; i < m_level.objects.size(); ++i) {
            auto const& o = m_level.objects[i];
            if (o.kind == Kind::Orb && !s.usedOrbs[i] && o.rect.overlaps(this->playerRect())) m_ring = true;
        }
        s.prevHeld = s.held;
        s.held = held;
        bool const pressed = held && !s.prevHeld;
        if (pressed) s.buffered = true;
        if (!held) s.buffered = false;

        // orbs: a fresh press while overlapping
        if (s.buffered && !s.ship) {
            for (size_t i = 0; i < m_level.objects.size(); ++i) {
                auto const& o = m_level.objects[i];
                if (o.kind == Kind::Orb && !s.usedOrbs[i] && o.rect.overlaps(this->playerRect())) {
                    s.usedOrbs[i] = 1;
                    s.vy = kJump;
                    s.onGround = false;
                    s.buffered = false;
                    break;
                }
            }
        }

        if (s.ship) {
            s.vy += held ? kShipAccel : -kShipAccel;
            s.vy = std::clamp(s.vy, -kShipMax, kShipMax);
        }
        else {
            if (s.onGround && held) {
                s.vy = kJump;
                s.onGround = false;
                s.buffered = false;
            }
            else if (!s.onGround) {
                s.vy -= kGravity;
            }
        }

        float const oldY = s.y;
        s.x += kSpeed;
        s.y += s.vy;
        s.onGround = false;

        if (s.y <= 0.f) {
            s.y = 0.f;
            s.vy = std::max(s.vy, 0.f);
            if (!s.ship) s.vy = 0.f;
            s.onGround = true;
        }
        if (s.y + kSize >= m_level.ceiling) {
            return Event::Died;
        }

        for (size_t i = 0; i < m_level.objects.size(); ++i) {
            auto const& o = m_level.objects[i];
            switch (o.kind) {
                case Kind::Spike:
                    if (o.rect.overlaps(this->hazardRect())) return Event::Died;
                    break;
                case Kind::Block:
                    if (o.rect.overlaps(this->playerRect())) {
                        if (oldY >= o.rect.y1 - 6.f && s.vy <= 0.f) {
                            s.y = o.rect.y1;
                            s.vy = 0.f;
                            s.onGround = true;
                        }
                        else if (s.ship && oldY + kSize <= o.rect.y0 + 6.f && s.vy >= 0.f) {
                            s.y = o.rect.y0 - kSize;
                            s.vy = 0.f;
                        }
                        else {
                            return Event::Died;
                        }
                    }
                    break;
                case Kind::ShipPortal:
                    if (o.rect.overlaps(this->playerRect())) s.ship = true;
                    break;
                case Kind::CubePortal:
                    if (o.rect.overlaps(this->playerRect())) s.ship = false;
                    break;
                case Kind::Orb:
                    break;
            }
        }

        s.tick += 1;
        if (s.x >= m_level.end) return Event::Completed;
        return Event::None;
    }

    Level const& m_level;
    bool m_ring = false;
    float m_noise = 0.f;
    State m_state;
    std::vector<State> m_snaps;
    std::vector<bool> m_live;
    long m_ticks = 0;
    long m_loads = 0;
    long m_saves = 0;
};

Object spike(float x, float y = 0.f) {
    return Object{ Kind::Spike, Rect{ x + 10.f, y, x + 20.f, y + 18.f } };
}
Object block(float x, float y, float w = 30.f, float h = 30.f) {
    return Object{ Kind::Block, Rect{ x, y, x + w, y + h } };
}
Object orb(float x, float y) {
    return Object{ Kind::Orb, Rect{ x, y, x + 36.f, y + 36.f } };
}
Object portal(Kind kind, float x) {
    return Object{ kind, Rect{ x, -1000.f, x + 10.f, 10000.f } };
}

std::vector<Level> makeLevels() {
    std::vector<Level> levels;

    {
        Level l; l.name = "spikes";
        for (float x = 300.f; x < 3000.f; x += 350.f) l.objects.push_back(spike(x));
        l.end = 3300.f;
        levels.push_back(l);
    }
    {
        Level l; l.name = "triple spike + stairs";
        l.objects.push_back(spike(300.f));
        l.objects.push_back(spike(330.f));
        l.objects.push_back(spike(360.f));
        l.objects.push_back(block(700.f, 0.f));
        l.objects.push_back(block(850.f, 0.f, 30.f, 60.f));
        l.objects.push_back(block(1000.f, 0.f, 200.f, 90.f));
        l.objects.push_back(spike(1300.f));
        l.objects.push_back(spike(1330.f));
        l.end = 1700.f;
        levels.push_back(l);
    }
    {
        Level l; l.name = "orb over spike pit";
        // 180 units of spikes: too wide for a single jump (~130 units), needs the orb.
        for (float x = 400.f; x < 570.f; x += 30.f) l.objects.push_back(spike(x));
        l.objects.push_back(orb(470.f, 40.f));
        l.end = 1000.f;
        levels.push_back(l);
    }
    {
        Level l; l.name = "ship corridor";
        l.ceiling = 300.f;
        l.objects.push_back(portal(Kind::ShipPortal, 200.f));
        l.objects.push_back(block(500.f, 0.f, 60.f, 150.f));
        l.objects.push_back(block(800.f, 150.f, 60.f, 150.f));
        l.objects.push_back(block(1100.f, 0.f, 60.f, 120.f));
        l.objects.push_back(block(1400.f, 180.f, 60.f, 120.f));
        l.objects.push_back(portal(Kind::CubePortal, 1700.f));
        l.objects.push_back(spike(1900.f));
        l.end = 2200.f;
        levels.push_back(l);
    }
    {
        // Long level with irregular obstacle spacing (~2 minutes of gameplay).
        Level l; l.name = "long generated";
        uint32_t rng = 12345;
        auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        float x = 300.f;
        while (x < 36000.f) {
            switch (next() % 4) {
                case 0: l.objects.push_back(spike(x)); x += 220.f + next() % 200; break;
                case 1: l.objects.push_back(spike(x)); l.objects.push_back(spike(x + 30.f)); x += 260.f + next() % 200; break;
                case 2: l.objects.push_back(block(x, 0.f, 90.f, 30.f)); l.objects.push_back(spike(x + 90.f)); x += 300.f + next() % 200; break;
                case 3: l.objects.push_back(spike(x)); l.objects.push_back(spike(x + 30.f)); l.objects.push_back(spike(x + 60.f)); x += 300.f + next() % 200; break;
            }
        }
        l.end = x + 300.f;
        levels.push_back(l);
    }
    {
        Level l; l.name = "impossible wall";
        l.objects.push_back(block(600.f, 0.f, 30.f, 400.f));
        l.end = 900.f;
        levels.push_back(l);
    }
    return levels;
}

// Replays the planned inputs from the start without snapshots.
Event replay(Level const& level, std::vector<uint8_t> const& bits, int ticks) {
    ToySim sim(level);
    InputFn input = [&](int t, Mode) { return t < static_cast<int>(bits.size()) && bits[t]; };
    SampleFn sample = [](int, Sample const&) {};
    return sim.run(ticks + 400, input, sample).event;
}

// Two orbs in a row, like the gravity orbs on "I Spy": orb A flips the player up, which on
// its own dies sooner than staying down; orb B, only reachable after A, saves it. The
// planner has to accept A even though it alone makes things worse.
class ChainSim final : public Simulator {
public:
    struct Rules {
        int aFrom = 40, aTo = 52;    // ticks the player touches orb A while down
        int bFrom = 20, bTo = 32;    // ticks after A that it touches orb B while up
        int downDies = 120;          // staying down dies here
        int upDies = 45;             // ticks after A that being up dies
        int end = 600;
        // A decoy: jumping in this window while down buys a few ticks and then dies anyway
        // (like jumping off the ceiling into the next hazard).
        int decoyFrom = -1, decoyTo = -1;
        int decoyGain = 8;
        // After B the player must click once more on a ledge from cFrom to cTo ticks after
        // B, or die; the ledge is only reached if B came at most earlyCut ticks into its
        // window (a late B falls past it). cTo <= 0: B alone is enough.
        int cFrom = 0, cTo = 0;
        int earlyCut = 0;
    };
    explicit ChainSim(Rules rules) : m_rules(rules) {}

    int tick() const override { return m_state.tick; }
    int save() override {
        m_snaps.push_back(m_state);
        m_live.push_back(true);
        return static_cast<int>(m_snaps.size() - 1);
    }
    void load(int handle) override {
        assert(handle >= 0 && handle < static_cast<int>(m_snaps.size()) && m_live[handle]);
        m_state = m_snaps[handle];
    }
    void release(int handle) override {
        assert(handle >= 0 && handle < static_cast<int>(m_snaps.size()) && m_live[handle]);
        m_live[handle] = false;
    }
    RunResult run(int untilTick, InputFn const& input, SampleFn const& sample) override {
        RunResult result;
        while (m_state.tick < untilTick) {
            int const t = m_state.tick;
            m_ticksRun += 1;
            bool const held = input(t, Mode::Cube);
            auto& s = m_state;
            if (held && !s.held) s.buffered = true;
            if (!held) s.buffered = false;
            s.held = held;

            bool ring = false;
            if (s.phase == 0 && s.buffered && t >= m_rules.decoyFrom && t <= m_rules.decoyTo) {
                s.phase = 3;
                s.buffered = false;
            }
            else if (s.phase == 0 && t >= m_rules.aFrom && t <= m_rules.aTo) {
                ring = true;
                if (s.buffered) {
                    s.phase = 1;
                    s.flipTick = t;
                    s.buffered = false;
                }
            }
            else if (s.phase == 1 && t >= s.flipTick + m_rules.bFrom && t <= s.flipTick + m_rules.bTo) {
                ring = true;
                if (s.buffered) {
                    s.phase = 2;
                    s.bTick = t;
                    s.early = t <= s.flipTick + m_rules.bFrom + m_rules.earlyCut;
                    s.buffered = false;
                }
            }
            else if (s.phase == 2 && m_rules.cTo > 0 && s.early && t >= s.bTick + m_rules.cFrom &&
                     t <= s.bTick + m_rules.cTo && s.buffered) {
                s.phase = 4;
                s.buffered = false;
            }
            s.tick += 1;

            Sample out;
            out.x = static_cast<float>(t);
            out.y = s.phase == 1 ? 100.f : 0.f;
            out.hold = held;
            out.mode = Mode::Cube;
            bool const onLedge = m_rules.cTo <= 0 || (s.early && t >= s.bTick + m_rules.cFrom);
            out.grounded = s.phase == 0 || (s.phase == 2 && onLedge) || s.phase == 4;
            out.ring = ring;
            sample(t, out);

            bool const died = (s.phase == 0 && t >= m_rules.downDies) ||
                (s.phase == 1 && t >= s.flipTick + m_rules.upDies) ||
                (s.phase == 2 && m_rules.cTo > 0 && t > s.bTick + m_rules.cTo) ||
                (s.phase == 3 && t >= m_rules.downDies + m_rules.decoyGain);
            if (died || t + 1 >= m_rules.end) {
                result.event = died ? Event::Died : Event::Completed;
                result.eventTick = t;
                break;
            }
        }
        result.endTick = m_state.tick;
        return result;
    }
    int liveSnapshots() const {
        int n = 0;
        for (bool b : m_live) n += b ? 1 : 0;
        return n;
    }
    long long ticksRun() const { return m_ticksRun; }
    uint64_t stateKey() const override {
        uint64_t hash = 0xcbf29ce484222325ull;
        auto mix = [&hash](uint64_t v) { hash ^= v + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2); };
        auto const& s = m_state;
        mix(static_cast<uint64_t>(s.tick));
        mix(static_cast<uint64_t>(s.phase));
        // Only what still decides the future: when A flipped while up, B and the ledge
        // while one is still to come.
        if (s.phase == 1) mix(static_cast<uint64_t>(s.flipTick));
        if (s.phase == 2 && m_rules.cTo > 0) {
            mix(static_cast<uint64_t>(s.bTick));
            mix(s.early ? 1u : 0u);
        }
        mix((s.held ? 2u : 0u) | (s.buffered ? 4u : 0u));
        return hash == 0 ? 1 : hash;
    }

private:
    struct State {
        int tick = 0;
        int phase = 0;
        int flipTick = 0;
        int bTick = 0;
        bool early = false;
        bool held = false;
        bool buffered = false;
    };
    Rules m_rules;
    State m_state;
    std::vector<State> m_snaps;
    std::vector<bool> m_live;
    long long m_ticksRun = 0;
};

Event replayChain(ChainSim::Rules rules, std::vector<uint8_t> const& bits) {
    ChainSim sim(rules);
    InputFn input = [&](int t, Mode) { return t < static_cast<int>(bits.size()) && bits[t]; };
    SampleFn sample = [](int, Sample const&) {};
    return sim.run(rules.end + 10, input, sample).event;
}

struct SlicedRun {
    Event outcome = Event::None;
    bool leaked = false;
    int extended = 0;
    int committed = 0;
    int restarted = 0;
    int desyncs = 0;
};

// How much planning a frame gets: planner steps, or (like the bot's milliseconds) ticks
// simulated, more for a search that took more than a frame and the most for one whose
// death is close, a rescue, or right after a stuck search.
struct FrameBudget {
    int steps = 0;
    int simTicks = 0;
};

// Bot::onFrame on a ChainSim level: each frame the game runs ticksPerFrame physics steps
// and the planner gets its budget. A search whose fixed part runs out keeps going if it
// hasn't changed what is being played (extendFloor), or has its current path played
// (commitFloor, playCommitted), otherwise it starts over; a stuck search is followed at once
// by one with nothing fixed. The game's state is only ever what it really played.
SlicedRun runSliced(ChainSim::Rules const& rules, PlayStyle style, FrameBudget budgetPerFrame, int ticksPerFrame) {
    constexpr int kHorizon = 300;
    constexpr int kFloor = 12;
    constexpr int kEvery = 16;
    constexpr int kMaxAge = 280;
    constexpr int kFloorSlack = 8;
    constexpr int kFrameTicks = 6;
    struct Live {
        int t0 = 0;
        int floor = 0;
        int seedSize = 0;
        std::unique_ptr<ChainSim> sim; // declared first: outlives the planner
        std::unique_ptr<Planner> planner;
    };
    SlicedRun out;
    ChainSim real(rules);
    std::vector<uint8_t> plan;   // m_inputs
    std::vector<uint8_t> played; // what the game really pressed, per tick
    int planEnd = 0;
    int lastCheck = -1000000;
    bool held = false;
    bool force = false;
    bool rescue = false;
    std::unique_ptr<Live> live;

    // A running or finished search's path must be what the game played up to now:
    // otherwise it planned from a state the game isn't in.
    auto checkPath = [&](std::vector<uint8_t> const& bits, int from, int to) {
        to = std::min({ to, static_cast<int>(played.size()), static_cast<int>(bits.size()) });
        for (int t = from; t < to; ++t) {
            if ((bits[t] != 0) != (played[t] != 0)) {
                out.desyncs += 1;
                return;
            }
        }
    };
    auto drop = [&] {
        live->planner.reset();
        if (live->sim->liveSnapshots() != 0) out.leaked = true;
        live.reset();
    };
    auto start = [&](int now, int floor) {
        live = std::make_unique<Live>();
        live->t0 = now;
        live->floor = floor;
        live->sim = std::make_unique<ChainSim>(rules);
        InputFn replayInput = [&](int t, Mode) { return t < static_cast<int>(played.size()) && played[t]; };
        SampleFn none = [](int, Sample const&) {};
        if (now > 0) live->sim->run(now, replayInput, none);
        // This planner counts ticks from the level start: the seed is what was played, then
        // the plan up to its end, at most a horizon ahead.
        std::vector<uint8_t> seed(played.begin(), played.begin() + now);
        int const seedEnd = std::min(planEnd, now + kHorizon);
        for (int t = now; t < seedEnd; ++t) seed.push_back(t < static_cast<int>(plan.size()) ? plan[t] : 0);
        live->seedSize = std::max(seedEnd - now, 0);
        PlannerConfig config = PlannerConfig::forStyle(style, 5);
        config.goalTicks = now + kHorizon;
        config.robust = true;
        live->planner = std::make_unique<Planner>(*live->sim, config, held, seed, now + floor);
        lastCheck = now;
        force = false;
    };
    // Bot::playCommitted
    auto playCommitted = [&] {
        auto const& planner = *live->planner;
        auto const& bits = planner.pathInputs();
        int const end = std::min<int>(planner.frontier(), static_cast<int>(bits.size()));
        if (end <= live->t0 + live->floor) return;
        if (static_cast<int>(plan.size()) < end) plan.resize(end, 0);
        for (int t = live->t0 + live->floor; t < end; ++t) plan[t] = bits[t];
        live->seedSize = std::max(live->seedSize, end - live->t0);
        planEnd = end;
    };
    // Bot::finishCheck
    auto finish = [&](bool stuck) {
        auto const& planner = *live->planner;
        auto const& bits = planner.inputs();
        int end = stuck ? planner.bestFrontier() : planner.endTick();
        end = std::max(std::min<int>(end, static_cast<int>(bits.size())), live->t0);
        checkPath(bits, live->t0, end);
        int const from = live->t0 + std::min(live->floor, live->seedSize);
        if (static_cast<int>(plan.size()) < end) plan.resize(end, 0);
        for (int t = from; t < end; ++t) plan[t] = bits[t];
        if (end > from || end > planEnd) planEnd = std::max(end, from);
    };

    while (out.outcome == Event::None && real.tick() < rules.end + 50) {
        int const now = real.tick();
        if (live) {
            int const age = now - live->t0;
            if (age + kFloorSlack >= live->floor) {
                auto& planner = *live->planner;
                int const floor = age + kFloor;
                bool moved = false;
                int newFloor = floor;
                if (age <= kMaxAge) {
                    int const to = std::min(floor, planner.frontier() - live->t0);
                    if (planner.extendFloor(live->t0 + floor)) {
                        moved = true;
                        out.extended += 1;
                    }
                    else if (age < live->floor && planner.seedDeath() >= 0 && to >= age + kFrameTicks &&
                             planner.commitFloor(live->t0 + to)) {
                        newFloor = to;
                        playCommitted();
                        moved = true;
                        out.committed += 1;
                    }
                    else if (planner.extendFloor(live->t0 + age + kFrameTicks)) {
                        newFloor = age + kFrameTicks;
                        moved = true;
                        out.extended += 1;
                    }
                }
                if (moved) {
                    live->floor = std::max(live->floor, newFloor);
                    checkPath(planner.pathInputs(), live->t0, planner.frontier());
                }
                else if (age >= live->floor || age > kMaxAge) {
                    drop();
                    force = true;
                    out.restarted += 1;
                }
            }
        }
        if (!live && (force || now - lastCheck >= kEvery || planEnd < now + kHorizon / 2)) {
            start(now, rescue ? 0 : kFloor);
        }
        // Bot::onFrame's budget: 3x for a rescue, else 2x once a search took more than a
        // frame and 3x when the death it is fixing is close.
        int factor = rescue ? 3 : 1;
        if (live && !rescue) {
            int const death = live->planner->seedDeath();
            bool const close = death >= 0 && death - now < 150; // this planner counts from the level start
            factor = close ? 3 : live->t0 < now ? 2 : 1;
        }
        long long budget = budgetPerFrame.simTicks > 0 ? static_cast<long long>(budgetPerFrame.simTicks) * factor
                                                        : static_cast<long long>(budgetPerFrame.steps);
        while (live && budget > 0) {
            while (budget > 0 && live->planner->status() == Planner::Status::Running) {
                long long const before = live->sim->ticksRun();
                live->planner->step();
                budget -= budgetPerFrame.simTicks > 0 ? std::max(1LL, live->sim->ticksRun() - before) : 1;
            }
            auto const status = live->planner->status();
            if (status == Planner::Status::Running) break;
            bool const stuck = status == Planner::Status::Stuck;
            bool const fixedPart = live->floor > 0;
            finish(stuck);
            drop();
            rescue = stuck;
            if (stuck && fixedPart) {
                // Plan again at once with nothing fixed (and a bigger budget this frame).
                start(now, 0);
                budget += budgetPerFrame.simTicks > 0 ? 2LL * budgetPerFrame.simTicks : 2LL * budgetPerFrame.steps;
            }
        }
        for (int i = 0; i < ticksPerFrame && out.outcome == Event::None; ++i) {
            int const t = real.tick();
            bool const want = t < planEnd && t < static_cast<int>(plan.size()) && plan[t];
            held = want;
            played.resize(t + 1, 0);
            played[t] = want;
            InputFn input = [want](int, Mode) { return want; };
            SampleFn sample = [](int, Sample const&) {};
            out.outcome = real.run(t + 1, input, sample).event;
        }
    }
    if (live) drop();
    return out;
}

char const* styleName(PlayStyle s) {
    switch (s) {
        case PlayStyle::Default: return "Default";
        case PlayStyle::Perfect: return "Perfect";
        case PlayStyle::Human: return "Human";
        case PlayStyle::Minimal: return "Minimal";
    }
    return "?";
}

} // namespace

int main() {
    int failures = 0;
    auto levels = makeLevels();
    PlayStyle const styles[] = { PlayStyle::Default, PlayStyle::Perfect, PlayStyle::Human, PlayStyle::Minimal };

    for (auto const& level : levels) {
        bool const expectStuck = level.name == "impossible wall";
        for (PlayStyle style : styles) {
            ToySim sim(level);
            auto start = std::chrono::steady_clock::now();
            Planner::Status status;
            {
                Planner planner(sim, PlannerConfig::forStyle(style, 1234), false);
                long steps = 0;
                while ((status = planner.step()) == Planner::Status::Running) {
                    if (++steps > 2000000) break;
                }

                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                int clicks = 0;
                auto const& bits = planner.inputs();
                for (size_t i = 1; i < bits.size(); ++i) clicks += (bits[i] && !bits[i - 1]) ? 1 : 0;

                std::printf("%-24s %-8s %-9s end=%5d best=%5d clicks=%3d evals=%5d decisions=%3d ticks=%7ld %.1fms\n",
                    level.name.c_str(), styleName(style),
                    status == Planner::Status::Completed ? "completed" : status == Planner::Status::Stuck ? "stuck" : "running",
                    planner.endTick(), planner.bestFrontier(), clicks, planner.evaluations(), planner.decisions(),
                    sim.ticks(), ms);

                if (expectStuck) {
                    if (status != Planner::Status::Stuck) {
                        std::printf("  FAIL: expected stuck\n");
                        failures++;
                    }
                }
                else if (status != Planner::Status::Completed) {
                    std::printf("  FAIL: expected completion\n");
                    failures++;
                }
                else {
                    Event ev = replay(level, planner.inputs(), planner.endTick());
                    if (ev != Event::Completed) {
                        std::printf("  FAIL: replay of the plan did not complete (%d)\n", static_cast<int>(ev));
                        failures++;
                    }
                    if (static_cast<int>(planner.inputs().size()) < planner.endTick() ||
                        static_cast<int>(planner.samples().size()) < planner.endTick()) {
                        std::printf("  FAIL: plan arrays shorter than end tick\n");
                        failures++;
                    }
                }
            }
            if (sim.liveSnapshots() != 0) {
                std::printf("  FAIL: %d snapshots leaked\n", sim.liveSnapshots());
                failures++;
            }
        }
    }

    // Robustness: plan with a simulator whose restores are slightly off, then replay exactly.
    for (auto const& level : levels) {
        if (level.name == "impossible wall") continue;
        ToySim sim(level, 0.05f);
        Planner planner(sim, PlannerConfig::forStyle(PlayStyle::Default, 99), false);
        while (planner.step() == Planner::Status::Running) {}
        bool ok = planner.status() == Planner::Status::Completed &&
            replay(level, planner.inputs(), planner.endTick()) == Event::Completed;
        std::printf("noisy restore %-24s %s\n", level.name.c_str(), ok ? "ok" : "FAILED");
        if (!ok) failures++;
    }

    // Live use: the "player" advances 4 ticks per frame while the planner stays about a
    // second ahead, with everything behind the player frozen. The inputs that were actually
    // played must still beat the level when replayed from scratch.
    for (auto const& level : levels) {
        if (level.name == "impossible wall") continue;
        for (PlayStyle style : styles) {
            ToySim sim(level);
            bool ok = true;
            int player = 0;
            std::vector<uint8_t> played;
            {
                Planner planner(sim, PlannerConfig::forStyle(style, 7), false);
                bool finished = false;
                while (!finished && player < 100000) {
                    planner.setFloor(player);
                    long steps = 0;
                    while (planner.status() == Planner::Status::Running && planner.frontier() - player < 240) {
                        planner.step();
                        if (++steps > 2000000) break;
                    }
                    if (planner.status() == Planner::Status::Stuck) { ok = false; break; }
                    for (int i = 0; i < 4; ++i) {
                        auto const& bits = planner.pathInputs();
                        bool want = player < planner.frontier() && player < static_cast<int>(bits.size()) && bits[player];
                        played.push_back(want ? 1 : 0);
                        player += 1;
                        if (planner.status() == Planner::Status::Completed && player >= planner.endTick()) {
                            finished = true;
                            break;
                        }
                    }
                }
                ok = ok && finished;
            }
            if (ok) ok = replay(level, played, player) == Event::Completed;
            if (sim.liveSnapshots() != 0) ok = false;
            std::printf("live %-24s %-8s %s\n", level.name.c_str(), styleName(style), ok ? "ok" : "FAILED");
            if (!ok) failures++;
        }
    }

    // Look-ahead pilot, exactly like the game loop: every 8 ticks plan ~200 ticks ahead
    // from the real state, keep the next 24 ticks fixed, splice the result into the plan.
    for (auto const& level : levels) {
        if (level.name == "impossible wall") continue;
        for (PlayStyle style : styles) {
            constexpr int kHorizon = 200;
            constexpr int kFloor = 12;
            constexpr int kEvery = 16;

            ToySim real(level);
            std::vector<uint8_t> plan;
            int planEnd = 0;
            int lastCheck = -1000000;
            long simTicks = 0;
            Event outcome = Event::None;

            for (int frame = 0; frame < 100000 && outcome == Event::None; ++frame) {
                int const t0 = real.tick();
                if (t0 - lastCheck >= kEvery || planEnd < t0 + kHorizon / 2) {
                    ToySim sim(level);
                    sim.copyStateFrom(real);
                    std::vector<uint8_t> seed;
                    for (int t = t0; t < planEnd && t < static_cast<int>(plan.size()); ++t) seed.push_back(plan[t]);
                    int const seedSize = static_cast<int>(seed.size());

                    PlannerConfig config = PlannerConfig::forStyle(style, 5);
                    config.goalTicks = kHorizon;
                    {
                        Planner planner(sim, config, real.held(), seed, kFloor);
                        while (planner.step() == Planner::Status::Running) {}
                        int end = planner.status() == Planner::Status::Completed ? planner.endTick() : planner.bestFrontier();
                        end = std::min<int>(end, static_cast<int>(planner.inputs().size()));
                        int const from = std::min(kFloor, seedSize);
                        if (static_cast<int>(plan.size()) < t0 + end) plan.resize(t0 + end, 0);
                        for (int rel = from; rel < end; ++rel) plan[t0 + rel] = planner.inputs()[rel];
                        if (end > from || t0 + end > planEnd) planEnd = std::max(t0 + end, std::min(planEnd, t0 + from));
                    }
                    simTicks += sim.ticks();
                    lastCheck = t0;
                    if (sim.liveSnapshots() != 0) { outcome = Event::Died; break; }
                }

                for (int i = 0; i < 4 && outcome == Event::None; ++i) {
                    int const t = real.tick();
                    bool const want = t < planEnd && t < static_cast<int>(plan.size()) && plan[t];
                    InputFn input = [want](int, Mode) { return want; };
                    SampleFn sample = [](int, Sample const&) {};
                    outcome = real.run(t + 1, input, sample).event;
                }
            }

            bool const ok = outcome == Event::Completed;
            std::printf("pilot %-24s %-8s %s (%ld sim ticks for %d real ticks)\n",
                level.name.c_str(), styleName(style), ok ? "ok" : "FAILED", simTicks, real.tick());
            if (!ok) failures++;
        }
    }

    // Stale plan: a check that starts from the previous plan with one press in the wrong
    // place. The search can only move that press earlier, so it may get stuck; the bot
    // then plans the same stretch again from scratch (only the fixed floor kept).
    for (auto const& level : levels) {
        if (level.name != "spikes" && level.name != "triple spike + stairs") continue;
        for (PlayStyle style : styles) {
            constexpr int kHorizon = 200;
            constexpr int kFloor = 12;
            int stuckFirst = 0;
            int failed = 0;
            for (int press = kFloor; press + 3 <= kHorizon; press += 3) {
                ToySim sim(level);
                std::vector<uint8_t> seed(kHorizon, 0);
                for (int t = press; t < press + 3; ++t) seed[t] = 1;

                PlannerConfig config = PlannerConfig::forStyle(style, 11);
                config.goalTicks = kHorizon;
                auto planner = std::make_unique<Planner>(sim, config, false, seed, kFloor);
                while (planner->step() == Planner::Status::Running) {}
                if (planner->status() == Planner::Status::Stuck) {
                    stuckFirst += 1;
                    sim.load(planner->startHandle());
                    planner.reset();
                    std::vector<uint8_t> floorOnly(seed.begin(), seed.begin() + kFloor);
                    planner = std::make_unique<Planner>(sim, config, false, floorOnly, kFloor);
                    while (planner->step() == Planner::Status::Running) {}
                }
                if (planner->status() != Planner::Status::Completed) failed += 1;
                planner.reset();
                if (sim.liveSnapshots() != 0) failed += 1;
            }
            std::printf("stale plan %-24s %-8s %s (%d of the seeds needed a fresh plan)\n",
                level.name.c_str(), styleName(style), failed == 0 ? "ok" : "FAILED", stuckFirst);
            if (failed) failures++;
        }
    }

    // Orb chains: the first orb alone makes things worse, the second one saves it.
    {
        std::vector<std::pair<char const*, ChainSim::Rules>> chains;
        chains.push_back({ "orb chain", ChainSim::Rules{} });
        ChainSim::Rules tight;
        tight.aFrom = 60;
        tight.aTo = 66;
        tight.bFrom = 30;
        tight.bTo = 36;
        tight.downDies = 112;
        tight.upDies = 44;
        chains.push_back({ "tight orb chain", tight });
        ChainSim::Rules dead;
        dead.bFrom = 50;
        dead.bTo = 60; // B only after being up already killed the player
        chains.push_back({ "orb chain, no way out", dead });
        ChainSim::Rules decoy;
        decoy.decoyFrom = 70;
        decoy.decoyTo = 110;
        chains.push_back({ "orb chain + decoy jumps", decoy });
        ChainSim::Rules ledge;
        ledge.cFrom = 10;
        ledge.cTo = 16;
        ledge.earlyCut = 3;
        chains.push_back({ "orb chain, early 2nd orb", ledge });

        for (auto const& [name, rules] : chains) {
            bool const solvable = rules.bFrom < rules.upDies;
            for (PlayStyle style : styles) {
                for (bool robust : { true, false }) {
                    ChainSim sim(rules);
                    bool ok = false;
                    {
                        PlannerConfig config = PlannerConfig::forStyle(style, 21);
                        config.robust = robust;
                        Planner planner(sim, config, false);
                        long steps = 0;
                        while (planner.step() == Planner::Status::Running) {
                            if (++steps > 2000000) break;
                        }
                        if (solvable) {
                            ok = planner.status() == Planner::Status::Completed &&
                                replayChain(rules, planner.inputs()) == Event::Completed;
                        }
                        else {
                            ok = planner.status() == Planner::Status::Stuck;
                        }
                    }
                    if (sim.liveSnapshots() != 0) ok = false;
                    std::printf("%-24s %-8s %-6s %s\n", name, styleName(style), robust ? "robust" : "exact", ok ? "ok" : "FAILED");
                    if (!ok && std::getenv("AC_TRACE")) {
                        ChainSim again(rules);
                        PlannerConfig config = PlannerConfig::forStyle(style, 21);
                        config.robust = robust;
                        Planner planner(again, config, false);
                        while (planner.step() == Planner::Status::Running) {}
                        for (auto const& e : planner.trace()) {
                            std::printf(" %d:%d,%d", static_cast<int>(e.kind), e.a, e.b);
                        }
                        std::printf("\n");
                    }
                    if (!ok) failures++;
                }
            }
        }
    }

    // The same chains through the bot's loop: every 16 ticks plan 200 ahead from the real
    // state with the next 12 fixed, following the previous plan first, then from scratch,
    // then with exact timings.
    {
        std::vector<std::pair<char const*, ChainSim::Rules>> chains;
        chains.push_back({ "orb chain", ChainSim::Rules{} });
        ChainSim::Rules late;
        late.aFrom = 230;
        late.aTo = 242;
        late.downDies = 300;
        late.end = 700;
        chains.push_back({ "late orb chain", late });
        ChainSim::Rules decoy;
        decoy.decoyFrom = 70;
        decoy.decoyTo = 110;
        chains.push_back({ "orb chain + decoy jumps", decoy });
        ChainSim::Rules ledge;
        ledge.cFrom = 10;
        ledge.cTo = 16;
        ledge.earlyCut = 3;
        chains.push_back({ "orb chain, early 2nd orb", ledge });
        for (auto const& [name, rules] : chains) {
            for (PlayStyle style : styles) {
                constexpr int kHorizon = 200;
                constexpr int kFloor = 12;
                constexpr int kEvery = 16;
                ChainSim real(rules);
                std::vector<uint8_t> plan;
                int planEnd = 0;
                int lastCheck = -1000000;
                bool held = false;
                Event outcome = Event::None;
                bool leaked = false;
                while (outcome == Event::None && real.tick() < rules.end + 50) {
                    int const t0 = real.tick();
                    if (t0 - lastCheck >= kEvery || planEnd < t0 + kHorizon / 2) {
                        std::vector<uint8_t> seed;
                        for (int t = t0; t < planEnd && t < static_cast<int>(plan.size()); ++t) seed.push_back(plan[t]);
                        for (int attempt = 0; attempt < 3; ++attempt) {
                            ChainSim sim(rules);
                            // Bring the copy to the real state by replaying what was played.
                            InputFn replayInput = [&](int t, Mode) { return t < static_cast<int>(plan.size()) && plan[t]; };
                            SampleFn none = [](int, Sample const&) {};
                            if (t0 > 0) sim.run(t0, replayInput, none);
                            PlannerConfig config = PlannerConfig::forStyle(style, 5 + attempt);
                            config.goalTicks = t0 + kHorizon;
                            config.robust = attempt < 2;
                            std::vector<uint8_t> useSeed(seed.begin(), seed.begin() + std::min<size_t>(seed.size(), attempt == 0 ? seed.size() : kFloor));
                            // The planner works in absolute ticks here; pad the seed to the start.
                            std::vector<uint8_t> full(plan.begin(), plan.begin() + std::min<size_t>(plan.size(), t0));
                            full.resize(t0, 0);
                            full.insert(full.end(), useSeed.begin(), useSeed.end());
                            bool stuck = false;
                            {
                                Planner planner(sim, config, held, full, t0 + kFloor);
                                while (planner.step() == Planner::Status::Running) {}
                                stuck = planner.status() == Planner::Status::Stuck;
                                if (!stuck || attempt == 2) {
                                    int end = stuck ? planner.bestFrontier() : planner.endTick();
                                    end = std::min<int>(end, static_cast<int>(planner.inputs().size()));
                                    if (static_cast<int>(plan.size()) < end) plan.resize(end, 0);
                                    for (int t = t0 + kFloor; t < end; ++t) plan[t] = planner.inputs()[t];
                                    planEnd = std::max(planEnd, end);
                                }
                            }
                            if (sim.liveSnapshots() != 0) leaked = true;
                            if (!stuck) break;
                        }
                        lastCheck = t0;
                    }
                    for (int i = 0; i < 4 && outcome == Event::None; ++i) {
                        int const t = real.tick();
                        bool const want = t < planEnd && t < static_cast<int>(plan.size()) && plan[t];
                        held = want;
                        InputFn input = [want](int, Mode) { return want; };
                        SampleFn sample = [](int, Sample const&) {};
                        outcome = real.run(t + 1, input, sample).event;
                    }
                }
                bool const ok = outcome == Event::Completed && !leaked;
                std::printf("pilot %-24s %-8s %s\n", name, styleName(style), ok ? "ok" : "FAILED");
                if (!ok) failures++;
            }
        }
    }

    // The chains again, with the planner sliced like in the game: only a few planner steps
    // per frame, so a search takes many frames (see runSliced).
    {
        // (Not at the very start of a level: before the first plan exists nothing can carry
        // a search over several frames.)
        std::vector<std::pair<char const*, ChainSim::Rules>> chains;
        ChainSim::Rules late;
        late.aFrom = 230;
        late.aTo = 242;
        late.downDies = 300;
        late.end = 700;
        chains.push_back({ "late orb chain", late });
        ChainSim::Rules ledge;
        ledge.aFrom = 150;
        ledge.aTo = 162;
        ledge.downDies = 230;
        ledge.end = 700;
        ledge.cFrom = 10;
        ledge.cTo = 16;
        ledge.earlyCut = 3;
        chains.push_back({ "late chain, early 2nd orb", ledge });
        for (auto const& [name, rules] : chains) {
            for (PlayStyle style : styles) {
                for (int stepsPerFrame : { 4, 8, 16 }) {
                    SlicedRun const run = runSliced(rules, style, FrameBudget{ stepsPerFrame, 0 }, 4);
                    bool const ok = run.outcome == Event::Completed && !run.leaked && run.desyncs == 0;
                    std::printf(
                        "sliced %-26s %-8s %2d steps/frame %s (%d extended, %d committed, %d restarted, %d desynced)\n",
                        name, styleName(style), stepsPerFrame, ok ? "ok" : "FAILED", run.extended, run.committed,
                        run.restarted, run.desyncs
                    );
                    if (!ok) failures++;
                }
            }
        }
    }

    // Random orb chains, frame rates and budgets: whatever happens, a search must never
    // plan from a state the game isn't in (its path before now is what was played) and
    // must release its snapshots. Nearly every level the planner beats with time to spare
    // must be beaten sliced too (a few, like a one tick orb behind a decoy, need more time
    // than the game gives).
    {
        std::mt19937 rng(20261015);
        auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        int runs = 0;
        int completed = 0;
        int solvable = 0;
        int solvedSliced = 0;
        int desynced = 0;
        int leaks = 0;
        int commits = 0;
        int restarts = 0;
        for (int level = 0; level < 150; ++level) {
            ChainSim::Rules r;
            r.aFrom = pick(60, 300);
            r.aTo = r.aFrom + pick(4, 16);
            r.bFrom = pick(8, 30);
            r.bTo = r.bFrom + pick(4, 14);
            r.downDies = r.aTo + pick(10, 120);
            r.upDies = r.bTo + pick(5, 40);
            r.end = r.downDies + 300;
            if (pick(0, 1)) {
                r.decoyFrom = r.aFrom - pick(0, 40);
                r.decoyTo = r.decoyFrom + pick(2, 10);
                r.decoyGain = pick(2, 20);
            }
            if (pick(0, 1)) {
                r.cFrom = pick(4, 20);
                r.cTo = r.cFrom + pick(3, 10);
                r.earlyCut = pick(1, r.bTo - r.bFrom);
            }
            for (PlayStyle style : styles) {
                // With all the time it needs (and nothing fixed) can it be beaten at all?
                bool offline = false;
                {
                    ChainSim sim(r);
                    PlannerConfig config = PlannerConfig::forStyle(style, 5);
                    config.robust = true;
                    Planner planner(sim, config, false);
                    while (planner.step() == Planner::Status::Running) {}
                    offline = planner.status() == Planner::Status::Completed &&
                        replayChain(r, planner.inputs()) == Event::Completed;
                }
                // (simulated ticks per frame, physics steps per frame): 144 and 60 FPS with
                // about what the bot simulates in its 4 ms on a heavy level, then starved.
                for (auto [simTicks, ticksPerFrame] : { std::pair{ 300, 2 }, std::pair{ 300, 4 }, std::pair{ 150, 1 },
                                                        std::pair{ 60, 4 } }) {
                    SlicedRun const run = runSliced(r, style, FrameBudget{ 0, simTicks }, ticksPerFrame);
                    runs += 1;
                    commits += run.committed;
                    restarts += run.restarted;
                    if (run.outcome == Event::Completed) completed += 1;
                    if (run.desyncs) desynced += 1;
                    if (run.leaked) leaks += 1;
                    if (offline && simTicks >= 75 * ticksPerFrame) {
                        solvable += 1;
                        if (run.outcome == Event::Completed) solvedSliced += 1;
                    }
                }
            }
        }
        bool const ok = desynced == 0 && leaks == 0 && solvedSliced * 200 >= solvable * 199;
        std::printf(
            "sliced random: %d runs, %d completed; %d/%d beatable ones beaten; %d desynced, %d leaked; "
            "%d commits, %d restarts %s\n",
            runs, completed, solvedSliced, solvable, desynced, leaks, commits, restarts, ok ? "ok" : "FAILED"
        );
        if (!ok) failures++;
    }

    std::printf(failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
