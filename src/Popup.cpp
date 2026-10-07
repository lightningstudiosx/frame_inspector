#include "Popup.hpp"

#include <cstdlib>

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemToggler.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>

#include "Overlay.hpp"
#include "Scan.hpp"
#include "core/Macro.hpp"

using namespace geode::prelude;

namespace fi {

namespace {
struct ToggleDef { int tag; char const* key; bool def; };
ToggleDef const kToggles[] = {
    {1, "show-markers", true},
    {2, "show-counter", true},
    {3, "show-hardest", true},
    {4, "exact-mode", false},
};

double parseNum(std::string const& s, double def) {
    if (s.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !(v > 0)) return def;
    return v;
}

std::string fmtNum(double v) { return fmt::format("{:g}", v); }
} // namespace

FIPopup* FIPopup::create(PauseLayer* pause) {
    auto ret = new FIPopup();
    if (ret->init(pause)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

void FIPopup::addToggle(char const* key, char const* text, bool def, CCPoint pos, int tag) {
    auto tog = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(FIPopup::onToggle), 0.55f);
    tog->setTag(tag);
    tog->toggle(Mod::get()->getSavedValue<bool>(key, def));
    m_buttonMenu->addChildAtPosition(tog, Anchor::Center, pos);
    auto lbl = CCLabelBMFont::create(text, "bigFont.fnt");
    lbl->setScale(0.32f);
    lbl->setAnchorPoint({0.f, 0.5f});
    m_mainLayer->addChildAtPosition(lbl, Anchor::Center, pos + CCPoint(13.f, 0.f));
}

bool FIPopup::init(PauseLayer* pause) {
    if (!Popup::init(390.f, 270.f)) return false;
    m_pause = pause;
    this->setTitle("Frame Inspector");

    // macro row
    auto macroTitle = CCLabelBMFont::create("Macro:", "goldFont.fnt");
    macroTitle->setScale(0.55f);
    macroTitle->setAnchorPoint({0.f, 0.5f});
    m_mainLayer->addChildAtPosition(macroTitle, Anchor::Center, {-180.f, 82.f});

    m_macroLabel = CCLabelBMFont::create("", "bigFont.fnt");
    m_macroLabel->setAnchorPoint({0.f, 0.5f});
    m_mainLayer->addChildAtPosition(m_macroLabel, Anchor::Center, {-128.f, 82.f});

    m_infoLabel = CCLabelBMFont::create("", "chatFont.fnt");
    m_infoLabel->setAnchorPoint({0.f, 0.5f});
    m_infoLabel->setScale(0.6f);
    m_mainLayer->addChildAtPosition(m_infoLabel, Anchor::Center, {-180.f, 64.f});

    auto loadSpr = ButtonSprite::create("Load", "goldFont.fnt", "GJ_button_04.png", 0.8f);
    loadSpr->setScale(0.7f);
    auto loadBtn = CCMenuItemSpriteExtra::create(loadSpr, this, menu_selector(FIPopup::onLoadMacro));
    m_buttonMenu->addChildAtPosition(loadBtn, Anchor::Center, {158.f, 82.f});

    // numbers
    auto& sc = ScanController::get();
    double tps = Mod::get()->getSavedValue<double>("tps", sc.macro ? sc.macro->tps : 240.0);
    double fps = Mod::get()->getSavedValue<double>("fps", 60.0);
    double maxF = Mod::get()->getSavedValue<double>("max-frames", 16.0);

    m_tps = TextInput::create(80.f, "TPS");
    m_tps->setLabel("TPS (physics)");
    m_tps->setCommonFilter(CommonFilter::Float);
    m_tps->setString(fmtNum(tps));
    m_tps->setCallback([](std::string const& s) {
        double v = parseNum(s, 0);
        if (v > 0) Mod::get()->setSavedValue<double>("tps", v);
    });
    m_mainLayer->addChildAtPosition(m_tps, Anchor::Center, {-120.f, 22.f});

    m_fps = TextInput::create(80.f, "FPS");
    m_fps->setLabel("FPS (frame size)");
    m_fps->setCommonFilter(CommonFilter::Float);
    m_fps->setString(fmtNum(fps));
    m_fps->setCallback([this](std::string const& s) {
        double v = parseNum(s, 0);
        if (!(v > 0)) return;
        Mod::get()->setSavedValue<double>("fps", v);
        // windows are stored in ticks, so a new FPS only changes how they're shown: no rescan needed
        auto& sc = ScanController::get();
        if (sc.results) {
            sc.results->fps = v;
            std::string err;
            saveResults(*sc.results, err);
            if (auto pl = PlayLayer::get())
                if (auto ov = FIOverlay::find(pl)) ov->rebuild();
            this->refresh();
        }
    });
    m_mainLayer->addChildAtPosition(m_fps, Anchor::Center, {0.f, 22.f});

    m_max = TextInput::create(80.f, "16");
    m_max->setLabel("Max window (frames)");
    m_max->setCommonFilter(CommonFilter::Float);
    m_max->setString(fmtNum(maxF));
    m_max->setCallback([](std::string const& s) {
        double v = parseNum(s, 0);
        if (v > 0) Mod::get()->setSavedValue<double>("max-frames", v);
    });
    m_mainLayer->addChildAtPosition(m_max, Anchor::Center, {120.f, 22.f});

    // toggles
    addToggle("show-markers", "Markers", true, {-170.f, -22.f}, 1);
    addToggle("show-counter", "Counter", true, {-78.f, -22.f}, 2);
    addToggle("show-hardest", "Hardest", true, {14.f, -22.f}, 3);
    addToggle("exact-mode", "Exact (slow)", false, {106.f, -22.f}, 4);

    m_resultLabel = CCLabelBMFont::create("", "chatFont.fnt");
    m_resultLabel->setScale(0.62f);
    m_mainLayer->addChildAtPosition(m_resultLabel, Anchor::Center, {0.f, -56.f});

    // bottom buttons
    auto clearSpr = ButtonSprite::create("Clear", "goldFont.fnt", "GJ_button_06.png", 0.8f);
    clearSpr->setScale(0.75f);
    m_buttonMenu->addChildAtPosition(CCMenuItemSpriteExtra::create(clearSpr, this, menu_selector(FIPopup::onClear)),
                                     Anchor::Bottom, {-110.f, 28.f});

    m_scanSpr = ButtonSprite::create("Scan level", "goldFont.fnt", "GJ_button_01.png", 0.8f);
    m_scanSpr->setScale(0.85f);
    m_buttonMenu->addChildAtPosition(CCMenuItemSpriteExtra::create(m_scanSpr, this, menu_selector(FIPopup::onScan)),
                                     Anchor::Bottom, {20.f, 28.f});

    auto helpSpr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
    helpSpr->setScale(0.7f);
    m_buttonMenu->addChildAtPosition(CCMenuItemSpriteExtra::create(helpSpr, this, menu_selector(FIPopup::onHelp)),
                                     Anchor::BottomRight, {-22.f, 22.f});

    this->refresh();
    return true;
}

void FIPopup::refresh() {
    auto& sc = ScanController::get();
    if (sc.macro) {
        m_macroLabel->setString(sc.macro->name.c_str());
        m_macroLabel->limitLabelWidth(260.f, 0.45f, 0.1f);
        auto info = fmt::format("{} - {} inputs - {:g} TPS in file{}", sc.macro->format, sc.macro->inputs.size(), sc.macro->tps,
                                sc.macro->bot.empty() ? "" : " - " + sc.macro->bot);
        if (sc.macro->practiceDeaths > 0) info += " - has practice deaths!";
        m_infoLabel->setString(info.c_str());
    } else {
        m_macroLabel->setString("(none loaded)");
        m_macroLabel->limitLabelWidth(260.f, 0.45f, 0.1f);
        m_infoLabel->setString("Supports .gdr .gdr2 .json (GDR / Mega Hack / TASBot) .zbf .re3 .slc .txt .xd");
    }
    m_infoLabel->limitLabelWidth(360.f, 0.6f, 0.2f);

    std::string res;
    auto pl = PlayLayer::get();
    if (sc.scanning()) {
        res = fmt::format("Scanning... {}", sc.statusText());
    } else if (sc.results && pl && sc.resultsLevelKey == ScanController::levelKey(pl)) {
        auto const& r = *sc.results;
        int sub = 0, bad = 0;
        double hardest = 1e9;
        for (auto const& e : r.events) {
            if (e.status != EventStatus::Ok) { bad++; continue; }
            double f = r.framesOf(e);
            if (!e.capped) hardest = std::min(hardest, f);
            if (!e.capped && f < 1.0) sub++;
        }
        res = fmt::format("This level: {} inputs scanned with {} ({:g} TPS, shown at {:g} FPS)\n", r.events.size(),
                          r.macroName.empty() ? "a macro" : r.macroName, r.tps, r.fps);
        res += fmt::format("Hardest: {}   Under 1 frame: {}", hardest < 1e8 ? fmt::format("{:.2f}f", hardest) : std::string("-"), sub);
        if (bad) res += fmt::format("   Unclear: {}", bad);
        if (r.offset != 0) res += fmt::format("   (frame offset {:+})", r.offset);
    } else {
        res = "No scan for this level yet. Load a macro that beats it, then Scan.";
    }
    m_resultLabel->setString(res.c_str());
    m_resultLabel->limitLabelWidth(370.f, 0.62f, 0.2f);
    m_scanSpr->setString(sc.scanning() ? "Stop scan" : "Scan level");
}

void FIPopup::onToggle(CCObject* sender) {
    auto tog = static_cast<CCMenuItemToggler*>(sender);
    for (auto const& t : kToggles) {
        if (t.tag != tog->getTag()) continue;
        // the toggler flips its state after this callback, so the new value is the opposite of the current one
        Mod::get()->setSavedValue<bool>(t.key, !tog->isToggled());
    }
    if (auto pl = PlayLayer::get())
        if (auto ov = FIOverlay::find(pl)) ov->rebuild();
}

void FIPopup::onLoadMacro(CCObject*) {
    file::FilePickOptions opts;
    opts.filters.push_back({"Macros", {"*.gdr", "*.gdr2", "*.json", "*.zbf", "*.re3", "*.slc", "*.txt", "*.xd"}});
    auto last = Mod::get()->getSavedValue<std::string>("last-macro-dir", "");
    std::error_code ec;
    if (!last.empty() && std::filesystem::exists(last, ec)) opts.defaultPath = std::filesystem::path(last);
    else opts.defaultPath = dirs::getGameDir();

    m_pick.spawn(file::pick(file::PickMode::OpenFile, opts), [this](file::PickResult res) {
        if (res.isErr()) {
            FLAlertLayer::create("Frame Inspector", fmt::format("Couldn't open the file picker: {}", res.unwrapErr()), "OK")->show();
            return;
        }
        auto picked = std::move(res).unwrap();
        if (!picked) return;  // cancelled
        auto loaded = loadMacroFile(*picked);
        if (!loaded.ok) {
            FLAlertLayer::create("Frame Inspector", loaded.error, "OK")->show();
            return;
        }
        auto& sc = ScanController::get();
        sc.macro = std::move(loaded.macro);
        sc.macroPath = picked->string();
        Mod::get()->setSavedValue<std::string>("last-macro-dir", picked->parent_path().string());
        // use the TPS stored in the macro
        m_tps->setString(fmtNum(sc.macro->tps));
        Mod::get()->setSavedValue<double>("tps", sc.macro->tps);
        this->refresh();
    });
}

void FIPopup::onScan(CCObject*) {
    auto& sc = ScanController::get();
    if (sc.scanning()) {
        sc.stop(true);
        this->refresh();
        return;
    }
    auto pl = PlayLayer::get();
    ScanConfig cfg;
    cfg.tps = parseNum(m_tps->getString(), sc.macro ? sc.macro->tps : 240);
    cfg.fps = parseNum(m_fps->getString(), 60);
    cfg.maxFrames = parseNum(m_max->getString(), 16);
    cfg.exactOnly = Mod::get()->getSavedValue<bool>("exact-mode", false);
    cfg.includeReleases = Mod::get()->getSettingValue<bool>("scan-releases");
    cfg.extraTicks = static_cast<int>(Mod::get()->getSettingValue<int64_t>("extra-survive-ticks"));

    std::string err;
    if (!sc.start(pl, cfg, err)) {
        FLAlertLayer::create("Frame Inspector", err, "OK")->show();
        return;
    }
    auto pause = m_pause;
    this->onClose(nullptr);
    if (pause) pause->onResume(nullptr);  // the scan runs while the game is unpaused
}

void FIPopup::onClear(CCObject*) {
    auto& sc = ScanController::get();
    auto pl = PlayLayer::get();
    if (!pl) return;
    createQuickPopup("Frame Inspector", "Delete the saved scan for this level?", "Cancel", "Delete", [this, pl](FLAlertLayer*, bool yes) {
        if (!yes) return;
        auto& sc = ScanController::get();
        deleteResults(ScanController::levelKey(pl));
        sc.results.reset();
        if (auto ov = FIOverlay::find(pl)) ov->rebuild();
        this->refresh();
    });
    (void)sc;
}

void FIPopup::onHelp(CCObject*) {
    FLAlertLayer::create(nullptr, "How it works",
        "The bot replays your macro, then moves <cy>one click at a time</c> earlier and later, tick by tick, "
        "while every other click keeps its timing. A click's <cg>window</c> is how many ticks it can be off and the player "
        "still survives until their next click.\n\n"
        "frames = ticks x FPS / TPS, so at 240 TPS and 60 FPS one tick is <cp>0.25 frames</c>. Anything under 1 frame is "
        "listed in the top right.\n\n"
        "TPS must match the macro. Practice mode and start positions must be off. Releases get markers too (smaller rings). "
        "<co>?</c> markers mean the bot couldn't reproduce that moment exactly (usually random triggers).",
        "OK", nullptr, 380.f)->show();
}

} // namespace fi
