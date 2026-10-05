#pragma once

#include <Geode/Geode.hpp>

namespace ac {

// Draws the planned route inside the level: green where the button is held, white
// where it is released, with dots where clicks start.
class PathViewer {
public:
    static PathViewer& get();

    void attach(PlayLayer* layer);
    void detach();
    void update(PlayLayer* layer);

private:
    PathViewer() = default;

    void redraw(int from, int to, int stride);

    cocos2d::CCDrawNode* m_node = nullptr; // retained while attached
    unsigned m_drawnRevision = ~0u;
    unsigned m_drawnTick = ~0u;
    bool m_drawnFull = false;
};

} // namespace ac
