// Native test: runs the real scan Engine against a small deterministic fake game ("wave" corridor),
// and compares every window with a brute-force oracle that replays the whole level from the start
// for every single shift.
#include "../src/core/Engine.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>

using namespace fi;

struct Level {
    int length = 0;
    std::vector<float> lo, hi;  // corridor per x
};

struct State {
    int64_t p = 0;
    float x = 0, y = 0;
    double yv = 0;
    bool hold = false;
    bool dead = false, complete = false;
    int64_t corruptAt = -1;  // hidden state a bad save state can carry: y drifts a few ticks later
};

struct Mock : Backend {
    Level const& lvl;
    Engine* eng = nullptr;
    State s, cp;
    bool haveCp = false;
    // imperfection knobs
    double driftChance = 0;       // chance a restore is slightly wrong
    double lateDriftChance = 0;   // chance a restore is wrong in a way that only shows up later
    bool restoreLosesHold = true; // GD-like: held buttons are not part of the save
    std::mt19937 rng{7};
    std::vector<Input>* direct = nullptr;  // used by the oracle (no engine)
    size_t directCursor = 0;

    explicit Mock(Level const& l) : lvl(l) {}

    void restartFromStart() override { s = State{}; }
    int64_t cpFlaw = -1;  // a bad save state is bad the same way every time it's loaded (the game is deterministic)
    bool makeCheckpoint() override {
        cp = s; haveCp = true;
        std::uniform_real_distribution<double> u(0, 1);
        cpFlaw = u(rng) < lateDriftChance ? 1 + static_cast<int64_t>(u(rng) * 30) : -1;
        return true;
    }
    bool restoreCheckpoint() override {
        if (!haveCp) return false;
        s = cp;
        if (restoreLosesHold) s.hold = false;
        std::uniform_real_distribution<double> u(0, 1);
        if (u(rng) < driftChance) s.y += 0.001f;
        if (cpFlaw > 0) s.corruptAt = s.p + cpFlaw;
        return true;
    }
    int64_t progress() const override { return s.p; }
    void stepTick() override {
        s.p++;
        if (eng) eng->applyInputs(*this, s.p);
        if (direct) {
            while (directCursor < direct->size() && (*direct)[directCursor].frame < s.p) {
                auto& in = (*direct)[directCursor++];
                sendInput(in.button, in.player2, in.down);
            }
        }
        if (s.p == s.corruptAt) s.y += 0.001f;
        s.yv = s.hold ? 1 : -1;
        s.y += static_cast<float>(s.yv);
        s.x += 1;
        int xi = static_cast<int>(s.x);
        if (xi >= lvl.length) { s.complete = true; return; }
        if (s.y < lvl.lo[xi] || s.y > lvl.hi[xi]) s.dead = true;
    }
    TickSample sample() const override {
        TickSample t;
        t.p1 = {s.x, s.y, s.yv, s.x, s.y};
        t.valid = true;
        return t;
    }
    bool isDead() const override { return s.dead; }
    bool isComplete() const override { return s.complete; }
    void clearFlags() override { s.dead = false; s.complete = false; }
    void sendInput(uint8_t, bool, bool down) override { s.hold = down; }
    void releaseAllInputs() override { s.hold = false; }
};

// Build a random wave macro and a corridor around its path with random slack.
static void makeLevel(int seed, Level& lvl, std::vector<Input>& inputs, std::vector<int>& slackAtInput) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> gap(3, 40), slack(0, 14);
    int t = 20;
    bool down = true;
    inputs.clear();
    while (t < 2400) {
        inputs.push_back({t, 1, false, down});
        down = !down;
        t += gap(rng);
    }
    lvl.length = t + 30;
    // reference path
    std::vector<float> y(lvl.length + 2, 0);
    {
        float yy = 0;
        bool hold = false;
        size_t c = 0;
        for (int p = 1; p <= lvl.length; p++) {
            while (c < inputs.size() && inputs[c].frame < p) hold = inputs[c++].down;
            yy += hold ? 1.f : -1.f;
            if (p < static_cast<int>(y.size())) y[p] = yy;  // x == p after p ticks
        }
    }
    lvl.lo.assign(lvl.length + 2, -1e9f);
    lvl.hi.assign(lvl.length + 2, 1e9f);
    // put a "spike" between inputs: corridor tightens with a random slack in each segment
    slackAtInput.clear();
    for (size_t i = 0; i + 1 < inputs.size(); i++) {
        int a = static_cast<int>(inputs[i].frame) + 1, b = static_cast<int>(inputs[i + 1].frame) + 1;
        int sl = slack(rng);
        slackAtInput.push_back(sl);
        int mid = (a + b) / 2 + 1;
        if (mid >= 1 && mid < lvl.length) {
            lvl.lo[mid] = y[mid] - sl;
            lvl.hi[mid] = y[mid] + sl;
        }
    }
}

// Brute force: alive until horizon for every shift, contiguous window around 0.
struct Truth { int left = 0, right = 0; };

static bool runDirect(Level const& lvl, std::vector<Input> list, int64_t endP) {
    Mock m(lvl);
    std::stable_sort(list.begin(), list.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });
    m.direct = &list;
    m.restartFromStart();
    for (int guard = 0; guard < 100000; guard++) {
        m.stepTick();
        if (m.isDead()) return false;
        if (m.isComplete()) return true;
        if (m.progress() >= endP) return true;
    }
    return true;
}

int main() {
    int failures = 0, checked = 0;
    for (int seed = 1; seed <= 6; seed++) {
        Level lvl;
        std::vector<Input> inputs;
        std::vector<int> slack;
        makeLevel(seed, lvl, inputs, slack);

        // the "file" stores frames one lower than the game needs (like xdBot vs zBot numbering)
        Macro mac;
        for (auto in : inputs) { in.frame -= 1; mac.inputs.push_back(in); }
        mac.tps = 240;

        for (int mode = 0; mode < 4; mode++) {
            Mock mock(lvl);
            Engine eng;
            mock.eng = &eng;
            ScanConfig cfg;
            cfg.tps = 240; cfg.fps = 60; cfg.maxFrames = 8;  // 32 ticks
            cfg.exactOnly = mode == 1;
            mock.driftChance = mode == 2 ? 0.3 : 0.0;
            mock.lateDriftChance = mode == 3 ? 0.3 : 0.0;
            eng.begin(mac, cfg);
            long guard = 0;
            while (eng.active() && guard++ < 50'000'000) eng.pump(mock);
            if (eng.phase() != Engine::Phase::Done) {
                std::printf("seed %d mode %d: engine failed: %s\n", seed, mode, eng.error().c_str());
                failures++;
                continue;
            }
            auto const& res = eng.result();
            int maxT = res.maxTicks;
            // oracle with the detected offset
            std::vector<Input> base;
            for (auto in : mac.inputs) { in.frame = std::max<int64_t>(0, in.frame + res.offset); base.push_back(in); }
            size_t ri = 0;
            for (size_t i = 0; i < base.size(); i++) {
                if (base[i].frame >= res.endProgress - 1) continue;
                auto const& r = res.events[ri++];
                if (r.frame != base[i].frame) { std::printf("event order mismatch\n"); failures++; break; }
                int64_t prev = i > 0 ? base[i - 1].frame : -1;
                int64_t next = i + 1 < base.size() ? base[i + 1].frame : -1;
                int lo = -(maxT - 1), hi = maxT - 1;
                if (prev >= 0) lo = std::max<int>(lo, static_cast<int>(prev + 1 - base[i].frame));
                if (base[i].frame + lo < 0) lo = static_cast<int>(-base[i].frame);
                int64_t hiLim = next >= 0 ? next - 1 : res.endProgress - 2;
                hiLim = std::min<int64_t>(hiLim, res.endProgress - 2);
                hi = std::min<int>(hi, static_cast<int>(std::max<int64_t>(0, hiLim - base[i].frame)));
                lo = std::min(lo, 0);
                int64_t nextP = next >= 0 ? next : res.endProgress;
                auto alive = [&](int d) {
                    auto list = base;
                    list[i].frame += d;
                    int64_t endP = std::max<int64_t>(std::min<int64_t>(res.endProgress, nextP + 1), base[i].frame + d + 2);
                    return runDirect(lvl, list, endP);
                };
                Truth t;
                while (t.right < hi && alive(t.right + 1)) t.right++;
                int leftCap = std::min(-lo, (maxT - 1) - t.right);
                while (t.left < leftCap && alive(-(t.left + 1))) t.left++;
                int win = t.right + t.left + 1;
                checked++;
                if (r.status != EventStatus::Ok || r.right != t.right || -r.left != t.left || r.windowTicks != win) {
                    if (failures < 15)
                        std::printf("seed %d mode %d input %zu frame %lld: engine [%d,%d]=%d (status %d exact %d) truth [%d,%d]=%d\n",
                                    seed, mode, i, (long long)base[i].frame, r.left, r.right, r.windowTicks, (int)r.status, r.exact,
                                    -t.left, t.right, win);
                    failures++;
                }
            }
            std::printf("seed %d mode %d: offset %+d, %zu inputs, %d exact, ticks %llu, restores %llu\n", seed, mode, res.offset,
                        res.events.size(), res.exactEvents, (unsigned long long)eng.ticksSimulated(), (unsigned long long)eng.restores());
        }
    }
    std::printf("\nchecked %d windows, %d mismatches\n", checked, failures);
    return failures == 0 ? 0 : 1;
}
