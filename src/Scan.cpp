#include "Scan.hpp"

#include <chrono>

#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>

#include "Overlay.hpp"

using namespace geode::prelude;

namespace fi {

// ------------------------------------------------------------------ backend: the real game

void GeodeBackend::restartFromStart() {
    selfReset = true;
    pl->m_isPracticeMode = false;
    pl->resetLevelFromStart();
    selfReset = false;
    pl->m_levelEndAnimationStarted = false;
}

bool GeodeBackend::makeCheckpoint() {
    auto cp = pl->createCheckpoint();
    if (!cp) return false;
    cp->retain();
    dropCheckpoint();
    m_cp = cp;
    return true;
}

void GeodeBackend::dropCheckpoint() {
    if (m_cp) {
        if (pl && static_cast<cocos2d::CCObject*>(pl->m_currentCheckpoint) == m_cp) pl->m_currentCheckpoint = nullptr;
        m_cp->release();
        m_cp = nullptr;
    }
}

bool GeodeBackend::restoreCheckpoint() {
    if (!m_cp || !pl->m_checkpointArray) return false;
    // Let the game do a normal practice-mode respawn onto our save state, then switch practice back off.
    // (Practice stays off while ticking, so the game never places its own checkpoints.)
    pl->m_checkpointArray->addObject(m_cp);
    selfReset = true;
    pl->m_isPracticeMode = true;
    pl->resetLevel();
    pl->m_isPracticeMode = false;
    selfReset = false;
    pl->m_checkpointArray->removeObject(m_cp, true);
    // the game may keep a plain pointer to the checkpoint it just loaded; ours can be freed later
    if (static_cast<cocos2d::CCObject*>(pl->m_currentCheckpoint) == m_cp) pl->m_currentCheckpoint = nullptr;
    pl->m_levelEndAnimationStarted = false;  // every save state is before the level end
    return true;
}

int64_t GeodeBackend::progress() const {
    return static_cast<int64_t>(pl->m_gameState.m_currentProgress);
}

void GeodeBackend::stepTick() {
    if (!tickFn) return;
    inTick = true;
    tickFn(static_cast<float>(1.0 / tps));
    inTick = false;
}

TickSample GeodeBackend::sample() const {
    TickSample s;
    s.valid = true;
    s.dual = pl->m_gameState.m_isDualMode;
    auto fill = [](PlayerObject* p, PlayerSample& out) {
        if (!p) return;
        out.x = p->m_position.x;
        out.y = p->m_position.y;
        out.yv = p->m_yVelocity;
        out.vx = p->m_position.x;  // markers live in the same layer as the player
        out.vy = p->m_position.y;
    };
    fill(pl->m_player1, s.p1);
    if (s.dual) fill(pl->m_player2, s.p2);
    return s;
}

bool GeodeBackend::isDead() const { return dead; }

bool GeodeBackend::isComplete() const { return complete || pl->m_levelEndAnimationStarted; }

void GeodeBackend::clearFlags() {
    dead = false;
    complete = false;
}

void GeodeBackend::sendInput(uint8_t button, bool player2, bool down) {
    injecting = true;
    pl->handleButton(down, static_cast<int>(button), !player2);
    injecting = false;
}

void GeodeBackend::releaseAllInputs() {
    if (pl->m_player1) pl->m_player1->releaseAllButtons();
    if (pl->m_player2) pl->m_player2->releaseAllButtons();
}

// ------------------------------------------------------------------ controller

ScanController& ScanController::get() {
    static ScanController inst;
    return inst;
}

std::string ScanController::levelKey(PlayLayer* pl) {
    if (!pl || !pl->m_level) return "none";
    int id = pl->m_level->m_levelID.value();
    return levelKeyFor(id, std::string(pl->m_level->m_levelName));
}

void ScanController::loadResultsFor(PlayLayer* pl) {
    resultsLevelKey = levelKey(pl);
    results = loadResults(resultsLevelKey);
}

void ScanController::saveStats() {
    m_stats = {};
    auto pl = m_backend.pl;
    if (!pl || !pl->m_level) return;
    m_stats.valid = true;
    m_stats.attempts = pl->m_level->m_attempts;
    m_stats.jumps = pl->m_level->m_jumps;
    m_stats.clicks = pl->m_level->m_clicks;
    m_stats.attemptTime = pl->m_level->m_attemptTime;
    m_stats.layerAttempts = pl->m_attempts;
    m_stats.layerJumps = pl->m_jumps;
}

void ScanController::restoreStats() {
    auto pl = m_backend.pl;
    if (!m_stats.valid || !pl || !pl->m_level) return;
    pl->m_level->m_attempts = m_stats.attempts;
    pl->m_level->m_jumps = m_stats.jumps;
    pl->m_level->m_clicks = m_stats.clicks;
    pl->m_level->m_attemptTime = m_stats.attemptTime;
    pl->m_attempts = m_stats.layerAttempts;
    pl->m_jumps = m_stats.layerJumps;
    m_stats.valid = false;
}

bool ScanController::start(PlayLayer* pl, ScanConfig const& cfg, std::string& err) {
    if (m_scanning) { err = "A scan is already running."; return false; }
    if (!pl) { err = "Open a level first."; return false; }
    if (!macro) { err = "Load a macro first."; return false; }
    if (pl->m_isPracticeMode) { err = "Turn off practice mode first (the scan has to start from the real level start)."; return false; }
    if (pl->m_startPosObject) { err = "This attempt starts at a start position. Turn start positions off so the macro starts at 0%."; return false; }
    if (pl->m_levelEndAnimationStarted || (pl->m_player1 && pl->m_player1->m_isDead) || pl->m_playerDied) {
        err = "Wait until you've respawned, then try again.";
        return false;
    }
    if (cfg.tps < 240 || cfg.tps > 100000) { err = "TPS must be at least 240 (GD 2.2 physics never runs slower than that)."; return false; }
    if (cfg.fps < 1 || cfg.fps > 100000) { err = "FPS must be between 1 and 100000."; return false; }

    m_cfg = cfg;
    m_backend.dropCheckpoint();
    m_backend = GeodeBackend{};
    m_backend.pl = pl;
    m_backend.engine = &m_engine;
    m_backend.tps = cfg.tps;
    saveStats();
    m_engine.begin(*macro, cfg);
    if (!m_engine.active()) { err = m_engine.error(); return false; }

    // Save states only restore moved/toggled objects if the game has listed them, which it does when practice
    // mode is turned on. Do that once (practice stays logically off while the bot ticks).
    m_enteredPractice = false;
    if (!cfg.exactOnly && pl->m_dynamicSaveObjects.empty() && pl->m_activeSaveObjects1.empty() && pl->m_activeSaveObjects2.empty()) {
        m_backend.selfReset = true;
        pl->togglePracticeMode(true);
        m_backend.selfReset = false;
        pl->m_isPracticeMode = false;
        m_enteredPractice = true;
    }
    log::info("Frame Inspector: save-state object lists: dynamic {}, active {}+{}", pl->m_dynamicSaveObjects.size(),
              pl->m_activeSaveObjects1.size(), pl->m_activeSaveObjects2.size());
    m_tainted = pl;
    m_fastScan = Mod::get()->getSettingValue<bool>("fast-scan");
    m_scanning = true;
    log::info("Frame Inspector: scanning {} ({} inputs) at {} TPS / {} FPS, max {} frames",
              macro->name, macro->inputs.size(), cfg.tps, cfg.fps, cfg.maxFrames);
    if (auto ov = FIOverlay::ensure(pl)) ov->updateScanStatus();
    return true;
}

void ScanController::endScan(bool resetToStart) {
    m_scanning = false;
    auto pl = m_backend.pl;
    m_backend.dropCheckpoint();
    if (pl && resetToStart) {
        m_backend.selfReset = true;
        if (m_enteredPractice) {
            pl->m_isPracticeMode = true;
            pl->togglePracticeMode(false);
        }
        pl->m_isPracticeMode = false;
        pl->resetLevelFromStart();
        pl->m_levelEndAnimationStarted = false;
        m_backend.selfReset = false;
        m_tainted = nullptr;  // a clean attempt from the real start: nothing from the bot is left
    }
    m_enteredPractice = false;
    restoreStats();  // after the reset, so it doesn't add an attempt
}

void ScanController::stop(bool userCancelled) {
    if (!m_scanning) return;
    m_engine.cancel();
    endScan(true);
    if (auto pl = m_backend.pl)
        if (auto ov = FIOverlay::find(pl)) ov->updateScanStatus();
    if (userCancelled) Notification::create("Frame Inspector: scan stopped", NotificationIcon::Info)->show();
    else Notification::create("Frame Inspector: scan stopped (the level was restarted)", NotificationIcon::Warning)->show();
}

void ScanController::onLevelReset(PlayLayer* pl) {
    if (m_backend.selfReset) return;
    if (m_scanning && m_backend.pl == pl) {
        // something other than the bot restarted the level mid-scan: the run would be garbage, so stop
        log::warn("Frame Inspector: level was reset during a scan, stopping");
        geode::queueInMainThread([] { ScanController::get().stop(false); });
        return;
    }
    if (m_tainted == pl) m_tainted = nullptr;  // you restarted: fresh attempt
}

void ScanController::forget(PlayLayer* pl) {
    if (m_backend.pl != pl) return;
    if (m_scanning) {
        m_engine.cancel();
        endScan(false);  // the level is closing; don't touch it
    }
    m_backend.pl = nullptr;
}

void ScanController::onProcessCommands(GJBaseGameLayer* layer, int64_t progress) {
    if (!inTick() || static_cast<GJBaseGameLayer*>(m_backend.pl) != layer) return;
    m_engine.applyInputs(m_backend, progress);
}

void ScanController::runFrame(PlayLayer* pl, std::function<void(float)> tick) {
    if (!m_scanning) return;
    if (pl != m_backend.pl) return;
    m_backend.tickFn = std::move(tick);

    double budgetMs = Mod::get()->getSettingValue<double>("scan-budget-ms");
    if (!(budgetMs > 0)) budgetMs = 14;
    auto t0 = std::chrono::steady_clock::now();
    while (m_engine.active()) {
        m_engine.pump(m_backend);
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms >= budgetMs) break;
    }
    m_backend.tickFn = nullptr;

    // keep the scan quiet: every restart would otherwise restart the song and replay orb sounds
    if (auto fmod = FMODAudioEngine::get()) {
        fmod->stopAllMusic(true);
        fmod->stopAllEffects();
    }

    if (!m_engine.active()) finish();
    else if (auto ov = FIOverlay::find(pl)) ov->updateScanStatus();
}

void ScanController::finish() {
    auto pl = m_backend.pl;
    endScan(true);

    if (m_engine.phase() == Engine::Phase::Done) {
        auto const& res = m_engine.result();
        LevelResults r;
        r.levelKey = levelKey(pl);
        r.levelName = pl && pl->m_level ? std::string(pl->m_level->m_levelName) : "";
        r.macroName = macro ? macro->name : "";
        r.tps = res.tps;
        r.fps = res.fps;
        r.maxTicks = res.maxTicks;
        r.offset = res.offset;
        r.endDetected = res.endDetected;
        r.events = res.events;
        std::string err;
        if (!saveResults(r, err)) log::warn("Frame Inspector: couldn't save results: {}", err);
        results = r;
        resultsLevelKey = r.levelKey;

        int sub = 0;
        for (auto const& e : r.events) if (e.status == EventStatus::Ok && r.framesOf(e) < 1.0) sub++;
        auto msg = fmt::format("Scanned {} inputs ({} under 1 frame)", r.events.size(), sub);
        if (res.unreliable > 0) msg += fmt::format(", {} unclear", res.unreliable);
        Notification::create(msg, NotificationIcon::Success, 4.f)->show();
        log::info("Frame Inspector: done. offset {:+}, {} ticks simulated, {} restores, {} exact inputs, {} unclear",
                  res.offset, m_engine.ticksSimulated(), m_engine.restores(), res.exactEvents, res.unreliable);
    } else if (m_engine.phase() == Engine::Phase::Failed) {
        FLAlertLayer::create("Frame Inspector", m_engine.error(), "OK")->show();
    }

    if (pl) {
        if (auto ov = FIOverlay::ensure(pl)) {
            ov->rebuild();
            ov->updateScanStatus();
        }
        // pause so you can look at the results before playing
        geode::queueInMainThread([pl] {
            if (PlayLayer::get() == pl && !pl->m_isPaused) pl->pauseGame(false);
        });
    }
}

} // namespace fi
