#pragma once
// What you see in the level:
//  - circles with the frame window that pop up where YOU click/release (or all of them shown ahead, if you pick that)
//  - the counter in the top left (goes up as you hit each click)
//  - "1 frames" list in the top right
//  - a "1 frames: N" banner every new attempt
//  - a banner while the bot is scanning
#include <Geode/Geode.hpp>

#include <string>
#include <vector>

#include "Results.hpp"

namespace fi {

class FIOverlay : public cocos2d::CCNode {
public:
    static FIOverlay* find(PlayLayer* pl);
    static FIOverlay* ensure(PlayLayer* pl);

    void rebuild();               // after new results or option changes
    void onPlayerProgress();      // every frame while playing
    void onPlayerInput(bool down, int button, bool player2);  // you clicked / released
    void onReset();               // new attempt
    void updateScanStatus();      // show/refresh/hide the scanning banner

    static bool hiddenByKey();    // the toggle keybind hides everything
    static void toggleHidden();

protected:
    enum class CountMode { AsYouClick, Passed, Whole };
    enum class MarkerMode { OnClick, Always };

    struct Ev {
        float x = 0, y = 0;
        int bucket = -1;          // -1 = couldn't be measured
        bool sub = false;         // under 1 frame
        bool down = true;
        uint8_t button = 1;
        bool player2 = false;
        bool used = false;        // you already hit this one this attempt
        std::string text;
        cocos2d::ccColor3B color;
    };

    bool init(PlayLayer* pl);
    void buildMarkers();
    void buildCounter(LevelResults const& r);
    void buildHardest(LevelResults const& r);
    void refreshCounts();
    void applyVisibility();
    void cullLabels();
    void bump(int bucket, bool sub);
    void drawCircle(cocos2d::CCNode* parent, cocos2d::CCDrawNode* draw, Ev const& e, cocos2d::CCPoint at, bool animate);
    void showAttemptBanner();
    cocos2d::CCNode* playerParent();

    PlayLayer* m_pl = nullptr;
    cocos2d::CCNode* m_markers = nullptr;   // shown-ahead markers (child of the player's parent layer)
    cocos2d::CCNode* m_live = nullptr;      // circles from your own clicks this attempt
    cocos2d::CCNode* m_counter = nullptr;
    cocos2d::CCNode* m_hardest = nullptr;
    cocos2d::CCNode* m_scanBox = nullptr;
    cocos2d::CCLabelBMFont* m_scanLabel = nullptr;
    cocos2d::CCLayerColor* m_scanBar = nullptr;

    CountMode m_countMode = CountMode::AsYouClick;
    MarkerMode m_markerMode = MarkerMode::OnClick;
    float m_scale = 1.f;

    std::vector<Bucket> m_buckets;
    std::vector<Ev> m_events;                            // counted inputs, sorted by x
    std::vector<cocos2d::CCLabelBMFont*> m_countLabels;  // one per bucket, then "<1"
    std::vector<int> m_totals;                           // per bucket (+1 for "<1")
    std::vector<int> m_counts;                           // this attempt
    size_t m_passedIdx = 0;
    int m_oneFrames = 0, m_subFrames = 0;
    std::vector<std::pair<float, cocos2d::CCNode*>> m_labels;  // ahead-marker labels sorted by x (only nearby ones drawn)
    size_t m_visLo = 0, m_visHi = 0;
};

} // namespace fi
