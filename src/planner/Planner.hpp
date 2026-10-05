#pragma once

// Path planner for the auto completer.
//
// This file has no Geode or cocos dependencies on purpose: the planner only talks to
// the game through the `Simulator` interface, so it can be unit tested natively
// (see tests/) against a toy physics model.
//
// How it works, in short:
//   1. Run the level forward with a "default" input policy (keep holding in ship/wave,
//      short taps everywhere else) and keep a snapshot every few ticks.
//   2. When the player dies at tick `td`, look back from `td` for ticks where flipping the
//      input avoids the death and survives a bit past it. The valid ticks form a window.
//   3. Pick a tick inside that window according to the play style (centre of the window
//      gives the most timing margin, Human adds jitter, Minimal takes the latest tick).
//   4. If nothing in the window works, backtrack to the previous decision and try its
//      next best alternative (depth first search with a work budget).

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace ac {

enum class Mode : uint8_t { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

// Game modes where the natural "do nothing" action is to keep the current button state.
inline bool isHoldMode(Mode mode) {
    return mode == Mode::Ship || mode == Mode::Wave;
}

enum class Event : uint8_t { None, Died, Completed };

enum class PlayStyle : uint8_t { Default, Perfect, Human, Minimal };

struct Sample {
    float x = 0.f;
    float y = 0.f;
    bool hold = false;
    Mode mode = Mode::Cube;
};

// input(tick, mode) -> whether the jump button should be held during that tick
using InputFn = std::function<bool(int, Mode)>;
// sample(tick, sample) is called after every executed tick
using SampleFn = std::function<void(int, Sample const&)>;

struct RunResult {
    Event event = Event::None;
    int eventTick = -1; // tick during which the event happened
    int endTick = 0;    // the simulator is now positioned before this tick
};

class Simulator {
public:
    virtual ~Simulator() = default;
    // Tick that will be executed next, relative to the start of planning.
    virtual int tick() const = 0;
    // Snapshot the current state. Returns a handle, or -1 if snapshots are unavailable.
    virtual int save() = 0;
    // Restore a snapshot. Afterwards tick() returns the tick the snapshot was taken at.
    virtual void load(int handle) = 0;
    virtual void release(int handle) = 0;
    // Execute ticks until tick() >= untilTick or an event happens. May overshoot untilTick
    // when the underlying engine advances several ticks at once.
    virtual RunResult run(int untilTick, InputFn const& input, SampleFn const& sample) = 0;
};

struct PlannerConfig {
    PlayStyle style = PlayStyle::Default;
    int snapshotInterval = 16; // ticks between snapshots along the planned path
    int maxLookback = 180;     // how far before a death we look for a fix
    int horizonMin = 4;        // a fix must survive at least this many ticks past the old death
    int horizonMax = 60;       // survival is measured up to this many ticks past the old death
    int scanStride = 2;        // tick step while scanning for a fix
    int windowCap = 24;        // how far the scan continues past the first working tick
    int tapLen = 5;            // ticks a tap is held in tap based modes
    int tapLenJitter = 0;      // +- random ticks added to tapLen (Human)
    double timingJitter = 0.0; // stddev of the chosen tick, as a fraction of the window
    int lockTicks = 2400;      // decisions further back than this are never revisited
    int regionBudget = 4000;   // fix evaluations allowed without progress before giving up
    uint32_t seed = 0;

    static PlannerConfig forStyle(PlayStyle style, uint32_t seed);
};

class Planner {
public:
    enum class Status { Running, Completed, Stuck };

    // The simulator must be positioned at the state planning should start from.
    Planner(Simulator& sim, PlannerConfig config, bool initialHold);
    ~Planner();

    Planner(Planner const&) = delete;
    Planner& operator=(Planner const&) = delete;

    // Performs one bounded unit of work (one simulation chunk or one fix evaluation).
    Status step();

    Status status() const { return m_status; }
    // Number of ticks from the start that are planned and verified.
    int frontier() const { return m_frontier; }
    int bestFrontier() const { return m_maxFrontier; }
    // When completed, the tick the level ended on. When stuck, the tick planning gave up at.
    int endTick() const { return m_endTick; }
    int evaluations() const { return m_totalEvals; }
    int decisions() const { return static_cast<int>(m_plan.size()); }

    // Input bits and player samples per tick along the furthest verified path.
    // Valid for ticks < bestFrontier() (== endTick() once completed).
    std::vector<uint8_t> const& inputs() const { return m_bestBits; }
    std::vector<Sample> const& samples() const { return m_bestSamples; }

    // Snapshot handle of the starting state; owned by the planner until it is destroyed.
    int startHandle() const;

private:
    struct Decision {
        bool state = false;
        int tapLen = 0;
    };

    struct Policy {
        bool held = false;
        int pressTick = -1000000;
        int tapLen = 0;
    };

    struct Snap {
        int handle = -1;
        Policy policy;
    };

    struct Candidate {
        int tick = 0;
        Decision decision;
        int survival = 0;
    };

    struct StackEntry {
        int tick = 0;
        Decision decision;
        std::vector<Candidate> alternatives; // best first
        // Where the fix scan stopped, so it can be resumed past the window if every
        // alternative inside the window turns out to be a dead end.
        int deathTick = 0;
        int lo = 0;
        int scanNext = -1;
    };

    // State of an in-progress fix search (spread over several step() calls).
    struct FixSearch {
        bool active = false;
        int deathTick = 0;
        int lo = 0;
        int next = 0;          // next candidate tick to evaluate
        int firstValid = -1;
        int invalidStreak = 0;
        std::vector<Candidate> valid;
    };

    bool policyInput(Policy& policy, int tick, Mode mode, Decision const* extra, int extraTick) const;
    Decision makeDecision(bool state);

    void advance();
    void beginFix(int deathTick);
    void stepFix();
    void finishFix();
    void backtrack();
    int evaluate(int tick, Decision const& decision, int deathTick);

    void applyDecision(int tick, Decision const& decision);
    void invalidateAfter(int tick);
    int snapAtOrBefore(int tick) const;
    void loadSnap(int tick);
    void pushSnap();
    void prune();
    void recordBit(int tick, bool held);
    void recordSample(int tick, Sample const& sample);
    int lockTick() const;
    void giveUp(int tick);
    void markDirty(int tick);
    void syncBest();

    Simulator& m_sim;
    PlannerConfig m_cfg;
    std::mt19937 m_rng;

    std::map<int, Decision> m_plan;
    std::vector<StackEntry> m_stack;
    std::map<int, Snap> m_snaps;
    std::vector<uint8_t> m_bits;
    std::vector<Sample> m_samples;
    std::vector<uint8_t> m_bestBits;
    std::vector<Sample> m_bestSamples;
    int m_bestEnd = 0;
    int m_dirtyFrom = 0;

    Status m_status = Status::Running;
    int m_frontier = 0;
    int m_maxFrontier = 0;
    int m_endTick = 0;
    int m_regionEvals = 0;
    int m_totalEvals = 0;
    int m_startHandle = -1;

    // The simulator is positioned at m_cursorTick following the current plan.
    bool m_cursorValid = false;
    int m_cursorTick = 0;
    Policy m_cursorPolicy;

    FixSearch m_fix;
};

} // namespace ac
