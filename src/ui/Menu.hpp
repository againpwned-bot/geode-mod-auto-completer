#pragma once

// Mega Hack style ImGui overlay: a row of category windows toggled with the menu key,
// plus a small status HUD and a progress popup while a path is being computed.
namespace ac::menu {

void setup();
void draw();
void toggle();
bool isOpen();

} // namespace ac::menu
