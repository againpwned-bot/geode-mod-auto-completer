#pragma once

#include <Geode/loader/Mod.hpp>

#include "planner/Planner.hpp"

#include <string>

// Typed access to the mod settings declared in mod.json. The in-game menu reads and
// writes the same Geode settings, so both stay in sync.
namespace ac::settings {

inline bool getBool(char const* key) {
    return geode::Mod::get()->getSettingValue<bool>(key);
}

inline void setBool(char const* key, bool value) {
    geode::Mod::get()->setSettingValue<bool>(key, value);
}

inline int64_t getInt(char const* key) {
    return geode::Mod::get()->getSettingValue<int64_t>(key);
}

inline void setInt(char const* key, int64_t value) {
    geode::Mod::get()->setSettingValue<int64_t>(key, value);
}

inline double getFloat(char const* key) {
    return geode::Mod::get()->getSettingValue<double>(key);
}

inline void setFloat(char const* key, double value) {
    geode::Mod::get()->setSettingValue<double>(key, value);
}

inline bool enabled() { return getBool("enabled"); }
inline bool ignoreInputs() { return getBool("ignore-inputs"); }
inline bool autoCorrect() { return getBool("auto-correct"); }
inline bool pathViewer() { return getBool("path-viewer"); }
inline bool fullPath() { return getBool("path-full"); }
inline bool clickMarkers() { return getBool("path-clicks"); }
inline double pathLookahead() { return getFloat("path-lookahead"); }
inline double pathThickness() { return getFloat("path-thickness"); }
inline bool showHud() { return getBool("show-hud"); }
inline int64_t computeBudget() { return getInt("compute-budget"); }

inline char const* styleName(PlayStyle style) {
    switch (style) {
        case PlayStyle::Default: return "Default";
        case PlayStyle::Perfect: return "Perfect";
        case PlayStyle::Human: return "Human";
        case PlayStyle::Minimal: return "Minimal";
    }
    return "Default";
}

inline PlayStyle playStyle() {
    auto value = geode::Mod::get()->getSettingValue<std::string>("play-style");
    if (value == "Perfect") return PlayStyle::Perfect;
    if (value == "Human") return PlayStyle::Human;
    if (value == "Minimal") return PlayStyle::Minimal;
    return PlayStyle::Default;
}

inline void setPlayStyle(PlayStyle style) {
    geode::Mod::get()->setSettingValue<std::string>("play-style", styleName(style));
}

} // namespace ac::settings
