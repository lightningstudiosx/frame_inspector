#pragma once
// What you see in the level: a marker with the frame window on every click/release (moves with the level),
// the bucket counter in the top left, the sub-1-frame list in the top right, and a banner while scanning.
#include <Geode/Geode.hpp>

#include <array>
#include <vector>

#include "Results.hpp"

namespace fi {

class FIOverlay : public cocos2d::CCNode {
public:
    static FIOverlay* find(PlayLayer* pl);
    static FIOverlay* ensure(PlayLayer* pl);

    void rebuild();               // after new results or option changes
    void onPlayerProgress();      // every frame while playing (for "passed so far" counting)
    void onReset();               // new attempt
    void updateScanStatus();      // show/refresh/hide the scanning banner

    static bool hiddenByKey();    // the toggle keybind hides everything
    static void toggleHidden();

protected:
    bool init(PlayLayer* pl);
    void buildMarkers(LevelResults const& r);
    void buildCounter(LevelResults const& r);
    void buildHardest(LevelResults const& r);
    void refreshCounts();
    void applyVisibility();
    void cullLabels();

    PlayLayer* m_pl = nullptr;
    cocos2d::CCNode* m_markers = nullptr;   // child of the player's parent layer
    cocos2d::CCNode* m_counter = nullptr;
    cocos2d::CCNode* m_hardest = nullptr;
    cocos2d::CCNode* m_scanBox = nullptr;
    cocos2d::CCLabelBMFont* m_scanLabel = nullptr;
    cocos2d::CCLayerColor* m_scanBar = nullptr;

    std::vector<Bucket> m_buckets;
    std::vector<cocos2d::CCLabelBMFont*> m_countLabels;  // one per bucket, then "<1"
    std::vector<int> m_totals;                           // per bucket (+1 for "<1")
    std::vector<float> m_eventX;                         // sorted x of counted events
    std::vector<int> m_eventBucket;                      // bucket per sorted event (-1 none)
    std::vector<bool> m_eventSub;                        // under 1 frame
    std::vector<int> m_passed;
    size_t m_passedIdx = 0;
    bool m_passedMode = false;
    std::vector<std::pair<float, cocos2d::CCNode*>> m_labels;  // marker labels sorted by x (only nearby ones drawn)
    size_t m_visLo = 0, m_visHi = 0;
};

} // namespace fi
