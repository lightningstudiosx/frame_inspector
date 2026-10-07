#include "Results.hpp"

#include <cmath>

using namespace geode::prelude;

namespace fi {

std::vector<Bucket> makeBuckets(double maxFrames) {
    struct Edge { double lo; cocos2d::ccColor3B color; };
    // colours follow the usual frame-window counter: red = frame perfect ... blue = free
    static Edge const edges[] = {
        {1, {255, 70, 70}},     // 1
        {2, {255, 150, 60}},    // 2
        {3, {255, 222, 70}},    // 3
        {4, {255, 255, 255}},   // 4
        {5, {90, 230, 120}},    // 5-6
        {7, {70, 225, 215}},    // 7-8
        {9, {150, 130, 255}},   // 9-15
        {16, {120, 190, 255}},  // 16+
    };
    double cap = std::max(1.0, std::floor(maxFrames + 1e-9));
    std::vector<Bucket> out;
    for (auto const& e : edges) {
        if (e.lo >= cap) break;
        Bucket b;
        b.lo = e.lo;
        b.color = e.color;
        out.push_back(b);
    }
    for (size_t i = 0; i < out.size(); i++) {
        double hi = i + 1 < out.size() ? out[i + 1].lo : cap;
        out[i].hi = hi;
        int a = static_cast<int>(out[i].lo), z = static_cast<int>(hi) - 1;
        out[i].label = a == z ? fmt::format("{}", a) : fmt::format("{}-{}", a, z);
    }
    Bucket top;
    top.lo = cap;
    top.hi = 1e18;
    top.label = fmt::format("{}+", static_cast<int>(cap));
    top.color = edges[7].color;
    out.push_back(top);
    return out;
}

int bucketOf(std::vector<Bucket> const& buckets, double frames, bool capped) {
    if (buckets.empty()) return -1;
    if (capped) return static_cast<int>(buckets.size()) - 1;
    double f = std::max(1.0, std::floor(frames + 1e-9));
    for (size_t i = 0; i < buckets.size(); i++)
        if (f >= buckets[i].lo && f < buckets[i].hi) return static_cast<int>(i);
    return static_cast<int>(buckets.size()) - 1;
}

cocos2d::ccColor3B subFrameColor() { return {255, 60, 210}; }
cocos2d::ccColor3B unreliableColor() { return {140, 140, 140}; }

std::string windowText(LevelResults const& r, EventResult const& e, bool decimals) {
    if (e.status != EventStatus::Ok) return "?";
    double f = r.framesOf(e);
    if (e.capped) return fmt::format("{}+", static_cast<int>(std::floor(r.maxFrames() + 1e-9)));
    if (f < 1.0) return fmt::format("{:.2f}", f);
    if (decimals) return fmt::format("{:.1f}", f);
    return fmt::format("{}", static_cast<int>(std::floor(f + 1e-9)));
}

std::string levelKeyFor(int levelID, std::string const& levelName) {
    if (levelID > 0) return fmt::format("id_{}", levelID);
    std::string safe;
    for (char c : levelName) safe += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (safe.size() > 40) safe.resize(40);
    return fmt::format("local_{}_{:08x}", safe, static_cast<uint32_t>(std::hash<std::string>{}(levelName)));
}

std::filesystem::path resultsPath(std::string const& levelKey) {
    return Mod::get()->getSaveDir() / "results" / (levelKey + ".json");
}

bool saveResults(LevelResults const& r, std::string& err) {
    auto ev = matjson::Value::array();
    for (auto const& e : r.events) {
        auto a = matjson::Value::array();
        a.push(matjson::Value(static_cast<std::intmax_t>(e.frame)));
        a.push(matjson::Value(static_cast<std::intmax_t>(e.button)));
        a.push(matjson::Value(e.player2));
        a.push(matjson::Value(e.down));
        a.push(matjson::Value(static_cast<double>(e.x)));
        a.push(matjson::Value(static_cast<double>(e.y)));
        a.push(matjson::Value(static_cast<std::intmax_t>(e.left)));
        a.push(matjson::Value(static_cast<std::intmax_t>(e.right)));
        a.push(matjson::Value(static_cast<std::intmax_t>(e.windowTicks)));
        int flags = (e.capped ? 1 : 0) | (e.limited ? 2 : 0) | (e.exact ? 4 : 0);
        a.push(matjson::Value(static_cast<std::intmax_t>(flags)));
        a.push(matjson::Value(static_cast<std::intmax_t>(e.status)));
        ev.push(a);
    }
    auto root = matjson::makeObject({
        {"version", 1},
        {"levelKey", r.levelKey},
        {"levelName", r.levelName},
        {"macro", r.macroName},
        {"tps", r.tps},
        {"fps", r.fps},
        {"maxTicks", r.maxTicks},
        {"offset", r.offset},
        {"endDetected", r.endDetected},
        {"events", ev},
    });
    auto path = resultsPath(r.levelKey);
    if (auto res = file::createDirectoryAll(path.parent_path()); res.isErr()) { err = res.unwrapErr(); return false; }
    if (auto res = file::writeStringSafe(path, root.dump(0)); res.isErr()) { err = res.unwrapErr(); return false; }
    return true;
}

std::optional<LevelResults> loadResults(std::string const& levelKey) {
    auto path = resultsPath(levelKey);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return std::nullopt;
    auto text = file::readString(path);
    if (text.isErr()) return std::nullopt;
    auto parsed = matjson::parse(text.unwrap());
    if (parsed.isErr()) return std::nullopt;
    auto root = parsed.unwrap();
    LevelResults r;
    r.levelKey = levelKey;
    r.levelName = root["levelName"].asString().unwrapOr("");
    r.macroName = root["macro"].asString().unwrapOr("");
    r.tps = root["tps"].asDouble().unwrapOr(240);
    r.fps = root["fps"].asDouble().unwrapOr(60);
    r.maxTicks = static_cast<int>(root["maxTicks"].asInt().unwrapOr(64));
    r.offset = static_cast<int>(root["offset"].asInt().unwrapOr(0));
    r.endDetected = root["endDetected"].asBool().unwrapOr(true);
    if (!(r.tps > 0) || !(r.fps > 0) || r.maxTicks <= 0) return std::nullopt;
    auto events = root["events"].asArray();
    if (events.isErr()) return std::nullopt;
    for (auto const& a : events.unwrap()) {
        if (!a.isArray() || a.size() < 11) continue;
        EventResult e;
        e.frame = a[0].asInt().unwrapOr(0);
        e.button = static_cast<uint8_t>(a[1].asInt().unwrapOr(1));
        e.player2 = a[2].asBool().unwrapOr(false);
        e.down = a[3].asBool().unwrapOr(true);
        e.x = static_cast<float>(a[4].asDouble().unwrapOr(0));
        e.y = static_cast<float>(a[5].asDouble().unwrapOr(0));
        e.left = static_cast<int>(a[6].asInt().unwrapOr(0));
        e.right = static_cast<int>(a[7].asInt().unwrapOr(0));
        e.windowTicks = static_cast<int>(a[8].asInt().unwrapOr(1));
        int flags = static_cast<int>(a[9].asInt().unwrapOr(0));
        e.capped = flags & 1;
        e.limited = flags & 2;
        e.exact = flags & 4;
        e.status = a[10].asInt().unwrapOr(0) == 0 ? EventStatus::Ok : EventStatus::Unreliable;
        r.events.push_back(e);
    }
    return r;
}

bool deleteResults(std::string const& levelKey) {
    std::error_code ec;
    return std::filesystem::remove(resultsPath(levelKey), ec);
}

} // namespace fi
