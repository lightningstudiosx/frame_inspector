#include "Macro.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <matjson.hpp>

namespace fi {

namespace {

// ------------------------------------------------------------------ small helpers
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(std::string const& s, std::string const& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string trim(std::string const& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(std::string const& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

MacroLoadResult fail(std::string msg) {
    MacroLoadResult r;
    r.ok = false;
    r.error = std::move(msg);
    return r;
}

// Little-endian byte reader with bounds checks.
struct Reader {
    std::vector<uint8_t> const& d;
    size_t pos = 0;
    bool bad = false;

    explicit Reader(std::vector<uint8_t> const& data, size_t start = 0) : d(data), pos(start) {}

    bool has(size_t n) const { return !bad && pos + n <= d.size(); }
    size_t left() const { return pos <= d.size() ? d.size() - pos : 0; }

    template <class T> T le() {
        T v{};
        if (!has(sizeof(T))) { bad = true; return v; }
        std::memcpy(&v, d.data() + pos, sizeof(T));  // all supported platforms are little endian
        pos += sizeof(T);
        return v;
    }
    template <class T> T be() {
        T v{};
        if (!has(sizeof(T))) { bad = true; return v; }
        uint8_t tmp[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); i++) tmp[i] = d[pos + sizeof(T) - 1 - i];
        std::memcpy(&v, tmp, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    uint8_t u8() { return le<uint8_t>(); }
    uint64_t varint() {
        uint64_t result = 0;
        for (int i = 0; i < 10; i++) {
            if (!has(1)) { bad = true; return result; }
            uint8_t b = d[pos++];
            result |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
            if ((b & 0x80) == 0) return result;
        }
        bad = true;
        return result;
    }
    std::string str(size_t n) {
        if (!has(n)) { bad = true; return {}; }
        std::string s(reinterpret_cast<char const*>(d.data() + pos), n);
        pos += n;
        return s;
    }
    std::string varstr() {
        uint64_t n = varint();
        if (n > 0xFFFF) { bad = true; return {}; }
        return str(static_cast<size_t>(n));
    }
    void skip(size_t n) {
        if (!has(n)) { bad = true; pos = d.size(); return; }
        pos += n;
    }
};

double numberOf(matjson::Value const& v, double def = 0) {
    if (v.isNumber()) {
        if (auto d = v.asDouble(); d.isOk()) return d.unwrap();
        if (auto i = v.asInt(); i.isOk()) return static_cast<double>(i.unwrap());
    }
    if (v.isBool()) return v.asBool().unwrapOr(false) ? 1 : 0;
    return def;
}

bool boolOf(matjson::Value const& v, bool def = false) {
    if (v.isBool()) return v.asBool().unwrapOr(def);
    if (v.isNumber()) return numberOf(v) != 0;
    return def;
}

matjson::Value const* field(matjson::Value const& obj, std::initializer_list<char const*> keys) {
    if (!obj.isObject()) return nullptr;
    for (auto k : keys) {
        if (obj.contains(k)) {
            auto r = obj.get(k);
            if (r.isOk()) return &r.unwrap();
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------ msgpack -> matjson (enough for GDR1)
struct MsgPack {
    Reader r;
    int depth = 0;
    explicit MsgPack(std::vector<uint8_t> const& d) : r(d) {}

    matjson::Value readStr(size_t n) { return matjson::Value(r.str(n)); }

    matjson::Value readArr(size_t n) {
        auto arr = matjson::Value::array();
        for (size_t i = 0; i < n && !r.bad; i++) arr.push(read());
        return arr;
    }
    matjson::Value readMap(size_t n) {
        auto obj = matjson::Value::object();
        for (size_t i = 0; i < n && !r.bad; i++) {
            matjson::Value k = read();
            matjson::Value v = read();
            std::string key;
            if (k.isString()) key = k.asString().unwrapOr("");
            else if (k.isNumber()) key = std::to_string(static_cast<long long>(numberOf(k)));
            else continue;
            obj.set(key, std::move(v));
        }
        return obj;
    }

    matjson::Value read() {
        if (++depth > 64) { r.bad = true; --depth; return nullptr; }
        matjson::Value out = nullptr;
        uint8_t t = r.u8();
        if (r.bad) { --depth; return nullptr; }
        if (t <= 0x7f) out = matjson::Value(static_cast<std::intmax_t>(t));
        else if (t >= 0xe0) out = matjson::Value(static_cast<std::intmax_t>(static_cast<int8_t>(t)));
        else if ((t & 0xf0) == 0x80) out = readMap(t & 0x0f);
        else if ((t & 0xf0) == 0x90) out = readArr(t & 0x0f);
        else if ((t & 0xe0) == 0xa0) out = readStr(t & 0x1f);
        else switch (t) {
            case 0xc0: out = nullptr; break;
            case 0xc2: out = false; break;
            case 0xc3: out = true; break;
            case 0xc4: r.skip(r.u8()); break;
            case 0xc5: r.skip(r.be<uint16_t>()); break;
            case 0xc6: r.skip(r.be<uint32_t>()); break;
            case 0xc7: { auto n = r.u8(); r.skip(1 + n); } break;
            case 0xc8: { auto n = r.be<uint16_t>(); r.skip(1 + n); } break;
            case 0xc9: { auto n = r.be<uint32_t>(); r.skip(1 + n); } break;
            case 0xca: out = matjson::Value(static_cast<double>(r.be<float>())); break;
            case 0xcb: out = matjson::Value(r.be<double>()); break;
            case 0xcc: out = matjson::Value(static_cast<std::intmax_t>(r.u8())); break;
            case 0xcd: out = matjson::Value(static_cast<std::intmax_t>(r.be<uint16_t>())); break;
            case 0xce: out = matjson::Value(static_cast<std::intmax_t>(r.be<uint32_t>())); break;
            case 0xcf: out = matjson::Value(static_cast<std::uintmax_t>(r.be<uint64_t>())); break;
            case 0xd0: out = matjson::Value(static_cast<std::intmax_t>(r.be<int8_t>())); break;
            case 0xd1: out = matjson::Value(static_cast<std::intmax_t>(r.be<int16_t>())); break;
            case 0xd2: out = matjson::Value(static_cast<std::intmax_t>(r.be<int32_t>())); break;
            case 0xd3: out = matjson::Value(static_cast<std::intmax_t>(r.be<int64_t>())); break;
            case 0xd4: r.skip(2); break;
            case 0xd5: r.skip(3); break;
            case 0xd6: r.skip(5); break;
            case 0xd7: r.skip(9); break;
            case 0xd8: r.skip(17); break;
            case 0xd9: out = readStr(r.u8()); break;
            case 0xda: out = readStr(r.be<uint16_t>()); break;
            case 0xdb: out = readStr(r.be<uint32_t>()); break;
            case 0xdc: out = readArr(r.be<uint16_t>()); break;
            case 0xdd: out = readArr(r.be<uint32_t>()); break;
            case 0xde: out = readMap(r.be<uint16_t>()); break;
            case 0xdf: out = readMap(r.be<uint32_t>()); break;
            default: r.bad = true; break;
        }
        --depth;
        return out;
    }
};

// ------------------------------------------------------------------ format parsers
MacroLoadResult parseGdrJson(matjson::Value const& root, std::string fmt) {
    MacroLoadResult res;
    auto& m = res.macro;
    m.format = std::move(fmt);
    if (auto fr = field(root, {"framerate", "fps", "tps"})) m.tps = numberOf(*fr, 240);
    else m.tps = 240;
    if (auto bot = field(root, {"bot"})) {
        if (auto n = field(*bot, {"name"}); n && n->isString()) m.bot = n->asString().unwrapOr("");
    }
    if (auto pl = field(root, {"platformer"})) m.platformer = boolOf(*pl);
    auto inputs = field(root, {"inputs"});
    if (!inputs || !inputs->isArray()) return fail("GDR file has no \"inputs\" list");
    for (auto const& in : inputs->asArray().unwrap()) {
        auto fr = field(in, {"frame"});
        if (!fr || !fr->isNumber()) continue;
        Input i;
        i.frame = static_cast<int64_t>(numberOf(*fr));
        if (auto b = field(in, {"btn", "button"})) i.button = static_cast<uint8_t>(std::clamp(numberOf(*b, 1), 1.0, 3.0));
        if (auto p = field(in, {"2p", "player2", "p2"})) i.player2 = boolOf(*p);
        if (auto d = field(in, {"down", "hold", "holding"})) i.down = boolOf(*d, true);
        m.inputs.push_back(i);
    }
    res.ok = true;
    return res;
}

MacroLoadResult parseMhrJson(matjson::Value const& root) {
    MacroLoadResult res;
    auto& m = res.macro;
    m.format = "Mega Hack (.mhr.json)";
    m.bot = "Mega Hack";
    auto meta = field(root, {"meta"});
    m.tps = meta ? numberOf(field(*meta, {"fps"}) ? *field(*meta, {"fps"}) : matjson::Value(240.0), 240) : 240;
    auto events = field(root, {"events"});
    if (!events || !events->isArray()) return fail("Mega Hack file has no \"events\" list");
    for (auto const& ev : events->asArray().unwrap()) {
        auto fr = field(ev, {"frame"});
        auto dn = field(ev, {"down"});
        if (!fr || !dn) continue;
        Input i;
        i.frame = static_cast<int64_t>(numberOf(*fr));
        i.down = boolOf(*dn);
        if (auto p = field(ev, {"p2"})) i.player2 = boolOf(*p);
        if (auto b = field(ev, {"btn"})) i.button = static_cast<uint8_t>(std::clamp(numberOf(*b, 1), 1.0, 3.0));
        m.inputs.push_back(i);
    }
    res.ok = true;
    return res;
}

MacroLoadResult parseTasbot(matjson::Value const& root) {
    MacroLoadResult res;
    auto& m = res.macro;
    m.format = "TASBot (.json)";
    m.bot = "TASBot";
    m.tps = numberOf(*field(root, {"fps"}), 240);
    auto events = field(root, {"macro"});
    if (!events || !events->isArray()) return fail("TASBot file has no \"macro\" list");
    int prev[2] = {0, 0};
    for (auto const& ev : events->asArray().unwrap()) {
        auto fr = field(ev, {"frame"});
        if (!fr) continue;
        int64_t frame = static_cast<int64_t>(numberOf(*fr));
        int clicks[2] = {0, 0};
        if (auto p1 = field(ev, {"player_1"})) if (auto c = field(*p1, {"click"})) clicks[0] = static_cast<int>(numberOf(*c));
        if (auto p2 = field(ev, {"player_2"})) if (auto c = field(*p2, {"click"})) clicks[1] = static_cast<int>(numberOf(*c));
        for (int p = 0; p < 2; p++) {
            // 0 = nothing, 1 = click, 2 = release. Two clicks in a row mean there was a release in between.
            if (clicks[p] == 0) { prev[p] = 0; continue; }
            if (clicks[p] == 1 && prev[p] == 1) m.inputs.push_back({frame, 1, p == 1, false});
            m.inputs.push_back({frame, 1, p == 1, clicks[p] == 1});
            prev[p] = clicks[p];
        }
    }
    res.ok = true;
    return res;
}

MacroLoadResult parseGdr2(std::vector<uint8_t> const& data) {
    MacroLoadResult res;
    auto& m = res.macro;
    Reader r(data, 3);
    uint64_t version = r.varint();
    std::string inputTag = r.varstr();
    std::string author = r.varstr();
    std::string description = r.varstr();
    float duration = r.be<float>();
    (void)duration;
    r.varint();                         // gameVersion
    double framerate = r.be<double>();
    r.varint();                         // seed
    r.varint();                         // coins
    r.varint();                         // ldm
    bool platformer = r.varint() != 0;
    m.bot = r.varstr();
    r.varint();                         // bot version
    r.varint();                         // level id
    r.varstr();                         // level name
    uint64_t extSize = r.varint();
    r.skip(static_cast<size_t>(extSize));
    uint64_t deaths = r.varint();
    for (uint64_t i = 0; i < deaths && !r.bad; i++) r.varint();
    r.varint();                         // total input count
    uint64_t p1Inputs = r.varint();
    if (r.bad) return fail("GDR2 header is damaged or cut off");

    bool hasInputExt = !inputTag.empty();
    uint64_t p = 0;
    bool readingP1 = p1Inputs > 0;
    while (r.left() > 0) {
        uint64_t packed = r.varint();
        if (r.bad) break;
        Input in;
        if (platformer) {
            in.frame = static_cast<int64_t>((packed >> 3) + p);
            in.button = static_cast<uint8_t>((packed >> 1) & 3);
            if (in.button == 0) in.button = 1;
            in.down = (packed & 1) != 0;
        } else {
            in.frame = static_cast<int64_t>((packed >> 1) + p);
            in.button = 1;
            in.down = (packed & 1) != 0;
        }
        in.player2 = !readingP1;
        if (hasInputExt) {
            uint64_t n = r.varint();
            r.skip(static_cast<size_t>(n));
        }
        m.inputs.push_back(in);
        p = static_cast<uint64_t>(in.frame);
        if (readingP1 && p1Inputs > 0) {
            if (--p1Inputs == 0) { readingP1 = false; p = 0; }
        }
    }
    m.format = "GDR2 (v" + std::to_string(version) + ")";
    m.tps = framerate > 0 ? framerate : 240;
    m.platformer = platformer;
    m.practiceDeaths = static_cast<size_t>(deaths);
    (void)author; (void)description;
    res.ok = true;
    return res;
}

MacroLoadResult parseZbf(std::vector<uint8_t> const& data) {
    MacroLoadResult res;
    auto& m = res.macro;
    Reader r(data);
    float delta = r.le<float>();
    float speed = r.le<float>();
    if (r.bad || delta <= 0) return fail("zBot file is too short");
    if (speed <= 0) speed = 1;
    m.tps = std::round(1.0 / delta / speed);
    m.format = "zBot (.zbf)";
    m.bot = "zBot";
    while (r.left() >= 6) {
        int32_t frame = r.le<int32_t>();
        uint8_t down = r.u8();
        uint8_t p1 = r.u8();
        m.inputs.push_back({frame, 1, p1 != 0x31, down == 0x31});
    }
    res.ok = true;
    return res;
}

MacroLoadResult parseRe3(std::vector<uint8_t> const& data) {
    MacroLoadResult res;
    auto& m = res.macro;
    Reader r(data);
    float fps = r.le<float>();
    uint32_t p1f = r.le<uint32_t>(), p2f = r.le<uint32_t>(), p1i = r.le<uint32_t>(), p2i = r.le<uint32_t>();
    if (r.bad) return fail("ReplayEngine file is too short");
    size_t need = static_cast<size_t>(p1f + p2f) * 32 + static_cast<size_t>(p1i + p2i) * 16;
    if (r.left() < need) return fail("ReplayEngine file is cut off (or a different version)");
    r.skip(static_cast<size_t>(p1f + p2f) * 32);  // physics snapshots, not needed
    for (uint32_t s = 0; s < 2; s++) {
        uint32_t n = s == 0 ? p1i : p2i;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t frame = r.le<uint32_t>();
            uint8_t down = r.u8();
            r.skip(3);
            int32_t button = r.le<int32_t>();
            r.u8();  // player1 flag (section already tells us)
            r.skip(3);
            m.inputs.push_back({frame, static_cast<uint8_t>(std::clamp(button, 1, 3)), s == 1, down != 0});
        }
    }
    m.tps = fps > 0 ? fps : 240;
    m.format = "ReplayEngine 3 (.re3)";
    m.bot = "GDH";
    res.ok = true;
    return res;
}

MacroLoadResult parseSlc(std::vector<uint8_t> const& data) {
    if (data.size() >= 4 && (std::memcmp(data.data(), "SILL", 4) == 0 || std::memcmp(data.data(), "SLC3", 4) == 0))
        return fail("This is a newer Silicate file (slc2/slc3), which isn't supported yet. Export it as .gdr instead.");
    MacroLoadResult res;
    auto& m = res.macro;
    Reader r(data);
    double fps = r.le<double>();
    uint32_t n = r.le<uint32_t>();
    if (r.bad || r.left() < static_cast<size_t>(n) * 4) return fail("Silicate file is cut off");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = r.le<uint32_t>();
        uint8_t btn = static_cast<uint8_t>((a & 0b0110) >> 1);
        m.inputs.push_back({static_cast<int64_t>(a >> 4), btn == 0 ? uint8_t(1) : btn, (a & 0b1000) != 0, (a & 1) != 0});
    }
    m.tps = fps > 0 ? fps : 240;
    m.format = "Silicate (.slc)";
    m.bot = "Silicate";
    res.ok = true;
    return res;
}

MacroLoadResult parsePlainText(std::string const& text) {
    MacroLoadResult res;
    auto& m = res.macro;
    std::istringstream ss(text);
    std::string line;
    if (!std::getline(ss, line)) return fail("Text macro is empty");
    try { m.tps = std::stod(trim(line)); } catch (...) { return fail("First line of a .txt macro must be the TPS"); }
    while (std::getline(ss, line)) {
        std::istringstream ls(trim(line));
        double frame; int down, button, p1;
        if (!(ls >> frame >> down >> button >> p1)) continue;
        m.inputs.push_back({static_cast<int64_t>(frame), static_cast<uint8_t>(std::clamp(button, 1, 3)), p1 == 0, down == 1});
    }
    m.format = "Plain text (.txt)";
    res.ok = true;
    return res;
}

MacroLoadResult parseXd(std::string const& text) {
    MacroLoadResult res;
    auto& m = res.macro;
    m.tps = 240;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.empty()) continue;
        auto parts = split(line, '|');
        if (parts.size() == 1) {
            try { m.tps = std::stod(parts[0]); } catch (...) {}
            continue;
        }
        if (parts.size() < 4) continue;
        try {
            Input in;
            in.frame = std::stoll(parts[0]);
            in.down = std::stoi(parts[1]) == 1;
            int b = std::stoi(parts[2]);
            in.button = static_cast<uint8_t>(b == 2 || b == 3 ? b : 1);
            in.player2 = std::stoi(parts[3]) != 1;
            m.inputs.push_back(in);
        } catch (...) {}
    }
    m.format = "xdBot old (.xd)";
    m.bot = "xdBot";
    res.ok = true;
    return res;
}

MacroLoadResult parseJsonText(std::string const& text) {
    auto parsed = matjson::parse(text);
    if (parsed.isErr()) return fail("Couldn't read the JSON in this file");
    auto root = parsed.unwrap();
    if (root.contains("meta") && root.contains("events")) return parseMhrJson(root);
    if (root.contains("macro") && root.contains("fps")) return parseTasbot(root);
    if (root.contains("inputs")) return parseGdrJson(root, "GDR (json)");
    return fail("This JSON file isn't a macro format I know (GDR, Mega Hack or TASBot)");
}

} // namespace

std::vector<std::string> supportedMacroExtensions() {
    return {"*.gdr", "*.gdr2", "*.json", "*.zbf", "*.re3", "*.slc", "*.txt", "*.xd"};
}

MacroLoadResult parseMacro(std::vector<uint8_t> const& data, std::string const& fileName) {
    std::string name = lower(fileName);
    MacroLoadResult res;
    if (data.empty()) return fail("The file is empty");

    size_t first = 0;
    while (first < data.size() && std::isspace(data[first])) first++;
    bool looksJson = first < data.size() && (data[first] == '{' || data[first] == '[');

    if (data.size() >= 3 && std::memcmp(data.data(), "GDR", 3) == 0) res = parseGdr2(data);
    else if (looksJson) res = parseJsonText(std::string(data.begin(), data.end()));
    else if (endsWith(name, ".zbf")) res = parseZbf(data);
    else if (endsWith(name, ".re3")) res = parseRe3(data);
    else if (endsWith(name, ".slc")) res = parseSlc(data);
    else if (endsWith(name, ".txt")) res = parsePlainText(std::string(data.begin(), data.end()));
    else if (endsWith(name, ".xd")) res = parseXd(std::string(data.begin(), data.end()));
    else if (endsWith(name, ".gdr") || (data[0] & 0xf0) == 0x80 || data[0] == 0xde || data[0] == 0xdf) {
        MsgPack mp(data);
        auto root = mp.read();
        if (mp.r.bad || !root.isObject()) return fail("Couldn't read this .gdr file (not GDR msgpack/json/GDR2)");
        res = parseGdrJson(root, "GDR (msgpack)");
    }
    else if (endsWith(name, ".mhr")) return fail("Binary Mega Hack (.mhr) isn't supported. Save it as .mhr.json or .gdr.");
    else return fail("Unknown macro format. Supported: .gdr .gdr2 .json (GDR / Mega Hack / TASBot) .zbf .re3 .slc .txt .xd");

    if (!res.ok) return res;
    auto& in = res.macro.inputs;
    in.erase(std::remove_if(in.begin(), in.end(), [](Input const& i) { return i.frame < 0; }), in.end());
    std::stable_sort(in.begin(), in.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });
    if (in.empty()) return fail("The macro has no inputs in it");
    if (!(res.macro.tps > 0) || !std::isfinite(res.macro.tps)) res.macro.tps = 240;
    for (auto& i : in) if (i.button == 2 || i.button == 3) res.macro.platformer = true;
    res.macro.name = fileName;
    return res;
}

MacroLoadResult loadMacroFile(std::filesystem::path const& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("Couldn't open the file");
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseMacro(data, path.filename().string());
}

} // namespace fi
