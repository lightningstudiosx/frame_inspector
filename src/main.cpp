// Frame Inspector - load a macro, let the bot measure every click's frame window, then play with the windows shown.
#include <Geode/Geode.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>

#include "Overlay.hpp"
#include "Popup.hpp"
#include "Scan.hpp"

using namespace geode::prelude;
using fi::FIOverlay;
using fi::ScanController;

namespace {
bool isScanLayer(GJBaseGameLayer* layer) {
    auto& sc = ScanController::get();
    return sc.scanning() && static_cast<GJBaseGameLayer*>(sc.layer()) == layer;
}
} // namespace

class $modify(FIBaseLayer, GJBaseGameLayer) {
    // While scanning, the bot drives the game tick by tick instead of the normal frame update.
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (pl && isScanLayer(this) && !pl->m_isPaused) {
            ScanController::get().runFrame(pl, [this](float tickDt) { this->GJBaseGameLayer::update(tickDt); });
            return;
        }
        GJBaseGameLayer::update(dt);
    }

    // Physics bypass while scanning: exactly one tick per update at the macro's TPS.
    double getModifiedDelta(float dt) {
        double ret = GJBaseGameLayer::getModifiedDelta(dt);
        auto& sc = ScanController::get();
        if (sc.inTick() && isScanLayer(this)) return 1.0 / sc.tps();
        return ret;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        auto& sc = ScanController::get();
        if (sc.inTick() && isScanLayer(this))
            sc.onProcessCommands(this, static_cast<int64_t>(m_gameState.m_currentProgress));
    }

    // Your own clicks are ignored while the bot is scanning.
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& sc = ScanController::get();
        if (isScanLayer(this) && !sc.injecting()) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (sc.scanning()) return;
        // your own click: count it and pop a circle where you did it
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this)) {
            bool twoPlayer = m_levelSettings && m_levelSettings->m_twoPlayerMode;
            if (auto ov = FIOverlay::find(pl)) ov->onPlayerInput(down, button, twoPlayer && !isPlayer1);
        }
    }
};

class $modify(FIPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        ScanController::get().loadResultsFor(this);
        FIOverlay::ensure(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (isScanLayer(this)) {
            if (object == m_anticheatSpike) return PlayLayer::destroyPlayer(player, object);
            ScanController::get().markDead();
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto& sc = ScanController::get();
        if (isScanLayer(this)) {
            sc.markComplete();
            return;
        }
        // never let a completion through while the level is still in a state the bot left it in
        if (sc.tainted(this)) {
            log::warn("Frame Inspector: blocked a level complete from a bot-made state");
            return;
        }
        PlayLayer::levelComplete();
    }

    // the bot reaching the end: count it, but don't play the end animation
    void playEndAnimationToPos(CCPoint pos) {
        if (isScanLayer(this)) {
            ScanController::get().markComplete();
            return;
        }
        PlayLayer::playEndAnimationToPos(pos);
    }

    void playPlatformerEndAnimationToPos(CCPoint pos, bool instant) {
        if (isScanLayer(this)) {
            ScanController::get().markComplete();
            return;
        }
        PlayLayer::playPlatformerEndAnimationToPos(pos, instant);
    }

    // platformer checkpoints the bot touches would otherwise become respawn points
    void checkpointActivated(CheckpointGameObject* object) {
        if (isScanLayer(this)) return;
        PlayLayer::checkpointActivated(object);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        ScanController::get().onLevelReset(this);
        if (!isScanLayer(this))
            if (auto ov = FIOverlay::find(this)) ov->onReset();
    }

    void togglePracticeMode(bool practice) {
        auto& sc = ScanController::get();
        if (isScanLayer(this) && !sc.selfResetting()) {
            // switching practice mid-scan would break the bot's runs
            PlayLayer::togglePracticeMode(practice);
            geode::queueInMainThread([] { ScanController::get().stop(false); });
            return;
        }
        PlayLayer::togglePracticeMode(practice);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!isScanLayer(this))
            if (auto ov = FIOverlay::find(this)) ov->onPlayerProgress();
    }

    // skipping the visual culling while the bot fast-forwards (it doesn't affect physics) makes scans much faster
    void updateVisibility(float dt) {
        if (ScanController::get().inTick() && isScanLayer(this) && ScanController::get().fastScan()) return;
        PlayLayer::updateVisibility(dt);
    }

    void onQuit() {
        ScanController::get().forget(this);
        PlayLayer::onQuit();
    }

    void onExit() {
        ScanController::get().forget(this);
        PlayLayer::onExit();
    }
};

class $modify(FIStats, GameStatsManager) {
    // don't count the bot's thousands of attempts and jumps in your stats
    void incrementStat(char const* key, int amount) {
        if (ScanController::get().scanning()) return;
        GameStatsManager::incrementStat(key, amount);
    }
    void incrementChallenge(GJChallengeType type, int amount) {
        if (ScanController::get().scanning()) return;
        GameStatsManager::incrementChallenge(type, amount);
    }
};

class $modify(FIPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto text = CCLabelBMFont::create("FI", "bigFont.fnt");
        auto spr = CircleButtonSprite::create(text, CircleBaseColor::Cyan, CircleBaseSize::Small);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FIPauseLayer::onFrameInspector));
        btn->setID("frame-inspector-button"_spr);
        if (auto menu = this->getChildByID("left-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto m = CCMenu::create();
            m->addChild(btn);
            auto win = CCDirector::get()->getWinSize();
            m->setPosition({36.f, win.height - 36.f});
            this->addChild(m, 10);
        }
    }

    void onFrameInspector(CCObject*) {
        if (auto popup = fi::FIPopup::create(this)) popup->show();
    }
};

$on_mod(Loaded) {
    listenForKeybindSettingPresses("toggle-overlay", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) FIOverlay::toggleHidden();
        return false;
    });
}
