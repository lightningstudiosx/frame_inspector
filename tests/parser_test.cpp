#include "../src/core/Macro.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <matjson.hpp>
#include <sstream>
#include <tuple>
using namespace fi;
int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "tests/macros";
    std::ifstream ef(dir + "/expected.json");
    std::stringstream ss; ss << ef.rdbuf();
    auto exp = matjson::parse(ss.str()).unwrap();
    std::vector<std::tuple<int64_t, int, bool, bool>> want;
    for (auto& e : exp.asArray().unwrap())
        want.emplace_back(e[0].asInt().unwrap(), (int)e[1].asInt().unwrap(), e[2].asBool().unwrap(), e[3].asBool().unwrap());
    std::sort(want.begin(), want.end());
    int bad = 0;
    for (auto name : {"a.gdr.json", "b.gdr", "c.gdr2", "c2.gdr2", "d.mhr.json", "e.zbf", "f.re3", "g.slc", "h.txt", "i.xd", "j.json"}) {
        auto r = loadMacroFile(dir + "/" + name);
        if (!r.ok) { std::printf("%-12s FAILED: %s\n", name, r.error.c_str()); bad++; continue; }
        std::vector<std::tuple<int64_t, int, bool, bool>> got;
        for (auto& i : r.macro.inputs) got.emplace_back(i.frame, i.button, i.player2, i.down);
        std::sort(got.begin(), got.end());
        bool same = got == want && r.macro.tps == 240;
        std::printf("%-12s %-24s tps %.0f inputs %zu bot '%s' %s\n", name, r.macro.format.c_str(), r.macro.tps, got.size(),
                    r.macro.bot.c_str(), same ? "OK" : "MISMATCH");
        if (!same) {
            bad++;
            for (auto& [f, b, p, d] : got) std::printf("   %lld b%d p2=%d down=%d\n", (long long)f, b, p, d);
        }
    }
    // garbage must fail cleanly, not crash
    std::vector<uint8_t> junk(300); for (size_t i = 0; i < junk.size(); i++) junk[i] = (uint8_t)(i * 37 + 11);
    for (auto n : {"x.gdr", "x.gdr2", "x.zbf", "x.re3", "x.slc", "x.json", "x.txt", "x.xd", "x.bin"}) {
        auto r = parseMacro(junk, n);
        std::printf("junk %-7s -> %s\n", n, r.ok ? ("ok?? " + std::to_string(r.macro.inputs.size())).c_str() : r.error.c_str());
    }
    std::vector<uint8_t> gdrJunk = {'G','D','R', 2, 0xff, 0xff, 0xff};
    auto r = parseMacro(gdrJunk, "y.gdr");
    std::printf("cut GDR2 -> %s\n", r.ok ? "ok??" : r.error.c_str());
    std::printf(bad ? "\n%d FAILED\n" : "\nall formats OK\n", bad);
    return bad;
}
