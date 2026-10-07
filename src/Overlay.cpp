#include "Overlay.hpp"

#include <algorithm>
#include <cmath>

#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>

#include "Scan.hpp"

using namespace geode::prelude;

namespace fi {

namespace {
constexpr char const* kOverlayID = "frame-inspector-overlay"_spr;
constexpr char const* kMarkersID = "frame-inspector-markers"_spr;
constexpr char const* kLiveID = "frame-inspector-live"_spr;
constexpr float kMatchRange = 75.f;  // how far (in level units, 30 = one block) your click may be from the bot's
constexpr float kPassMargin = 30.f;  // a hit click counts once you're a block past it (still alive)
bool s_hidden = false;

ccColor4F c4(ccColor3B c, float a) { return {c.r / 255.f, c.g / 255.f, c.b / 255.f, a}; }

bool saved(char const* key, bool def) { return Mod::get()->getSavedValue<bool>(key, def); }

CCLabelBMFont* label(std::string const& text, char const* font, float scale, ccColor3B color, CCPoint anchor) {
    auto l = CCLabelBMFont::create(text.c_str(), font);
    l->setScale(scale);
    l->setColor(color);
    l->setAnchorPoint(anchor);
    return l;
}

std::string timeText(double seconds) {
    int m = static_cast<int>(seconds / 60);
    double s = seconds - m * 60;
    return m > 0 ? fmt::format("{}:{:05.2f}", m, s) : fmt::format("{:.2f}s", s);
}

// releases only matter for things like wave / ship / swing; on a cube they have a huge window, so hide those
bool counts(EventResult const& e) {
    if (e.down) return true;
    if (!Mod::get()->getSettingValue<bool>("show-releases")) return false;
    return e.status == EventStatus::Ok && !e.capped;
}
} // namespace

bool FIOverlay::hiddenByKey() { return s_hidden; }

void FIOverlay::toggleHidden() {
    s_hidden = !s_hidden;
    if (auto pl = PlayLayer::get())
        if (auto ov = find(pl)) ov->applyVisibility();
}

FIOverlay* FIOverlay::find(PlayLayer* pl) {
    if (!pl) return nullptr;
    return static_cast<FIOverlay*>(pl->getChildByID(kOverlayID));
}

FIOverlay* FIOverlay::ensure(PlayLayer* pl) {
    if (!pl) return nullptr;
    if (auto ov = find(pl)) return ov;
    auto ov = new FIOverlay();
    if (!ov->init(pl)) {
        delete ov;
        return nullptr;
    }
    ov->autorelease();
    ov->setID(kOverlayID);
    pl->addChild(ov, 10000);
    return ov;
}

bool FIOverlay::init(PlayLayer* pl) {
    if (!CCNode::init()) return false;
    m_pl = pl;
    this->setContentSize(CCDirector::get()->getWinSize());
    this->rebuild();
    return true;
}

CCNode* FIOverlay::playerParent() {
    CCNode* parent = m_pl->m_player1 ? m_pl->m_player1->getParent() : nullptr;
    return parent ? parent : m_pl->m_objectLayer;
}

void FIOverlay::applyVisibility() {
    bool on = !s_hidden;
    bool markers = on && saved("show-markers", true);
    if (m_markers) m_markers->setVisible(markers);
    if (m_live) m_live->setVisible(markers);
    if (m_counter) m_counter->setVisible(on && saved("show-counter", true));
    if (m_hardest) m_hardest->setVisible(on && saved("show-hardest", true));
}

void FIOverlay::rebuild() {
    if (m_markers) { m_markers->removeFromParent(); m_markers = nullptr; }
    if (m_live) { m_live->removeFromParent(); m_live = nullptr; }
    if (m_counter) { m_counter->removeFromParent(); m_counter = nullptr; }
    if (m_hardest) { m_hardest->removeFromParent(); m_hardest = nullptr; }
    m_countLabels.clear();
    m_events.clear();
    m_labels.clear();
    m_visLo = m_visHi = 0;

    auto& sc = ScanController::get();
    if (!sc.results || sc.resultsLevelKey != ScanController::levelKey(m_pl)) return;
    auto const& r = *sc.results;

    double displayMax = r.maxFrames();
    double counterMax = Mod::get()->getSettingValue<double>("counter-max");
    if (counterMax >= 1 && counterMax < displayMax) displayMax = counterMax;
    m_buckets = makeBuckets(displayMax, parseBucketStarts(Mod::get()->getSettingValue<std::string>("counter-rows")));
    auto cm = Mod::get()->getSettingValue<std::string>("count-mode");
    m_countMode = cm == "Whole level" ? CountMode::Whole : cm == "Passed this attempt" ? CountMode::Passed : CountMode::AsYouClick;
    m_markerMode = Mod::get()->getSettingValue<std::string>("marker-mode") == "Always (show ahead)" ? MarkerMode::Always : MarkerMode::OnClick;
    m_scale = static_cast<float>(Mod::get()->getSettingValue<double>("marker-scale"));
    if (!(m_scale > 0)) m_scale = 1.f;
    bool decimals = Mod::get()->getSettingValue<bool>("marker-decimals");

    m_totals.assign(m_buckets.size() + 1, 0);
    m_oneFrames = m_subFrames = 0;
    for (auto const& e : r.events) {
        if (!counts(e)) continue;
        Ev ev;
        ev.x = e.x;
        ev.y = e.y;
        ev.down = e.down;
        ev.button = e.button;
        ev.player2 = e.player2;
        bool ok = e.status == EventStatus::Ok;
        double f = r.framesOf(e);
        bool overCap = e.capped || f >= std::floor(displayMax + 1e-9);
        ev.frames = e.capped ? 1e9 : f;
        ev.bucket = ok ? bucketOf(m_buckets, f, overCap) : -1;
        ev.sub = ok && !e.capped && f < 1.0;
        ev.color = !ok ? unreliableColor() : ev.sub ? subFrameColor() : m_buckets[static_cast<size_t>(ev.bucket)].color;
        ev.text = ok && overCap ? fmt::format("{}+", static_cast<int>(std::floor(displayMax + 1e-9))) : windowText(r, e, decimals);
        if (e.button == 2) ev.text = "L" + ev.text;
        if (e.button == 3) ev.text = "R" + ev.text;
        if (ev.bucket >= 0) m_totals[static_cast<size_t>(ev.bucket)]++;
        if (ev.sub) m_totals[m_buckets.size()]++;
        if (ev.bucket == 0) m_oneFrames++;
        if (ev.sub) m_subFrames++;
        m_events.push_back(ev);
    }
    std::stable_sort(m_events.begin(), m_events.end(), [](Ev const& a, Ev const& b) { return a.x < b.x; });

    if (auto parent = playerParent()) {
        if (auto old = parent->getChildByID(kLiveID)) old->removeFromParent();
        m_live = CCNode::create();
        m_live->setID(kLiveID);
        parent->addChild(m_live, 9999);
    }
    if (m_markerMode == MarkerMode::Always) buildMarkers();
    buildCounter(r);
    buildHardest(r);
    m_counts.assign(m_totals.size(), 0);
    m_passedIdx = 0;
    m_pending.clear();
    refreshCounts();
    cullLabels();
    applyVisibility();
}

void FIOverlay::ding(Ev const& e) {
    if (!Mod::get()->getSettingValue<bool>("ding")) return;
    if (e.bucket < 0 || e.frames > Mod::get()->getSettingValue<double>("ding-max-frames") + 1e-9) return;
    auto custom = Mod::get()->getSettingValue<std::filesystem::path>("ding-file");
    std::error_code ec;
    std::string path;
    if (!custom.empty() && std::filesystem::exists(custom, ec)) path = geode::utils::string::pathToString(custom);
    else path = e.down ? "ding.ogg"_spr : "ding-release.ogg"_spr;
    if (auto fmod = FMODAudioEngine::get()) fmod->playEffect(path);
}

void FIOverlay::drawCircle(CCNode* parent, CCDrawNode* draw, Ev const& e, CCPoint c, bool animate) {
    float rad = (e.down ? 7.5f : 5.5f) * m_scale;
    int n = e.player2 ? 4 : 20;  // ring for player 1, diamond for player 2
    std::vector<CCPoint> verts;
    for (int k = 0; k < n; k++) {
        float a = static_cast<float>(k) / n * 6.2831853f;
        verts.emplace_back(c.x + std::cos(a) * rad, c.y + std::sin(a) * rad);
    }
    draw->drawPolygon(verts.data(), static_cast<unsigned>(verts.size()), c4(e.color, e.down ? 0.3f : 0.15f),
                      (e.down ? 1.7f : 1.2f) * m_scale, c4(e.color, e.down ? 0.95f : 0.75f));
    auto lbl = label(e.text, "bigFont.fnt", (e.down ? 0.34f : 0.27f) * m_scale, e.color, {0.5f, 0.f});
    lbl->setPosition(c.x, c.y + rad + 2.f * m_scale);
    parent->addChild(lbl);
    if (animate) {
        float s = lbl->getScale();
        lbl->setScale(s * 1.8f);
        lbl->runAction(CCEaseBackOut::create(CCScaleTo::create(0.18f, s)));
    } else {
        lbl->setVisible(false);  // cullLabels shows the ones near the player
        m_labels.emplace_back(c.x, lbl);
    }
}

void FIOverlay::buildMarkers() {
    auto parent = playerParent();
    if (!parent) return;
    if (auto old = parent->getChildByID(kMarkersID)) old->removeFromParent();
    m_markers = CCNode::create();
    m_markers->setID(kMarkersID);
    auto draw = CCDrawNode::create();
    m_markers->addChild(draw);
    for (auto const& e : m_events) drawCircle(m_markers, draw, e, {e.x, e.y}, false);
    std::stable_sort(m_labels.begin(), m_labels.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    parent->addChild(m_markers, 9999);
}

void FIOverlay::buildCounter(LevelResults const& r) {
    auto win = CCDirector::get()->getWinSize();
    m_counter = CCNode::create();
    m_counter->setID("counter"_spr);
    this->addChild(m_counter);

    float y = win.height - 10.f;
    float lineH = 13.f;
    std::vector<CCLabelBMFont*> display;
    auto row = [&](std::string const& name, ccColor3B color) {
        auto nameLbl = label(name + ":", "bigFont.fnt", 0.42f, color, {1.f, 0.5f});
        nameLbl->setPosition(48.f, y);
        auto countLbl = label("0", "bigFont.fnt", 0.42f, color, {0.f, 0.5f});
        countLbl->setPosition(55.f, y);
        m_counter->addChild(nameLbl);
        m_counter->addChild(countLbl);
        display.push_back(countLbl);
        y -= lineH;
    };
    for (size_t i = m_buckets.size(); i-- > 0;) row(m_buckets[i].label, m_buckets[i].color);  // top bucket first
    row("<1", subFrameColor());
    m_countLabels.assign(display.size(), nullptr);  // bucket order, "<1" last
    for (size_t i = 0; i < m_buckets.size(); i++) m_countLabels[i] = display[m_buckets.size() - 1 - i];
    m_countLabels[m_buckets.size()] = display.back();

    char const* mode = m_countMode == CountMode::AsYouClick ? "your clicks" : m_countMode == CountMode::Passed ? "passed" : "whole level";
    auto info = label(fmt::format("{:g} TPS / {:g} FPS - {}", r.tps, r.fps, mode), "chatFont.fnt", 0.5f, {200, 200, 200}, {0.f, 0.5f});
    info->setPosition(6.f, y - 2.f);
    m_counter->addChild(info);
}

void FIOverlay::buildHardest(LevelResults const& r) {
    auto win = CCDirector::get()->getWinSize();
    m_hardest = CCNode::create();
    m_hardest->setID("hardest"_spr);
    this->addChild(m_hardest);

    std::vector<size_t> idx;
    for (size_t i = 0; i < r.events.size(); i++) {
        auto const& e = r.events[i];
        if (e.status == EventStatus::Ok && !e.capped && counts(e)) idx.push_back(i);
    }
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        auto const& ea = r.events[a];
        auto const& eb = r.events[b];
        return ea.windowTicks != eb.windowTicks ? ea.windowTicks < eb.windowTicks : ea.frame < eb.frame;
    });

    float x = win.width - 8.f;
    float y = win.height - 34.f;
    int maxRows = static_cast<int>(Mod::get()->getSettingValue<int64_t>("hardest-count"));
    if (maxRows < 1) maxRows = 6;

    auto title = label(fmt::format("1 frames: {}", m_oneFrames), "goldFont.fnt", 0.55f,
                       m_oneFrames > 0 ? m_buckets[0].color : ccColor3B{255, 255, 255}, {1.f, 0.5f});
    title->setPosition(x, y);
    m_hardest->addChild(title);
    y -= 14.f;
    if (m_subFrames > 0) {
        auto subTitle = label(fmt::format("under 1 frame: {}", m_subFrames), "bigFont.fnt", 0.32f, subFrameColor(), {1.f, 0.5f});
        subTitle->setPosition(x, y);
        m_hardest->addChild(subTitle);
        y -= 12.f;
    }

    int shown = 0;
    for (auto i : idx) {
        auto const& e = r.events[i];
        double f = r.framesOf(e);
        if (m_oneFrames > 0 && f >= 2.0) break;  // when there are 1 frames, list only those
        if (shown >= maxRows) break;
        std::string what = e.down ? "click" : "release";
        if (e.button == 2) what = e.down ? "left" : "left up";
        if (e.button == 3) what = e.down ? "right" : "right up";
        auto line = fmt::format("{:.2f}f  {}{}  @ {}", f, what, e.player2 ? " (P2)" : "", timeText(static_cast<double>(e.frame) / r.tps));
        auto col = f < 1.0 ? subFrameColor() : m_buckets[static_cast<size_t>(bucketOf(m_buckets, f, false))].color;
        auto l = label(line, "bigFont.fnt", 0.3f, col, {1.f, 0.5f});
        l->setPosition(x, y);
        m_hardest->addChild(l);
        y -= 11.f;
        shown++;
    }
    if (m_oneFrames > 0 && shown < m_oneFrames) {
        auto more = label(fmt::format("+{} more", m_oneFrames - shown), "bigFont.fnt", 0.26f, {200, 200, 200}, {1.f, 0.5f});
        more->setPosition(x, y);
        m_hardest->addChild(more);
    } else if (shown == 0) {
        auto l = label("(no hard clicks)", "bigFont.fnt", 0.3f, {200, 200, 200}, {1.f, 0.5f});
        l->setPosition(x, y);
        m_hardest->addChild(l);
    }
}

void FIOverlay::refreshCounts() {
    for (size_t b = 0; b < m_countLabels.size() && b < m_totals.size(); b++) {
        int v = m_countMode == CountMode::Whole ? m_totals[b] : (b < m_counts.size() ? m_counts[b] : 0);
        m_countLabels[b]->setString(fmt::format("{}", v).c_str());
    }
}

void FIOverlay::bump(int bucket, bool sub) {
    auto pop = [](CCLabelBMFont* l) {
        if (!l) return;
        l->stopAllActions();
        l->setScale(0.42f);
        l->runAction(CCSequence::create(CCScaleTo::create(0.05f, 0.6f), CCScaleTo::create(0.12f, 0.42f), nullptr));
    };
    if (bucket >= 0 && static_cast<size_t>(bucket) < m_counts.size()) {
        m_counts[static_cast<size_t>(bucket)]++;
        if (static_cast<size_t>(bucket) < m_countLabels.size()) pop(m_countLabels[static_cast<size_t>(bucket)]);
    }
    if (sub && m_buckets.size() < m_counts.size()) {
        m_counts[m_buckets.size()]++;
        if (m_buckets.size() < m_countLabels.size()) pop(m_countLabels[m_buckets.size()]);
    }
    refreshCounts();
}

void FIOverlay::showAttemptBanner() {
    if (!Mod::get()->getSettingValue<bool>("attempt-banner") || m_events.empty() || s_hidden) return;
    if (auto old = this->getChildByID("attempt-banner"_spr)) old->removeFromParent();
    auto win = CCDirector::get()->getWinSize();
    auto node = CCNode::create();
    node->setID("attempt-banner"_spr);
    std::string text = m_oneFrames == 1 ? "1 frames: 1" : fmt::format("1 frames: {}", m_oneFrames);
    auto main = label(text, "goldFont.fnt", 0.75f, m_oneFrames > 0 ? m_buckets[0].color : ccColor3B{255, 255, 255}, {0.5f, 0.5f});
    main->setPosition(win.width / 2, win.height * 0.72f);
    node->addChild(main);
    if (m_subFrames > 0) {
        auto sub = label(fmt::format("{} under 1 frame", m_subFrames), "bigFont.fnt", 0.4f, subFrameColor(), {0.5f, 0.5f});
        sub->setPosition(win.width / 2, win.height * 0.72f - 20.f);
        node->addChild(sub);
    }
    this->addChild(node, 50);
    for (auto child : CCArrayExt<CCNode*>(node->getChildren()))
        child->runAction(CCSequence::create(CCDelayTime::create(1.6f), CCFadeOut::create(0.5f), nullptr));
    node->runAction(CCSequence::create(CCDelayTime::create(2.2f), CCRemoveSelf::create(), nullptr));
}

void FIOverlay::onReset() {
    m_passedIdx = 0;
    m_counts.assign(m_totals.size(), 0);
    m_pending.clear();
    for (auto& e : m_events) e.used = false;
    if (m_live) m_live->removeAllChildren();
    refreshCounts();
    cullLabels();
    showAttemptBanner();
}

void FIOverlay::onPlayerInput(bool down, int button, bool player2) {
    if (m_events.empty()) return;
    auto player = player2 && m_pl->m_player2 ? m_pl->m_player2 : m_pl->m_player1;
    if (!player) return;
    float px = player->m_position.x;
    float py = player->m_position.y;
    bool twoPlayer = m_pl->m_levelSettings && m_pl->m_levelSettings->m_twoPlayerMode;

    // match this input to the nearest scanned input of the same kind that you haven't hit yet
    auto key = [](Ev const& a, float v) { return a.x < v; };
    size_t lo = static_cast<size_t>(std::lower_bound(m_events.begin(), m_events.end(), px - kMatchRange, key) - m_events.begin());
    Ev* best = nullptr;
    float bestDist = kMatchRange + 1.f;
    for (size_t i = lo; i < m_events.size() && m_events[i].x <= px + kMatchRange; i++) {
        auto& e = m_events[i];
        if (e.used || e.down != down || e.button != static_cast<uint8_t>(button)) continue;
        if (twoPlayer && e.player2 != player2) continue;
        float d = std::abs(e.x - px);
        if (d < bestDist) { bestDist = d; best = &e; }
    }
    if (!best) return;
    best->used = true;
    // the counter only goes up once you've actually made it past this click
    if (m_countMode == CountMode::AsYouClick) m_pending.push_back(static_cast<size_t>(best - m_events.data()));
    if (m_markerMode == MarkerMode::OnClick && m_live && !s_hidden) {
        auto draw = CCDrawNode::create();
        m_live->addChild(draw);
        drawCircle(m_live, draw, *best, {px, py}, true);
    }
    ding(*best);
}

void FIOverlay::cullLabels() {
    if (m_labels.empty() || !m_pl->m_player1) return;
    float px = m_pl->m_player1->m_position.x;
    float w = CCDirector::get()->getWinSize().width;
    auto key = [](std::pair<float, CCNode*> const& a, float v) { return a.first < v; };
    size_t lo = static_cast<size_t>(std::lower_bound(m_labels.begin(), m_labels.end(), px - 2.5f * w, key) - m_labels.begin());
    size_t hi = static_cast<size_t>(std::lower_bound(m_labels.begin(), m_labels.end(), px + 4.f * w, key) - m_labels.begin());
    if (lo == m_visLo && hi == m_visHi) return;
    for (size_t i = m_visLo; i < m_visHi; i++)
        if (i < lo || i >= hi) m_labels[i].second->setVisible(false);
    for (size_t i = lo; i < hi; i++)
        if (i < m_visLo || i >= m_visHi) m_labels[i].second->setVisible(true);
    m_visLo = lo;
    m_visHi = hi;
}

void FIOverlay::onPlayerProgress() {
    cullLabels();
    if (!m_pending.empty() && m_pl->m_player1 && !m_pl->m_player1->m_isDead) {
        float px = m_pl->m_player1->m_position.x;
        auto it = std::remove_if(m_pending.begin(), m_pending.end(), [&](size_t i) {
            auto const& e = m_events[i];
            auto p = e.player2 && m_pl->m_player2 ? m_pl->m_player2 : m_pl->m_player1;
            float x = p ? p->m_position.x : px;
            if (x < e.x + kPassMargin) return false;
            bump(e.bucket, e.sub);
            return true;
        });
        m_pending.erase(it, m_pending.end());
    }
    if (m_countMode != CountMode::Passed || m_events.empty() || !m_pl->m_player1) return;
    float px = m_pl->m_player1->m_position.x;
    size_t before = m_passedIdx;
    while (m_passedIdx < m_events.size() && m_events[m_passedIdx].x <= px) {
        auto const& e = m_events[m_passedIdx];
        if (e.bucket >= 0) m_counts[static_cast<size_t>(e.bucket)]++;
        if (e.sub) m_counts[m_buckets.size()]++;
        m_passedIdx++;
    }
    if (m_passedIdx != before) refreshCounts();
}

void FIOverlay::updateScanStatus() {
    auto& sc = ScanController::get();
    bool scanning = sc.scanning() && sc.layer() == m_pl;
    if (!scanning) {
        if (m_scanBox) {
            m_scanBox->removeFromParent();
            m_scanBox = nullptr;
            m_scanLabel = nullptr;
            m_scanBar = nullptr;
        }
        return;
    }
    auto win = CCDirector::get()->getWinSize();
    if (!m_scanBox) {
        m_scanBox = CCNode::create();
        auto dim = CCLayerColor::create({0, 0, 0, 150}, win.width, win.height);
        m_scanBox->addChild(dim);
        auto title = label("Frame Inspector is scanning...", "goldFont.fnt", 0.7f, {255, 255, 255}, {0.5f, 0.5f});
        title->setPosition(win.width / 2, win.height / 2 + 30.f);
        m_scanBox->addChild(title);
        m_scanLabel = label("", "bigFont.fnt", 0.38f, {255, 255, 255}, {0.5f, 0.5f});
        m_scanLabel->setPosition(win.width / 2, win.height / 2);
        m_scanBox->addChild(m_scanLabel);
        auto barBg = CCLayerColor::create({255, 255, 255, 40}, 240.f, 8.f);
        barBg->setPosition(win.width / 2 - 120.f, win.height / 2 - 24.f);
        m_scanBox->addChild(barBg);
        m_scanBar = CCLayerColor::create({120, 220, 255, 255}, 1.f, 8.f);
        m_scanBar->setPosition(win.width / 2 - 120.f, win.height / 2 - 24.f);
        m_scanBox->addChild(m_scanBar);
        auto hint = label("Pause > Frame Inspector > Stop to cancel", "chatFont.fnt", 0.6f, {200, 200, 200}, {0.5f, 0.5f});
        hint->setPosition(win.width / 2, win.height / 2 - 44.f);
        m_scanBox->addChild(hint);
        this->addChild(m_scanBox, 100);
    }
    double frac = std::clamp(sc.fraction(), 0.0, 1.0);
    m_scanLabel->setString(fmt::format("{}   {:.0f}%", sc.statusText(), frac * 100).c_str());
    m_scanBar->setContentSize({std::max(1.f, 240.f * static_cast<float>(frac)), 8.f});
}

} // namespace fi
