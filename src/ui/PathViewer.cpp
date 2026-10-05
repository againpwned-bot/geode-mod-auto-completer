#include "PathViewer.hpp"

#include "../Settings.hpp"
#include "../bot/Bot.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace ac {

namespace {
    // GD's CCDrawNode skips geometry outside a cached area unless told otherwise.
    class PathDrawNode : public CCDrawNode {
    public:
        static PathDrawNode* create() {
            auto* node = new PathDrawNode();
            if (node->init()) {
                node->m_bUseArea = false;
                node->autorelease();
                return node;
            }
            delete node;
            return nullptr;
        }
    };

    constexpr int kTicksPerSecond = 240;
    constexpr int kTicksBehind = 60;
    constexpr float kMaxSegment = 120.f; // longer jumps are teleports, don't connect them

    ccColor4F const kHoldColor = { 0.25f, 1.f, 0.45f, 0.95f };
    ccColor4F const kReleaseColor = { 0.9f, 0.95f, 1.f, 0.8f };
    ccColor4F const kPressColor = { 1.f, 0.85f, 0.1f, 1.f };
    ccColor4F const kReleaseDotColor = { 1.f, 0.35f, 0.3f, 0.9f };
}

PathViewer& PathViewer::get() {
    static PathViewer viewer;
    return viewer;
}

void PathViewer::attach(PlayLayer* layer) {
    this->detach();
    if (!layer) return;

    auto* node = PathDrawNode::create();
    if (!node) return;

    CCNode* parent = layer->m_objectLayer;
    int z = 9999;
    if (layer->m_debugDrawNode && layer->m_debugDrawNode->getParent()) {
        parent = layer->m_debugDrawNode->getParent();
        z = layer->m_debugDrawNode->getZOrder() + 1;
    }
    if (!parent) return;

    node->setID("path-viewer"_spr);
    parent->addChild(node, z);
    node->retain();
    m_node = node;
    m_drawnRevision = ~0u;
    m_drawnTick = ~0u;
    m_drawnFull = false;
}

void PathViewer::detach() {
    if (!m_node) return;
    m_node->removeFromParent();
    m_node->release();
    m_node = nullptr;
}

void PathViewer::update(PlayLayer* layer) {
    if (!m_node || !layer) return;

    auto const& bot = Bot::get();
    bool const show = settings::enabled() && settings::pathViewer() && bot.hasPlan() &&
        bot.state() != BotState::Computing && bot.planEnd() > 1;
    m_node->setVisible(show);
    if (!show) return;

    unsigned const revision = bot.planRevision();
    bool const full = settings::fullPath();

    if (full) {
        if (m_drawnFull && revision == m_drawnRevision) return;
        this->redraw(0, bot.planEnd(), 3);
        m_drawnFull = true;
        m_drawnRevision = revision;
        return;
    }

    unsigned const tick = layer->m_gameState.m_currentProgress;
    if (!m_drawnFull && revision == m_drawnRevision && tick == m_drawnTick) return;

    int const ahead = static_cast<int>(std::clamp(settings::pathLookahead(), 0.1, 30.0) * kTicksPerSecond);
    int const from = std::max(0, static_cast<int>(tick) - kTicksBehind);
    this->redraw(from, static_cast<int>(tick) + ahead, 2);
    m_drawnFull = false;
    m_drawnRevision = revision;
    m_drawnTick = tick;
}

void PathViewer::redraw(int from, int to, int stride) {
    m_node->clear();

    auto const& bot = Bot::get();
    auto const& samples = bot.planSamples();
    auto const& inputs = bot.planInputs();
    int const end = std::min({ to, bot.planEnd(), static_cast<int>(samples.size()), static_cast<int>(inputs.size()) });
    if (end - from < 2) return;

    float const thickness = static_cast<float>(std::clamp(settings::pathThickness(), 0.2, 8.0));
    bool const clicks = settings::clickMarkers();

    // Segments
    int previous = from;
    for (int tick = from + stride; tick < end + stride; tick += stride) {
        int const current = std::min(tick, end - 1);
        if (current <= previous) break;

        auto const& a = samples[previous];
        auto const& b = samples[current];
        bool const teleport = std::abs(b.x - a.x) > kMaxSegment || std::abs(b.y - a.y) > kMaxSegment;
        bool const unset = (a.x == 0.f && a.y == 0.f) || (b.x == 0.f && b.y == 0.f);
        if (!teleport && !unset) {
            m_node->drawSegment(ccp(a.x, a.y), ccp(b.x, b.y), thickness, a.hold ? kHoldColor : kReleaseColor);
        }
        previous = current;
    }

    if (!clicks) return;

    // Click markers (checked at full resolution so no click is skipped)
    for (int tick = std::max(from, 1); tick < end; ++tick) {
        bool const now = inputs[tick] != 0;
        bool const before = inputs[tick - 1] != 0;
        if (now == before) continue;
        auto const& s = samples[tick];
        if (s.x == 0.f && s.y == 0.f) continue;
        if (now) {
            m_node->drawDot(ccp(s.x, s.y), thickness * 2.6f, kPressColor);
        }
        else {
            m_node->drawDot(ccp(s.x, s.y), thickness * 1.6f, kReleaseDotColor);
        }
    }
}

} // namespace ac
