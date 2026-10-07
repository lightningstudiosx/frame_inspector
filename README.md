# Frame Inspector (Geode mod for GD 2.2081)

You load a macro, the bot plays it and measures **how many frames of leeway every click and release has**. Then you can play the level yourself with those numbers shown on every click. Press **F7** to hide or show them.

- A **marker on every click** with its frame window. Releases get smaller rings. Colours: red = 1 frame, orange = 2, yellow = 3, white = 4, green = 5-6, cyan = 7-8, purple = 9-15, blue = 16+. **Pink means under 1 frame.**
- A **counter in the top left**: 16+, 9-15, 7-8, 5-6, 4, 3, 2, 1, plus a `<1` row.
- In the **top right**: every click that's **harder than 1 frame** (like `0.25f`), with when it happens.
- **Max window**: you pick how far it measures. Anything bigger shows as "16+" (or whatever max you chose).
- **Saved per level.** Next time you open the level, the markers are already there.

---

## 1. Get the .geode file (it has to be compiled once)

The easiest way: GitHub builds it for you, so you don't install anything.

1. Make a new **GitHub repository**. Private is fine.
2. Upload **everything in this folder**, including the hidden `.github` folder. If the website upload skips `.github`, create the file `.github/workflows/multi-platform.yml` by hand and paste the contents in.
3. Open the **Actions** tab and wait for "Build Geode Mod" to finish. It takes about 5–10 minutes.
4. Click the finished run and download **Build Output**. Inside is `kai.frame-inspector.geode`.
5. Put it in your GD mods folder. On Windows that's `Geometry Dash/geode/mods`. Restart GD.

If you already have the Geode CLI and SDK installed, run `geode build` in this folder instead.

## 2. Use it

1. Open the level the macro beats.
2. **Practice mode must be off and no start position may be active**, because the macro has to start from 0%.
3. Press **pause**, then the cyan **FI** button.
4. **Load**: pick the macro file. The TPS gets filled in from the file.
5. Check **TPS**. It must match what the macro was recorded at; 240 is normal.
6. Set **FPS**. This decides what "1 frame" means: at 60 FPS, one frame is 4 ticks at 240 TPS. You can change FPS later without rescanning.
7. Set **Max window (frames)**, for example 16.
8. Press **Scan level**. The game unpauses and you'll see "Frame Inspector is scanning…" with a progress bar. Your clicks are ignored while it works.
9. When it finishes, the level restarts and pauses. Resume and play: the markers are on every click.

**Turning things off:**
- **F7** hides or shows everything. You can change the key in the mod settings.
- In the FI menu, the **Markers / Counter / Hardest** toggles control each part.
- **Clear** deletes the scan for this level.

### Macro formats it reads
| Format | Bots |
|---|---|
| `.gdr` (json or binary), `.gdr.json` | xdBot, zBot, Prism, most Geode bots |
| `.gdr2` | Eclipse |
| `.mhr.json` | Mega Hack |
| `.json` | TASBot |
| `.zbf` | old zBot |
| `.re3` | GDH / ReplayEngine 3 |
| `.slc` | Silicate v1 |
| `.txt` | plain text: first line TPS, then `frame down button player1` |
| `.xd` | old xdBot |

Newer Silicate (slc2/slc3), binary `.mhr`, `.re4` and ToastyReplay aren't supported. Re-save the macro as `.gdr` in your bot.

---

## 3. What the number actually means

For each input, the bot moves **only that one input** earlier and later, one tick at a time, while every other input keeps its original timing. The window is how many tick positions still keep you alive **until your next input happens**. That's the moment you'd correct anyway.

`frames = ticks × FPS ÷ TPS`

So at 240 TPS and 60 FPS:

| Window | Shows as |
|---|---|
| 1 tick | **0.25** |
| 4 ticks | **1** |
| 64 ticks or more | **16+** |

Things to know:
- A press can't be moved past its own release (and the other way round). If that's what limits a window, the window really is that small for that click.
- The first time, the bot checks the macro beats the level. It also works out whether the macro's frame numbers are off by one or two, because different bots count differently.
- To be fast, it uses the game's save states (the same system practice checkpoints use). Every save state is checked against a clean replay. If one doesn't match exactly, that input is measured with full replays from the start instead. This is slower, but always exact.
- A **`?` marker** means the bot couldn't reproduce that moment exactly, even from the start. That usually means random triggers in the level.

### Things that make scans fail or go weird
- **Wrong TPS** → "The macro doesn't beat this level when replayed here".
- **A macro recorded with practice deaths** (Eclipse shows "has practice deaths") might not replay cleanly.
- **Long levels with lots of `exact` inputs are slow.** If a scan crawls, close other mods that hook physics, such as other bots or Click Between Frames.
- **If GD crashes when a scan starts**, tick **Exact (slow)** in the FI menu. That mode never uses save states.
- **If the scan seems different from the real game**, turn off **Fast scan** in the mod settings. Fast scan skips drawing level objects during the scan.

The bot's attempts and jumps are not added to your stats. A bot-made run can never complete the level for you: completions are blocked until the level has been restarted cleanly.

## Honest status
- **Has not been run in GD yet.** I don't have the game here.
- **Compiles cleanly.** All the mod code passes a full compiler check against the real Geode 5.10.1 headers and the GD 2.2081 bindings.
- **The scan logic is tested.** The macro readers and the window search ran on a PC against a fake deterministic game:
  - 2,700+ windows were checked against a brute-force answer, with zero mismatches.
  - This included cases with deliberately broken save states.
  - Every macro format was round-trip tested.
- **The part most likely to need a fix** is how the game behaves when the bot restarts and loads save states mid-level. If something breaks, open Geode → Frame Inspector → logs and send me the error and what happened.

Files: `src/core` is the engine and the macro readers (plain C++). `src/Scan.cpp` connects them to the game. `src/Overlay.cpp` draws the markers and counters. `src/Popup.cpp` is the menu. `tests/` has the PC tests.
