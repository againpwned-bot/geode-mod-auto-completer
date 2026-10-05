#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <imgui-cocos.hpp>

#include "ui/Menu.hpp"

using namespace geode::prelude;

$on_mod(Loaded) {
    ImGuiCocos::get()
        .setup([] {
            ac::menu::setup();
        })
        .draw([] {
            ac::menu::draw();
        });

    // The HUD and planning progress are drawn even while the menu itself is closed.
    ImGuiCocos::get().setVisible(true);

    listenForKeybindSettingPresses("menu-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            ac::menu::toggle();
        }
    });
}
