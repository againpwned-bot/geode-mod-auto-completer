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
            sample(t, Sample{ m_state.x, m_state.y, held, mode });
            if (ev != Event::None) {
                result.event = ev;
                result.eventTick = t;
                break;
            }
        }
        result.endTick = m_state.tick;
        return result;
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

    std::printf(failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
