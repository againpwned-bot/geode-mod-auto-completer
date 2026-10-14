#pragma once

#include <Geode/Geode.hpp>

#include "../planner/Planner.hpp"
#include "FakeSim.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ac {

enum class BotState {
    Off,         // disabled, not in a level, or in practice mode
    Playing,     // playing the level, planning ahead as it goes
    Finished,    // the level was completed this attempt
    Died,        // the player died this attempt
    Unsupported, // level type the bot can't handle
};

struct BotStats {
    int clicks = 0;
    int checks = 0;          // look-ahead checks run
    int searches = 0;        // checks that had to find a new path
    long evaluations = 0;
    int deaths = 0;
    int desyncs = 0;
    double microsPerTick = 0.0;    // cost of one simulated tick
    double predictionError = -1.0; // average distance between prediction and reality
    int inputDelay = 0;
    std::string model;             // how the hidden player runs GD's inlined per-tick code
    std::string tickCalls;         // physics calls GD makes per tick
    long shadowTicks = 0;          // real ticks re-simulated one at a time to compare
    long shadowMismatches = 0;     // of those, ticks where the simulation came out different
    int plannedAhead = 0;          // ticks planned ahead of the player
};

// Plays the level in real time. Every few ticks it copies the real player onto a hidden
// player, simulates about 0.8 s ahead and, if that path dies, searches for inputs that
// survive. The real game is never paused, rewound or touched by the simulation.
class Bot {
public:
    static Bot& get();

    // --- hooks ---
    void onLevelInit(PlayLayer* layer);
    void onLevelQuit(PlayLayer* layer);
    void onReset(PlayLayer* layer);
    void onFrame(PlayLayer* layer);
    void beforeTick(PlayLayer* layer);
    void endFrame(PlayLayer* layer);
    void onRealDeath(PlayLayer* layer, std::string const& killer);
    void onRealComplete(PlayLayer* layer);

    bool shouldBlockUserInput() const;
    // Progress, stats and verification must not be saved for this attempt.
    bool safeModeActive() const;

    // --- menu ---
    BotState state() const { return m_state; }
    char const* stateName() const;
    std::string const& statusText() const { return m_status; }
    BotStats const& stats() const { return m_stats; }
    bool inLevel() const { return m_layer != nullptr; }
    bool hasPlan() const { return m_planEnd > 0; }
    float leadSeconds() const { return m_stats.plannedAhead / 240.f; }
    void requestRestart();

    // --- path viewer ---
    std::vector<uint8_t> const& planInputs() const { return m_inputs; }
    std::vector<Sample> const& planSamples() const { return m_samples; }
    int planEnd() const { return m_planEnd; }
    unsigned planRevision() const { return m_planRevision; }
    unsigned currentTick() const;

private:
    Bot() = default;

    struct Check {
        unsigned t0 = 0;
        int seedSize = 0;
        std::vector<uint8_t> seed;
        bool initialHold = false;
        bool seeded = false;    // the previous plan has presses/releases after the floor
        bool retried = false;   // already re-planned without them
        PlannerConfig config;
        PlayerFix startFix;     // the real player when the check started
        // Declared before the planner so the planner (which uses it) is destroyed first.
        std::unique_ptr<FakeSim> sim;
        std::unique_ptr<Planner> planner;
    };

    struct Calibration {
        bool active = false;
        unsigned t0 = 0;
        std::array<std::vector<Sample>, PhysicsModel::kVariants> predicted;
        std::array<double, PhysicsModel::kVariants> error {};
        std::array<int, PhysicsModel::kVariants> count {};
    };

    bool supported(PlayLayer* layer, std::string& reason) const;
    bool readyToPlay(PlayLayer* layer) const;
    void resetAttempt();
    void releaseHold(PlayLayer* layer);
    void startCheck(PlayLayer* layer, unsigned now);
    void retryCheck(bool robust);
    void finishCheck();
    void logTrace(Planner const& planner, unsigned t0);
    void diagnoseStuck(Check& check);
    void runProbe(PlayLayer* layer);
    void startCalibration(PlayLayer* layer, unsigned now);
    void finishCalibration();
    void closeTick(PlayLayer* layer);
    void runShadow(PlayLayer* layer);
    void checkDeathTick(PlayLayer* layer);
    void runSelfTest(PlayLayer* layer);
    void logDeath(FakeSim const& sim);
    // `baseStep`: the real physics step the simulation's first tick stands for.
    std::unique_ptr<FakeSim> makeSim(PlayLayer* layer, PhysicsModel const& model, int baseStep) const;
    void addSimCost(FakeSim const& sim);
    void setStatus(std::string status);

    PlayLayer* m_layer = nullptr;
    BotState m_state = BotState::Off;
    std::string m_status = "Not in a level";

    // plan, indexed by physics step since the attempt started (m_stepIndex)
    std::vector<uint8_t> m_inputs;
    std::vector<Sample> m_samples;
    int m_planEnd = 0;
    unsigned m_planRevision = 0;

    std::unique_ptr<Check> m_check;
    int m_lastCheck = -1000000;
    bool m_forceCheck = false;
    bool m_desyncFlagged = false;

    Calibration m_calibration;
    std::array<double, PhysicsModel::kVariants> m_variantError {};
    PhysicsModel m_model;
    InlineProbe m_probe;
    unsigned m_probeRevision = 0;

    long m_simTicks = 0;
    double m_simSeconds = 0.0;

    bool m_appliedHold = false;
    unsigned m_tickBefore = 0;
    bool m_tickOpen = false;

    // One-tick check: the real player's state before the tick and the input it got.
    PlayerFix m_shadowPre;
    bool m_shadowValid = false;
    bool m_shadowHeld = false;
    bool m_shadowWant = false;
    int m_shadowLogged = 0;
    bool m_deathPending = false;
    std::string m_deathKiller;
    int m_shadowInputLogged = 0;
    long m_shadowInputTicks = 0;
    long m_shadowInputMismatches = 0;
    int m_selfTests = 0;
    bool m_selfTestDone = false;
    int m_tracesLogged = 0;
    int m_diagnoses = 0;

    // Physics steps since the attempt started (GD's own counter moves 2 per step).
    unsigned m_stepIndex = 0;
    unsigned m_lastProgress = 0;
    bool m_seenStep = false;
    bool m_strideLogged = false;
    bool m_usedThisAttempt = false;
    bool m_requestRestart = false;
    uint32_t m_attempt = 0;

    BotStats m_stats;
};

} // namespace ac
