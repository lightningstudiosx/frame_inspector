#pragma once
// The frame-window scanner. Pure C++ (no Geode headers): the game is reached through the Backend interface,
// so the whole search can be tested on a PC against a fake game.
//
// How a scan works
//   1. Reference run: replay the macro from the start (trying frame offsets 0, +1, -1, +2, -2 because bots
//      number their frames differently) until the level is beaten. Every tick's player position/velocity is
//      stored so later runs can be checked against it.
//   2. For every input (press and release) in order:
//        - get a save state shortly before it (game checkpoint; verified against the reference run),
//        - check that replaying from the save state reproduces the reference exactly (if not, this input is
//          tested with full replays from the level start instead - slower but always exact),
//        - move ONLY that input later/earlier by N ticks (all other inputs keep their timing) and see if the
//          player is still alive when the next input of that player happens. Galloping + binary search finds
//          the last surviving shift on each side.
//      window (ticks) = rightmost alive shift - leftmost alive shift + 1, capped at the chosen maximum.
//   3. frames = ticks * FPS / TPS (so 1 tick at 240 TPS / 60 FPS = 0.25 frames).
#include <cstdint>
#include <string>
#include <vector>

#include "Macro.hpp"

namespace fi {

struct PlayerSample {
    float x = 0, y = 0;   // physics position
    double yv = 0;        // y velocity
    float vx = 0, vy = 0; // node position (where a marker should be drawn)
};

struct TickSample {
    PlayerSample p1, p2;
    bool dual = false;
    bool valid = false;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void restartFromStart() = 0;        // back to the level start (progress 0)
    virtual bool makeCheckpoint() = 0;          // save the current state (replaces any previous one)
    virtual bool restoreCheckpoint() = 0;       // load it back; false if impossible
    virtual int64_t progress() const = 0;       // game tick counter
    virtual void stepTick() = 0;                // run exactly one physics tick (calls Engine::applyInputs inside)
    virtual TickSample sample() const = 0;
    virtual bool isDead() const = 0;
    virtual bool isComplete() const = 0;
    virtual void clearFlags() = 0;              // clear dead/complete flags
    virtual void sendInput(uint8_t button, bool player2, bool down) = 0;
    virtual void releaseAllInputs() = 0;
};

struct ScanConfig {
    double tps = 240;
    double fps = 60;
    double maxFrames = 16;           // biggest window worth measuring; bigger shows as "16+"
    bool exactOnly = false;          // never use save states (slow, but immune to checkpoint bugs)
    bool includeReleases = true;
    int extraTicks = 0;              // must also survive this many ticks after the next input
    int heldLookback = 480;          // look this far back for a save point where no button is held
    std::vector<int> offsets = {0, 1, -1, 2, -2};
};

enum class EventStatus : uint8_t { Ok = 0, Unreliable = 1 };

struct EventResult {
    int64_t frame = 0;          // tick of the input (macro frame + detected offset)
    uint8_t button = 1;
    bool player2 = false;
    bool down = true;
    float x = 0, y = 0;         // where the player was when the input happened
    int left = 0;               // furthest earlier shift (<= 0) that still survives
    int right = 0;              // furthest later shift (>= 0) that still survives
    int windowTicks = 0;        // right - left + 1
    bool capped = false;        // reached the max window: shown as "N+"
    bool limited = false;       // couldn't move further without passing a neighbouring input of the same button
    bool exact = false;         // measured with full replays from the start
    EventStatus status = EventStatus::Ok;
};

struct ScanResult {
    std::vector<EventResult> events;
    double tps = 240, fps = 60;
    int maxTicks = 64;
    int offset = 0;
    int64_t endProgress = 0;
    bool endDetected = true;
    int unreliable = 0;
    int exactEvents = 0;
};

class Engine {
public:
    enum class Phase { Idle, Reference, Events, Done, Failed };

    void begin(Macro const& macro, ScanConfig const& cfg);
    void cancel();

    bool active() const { return m_phase == Phase::Reference || m_phase == Phase::Events; }
    Phase phase() const { return m_phase; }
    std::string const& error() const { return m_error; }
    ScanResult const& result() const { return m_result; }

    // Advance the scan by one step (one restore or one tick). Call repeatedly within a time budget.
    void pump(Backend& b);
    // Called by the backend from inside stepTick (GD: right after processCommands) with the current tick.
    void applyInputs(Backend& b, int64_t progress);

    // Status for the UI
    double fraction() const;
    std::string statusText() const;
    uint64_t ticksSimulated() const { return m_ticks; }
    uint64_t restores() const { return m_restores; }

private:
    enum class RunKind { Reference, Advance, Test };
    enum class Outcome { Survived, Complete, Dead, Mismatch, Reached, Timeout, RestoreFailed };
    enum class Step { Prepare, Advance, Validate, Right, Left, Finish };

    struct Run {
        bool active = false;
        RunKind kind = RunKind::Test;
        bool compare = false;
        bool record = false;
        bool fromStart = true;
        int64_t endProgress = 0;     // stop (survived) when progress >= this
        int64_t targetBoundary = 0;  // Advance: stop when inputs below this tick are applied
        int64_t maxTicks = 0;
        int64_t compareUntil = -1;   // also check against the reference up to this progress (before the shift kicks in)
        int64_t ticks = 0;
        int shift = 0;
    };

    struct Ev {
        size_t inputIndex = 0;       // index in m_base
        int64_t t = 0;
        int lo = 0, hi = 0;          // allowed shift range (clamped by neighbours of the same button + cap)
        bool loLimitedByNeighbour = false, hiLimitedByNeighbour = false;
        int64_t nextSamePlayer = 0;  // frame of next input of the same player (or end)
        int64_t boundary = 0;        // save state boundary used for this event
    };

    // setup helpers
    void buildBase(int offset);
    void buildEvents();
    bool anyHeldBefore(int64_t boundary) const;
    void heldBefore(std::vector<Input> const& list, int64_t boundary, bool held[2][4]) const;

    // run helpers
    void startReference(Backend& b);
    void startAdvance(Backend& b, bool fromStart);
    void startTest(Backend& b, int shift, bool compare);
    bool restoreFor(Backend& b, bool fromStart);
    void positionCursor(int64_t boundary);
    void finishRun(Backend& b, Outcome o);
    bool matchesRef(int64_t progress, TickSample const& s) const;
    int64_t testEnd(int shift) const;

    // event state machine
    void scheduleEvent(Backend& b);
    void onEventRun(Backend& b, Outcome o);
    void nextProbe(Backend& b);
    void finishEvent(EventStatus st);

    Phase m_phase = Phase::Idle;
    std::string m_error;
    Macro m_macro;
    ScanConfig m_cfg;
    int m_maxTicks = 64;

    std::vector<Input> m_base;     // macro inputs with the offset applied (sorted)
    std::vector<Input> m_active;   // what is being fed to the game right now
    size_t m_cursor = 0;
    int64_t m_lastProgress = 0;    // inputs with frame < this have been sent

    // reference
    size_t m_offsetTry = 0;
    int m_offset = 0;
    std::vector<TickSample> m_ref; // indexed by progress
    int64_t m_endProgress = 0;
    bool m_endDetected = true;
    int64_t m_lastInputFrame = 0;
    std::string m_refFailInfo;

    // save state
    bool m_haveCp = false;
    int64_t m_cpBoundary = 0;
    int64_t m_cpProgress = 0;

    // events
    std::vector<Ev> m_events;
    size_t m_ev = 0;
    Step m_step = Step::Prepare;
    bool m_exact = false;          // current event uses full replays
    bool m_advanceFromStart = false;
    bool m_validatedFromCp = false;
    int m_dir = 1;                 // +1 right search, -1 left search
    int m_cap = 0;                 // furthest shift to try in this direction (>= 0, magnitude)
    int m_lastGood = 0;            // magnitude
    int m_firstBad = -1;           // magnitude, -1 = none yet
    int m_stepSize = 1;
    int m_probe = 0;               // magnitude being tested
    int m_right = 0, m_left = 0;   // results (magnitudes)

    Run m_run;
    ScanResult m_result;

    uint64_t m_ticks = 0;
    uint64_t m_restores = 0;
    int m_consecutiveCpFailures = 0;
};

} // namespace fi
