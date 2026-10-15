// End-to-end test of the bot's planning loop on a physics model matched to Geometry
// Dash 2.2081 measurements (the mod's in-game self test and death logs):
//   - 240 ticks per second, normal speed moves 1.298 units per tick
//   - a cube jump rises ~64 units and stays in the air ~93 ticks
//   - the player is 30x30; a ground spike kills inside a 6x12 box around its centre
//
// The loop below mirrors Bot.cpp: every 16 ticks (or when the plan runs short) a check
// plans 200 ticks ahead from the real state, keeps the next 12 ticks fixed, reuses the
// previous plan as a seed and plans again from scratch if that gets stuck. The "real"
// game can apply each press or release a tick late, like an imperfect input path.
//
// Build & run:  c++ -std=c++20 -O2 -I src tests/gd_pilot_test.cpp src/planner/Planner.cpp -o gd_pilot_test && ./gd_pilot_test

#include "planner/Planner.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace ac;

namespace {

constexpr float kSpeed = 1.298f;      // units per tick at normal speed
constexpr float kJump = 2.795f;       // 11.18 * 0.25
constexpr float kGravity = 0.0599f;   // 0.958 * 0.25 * 0.25
constexpr float kMaxFall = 3.75f;
constexpr float kHalf = 15.f;         // the player is 30x30 around (x, y + 15)
constexpr float kOrbJump = 2.6f;

enum class Kind { Spike, Block, Orb };

struct Object {
    Kind kind;
    float x;       // centre
    float y;       // bottom
    float w = 30.f;
    float h = 30.f;
};

struct Level {
    std::string name;
    std::vector<Object> objects;
    float end = 0.f;
};

struct State {
    int tick = 0;
    float x = 0.f;     // centre
    float y = 0.f;     // bottom, ground at 0
    float vy = 0.f;
    bool onGround = true;
    bool held = false;
    bool buffered = false; // fresh press not yet used by an orb
    std::vector<uint8_t> usedOrbs;
};

class GdSim final : public Simulator {
public:
    explicit GdSim(Level const& level) : m_level(level) {
        m_state.usedOrbs.assign(level.objects.size(), 0);
    }

    int tick() const override { return m_state.tick; }

    int save() override {
        m_snaps.push_back(m_state);
        m_live.push_back(true);
        return static_cast<int>(m_snaps.size() - 1);
    }

    void load(int handle) override {
        if (handle < 0 || handle >= static_cast<int>(m_snaps.size()) || !m_live[handle]) {
            std::printf("FATAL: bad load %d\n", handle);
            std::abort();
        }
        m_state = m_snaps[handle];
    }

    void release(int handle) override {
        if (handle < 0 || handle >= static_cast<int>(m_snaps.size()) || !m_live[handle]) {
            std::printf("FATAL: bad release %d\n", handle);
            std::abort();
        }
        m_live[handle] = false;
    }

    RunResult run(int untilTick, InputFn const& input, SampleFn const& sample) override {
        RunResult result;
        while (m_state.tick < untilTick) {
            int const t = m_state.tick;
            bool const held = input(t, Mode::Cube);
            bool ring = false;
            Event const ev = this->step(held, ring);
            m_ticks += 1;
            Sample s;
            s.x = m_state.x;
            s.y = m_state.y;
            s.hold = held;
            s.mode = Mode::Cube;
            s.grounded = m_state.onGround;
            s.ring = ring;
            sample(t, s);
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
        mix(static_cast<uint64_t>(m_state.tick));
        mix(q(m_state.x));
        mix(q(m_state.y));
        mix(q(m_state.vy * 16.f));
        mix((m_state.onGround ? 1u : 0u) | (m_state.held ? 2u : 0u) | (m_state.buffered ? 4u : 0u));
        for (uint8_t used : m_state.usedOrbs) mix(used);
        return hash == 0 ? 1 : hash;
    }

    void copyStateFrom(GdSim const& other) {
        m_state = other.m_state;
        m_state.tick = 0;
    }

    State const& state() const { return m_state; }
    int liveSnapshots() const {
        int n = 0;
        for (bool b : m_live) n += b ? 1 : 0;
        return n;
    }
    long ticks() const { return m_ticks; }

    // One tick, same order as GD: input, update (jump / gravity / move), collisions.
    Event step(bool held, bool& ring) {
        State& s = m_state;
        if (held && !s.held) s.buffered = true;
        if (!held) s.buffered = false;
        s.held = held;

        // Orbs: a fresh press while overlapping.
        for (size_t i = 0; i < m_level.objects.size(); ++i) {
            auto const& o = m_level.objects[i];
            if (o.kind != Kind::Orb || s.usedOrbs[i]) continue;
            if (!this->overlaps(o, s.x, s.y)) continue;
            ring = true;
            if (s.buffered) {
                s.usedOrbs[i] = 1;
                s.vy = kOrbJump;
                s.onGround = false;
                s.buffered = false;
            }
        }

        if (s.onGround && held) {
            s.vy = kJump;
            s.onGround = false;
            s.buffered = false;
        }
        else if (!s.onGround) {
            s.vy = std::max(s.vy - kGravity, -kMaxFall);
        }

        float const oldY = s.y;
        s.x += kSpeed;
        s.y += s.vy;
        s.onGround = false;

        if (s.y <= 0.f) {
            s.y = 0.f;
            s.vy = 0.f;
            s.onGround = true;
        }
        for (size_t i = 0; i < m_level.objects.size(); ++i) {
            auto const& o = m_level.objects[i];
            if (o.kind == Kind::Spike) {
                // 6x12 hazard box around the spike's centre (bottom 9..21).
                bool const hit = std::abs(s.x - o.x) < kHalf + 3.f && s.y < o.y + 21.f && s.y + 30.f > o.y + 9.f;
                if (hit) return Event::Died;
            }
            else if (o.kind == Kind::Block && this->overlaps(o, s.x, s.y)) {
                float const top = o.y + o.h;
                if (oldY >= top - 4.f && s.vy <= 0.f) {
                    s.y = top;
                    s.vy = 0.f;
                    s.onGround = true;
                }
                else {
                    return Event::Died;
                }
            }
        }
        // Still standing on a block after moving?
        if (!s.onGround && s.vy <= 0.f) {
            for (auto const& o : m_level.objects) {
                if (o.kind != Kind::Block) continue;
                float const top = o.y + o.h;
                if (std::abs(s.x - o.x) < kHalf + o.w / 2.f && std::abs(s.y - top) < 0.01f) {
                    s.onGround = true;
                    s.vy = 0.f;
                }
            }
        }

        s.tick += 1;
        if (s.x >= m_level.end) return Event::Completed;
        return Event::None;
    }

private:
    bool overlaps(Object const& o, float x, float y) const {
        return std::abs(x - o.x) < kHalf + o.w / 2.f && y < o.y + o.h && y + 30.f > o.y;
    }

    Level const& m_level;
    State m_state;
    std::vector<State> m_snaps;
    std::vector<bool> m_live;
    long m_ticks = 0;
};

Object spike(float x, float y = 0.f) { return Object{ Kind::Spike, x, y }; }
Object block(float x, float y, float w = 30.f, float h = 30.f) { return Object{ Kind::Block, x, y, w, h }; }
Object orb(float x, float y) { return Object{ Kind::Orb, x, y, 36.f, 36.f }; }

std::vector<Level> makeLevels() {
    std::vector<Level> levels;
    {
        // The start of Stereo Madness as seen in the mod's logs: a spike at x=525, a
        // double spike at 705/735, then blocks and more spikes.
        Level l; l.name = "stereo start";
        l.objects.push_back(spike(525.f));
        l.objects.push_back(spike(705.f));
        l.objects.push_back(spike(735.f));
        l.objects.push_back(block(1005.f, 0.f));
        l.objects.push_back(spike(1035.f));
        l.objects.push_back(block(1245.f, 0.f, 90.f, 30.f));
        l.objects.push_back(block(1365.f, 0.f, 30.f, 60.f));
        l.objects.push_back(spike(1530.f));
        l.objects.push_back(spike(1560.f));
        l.objects.push_back(spike(1590.f));
        l.objects.push_back(block(1800.f, 0.f, 150.f, 30.f));
        l.objects.push_back(spike(1800.f, 30.f));
        l.objects.push_back(spike(2100.f));
        l.objects.push_back(spike(2130.f));
        l.end = 2500.f;
        levels.push_back(l);
    }
    {
        Level l; l.name = "orb gap";
        for (float x = 600.f; x < 780.f; x += 30.f) l.objects.push_back(spike(x));
        l.objects.push_back(orb(675.f, 40.f));
        l.objects.push_back(spike(1100.f));
        l.end = 1400.f;
        levels.push_back(l);
    }
    {
        // Irregular single/double/triple spikes and block hops (~1 minute).
        Level l; l.name = "generated";
        uint32_t rng = 777;
        auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        float x = 400.f;
        while (x < 16000.f) {
            switch (next() % 5) {
                case 0: l.objects.push_back(spike(x)); x += 200.f + next() % 160; break;
                case 1: l.objects.push_back(spike(x)); l.objects.push_back(spike(x + 30.f)); x += 240.f + next() % 160; break;
                case 2: for (int i = 0; i < 3; ++i) l.objects.push_back(spike(x + 30.f * i)); x += 280.f + next() % 160; break;
                case 3: l.objects.push_back(block(x, 0.f)); l.objects.push_back(spike(x + 30.f)); x += 260.f + next() % 160; break;
                case 4: l.objects.push_back(block(x, 0.f, 90.f, 30.f)); x += 220.f + next() % 160; break;
            }
        }
        l.end = x + 300.f;
        levels.push_back(l);
    }
    return levels;
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

struct PilotResult {
    Event outcome = Event::None;
    int ticks = 0;
    int stuckChecks = 0;
    int retries = 0;
    int clicks = 0;
    int airPresses = 0; // presses applied while airborne and away from orbs
};

// Mirrors Bot::onFrame/startCheck/finishCheck/retryCheck with 4 ticks per frame.
PilotResult pilot(Level const& level, PlayStyle style, int jitterPercent, uint32_t seedValue) {
    constexpr int kHorizon = 200;
    constexpr int kFloor = 12;
    constexpr int kEvery = 16;
    constexpr float kTolerance = 2.f;

    PilotResult r;
    GdSim real(level);
    std::vector<uint8_t> plan;
    std::vector<Sample> samples;
    int planEnd = 0;
    int lastCheck = -1000000;
    bool force = false;
    bool realHold = false;
    uint32_t rng = seedValue;
    auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % 100u; };
    int pendingTick = -1;
    bool pendingState = false;

    for (int frame = 0; frame < 200000 && r.outcome == Event::None; ++frame) {
        int const t0 = real.tick();
        if (force || t0 - lastCheck >= kEvery || planEnd < t0 + kHorizon / 2) {
            GdSim sim(level);
            sim.copyStateFrom(real);
            std::vector<uint8_t> seed;
            for (int t = t0; t < planEnd && t < static_cast<int>(plan.size()) && t < t0 + kHorizon; ++t) seed.push_back(plan[t]);
            int const seedSize = static_cast<int>(seed.size());
            bool seeded = false;
            for (int t = std::min(kFloor, seedSize); t < seedSize; ++t) {
                bool const prev = t == 0 ? realHold : seed[t - 1] != 0;
                if ((seed[t] != 0) != prev) { seeded = true; break; }
            }

            PlannerConfig config = PlannerConfig::forStyle(style, seedValue ^ static_cast<uint32_t>(t0));
            config.goalTicks = kHorizon;
            config.robust = true;
            auto planner = std::make_unique<Planner>(sim, config, realHold, seed, kFloor);
            while (planner->step() == Planner::Status::Running) {}
            bool retried = false;
            for (;;) {
                if (planner->status() != Planner::Status::Stuck) break;
                bool const freshPlan = seeded && !retried;
                bool const exactPlan = !freshPlan && config.robust;
                if (!freshPlan && !exactPlan) break;
                r.retries += 1;
                sim.load(planner->startHandle());
                planner.reset();
                std::vector<uint8_t> floorOnly(seed.begin(), seed.begin() + std::min(kFloor, seedSize));
                config.robust = !exactPlan;
                planner = std::make_unique<Planner>(sim, config, realHold, floorOnly, kFloor);
                retried = true;
                while (planner->step() == Planner::Status::Running) {}
            }
            if (planner->status() == Planner::Status::Stuck) r.stuckChecks += 1;
            if (char const* from = std::getenv("GD_TRACE_FROM")) {
                int const lo = std::atoi(from);
                if (t0 >= lo && t0 <= lo + 150) {
                    std::printf("    check t0=%d seeded=%d status=%s robust=%d presses:", t0, seeded ? 1 : 0,
                        planner->status() == Planner::Status::Completed ? "done" : "stuck", config.robust ? 1 : 0);
                    auto const& bits = planner->inputs();
                    for (size_t k = 1; k < bits.size(); ++k) if (bits[k] && !bits[k - 1]) std::printf(" %d", t0 + static_cast<int>(k));
                    std::printf("\n");
                    using Kind = Planner::TraceEntry::Kind;
                    std::printf("      trace:");
                    for (auto const& e : planner->trace()) {
                        switch (e.kind) {
                            case Kind::Fix: std::printf(" | D%d lo%d:", t0 + e.a, t0 + e.b); break;
                            case Kind::Eval: std::printf(" %d:%d", t0 + e.a, e.b); break;
                            case Kind::Choose: std::printf(" ->%d", t0 + e.a); break;
                            case Kind::Explore: std::printf(" try%d", t0 + e.a); break;
                            case Kind::Backtrack: std::printf(" undo%d", t0 + e.a); break;
                            case Kind::GiveUp: std::printf(" giveup"); break;
                            case Kind::Deep: std::printf(" | deep %d..%d", t0 + e.a, t0 + e.b); break;
                            case Kind::DeepFound: std::printf(" found%d(%d)", t0 + e.a, e.b); break;
                            case Kind::DeepFailed: std::printf(" none(%d)", e.b); break;
                        }
                    }
                    std::printf("\n");
                }
            }

            int end = planner->status() == Planner::Status::Completed ? planner->endTick() : planner->bestFrontier();
            end = std::min<int>(end, static_cast<int>(std::min(planner->inputs().size(), planner->samples().size())));
            int const from = std::min(kFloor, seedSize);
            if (static_cast<int>(plan.size()) < t0 + end) {
                plan.resize(t0 + end, 0);
                samples.resize(t0 + end);
            }
            for (int rel = 0; rel < end; ++rel) {
                samples[t0 + rel] = planner->samples()[rel];
                if (rel >= from) plan[t0 + rel] = planner->inputs()[rel];
            }
            if (end > from || t0 + end > planEnd) planEnd = t0 + std::max(end, from);
            planner.reset();
            if (std::getenv("GD_COST")) {
                static long worst = 0, sum = 0, n = 0;
                worst = std::max(worst, sim.ticks());
                sum += sim.ticks();
                n += 1;
                if (n % 200 == 0) std::printf("    cost: %ld checks, avg %ld sim ticks, worst %ld\n", n, sum / n, worst);
            }
            if (sim.liveSnapshots() != 0) {
                std::printf("  FAIL: leaked snapshots\n");
                r.outcome = Event::Died;
                break;
            }
            lastCheck = t0;
            force = false;
        }

        for (int i = 0; i < 4 && r.outcome == Event::None; ++i) {
            int const t = real.tick();
            bool const want = t < planEnd && t < static_cast<int>(plan.size()) && plan[t];
            // The real input path may apply a change one tick late.
            bool apply = realHold;
            if (pendingTick >= 0 && t >= pendingTick) {
                apply = pendingState;
                pendingTick = -1;
            }
            if (want != realHold && pendingTick < 0) {
                if (static_cast<int>(next()) < jitterPercent) {
                    pendingTick = t + 1;
                    pendingState = want;
                }
                else {
                    apply = want;
                }
            }
            bool const pressed = apply && !realHold;
            bool const wasGrounded = real.state().onGround;
            realHold = apply;

            bool ring = false;
            Event const ev = real.step(apply, ring);
            if (pressed && std::getenv("GD_TRACE")) {
                std::printf("    press applied at tick %d (planned %d) x=%.1f y=%.1f grounded=%d\n", t,
                    want ? t : -1, real.state().x, real.state().y, wasGrounded ? 1 : 0);
            }
            if (ev != Event::None && std::getenv("GD_TRACE")) {
                std::printf("    END %s at tick %d x=%.1f y=%.1f\n", ev == Event::Died ? "died" : "done", t, real.state().x, real.state().y);
                for (auto const& o : level.objects) {
                    if (std::abs(o.x - real.state().x) < 120.f) {
                        std::printf("      obj %s at x=%.0f y=%.0f w=%.0f h=%.0f\n", o.kind == Kind::Spike ? "spike" : o.kind == Kind::Block ? "block" : "orb", o.x, o.y, o.w, o.h);
                    }
                }
                for (int k = std::max(0, t - 140); k <= t && k < static_cast<int>(plan.size()); ++k) {
                    if (plan[k] && (k == 0 || !plan[k - 1])) std::printf("      planned press at %d\n", k);
                }
            }
            if (pressed) {
                r.clicks += 1;
                if (!wasGrounded && !ring) r.airPresses += 1;
            }
            if (ev != Event::None) {
                r.outcome = ev;
                break;
            }
            int const done = real.tick() - 1;
            if (done < planEnd && done < static_cast<int>(samples.size())) {
                auto const& e = samples[done];
                if (std::abs(e.x - real.state().x) > kTolerance || std::abs(e.y - real.state().y) > kTolerance) force = true;
            }
        }
        r.ticks = real.tick();
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5) {
        // gd_pilot_test <level index> <style 0-3> <jitter> <seed>: trace one run
        auto levels = makeLevels();
        auto const& level = levels.at(std::atoi(argv[1]));
        PilotResult const r = pilot(level, static_cast<PlayStyle>(std::atoi(argv[2])), std::atoi(argv[3]), static_cast<uint32_t>(std::atoi(argv[4])));
        std::printf("%s: %s at tick %d\n", level.name.c_str(), r.outcome == Event::Completed ? "completed" : "died", r.ticks);
        return 0;
    }
    int failures = 0;
    auto levels = makeLevels();
    PlayStyle const styles[] = { PlayStyle::Default, PlayStyle::Perfect, PlayStyle::Human, PlayStyle::Minimal };
    int const jitters[] = { 0, 30 };

    for (auto const& level : levels) {
        for (PlayStyle style : styles) {
            for (int jitter : jitters) {
                for (uint32_t seed : { 1u, 2u, 3u }) {
                    PilotResult const r = pilot(level, style, jitter, seed);
                    bool const ok = r.outcome == Event::Completed;
                    std::printf("%-14s %-8s jitter %2d%% seed %u: %-9s tick %6d clicks %3d air presses %2d stuck %2d retries %2d\n",
                        level.name.c_str(), styleName(style), jitter, seed, ok ? "completed" : "DIED",
                        r.ticks, r.clicks, r.airPresses, r.stuckChecks, r.retries);
                    if (!ok) failures += 1;
                    // With a perfect input path the bot must never press uselessly in the
                    // air; with a delayed one the real player can lag behind the plan.
                    if (jitter == 0 && r.airPresses > 0) {
                        std::printf("  FAIL: pressed while airborne away from orbs\n");
                        failures += 1;
                    }
                }
            }
        }
    }

    std::printf(failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
