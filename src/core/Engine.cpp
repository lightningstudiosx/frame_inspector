#include "Engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace fi {

namespace {
constexpr int64_t kInf = std::numeric_limits<int64_t>::max() / 4;

int buttonSlot(uint8_t b) { return b <= 3 ? b : 1; }
} // namespace

// ------------------------------------------------------------------ setup

void Engine::begin(Macro const& macro, ScanConfig const& cfg) {
    m_macro = macro;
    m_cfg = cfg;
    if (!(m_cfg.tps > 0)) m_cfg.tps = 240;
    if (!(m_cfg.fps > 0)) m_cfg.fps = 60;
    if (!(m_cfg.maxFrames > 0)) m_cfg.maxFrames = 16;
    if (m_cfg.offsets.empty()) m_cfg.offsets = {0};
    m_maxTicks = std::max(1, static_cast<int>(std::ceil(m_cfg.maxFrames * m_cfg.tps / m_cfg.fps - 1e-9)));

    m_phase = m_macro.inputs.empty() ? Phase::Failed : Phase::Reference;
    m_error = m_macro.inputs.empty() ? "The macro has no inputs" : "";
    m_offsetTry = 0;
    m_offset = 0;
    m_ref.clear();
    m_events.clear();
    m_ev = 0;
    m_step = Step::Prepare;
    m_haveCp = false;
    m_run = {};
    m_result = {};
    m_ticks = 0;
    m_restores = 0;
    m_consecutiveCpFailures = 0;
    m_refFailInfo.clear();
    m_lastInputFrame = m_macro.inputs.empty() ? 0 : m_macro.inputs.back().frame;
}

void Engine::cancel() {
    m_run.active = false;
    if (active()) m_phase = Phase::Idle;
}

void Engine::buildBase(int offset) {
    m_base.clear();
    m_base.reserve(m_macro.inputs.size());
    for (auto in : m_macro.inputs) {
        in.frame = std::max<int64_t>(0, in.frame + offset);
        m_base.push_back(in);
    }
    std::stable_sort(m_base.begin(), m_base.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });
}

void Engine::heldBefore(std::vector<Input> const& list, int64_t boundary, bool held[2][4]) const {
    for (int p = 0; p < 2; p++) for (int k = 0; k < 4; k++) held[p][k] = false;
    for (auto const& in : list) {
        if (in.frame >= boundary) break;
        held[in.player2 ? 1 : 0][buttonSlot(in.button)] = in.down;
    }
}

bool Engine::anyHeldBefore(int64_t boundary) const {
    bool held[2][4];
    heldBefore(m_base, boundary, held);
    for (int p = 0; p < 2; p++) for (int k = 1; k < 4; k++) if (held[p][k]) return true;
    return false;
}

void Engine::buildEvents() {
    m_events.clear();
    int64_t lastUseful = m_endProgress - 1;
    for (size_t i = 0; i < m_base.size(); i++) {
        auto const& in = m_base[i];
        if (in.frame >= lastUseful) continue;
        if (!m_cfg.includeReleases && !in.down) continue;

        Ev e;
        e.inputIndex = i;
        e.t = in.frame;

        // neighbours with the same player + button: the input may not jump over them
        int64_t prevSame = -1, nextSame = -1, nextPlayer = -1;
        for (size_t j = i; j-- > 0;) {
            if (m_base[j].player2 == in.player2 && m_base[j].button == in.button) { prevSame = m_base[j].frame; break; }
        }
        for (size_t j = i + 1; j < m_base.size(); j++) {
            auto const& o = m_base[j];
            if (o.player2 != in.player2) continue;
            if (nextPlayer < 0 && o.frame > in.frame) nextPlayer = o.frame;
            if (nextSame < 0 && o.button == in.button) nextSame = o.frame;
            if (nextSame >= 0 && nextPlayer >= 0) break;
        }

        e.lo = -(m_maxTicks - 1);
        if (prevSame >= 0) {
            int lim = static_cast<int>(std::max<int64_t>(prevSame + 1 - e.t, -1000000));
            if (lim > e.lo) { e.lo = lim; e.loLimitedByNeighbour = true; }
        }
        if (e.t + e.lo < 0) { e.lo = static_cast<int>(-e.t); e.loLimitedByNeighbour = true; }

        e.hi = m_maxTicks - 1;
        int64_t hiLimit = (nextSame >= 0 ? nextSame - 1 : kInf);
        hiLimit = std::min<int64_t>(hiLimit, m_endProgress - 2);
        if (hiLimit - e.t < e.hi) { e.hi = static_cast<int>(std::max<int64_t>(hiLimit - e.t, 0)); e.hiLimitedByNeighbour = true; }

        e.lo = std::min(e.lo, 0);
        e.hi = std::max(e.hi, 0);
        e.nextSamePlayer = nextPlayer >= 0 ? nextPlayer : m_endProgress;
        m_events.push_back(e);
    }
}

// ------------------------------------------------------------------ input feeding

void Engine::positionCursor(int64_t boundary) {
    m_lastProgress = boundary;
    m_cursor = static_cast<size_t>(std::lower_bound(m_active.begin(), m_active.end(), boundary,
        [](Input const& a, int64_t f) { return a.frame < f; }) - m_active.begin());
}

void Engine::applyInputs(Backend& b, int64_t progress) {
    if (!active() || !m_run.active) return;
    if (progress < m_lastProgress) return;
    m_lastProgress = progress;
    while (m_cursor < m_active.size() && m_active[m_cursor].frame < progress) {
        auto const& in = m_active[m_cursor++];
        b.sendInput(in.button, in.player2, in.down);
    }
}

// ------------------------------------------------------------------ runs

bool Engine::matchesRef(int64_t progress, TickSample const& s) const {
    if (progress < 0 || progress >= static_cast<int64_t>(m_ref.size())) return true;
    auto const& r = m_ref[static_cast<size_t>(progress)];
    if (!r.valid) return true;
    if (r.p1.x != s.p1.x || r.p1.y != s.p1.y || r.p1.yv != s.p1.yv) return false;
    if (r.dual != s.dual) return false;
    if (r.dual && (r.p2.x != s.p2.x || r.p2.y != s.p2.y || r.p2.yv != s.p2.yv)) return false;
    return true;
}

bool Engine::restoreFor(Backend& b, bool fromStart) {
    b.clearFlags();
    if (fromStart) {
        b.restartFromStart();
        m_restores++;
        b.clearFlags();
        positionCursor(0);
        return true;
    }
    if (!m_haveCp) return false;
    if (!b.restoreCheckpoint()) return false;
    m_restores++;
    b.clearFlags();
    if (b.progress() != m_cpProgress) return false;
    positionCursor(m_cpBoundary);
    // buttons that should be held at this moment get pressed again
    bool held[2][4];
    heldBefore(m_active, m_cpBoundary, held);
    b.releaseAllInputs();
    for (int p = 0; p < 2; p++)
        for (int k = 1; k < 4; k++)
            if (held[p][k]) b.sendInput(static_cast<uint8_t>(k), p == 1, true);
    return true;
}

void Engine::startReference(Backend& b) {
    if (m_offsetTry >= m_cfg.offsets.size()) {
        m_phase = Phase::Failed;
        m_error = "The macro doesn't beat this level when replayed here. " + m_refFailInfo +
                  " Check the TPS matches the macro and that it was recorded on this exact level (and with no practice deaths).";
        return;
    }
    m_offset = m_cfg.offsets[m_offsetTry];
    buildBase(m_offset);
    m_active = m_base;
    m_ref.clear();
    m_ref.reserve(static_cast<size_t>(std::max<int64_t>(0, m_lastInputFrame + 4 * static_cast<int64_t>(m_cfg.tps))));
    restoreFor(b, true);
    m_run = {};
    m_run.active = true;
    m_run.kind = RunKind::Reference;
    m_run.record = true;
    m_run.fromStart = true;
    m_run.endProgress = kInf;
    m_run.maxTicks = m_lastInputFrame + m_offset + static_cast<int64_t>(60 * m_cfg.tps) + 10;
    int64_t p0 = b.progress();
    if (p0 >= 0) {
        if (static_cast<int64_t>(m_ref.size()) <= p0) m_ref.resize(static_cast<size_t>(p0 + 1));
        m_ref[static_cast<size_t>(p0)] = b.sample();
        m_ref[static_cast<size_t>(p0)].valid = true;
    }
}

void Engine::startAdvance(Backend& b, bool fromStart) {
    auto& e = m_events[m_ev];
    m_active = m_base;
    m_advanceFromStart = fromStart;
    if (!restoreFor(b, fromStart)) {
        m_advanceFromStart = true;
        restoreFor(b, true);
    }
    m_run = {};
    m_run.active = true;
    m_run.kind = RunKind::Advance;
    m_run.compare = true;
    m_run.fromStart = m_advanceFromStart;
    m_run.targetBoundary = e.boundary;
    m_run.endProgress = kInf;
    m_run.maxTicks = std::max<int64_t>(1, e.boundary - b.progress()) + static_cast<int64_t>(2 * m_cfg.tps) + 10;
    if (m_lastProgress >= e.boundary) { finishRun(b, Outcome::Reached); return; }
    if (!matchesRef(b.progress(), b.sample())) finishRun(b, Outcome::Mismatch);
}

int64_t Engine::testEnd(int shift) const {
    auto const& e = m_events[m_ev];
    int64_t base = std::min<int64_t>(m_endProgress, e.nextSamePlayer + 1 + m_cfg.extraTicks);
    return std::max<int64_t>(base, e.t + shift + 2);
}

void Engine::startTest(Backend& b, int shift, bool compare) {
    auto& e = m_events[m_ev];
    m_active = m_base;
    m_active[e.inputIndex].frame = e.t + shift;
    if (shift != 0)
        std::stable_sort(m_active.begin(), m_active.end(), [](Input const& a, Input const& c) { return a.frame < c.frame; });

    bool fromStart = m_exact;
    bool ok = restoreFor(b, fromStart);
    // a save-state load must land exactly on the reference run, every time
    if (ok && !fromStart && !matchesRef(b.progress(), b.sample())) ok = false;
    if (!ok) {
        // save state unusable: measure this input the slow, exact way
        m_exact = true;
        m_consecutiveCpFailures++;
        restoreFor(b, true);
    }
    m_run = {};
    m_run.active = true;
    m_run.kind = RunKind::Test;
    m_run.compare = compare;
    m_run.compareUntil = e.t + std::min(0, shift);
    m_run.fromStart = m_exact;
    m_run.shift = shift;
    m_run.endProgress = testEnd(shift);
    m_run.maxTicks = std::max<int64_t>(1, m_run.endProgress - b.progress()) + 10;
    if (compare && !matchesRef(b.progress(), b.sample())) finishRun(b, Outcome::Mismatch);
}

void Engine::pump(Backend& b) {
    if (!active()) return;
    if (!m_run.active) {
        if (m_phase == Phase::Reference) startReference(b);
        else scheduleEvent(b);
        return;
    }

    b.stepTick();
    m_ticks++;
    m_run.ticks++;
    int64_t p = b.progress();
    TickSample s = b.sample();
    s.valid = true;

    if (m_run.record && p >= 0) {
        if (static_cast<int64_t>(m_ref.size()) <= p) m_ref.resize(static_cast<size_t>(p + 1));
        m_ref[static_cast<size_t>(p)] = s;
    }
    if (b.isDead()) return finishRun(b, Outcome::Dead);
    if ((m_run.compare || p <= m_run.compareUntil) && !matchesRef(p, s)) return finishRun(b, Outcome::Mismatch);
    if (b.isComplete()) {
        // finishing clearly earlier than the reference run did means the game state is off: don't trust it
        if (m_run.kind != RunKind::Reference && p + static_cast<int64_t>(m_cfg.tps) < m_endProgress)
            return finishRun(b, Outcome::Mismatch);
        return finishRun(b, Outcome::Complete);
    }
    if (m_run.kind == RunKind::Advance && m_lastProgress >= m_run.targetBoundary) return finishRun(b, Outcome::Reached);
    if (m_run.kind == RunKind::Test && p >= m_run.endProgress) return finishRun(b, Outcome::Survived);
    if (m_run.ticks >= m_run.maxTicks) return finishRun(b, Outcome::Timeout);
}

void Engine::finishRun(Backend& b, Outcome o) {
    m_run.active = false;
    if (m_run.kind != RunKind::Reference) {
        onEventRun(b, o);
        return;
    }

    int64_t p = b.progress();
    if (o == Outcome::Dead) {
        char buf[160];
        double pct = m_lastInputFrame > 0 ? 100.0 * static_cast<double>(p) / static_cast<double>(m_lastInputFrame + m_offset) : 0;
        std::snprintf(buf, sizeof(buf), "(Best try: offset %+d died at tick %lld, about %.0f%% through the macro.)",
                      m_offset, static_cast<long long>(p), pct);
        if (m_refFailInfo.empty() || m_offset == 0) m_refFailInfo = buf;
        m_offsetTry++;
        return;  // next pump tries the next offset
    }
    if (o == Outcome::Complete) {
        m_endProgress = p;
        m_endDetected = true;
    } else {
        // never saw the level end; the macro finished and the player is still alive
        m_endProgress = std::min<int64_t>(p, m_lastInputFrame + m_offset + static_cast<int64_t>(2 * m_cfg.tps));
        m_endDetected = false;
    }
    buildEvents();
    m_phase = Phase::Events;
    m_ev = 0;
    m_step = Step::Prepare;
    m_haveCp = false;
}

// ------------------------------------------------------------------ per-input search

void Engine::scheduleEvent(Backend& b) {
    if (m_ev >= m_events.size()) {
        m_result.tps = m_cfg.tps;
        m_result.fps = m_cfg.fps;
        m_result.maxTicks = m_maxTicks;
        m_result.offset = m_offset;
        m_result.endProgress = m_endProgress;
        m_result.endDetected = m_endDetected;
        m_result.unreliable = 0;
        m_result.exactEvents = 0;
        for (auto const& r : m_result.events) {
            if (r.status != EventStatus::Ok) m_result.unreliable++;
            if (r.exact) m_result.exactEvents++;
        }
        m_phase = Phase::Done;
        return;
    }
    auto& e = m_events[m_ev];
    switch (m_step) {
        case Step::Prepare: {
            int64_t target = e.t + e.lo;
            e.boundary = target;
            if (target <= 1) {
                e.boundary = 0;
            } else {
                for (int64_t bd = target; bd >= std::max<int64_t>(2, target - m_cfg.heldLookback); bd--) {
                    if (!anyHeldBefore(bd)) { e.boundary = bd; break; }
                }
            }
            m_right = m_left = 0;
            m_exact = m_cfg.exactOnly || e.boundary <= 1 || m_consecutiveCpFailures >= 8;
            if (m_exact || (m_haveCp && m_cpBoundary == e.boundary)) {
                m_step = Step::Validate;
                startTest(b, 0, true);
                return;
            }
            m_step = Step::Advance;
            startAdvance(b, !(m_haveCp && m_cpBoundary < e.boundary));
            return;
        }
        case Step::Advance:
            startAdvance(b, true);  // only reached when a save-state advance failed: redo it from the start
            return;
        case Step::Validate:
            startTest(b, 0, true);
            return;
        case Step::Right:
            startTest(b, m_probe, false);
            return;
        case Step::Left:
            startTest(b, -m_probe, false);
            return;
        case Step::Finish: {
            EventResult r;
            auto const& in = m_base[e.inputIndex];
            r.frame = in.frame;
            r.button = in.button;
            r.player2 = in.player2;
            r.down = in.down;
            r.left = -m_left;
            r.right = m_right;
            r.windowTicks = m_right + m_left + 1;
            r.capped = r.windowTicks >= m_maxTicks;
            r.limited = (m_right == e.hi && e.hiLimitedByNeighbour) || (m_left == -e.lo && e.loLimitedByNeighbour);
            r.exact = m_exact;
            int64_t at = std::min<int64_t>(e.t + 1, static_cast<int64_t>(m_ref.size()) - 1);
            if (at >= 0 && m_ref[static_cast<size_t>(at)].valid) {
                auto const& smp = m_ref[static_cast<size_t>(at)];
                r.x = in.player2 && smp.dual ? smp.p2.vx : smp.p1.vx;
                r.y = in.player2 && smp.dual ? smp.p2.vy : smp.p1.vy;
            }
            m_result.events.push_back(r);
            m_ev++;
            m_step = Step::Prepare;
            return;
        }
    }
}

void Engine::finishEvent(EventStatus st) {
    auto const& e = m_events[m_ev];
    auto const& in = m_base[e.inputIndex];
    EventResult r;
    r.frame = in.frame;
    r.button = in.button;
    r.player2 = in.player2;
    r.down = in.down;
    r.status = st;
    r.exact = m_exact;
    int64_t at = std::min<int64_t>(e.t + 1, static_cast<int64_t>(m_ref.size()) - 1);
    if (at >= 0 && m_ref[static_cast<size_t>(at)].valid) {
        auto const& smp = m_ref[static_cast<size_t>(at)];
        r.x = in.player2 && smp.dual ? smp.p2.vx : smp.p1.vx;
        r.y = in.player2 && smp.dual ? smp.p2.vy : smp.p1.vy;
    }
    m_result.events.push_back(r);
    m_ev++;
    m_step = Step::Prepare;
}

void Engine::onEventRun(Backend& b, Outcome o) {
    auto& e = m_events[m_ev];
    bool alive = o == Outcome::Survived || o == Outcome::Complete;

    auto setupDirection = [&](int dir) {
        m_dir = dir;
        m_cap = dir > 0 ? e.hi : std::min(-e.lo, (m_maxTicks - 1) - m_right);
        m_lastGood = 0;
        m_firstBad = -1;
        m_stepSize = 1;
        m_probe = 1;
        m_step = dir > 0 ? Step::Right : Step::Left;
    };
    auto finishDirection = [&]() {
        if (m_dir > 0) {
            m_right = m_lastGood;
            setupDirection(-1);
            if (m_cap <= 0) { m_left = 0; m_step = Step::Finish; }
        } else {
            m_left = m_lastGood;
            m_step = Step::Finish;
        }
    };

    switch (m_step) {
        case Step::Advance: {
            if (o == Outcome::Reached && m_lastProgress == e.boundary && b.makeCheckpoint()) {
                m_haveCp = true;
                m_cpBoundary = m_lastProgress;
                m_cpProgress = b.progress();
                m_step = Step::Validate;
                return;
            }
            if (o == Outcome::Reached) {  // couldn't save a state here
                m_haveCp = false;
                m_exact = true;
                m_step = Step::Validate;
                return;
            }
            if (!m_advanceFromStart) {     // drifted after a save-state load: redo from the start
                m_haveCp = false;
                m_step = Step::Advance;
                return;
            }
            // even a clean replay from the start doesn't match: the level isn't deterministic here
            m_haveCp = false;
            m_exact = true;
            m_step = Step::Validate;
            return;
        }
        case Step::Validate: {
            if (alive) {
                if (!m_exact) m_consecutiveCpFailures = 0;
                setupDirection(+1);
                if (m_cap <= 0) finishDirection();
                return;
            }
            if (!m_exact) {
                m_exact = true;
                m_consecutiveCpFailures++;
                m_step = Step::Validate;
                return;
            }
            finishEvent(EventStatus::Unreliable);
            return;
        }
        case Step::Right:
        case Step::Left: {
            if (o == Outcome::Mismatch) {
                // the replay drifted from the reference before this input was even moved
                if (!m_exact) {
                    m_exact = true;              // redo the same probe with a clean replay from the start
                    m_consecutiveCpFailures++;
                    return;
                }
                finishEvent(EventStatus::Unreliable);
                return;
            }
            if (alive) m_lastGood = m_probe;
            else m_firstBad = m_probe;
            if (m_firstBad < 0) {
                if (m_lastGood >= m_cap) { finishDirection(); return; }
                m_stepSize *= 2;
                m_probe = std::min(m_cap, m_lastGood + m_stepSize);
                return;
            }
            if (m_firstBad - m_lastGood <= 1) { finishDirection(); return; }
            m_probe = (m_lastGood + m_firstBad) / 2;
            return;
        }
        default:
            return;
    }
}

// ------------------------------------------------------------------ status

double Engine::fraction() const {
    switch (m_phase) {
        case Phase::Reference: {
            double denom = static_cast<double>(std::max<int64_t>(1, m_lastInputFrame));
            return 0.05 * std::min(1.0, static_cast<double>(m_run.ticks) / denom);
        }
        case Phase::Events:
            return 0.05 + 0.95 * (m_events.empty() ? 1.0 : static_cast<double>(m_ev) / static_cast<double>(m_events.size()));
        case Phase::Done: return 1.0;
        default: return 0.0;
    }
}

std::string Engine::statusText() const {
    char buf[200];
    switch (m_phase) {
        case Phase::Reference:
            std::snprintf(buf, sizeof(buf), "Checking the macro beats the level (offset %+d)...", m_offset);
            return buf;
        case Phase::Events: {
            char const* what = "";
            switch (m_step) {
                case Step::Advance: what = "making save state"; break;
                case Step::Validate: what = m_exact ? "verifying (exact)" : "verifying"; break;
                case Step::Right: what = "testing later"; break;
                case Step::Left: what = "testing earlier"; break;
                default: what = "..."; break;
            }
            std::snprintf(buf, sizeof(buf), "Input %zu / %zu  (%s)", std::min(m_ev + 1, m_events.size()), m_events.size(), what);
            return buf;
        }
        case Phase::Done: return "Done";
        case Phase::Failed: return m_error;
        default: return "";
    }
}

} // namespace fi
