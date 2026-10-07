#pragma once
// Scan results for one level: saving/loading and turning tick windows into frame buckets.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <Geode/Geode.hpp>

#include "core/Engine.hpp"

namespace fi {

struct LevelResults {
    std::string levelKey;
    std::string levelName;
    std::string macroName;
    double tps = 240;
    double fps = 60;          // display FPS (can be changed later without rescanning)
    int maxTicks = 64;        // cap used while scanning
    int offset = 0;
    bool endDetected = true;
    std::vector<EventResult> events;
    int version = 2;          // 1 = scanned before v1.3 (no physics x / time per click)

    double framesOf(EventResult const& e) const { return e.windowTicks * fps / tps; }
    double maxFrames() const { return maxTicks * fps / tps; }
};

struct Bucket {
    double lo = 1;            // frames, inclusive
    double hi = 1e9;          // exclusive
    std::string label;
    cocos2d::ccColor3B color;
};

// Buckets like the classic counter: 1, 2, 3, 4, 5-6, 7-8, 9-15, 16+ (top one follows the max window).
// `starts` = first frame count of each row, e.g. {1,2,3,4,5,7,9,16}; rows at or above the max are merged into "max+".
std::vector<Bucket> makeBuckets(double maxFrames, std::vector<int> const& starts);
std::vector<int> parseBucketStarts(std::string const& text);  // "1,2,3,4,5,7,9,16" -> {1,2,...}
int bucketOf(std::vector<Bucket> const& buckets, double frames, bool capped);
cocos2d::ccColor3B subFrameColor();
cocos2d::ccColor3B unreliableColor();

// Text shown on a marker: "0.25", "3", "16+" ...
std::string windowText(LevelResults const& r, EventResult const& e, bool decimals);

std::string levelKeyFor(int levelID, std::string const& levelName);
std::filesystem::path resultsPath(std::string const& levelKey);
bool saveResults(LevelResults const& r, std::string& err);
std::optional<LevelResults> loadResults(std::string const& levelKey);
bool deleteResults(std::string const& levelKey);

} // namespace fi
