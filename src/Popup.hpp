#pragma once
// The menu opened from the pause screen: load a macro, set TPS / FPS / max window, scan, toggle what's shown.
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

namespace fi {

class FIPopup : public geode::Popup {
public:
    static FIPopup* create(PauseLayer* pause);

protected:
    bool init(PauseLayer* pause);
    void onLoadMacro(cocos2d::CCObject*);
    void onScan(cocos2d::CCObject*);
    void onClear(cocos2d::CCObject*);
    void onToggle(cocos2d::CCObject*);
    void onHelp(cocos2d::CCObject*);
    void onRanges(cocos2d::CCObject*);
    void refresh();
    void addToggle(char const* key, char const* text, bool def, cocos2d::CCPoint pos, int tag);

    PauseLayer* m_pause = nullptr;
    cocos2d::CCLabelBMFont* m_macroLabel = nullptr;
    cocos2d::CCLabelBMFont* m_infoLabel = nullptr;
    cocos2d::CCLabelBMFont* m_resultLabel = nullptr;
    geode::TextInput* m_tps = nullptr;
    geode::TextInput* m_fps = nullptr;
    geode::TextInput* m_max = nullptr;
    ButtonSprite* m_scanSpr = nullptr;
    geode::async::TaskHolder<geode::utils::file::PickResult> m_pick;
};

// "Counter ranges": choose which frame counts get their own row in the top-left counter (e.g. 9-14, 15-30, 31+).
class FIRangesPopup : public geode::Popup {
public:
    static FIRangesPopup* create();

protected:
    bool setup();  // (not named init: that would override CCNode::init)
    void rebuildChips();
    void onAdd(cocos2d::CCObject*);
    void onRemove(cocos2d::CCObject*);
    void onPreset(cocos2d::CCObject*);
    void onSave(cocos2d::CCObject*);
    double scanMax() const;

    std::vector<int> m_starts;
    double m_end = 0;  // 0 = up to the max window you scanned with
    cocos2d::CCNode* m_chips = nullptr;
    cocos2d::CCLabelBMFont* m_note = nullptr;
    geode::TextInput* m_addInput = nullptr;
    geode::TextInput* m_endInput = nullptr;
};

} // namespace fi
