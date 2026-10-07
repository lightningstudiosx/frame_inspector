#include "Overlay.hpp"

#include <algorithm>
#include <cmath>

#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>

#include "Scan.hpp"

using namespace geode::prelude;

namespace fi {

namespace {
constexpr char const* kOverlayID = "frame-inspector-overlay"_spr;
constexpr char const* kMarkersID = "frame-inspector-markers"_spr;
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

void FIOverlay::applyVisibility() {
    bool on = !s_hidden;
    if (m_markers) m_markers->setVisible(on && saved("show-markers", true));
    if (m_counter) m_counter->setVisible(on && saved("show-counter", true));
    if (m_hardest) m_hardest->setVisible(on && saved("show-hardest", true));
}

void FIOverlay::rebuild() {
    if (m_markers) {
        m_markers->removeFromParent();
        m_markers = nullptr;
    }
    if (m_counter) { m_counter->removeFromParent(); m_counter = nullptr; }
    if (m_hardest) { m_hardest->removeFromParent(); m_hardest = nullptr; }
    m_countLabels.clear();

    auto& sc = ScanController::get();
    if (!sc.results || sc.resultsLevelKey != ScanController::levelKey(m_pl)) return;
    auto const& r = *sc.results;

    m_buckets = makeBuckets(r.maxFrames());
    m_passedMode = Mod::get()->getSettingValue<std::string>("count-mode") == "Passed this attempt";

    // events that count, sorted by x for "passed so far"
    bool releases = Mod::get()->getSettingValue<bool>("show-releases");
    std::vector<size_t> order;
    for (size_t i = 0; i < r.events.size(); i++)
        if (releases || r.events[i].down) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return r.events[a].x < r.events[b].x; });
    m_eventX.clear();
    m_eventBucket.clear();
    m_eventSub.clear();
    m_totals.assign(m_buckets.size() + 1, 0);
    for (auto i : order) {
        auto const& e = r.events[i];
        int b = e.status == EventStatus::Ok ? bucketOf(m_buckets, r.framesOf(e), e.capped) : -1;
        bool sub = e.status == EventStatus::Ok && !e.capped && r.framesOf(e) < 1.0;
        m_eventX.push_back(e.x);
        m_eventBucket.push_back(b);
        m_eventSub.push_back(sub);
        if (b >= 0) m_totals[b]++;
        if (sub) m_totals[m_buckets.size()]++;
    }

    buildMarkers(r);
    buildCounter(r);
    buildHardest(r);
    onReset();
    applyVisibility();
}

void FIOverlay::buildMarkers(LevelResults const& r) {
    CCNode* parent = m_pl->m_player1 ? m_pl->m_player1->getParent() : nullptr;
    if (!parent) parent = m_pl->m_objectLayer;
    if (!parent) return;
    if (auto old = parent->getChildByID(kMarkersID)) old->removeFromParent();

    float scale = static_cast<float>(Mod::get()->getSettingValue<double>("marker-scale"));
    if (!(scale > 0)) scale = 1.f;
    bool decimals = Mod::get()->getSettingValue<bool>("marker-decimals");
    bool releases = Mod::get()->getSettingValue<bool>("show-releases");

    m_labels.clear();
    m_visLo = m_visHi = 0;
    m_markers = CCNode::create();
    m_markers->setID(kMarkersID);
    auto draw = CCDrawNode::create();
    m_markers->addChild(draw);

    for (auto const& e : r.events) {
        if (!releases && !e.down) continue;
        bool ok = e.status == EventStatus::Ok;
        double f = r.framesOf(e);
        ccColor3B col = !ok ? unreliableColor()
                      : (!e.capped && f < 1.0) ? subFrameColor()
                      : m_buckets[static_cast<size_t>(bucketOf(m_buckets, f, e.capped))].color;
        float rad = (e.down ? 7.5f : 5.f) * scale;
        CCPoint c(e.x, e.y);

        // ring for player 1, diamond for player 2
        int n = e.player2 ? 4 : 20;
        std::vector<CCPoint> verts;
        for (int k = 0; k < n; k++) {
            float a = static_cast<float>(k) / n * 6.2831853f + (e.player2 ? 0.f : 0.f);
            verts.emplace_back(c.x + std::cos(a) * rad, c.y + std::sin(a) * rad);
        }
        draw->drawPolygon(verts.data(), static_cast<unsigned>(verts.size()), c4(col, e.down ? 0.28f : 0.12f),
                          (e.down ? 1.6f : 1.1f) * scale, c4(col, e.down ? 0.95f : 0.7f));

        std::string text = windowText(r, e, decimals);
        if (e.button == 2) text = "L" + text;
        if (e.button == 3) text = "R" + text;
        auto lbl = label(text, "bigFont.fnt", (e.down ? 0.34f : 0.26f) * scale, col, {0.5f, 0.f});
        lbl->setPosition(c.x, c.y + rad + 2.f * scale);
        lbl->setOpacity(e.down ? 255 : 200);
        lbl->setVisible(false);  // cullLabels shows the ones near the player
        m_markers->addChild(lbl);
        m_labels.emplace_back(c.x, lbl);
    }
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
    auto row = [&](std::string const& name, ccColor3B color) {
        auto nameLbl = label(name + ":", "bigFont.fnt", 0.42f, color, {1.f, 0.5f});
        nameLbl->setPosition(48.f, y);
        auto countLbl = label("0", "bigFont.fnt", 0.42f, color, {0.f, 0.5f});
        countLbl->setPosition(55.f, y);
        m_counter->addChild(nameLbl);
        m_counter->addChild(countLbl);
        m_countLabels.push_back(countLbl);
        y -= lineH;
    };
    // top bucket first, like the classic counter
    std::vector<CCLabelBMFont*> tmp;
    for (size_t i = m_buckets.size(); i-- > 0;) row(m_buckets[i].label, m_buckets[i].color);
    row("<1", subFrameColor());
    // m_countLabels is in display order (top bucket first); flip into bucket order + "<1" last
    std::vector<CCLabelBMFont*> ordered(m_countLabels.size());
    for (size_t i = 0; i < m_buckets.size(); i++) ordered[i] = m_countLabels[m_buckets.size() - 1 - i];
    ordered[m_buckets.size()] = m_countLabels.back();
    m_countLabels = ordered;

    auto info = label(fmt::format("{:g} TPS / {:g} FPS{}", r.tps, r.fps, m_passedMode ? " - passed" : ""),
                      "chatFont.fnt", 0.5f, {200, 200, 200}, {0.f, 0.5f});
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
        if (e.status == EventStatus::Ok && !e.capped) idx.push_back(i);
    }
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return r.events[a].windowTicks < r.events[b].windowTicks; });

    float x = win.width - 8.f;
    float y = win.height - 34.f;
    int maxRows = static_cast<int>(Mod::get()->getSettingValue<int64_t>("hardest-count"));
    if (maxRows < 1) maxRows = 6;

    int sub = 0;
    for (auto i : idx) if (r.framesOf(r.events[i]) < 1.0) sub++;

    auto title = label(sub > 0 ? fmt::format("Under 1 frame: {}", sub) : "Nothing under 1 frame", "goldFont.fnt", 0.5f,
                       {255, 255, 255}, {1.f, 0.5f});
    title->setPosition(x, y);
    m_hardest->addChild(title);
    y -= 15.f;

    int shown = 0;
    for (auto i : idx) {
        auto const& e = r.events[i];
        double f = r.framesOf(e);
        if (sub > 0 && f >= 1.0) break;  // when there are sub-frame clicks, list only those
        if (shown >= maxRows) break;
        std::string what = e.down ? "click" : "release";
        if (e.button == 2) what = e.down ? "left" : "left up";
        if (e.button == 3) what = e.down ? "right" : "right up";
        auto line = fmt::format("{:.2f}f  {}{}  @ {}", f, what, e.player2 ? " (P2)" : "", timeText(static_cast<double>(e.frame) / r.tps));
        auto l = label(line, "bigFont.fnt", 0.3f, f < 1.0 ? subFrameColor() : m_buckets[static_cast<size_t>(bucketOf(m_buckets, f, false))].color, {1.f, 0.5f});
        l->setPosition(x, y);
        m_hardest->addChild(l);
        y -= 11.f;
        shown++;
    }
    if (sub == 0 && shown == 0) {
        auto l = label("(all clicks are easy)", "bigFont.fnt", 0.3f, {200, 200, 200}, {1.f, 0.5f});
        l->setPosition(x, y);
        m_hardest->addChild(l);
    }
}

void FIOverlay::refreshCounts() {
    for (size_t b = 0; b < m_countLabels.size() && b < m_totals.size(); b++) {
        int v = m_passedMode ? (b < m_passed.size() ? m_passed[b] : 0) : m_totals[b];
        m_countLabels[b]->setString(fmt::format("{}", v).c_str());
    }
}

void FIOverlay::onReset() {
    m_passedIdx = 0;
    m_passed.assign(m_totals.size(), 0);
    refreshCounts();
    cullLabels();
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
    if (!m_passedMode || m_eventX.empty() || !m_pl->m_player1) return;
    float px = m_pl->m_player1->m_position.x;
    size_t before = m_passedIdx;
    while (m_passedIdx < m_eventX.size() && m_eventX[m_passedIdx] <= px) {
        int b = m_eventBucket[m_passedIdx];
        if (b >= 0 && static_cast<size_t>(b) < m_passed.size()) m_passed[b]++;
        if (m_eventSub[m_passedIdx]) m_passed[m_buckets.size()]++;
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
