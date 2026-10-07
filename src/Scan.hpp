#pragma once
// Connects the pure scan Engine to the real game (PlayLayer) and owns the global mod state.
#include <Geode/Geode.hpp>

#include <chrono>
#include <functional>
#include <optional>

#include "Results.hpp"
#include "core/Engine.hpp"
#include "core/Macro.hpp"

namespace fi {

class GeodeBackend final : public Backend {
public:
    PlayLayer* pl = nullptr;
    Engine* engine = nullptr;
    double tps = 240;
    std::function<void(float)> tickFn;  // runs the ORIGINAL GJBaseGameLayer::update for one tick
    bool dead = false;
    bool complete = false;
    bool inTick = false;
    bool injecting = false;
    bool selfReset = false;   // the bot itself is restarting/loading (not you)

    void restartFromStart() override;
    bool makeCheckpoint() override;
    bool restoreCheckpoint() override;
    int64_t progress() const override;
    void stepTick() override;
    TickSample sample() const override;
    bool isDead() const override;
    bool isComplete() const override;
    void clearFlags() override;
    void sendInput(uint8_t button, bool player2, bool down) override;
    void releaseAllInputs() override;

    void dropCheckpoint();

private:
    cocos2d::CCObject* m_cp = nullptr;  // CheckpointObject, retained
};

class ScanController {
public:
    static ScanController& get();

    // macro picked in the popup
    std::optional<Macro> macro;
    std::string macroPath;

    // results for the level currently open
    std::optional<LevelResults> results;
    std::string resultsLevelKey;

    bool start(PlayLayer* pl, ScanConfig const& cfg, std::string& err);
    void stop(bool userCancelled);
    void forget(PlayLayer* pl);  // the level is being closed

    bool scanning() const { return m_scanning; }
    bool inTick() const { return m_scanning && m_backend.inTick; }
    bool injecting() const { return m_backend.injecting; }
    double tps() const { return m_cfg.tps; }
    bool fastScan() const { return m_fastScan; }
    PlayLayer* layer() const { return m_backend.pl; }

    void runFrame(PlayLayer* pl, std::function<void(float)> tick);
    void onProcessCommands(GJBaseGameLayer* layer, int64_t progress);
    void markDead() { m_backend.dead = true; }
    void markComplete() { m_backend.complete = true; }

    // the level is in a state the bot put it in (until a clean restart): no completions allowed
    bool tainted(PlayLayer* pl) const { return pl && m_tainted == pl; }
    // called from the resetLevel hook
    void onLevelReset(PlayLayer* pl);
    bool selfResetting() const { return m_backend.selfReset; }

    double fraction() const { return m_engine.fraction(); }
    std::string statusText() const { return m_engine.statusText(); }

    // load (or clear) the saved results for this level
    void loadResultsFor(PlayLayer* pl);
    static std::string levelKey(PlayLayer* pl);

private:
    void finish();
    void endScan(bool resetToStart);
    void saveStats();
    void restoreStats();

    bool m_scanning = false;
    bool m_fastScan = true;
    PlayLayer* m_tainted = nullptr;
    bool m_enteredPractice = false;
    ScanConfig m_cfg;
    Engine m_engine;
    GeodeBackend m_backend;

    struct Stats {
        bool valid = false;
        geode::SeedValueRSV attempts, jumps, clicks, attemptTime;
        int layerAttempts = 0;
        int layerJumps = 0;
    } m_stats;
};

} // namespace fi
