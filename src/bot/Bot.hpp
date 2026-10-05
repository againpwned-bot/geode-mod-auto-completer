#pragma once

#include <Geode/Geode.hpp>

#include "../planner/Planner.hpp"
#include "GameSim.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace ac {

enum class BotState {
    Off,         // disabled, not in a level, or in practice mode
    Waiting,     // in a level, will compute a path on the next frame
    Computing,   // planning (the game is frozen meanwhile)
    Playing,     // following the planned path
    Finished,    // the level was completed this attempt
    Failed,      // the player died while following the path
    Unsupported, // level type the bot can't handle
};

struct BotStats {
    int clicks = 0;
    int decisions = 0;
    int evaluations = 0;
    double computeSeconds = 0.0;
    float selfTestDeviation = -1.f;
    int plannedTicks = 0;
    bool complete = false;
};

class Bot {
public:
    static Bot& get();

    // --- hooks ---
    void onLevelInit(PlayLayer* layer);
    void onLevelQuit(PlayLayer* layer);
    void onReset(PlayLayer* layer);
    // Called before the real frame update. Returns true when the frame was used for
    // planning and the normal update must be skipped.
    bool onFrame(PlayLayer* layer, float dt);
    void afterFrame(PlayLayer* layer);
    void beforeTick(PlayLayer* layer);
    void afterTick(PlayLayer* layer);
    void onRealDeath(PlayLayer* layer);
    void onRealComplete(PlayLayer* layer);

    bool shouldBlockUserInput() const;
    // Progress, stats and verification must not be saved for this attempt.
    bool safeModeActive() const;

    // --- menu ---
    BotState state() const { return m_state; }
    char const* stateName() const;
    std::string const& statusText() const { return m_status; }
    float computeProgress() const { return m_computeProgress; }
    float planProgress() const { return m_planProgress; }
    BotStats const& stats() const { return m_stats; }
    PlayStyle planStyle() const { return m_planStyle; }
    bool hasPlan() const { return m_planValid; }
    bool inLevel() const { return m_layer != nullptr; }
    void requestRecompute();

    // --- path viewer ---
    std::vector<uint8_t> const& planInputs() const { return m_inputs; }
    std::vector<Sample> const& planSamples() const { return m_samples; }
    int planEnd() const { return m_planEnd; }
    unsigned currentTick() const;
    unsigned planRevision() const { return m_planRevision; }

private:
    Bot() = default;

    bool supported(PlayLayer* layer, std::string& reason) const;
    bool readyToCompute(PlayLayer* layer) const;
    void startCompute(PlayLayer* layer, bool replan);
    void runCompute(PlayLayer* layer);
    void finishCompute(PlayLayer* layer);
    void cancelCompute(bool restore);
    void pauseAudio();
    void resumeAudio();
    void invalidatePlan();
    void setStatus(std::string status);
    void updatePlanStats();

    PlayLayer* m_layer = nullptr;
    BotState m_state = BotState::Off;
    std::string m_status = "Not in a level";

    // planning
    std::unique_ptr<GameSim> m_sim;
    std::unique_ptr<Planner> m_planner;
    bool m_replanning = false;
    unsigned m_computeStart = 0;
    std::chrono::steady_clock::time_point m_computeBegan;
    double m_computeSeconds = 0.0;
    float m_computeProgress = 0.f;
    bool m_audioPaused = false;

    // plan (indexed by absolute tick, m_gameState.m_currentProgress)
    bool m_planValid = false;
    bool m_planComplete = false;
    PlayStyle m_planStyle = PlayStyle::Default;
    std::vector<uint8_t> m_inputs;
    std::vector<Sample> m_samples;
    int m_planEnd = 0;
    unsigned m_planBegin = 0; // first tick covered by the plan
    unsigned m_planRevision = 0;
    float m_planProgress = 0.f;
    BotStats m_stats;

    // playback
    bool m_appliedHold = false;
    unsigned m_tickBefore = 0;
    bool m_usedThisAttempt = false;
    bool m_divergence = false;
    int m_replans = 0;
    int m_failures = 0;
    bool m_requestRestart = false;
    uint32_t m_seedSalt = 0;
};

} // namespace ac
