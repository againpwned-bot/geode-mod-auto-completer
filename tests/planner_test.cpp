// Native test for the planner, using a small deterministic platformer that loosely
// mimics Geometry Dash (cube, ship, spikes, blocks, yellow orbs, ship portals).
//
// Build & run:  c++ -std=c++20 -O2 -I src tests/planner_test.cpp src/planner/Planner.cpp -o planner_test && ./planner_test

#include "planner/Planner.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
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

    std::printf(failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
