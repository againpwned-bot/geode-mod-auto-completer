#include "Menu.hpp"

#include "../Settings.hpp"
#include "../bot/Bot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/string.hpp>
#include <imgui-cocos.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <string>

using namespace geode::prelude;

namespace ac::menu {

namespace {

bool s_open = false;
float s_scale = 1.f;

ImVec4 const kAccent = { 0.53f, 0.36f, 1.f, 1.f };
ImVec4 const kAccentDim = { 0.36f, 0.24f, 0.72f, 1.f };
ImVec4 const kGood = { 0.35f, 0.95f, 0.5f, 1.f };
ImVec4 const kWarn = { 1.f, 0.78f, 0.25f, 1.f };
ImVec4 const kBad = { 1.f, 0.4f, 0.4f, 1.f };
ImVec4 const kMuted = { 0.62f, 0.62f, 0.7f, 1.f };

constexpr float kColumnWidth = 250.f;

float uiScale() {
    auto* view = CCDirector::sharedDirector()->getOpenGLView();
    if (!view) return 1.f;
    float const height = view->getFrameSize().height;
    return std::clamp(height / 1080.f, 0.8f, 2.5f);
}

ImVec4 stateColor(BotState state) {
    switch (state) {
        case BotState::Playing:
        case BotState::Finished: return kGood;
        case BotState::Died: return kWarn;
        case BotState::Unsupported: return kBad;
        case BotState::Off: return kMuted;
    }
    return kMuted;
}

void tooltip(char const* text) {
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
}

// Label on the left, pill switch on the right, the whole row is clickable.
bool toggleSwitch(char const* label, bool* value) {
    float const rowHeight = ImGui::GetFrameHeight();
    float const width = std::max(ImGui::GetContentRegionAvail().x, 1.f);
    ImVec2 const pos = ImGui::GetCursorScreenPos();

    ImGui::PushID(label);
    bool const clicked = ImGui::InvisibleButton("##switch", ImVec2(width, rowHeight));
    ImGui::PopID();
    if (clicked) {
        *value = !*value;
    }
    bool const hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    float const textY = pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f;
    draw->AddText(ImVec2(pos.x, textY), ImGui::GetColorU32(ImGuiCol_Text), label);

    float const h = rowHeight * 0.72f;
    float const w = h * 1.85f;
    ImVec2 const p0(pos.x + width - w, pos.y + (rowHeight - h) * 0.5f);
    ImVec2 const p1(p0.x + w, p0.y + h);
    ImU32 const bg = *value
        ? ImGui::GetColorU32(hovered ? kAccent : kAccentDim)
        : ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    draw->AddRectFilled(p0, p1, bg, h * 0.5f);

    float const radius = h * 0.5f - 2.f * s_scale;
    float const cx = *value ? p1.x - h * 0.5f : p0.x + h * 0.5f;
    draw->AddCircleFilled(ImVec2(cx, p0.y + h * 0.5f), radius, IM_COL32(245, 245, 255, 255));
    return clicked;
}

void settingSwitch(char const* label, char const* key, char const* help = nullptr) {
    bool value = settings::getBool(key);
    if (toggleSwitch(label, &value)) {
        settings::setBool(key, value);
    }
    tooltip(help);
}

void settingSliderFloat(char const* label, char const* key, float min, float max, char const* format, char const* help = nullptr) {
    float value = static_cast<float>(settings::getFloat(key));
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(key);
    if (ImGui::SliderFloat("##slider", &value, min, max, format)) {
        settings::setFloat(key, std::clamp(value, min, max));
    }
    ImGui::PopID();
    tooltip(help);
}

void settingSliderInt(char const* label, char const* key, int min, int max, char const* format, char const* help = nullptr) {
    int value = static_cast<int>(settings::getInt(key));
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(key);
    if (ImGui::SliderInt("##slider", &value, min, max, format)) {
        settings::setInt(key, std::clamp(value, min, max));
    }
    ImGui::PopID();
    tooltip(help);
}

bool beginColumn(char const* title, int index) {
    float const width = kColumnWidth * s_scale;
    float const gap = 12.f * s_scale;
    ImGui::SetNextWindowPos(ImVec2(gap + index * (width + gap), gap), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, FLT_MAX));
    return ImGui::Begin(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
}

void sectionLabel(char const* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void drawBotWindow(Bot& bot) {
    if (beginColumn("Auto Completer", 0)) {
        settingSwitch("Enabled", "enabled", "Pathfind and play levels automatically.");
        ImGui::Separator();

        ImGui::TextColored(stateColor(bot.state()), "%s", bot.stateName());
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("%s", bot.statusText().c_str());
        ImGui::PopStyleColor();

        if (bot.state() == BotState::Playing) {
            auto label = fmt::format("planned {:.2f} s ahead", bot.leadSeconds());
            ImGui::ProgressBar(std::min(bot.leadSeconds() / 0.83f, 1.f), ImVec2(-FLT_MIN, 0.f), label.c_str());
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(!bot.inLevel());
        if (ImGui::Button("Restart level", ImVec2(-FLT_MIN, 0.f))) {
            bot.requestRestart();
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        settingSwitch(
            "Safe mode", "safe-mode",
            "While the bot plays, nothing is saved: no progress, stars, coins or leaderboard submissions."
        );
        if (!settings::safeMode()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped("Safe mode is off: levels the bot completes count like normal runs.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

void drawStyleWindow(Bot& bot) {
    if (beginColumn("Play Style", 1)) {
        struct Option {
            PlayStyle style;
            char const* name;
            char const* description;
        };
        static Option const options[] = {
            { PlayStyle::Default, "Default", "Balanced. Clicks in the middle of each safe timing window." },
            { PlayStyle::Perfect, "Perfect", "Frame precise scan with the widest timing margins." },
            { PlayStyle::Human, "Legit Human", "Natural timing spread and tap lengths, like a real player." },
            { PlayStyle::Minimal, "Minimal", "Latest safe click and shortest taps. Fewest inputs." },
        };

        PlayStyle const current = settings::playStyle();
        for (auto const& option : options) {
            if (ImGui::RadioButton(option.name, current == option.style)) {
                settings::setPlayStyle(option.style);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::Indent();
            ImGui::TextWrapped("%s", option.description);
            ImGui::Unindent();
            ImGui::PopStyleColor();
        }

        (void)bot;
    }
    ImGui::End();
}

void drawPathWindow() {
    if (beginColumn("Path Viewer", 2)) {
        settingSwitch("Show path", "path-viewer", "Draw the planned route in the level.");
        settingSwitch("Whole level", "path-full", "Draw the full route instead of only the part ahead.");
        settingSwitch("Click markers", "path-clicks", "Yellow dots where clicks start, red where they end.");
        ImGui::Spacing();
        settingSliderFloat("Lookahead", "path-lookahead", 0.5f, 15.f, "%.1f s", "How far ahead of the player the route is drawn.");
        settingSliderFloat("Thickness", "path-thickness", 0.5f, 5.f, "%.1f");
        ImGui::Spacing();
        sectionLabel("Green: holding   White: released");
    }
    ImGui::End();
}

void drawSettingsWindow() {
    if (beginColumn("Settings", 3)) {
        settingSwitch("Ignore inputs", "ignore-inputs", "Block your own clicks while the bot is playing.");
        settingSwitch("Auto correct", "auto-correct", "If the real player drifts from the prediction, re-check right away.");
        settingSwitch("Status HUD", "show-hud", "Small status line in the corner while in a level.");
        ImGui::Spacing();
        settingSliderInt("Planning budget", "live-budget", 1, 30, "%d ms / frame",
            "Most time the bot may plan in one frame. It usually needs far less; lower it if your frame rate drops.");
        ImGui::Spacing();
        sectionLabel("Menu key: Geode > Auto Completer settings");
    }
    ImGui::End();
}

void drawStatsWindow(Bot& bot) {
    if (beginColumn("Stats", 4)) {
        auto const& stats = bot.stats();
        auto row = [](char const* name, std::string const& value, ImVec4 const* color = nullptr) {
            ImGui::TextColored(kMuted, "%s", name);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
            if (color) {
                ImGui::TextColored(*color, "%s", value.c_str());
            }
            else {
                ImGui::TextUnformatted(value.c_str());
            }
        };
        row("Style", settings::styleName(settings::playStyle()));
        row("Clicks", std::to_string(stats.clicks));
        row("Planned ahead", fmt::format("{:.2f} s", bot.leadSeconds()));
        row("Checks", std::to_string(stats.checks));
        row("Path searches", std::to_string(stats.searches));
        row("Evaluations", std::to_string(stats.evaluations));
        row("Sim cost", stats.microsPerTick > 0.0 ? fmt::format("{:.1f} us/tick", stats.microsPerTick) : std::string("-"));

        if (stats.predictionError < 0.0) {
            row("Prediction", "-");
        }
        else {
            ImVec4 const& color = stats.predictionError < 0.5 ? kGood : stats.predictionError < 2.0 ? kWarn : kBad;
            row("Prediction", fmt::format("off by {:.2f}", stats.predictionError), &color);
        }
        if (stats.shadowTicks > 0) {
            double const match = 100.0 * static_cast<double>(stats.shadowTicks - stats.shadowMismatches) /
                static_cast<double>(stats.shadowTicks);
            ImVec4 const& color = match > 99.0 ? kGood : match > 90.0 ? kWarn : kBad;
            row("Tick match", fmt::format("{:.1f}%", match), &color);
        }
        else {
            row("Tick match", "-");
        }
        row("Input delay", fmt::format("{} tick", stats.inputDelay));
        row("Desyncs", std::to_string(stats.desyncs));
        row("Deaths", std::to_string(stats.deaths));

        ImGui::Separator();
        sectionLabel("Physics model");
        ImGui::TextWrapped("%s", stats.model.empty() ? "-" : stats.model.c_str());
        sectionLabel("Tick calls");
        ImGui::TextWrapped("%s", stats.tickCalls.empty() ? "-" : stats.tickCalls.c_str());
    }
    ImGui::End();
}

void drawHud(Bot& bot) {
    if (!settings::showHud() || !bot.inLevel()) return;
    if (!settings::enabled() && !bot.safeModeActive()) return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(10.f * s_scale, io.DisplaySize.y - 10.f * s_scale), ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGuiWindowFlags const flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##ac-hud", nullptr, flags)) {
        ImGui::TextColored(kAccent, "AUTO");
        ImGui::SameLine();
        ImGui::TextUnformatted(settings::styleName(settings::playStyle()));
        ImGui::SameLine();
        ImGui::TextColored(stateColor(bot.state()), "%s", bot.stateName());
        if (bot.state() == BotState::Playing) {
            ImGui::SameLine();
            ImGui::TextColored(kMuted, "%.2fs ahead", bot.leadSeconds());
        }
        if (bot.safeModeActive()) {
            ImGui::SameLine();
            ImGui::TextColored(kMuted, "| safe mode");
        }
    }
    ImGui::End();
}

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 7.f;
    style.ChildRounding = 5.f;
    style.FrameRounding = 4.f;
    style.PopupRounding = 5.f;
    style.GrabRounding = 4.f;
    style.ScrollbarRounding = 6.f;
    style.WindowBorderSize = 1.f;
    style.FrameBorderSize = 0.f;
    style.WindowPadding = ImVec2(10.f, 10.f);
    style.FramePadding = ImVec2(8.f, 4.f);
    style.ItemSpacing = ImVec2(8.f, 6.f);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.ScaleAllSizes(s_scale);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.93f, 0.97f, 1.f);
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.07f, 0.1f, 0.96f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.085f, 0.12f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.3f, 0.22f, 0.55f, 0.6f);
    c[ImGuiCol_TitleBg] = kAccentDim;
    c[ImGuiCol_TitleBgActive] = kAccent;
    c[ImGuiCol_TitleBgCollapsed] = kAccentDim;
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.15f, 0.21f, 1.f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.2f, 0.32f, 1.f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.23f, 0.4f, 1.f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.65f, 0.5f, 1.f, 1.f);
    c[ImGuiCol_Button] = ImVec4(0.2f, 0.18f, 0.28f, 1.f);
    c[ImGuiCol_ButtonHovered] = kAccentDim;
    c[ImGuiCol_ButtonActive] = kAccent;
    c[ImGuiCol_Header] = kAccentDim;
    c[ImGuiCol_HeaderHovered] = kAccent;
    c[ImGuiCol_HeaderActive] = kAccent;
    c[ImGuiCol_Separator] = ImVec4(0.3f, 0.25f, 0.45f, 0.7f);
    c[ImGuiCol_PlotHistogram] = kAccent;
}

} // namespace

void setup() {
    s_scale = uiScale();

    ImGuiIO& io = ImGui::GetIO();
    static std::string iniPath = geode::utils::string::pathToString(Mod::get()->getSaveDir() / "imgui.ini");
    io.IniFilename = iniPath.c_str();

    float const fontSize = std::round(16.f * s_scale);
    ImFont* font = nullptr;
    auto const fontPath = Mod::get()->getResourcesDir() / "Roboto-Medium.ttf";
    std::error_code error;
    if (std::filesystem::exists(fontPath, error)) {
        font = io.Fonts->AddFontFromFileTTF(geode::utils::string::pathToString(fontPath).c_str(), fontSize);
    }
    if (!font) {
        ImFontConfig config;
        config.SizePixels = fontSize;
        io.Fonts->AddFontDefault(&config);
    }

    applyStyle();
    ImGui::GetStyle().FontSizeBase = fontSize;
}

void draw() {
    auto& bot = Bot::get();
    drawHud(bot);

    if (!s_open) return;
    drawBotWindow(bot);
    drawStyleWindow(bot);
    drawPathWindow();
    drawSettingsWindow();
    drawStatsWindow(bot);
}

void toggle() {
    s_open = !s_open;
}

bool isOpen() {
    return s_open;
}

} // namespace ac::menu
