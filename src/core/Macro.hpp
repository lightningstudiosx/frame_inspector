#pragma once
// Macro (replay) loading. Pure C++ + matjson, no Geode/GD headers, so it can be unit tested on a PC.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fi {

struct Input {
    int64_t frame = 0;   // physics tick the input happens on (bot's own numbering)
    uint8_t button = 1;  // 1 = jump/click, 2 = left, 3 = right (platformer)
    bool player2 = false;
    bool down = true;    // press (true) or release (false)
};

struct Macro {
    std::vector<Input> inputs;  // sorted by frame (stable)
    double tps = 0;             // ticks per second stored in the file (0 = unknown)
    std::string format;         // e.g. "GDR (msgpack)"
    std::string bot;            // bot name if the file says
    std::string name;           // file name without folders
    bool platformer = false;
    size_t practiceDeaths = 0;  // GDR2 "deaths" list (macros recorded with practice deaths may not replay cleanly)
};

struct MacroLoadResult {
    bool ok = false;
    std::string error;
    Macro macro;
};

// Load from bytes; `fileName` is only used to pick a format from the extension when the bytes are ambiguous.
MacroLoadResult parseMacro(std::vector<uint8_t> const& data, std::string const& fileName);
MacroLoadResult loadMacroFile(std::filesystem::path const& path);

// List of extensions the file picker should show.
std::vector<std::string> supportedMacroExtensions();

} // namespace fi
