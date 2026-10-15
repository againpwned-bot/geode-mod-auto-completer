#include "PlayerState.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace geode::prelude;

namespace ac {

bool& botInputFlag() {
    static bool flag = false;
    return flag;
}

void applyJump(GJBaseGameLayer* layer, bool down) {
    if (!layer) return;
    auto& flag = botInputFlag();
    bool const previous = flag;
    flag = true;
    layer->handleButton(down, static_cast<int>(PlayerButton::Jump), true);
    flag = previous;
}

Mode modeOf(PlayerObject* player) {
    if (!player) return Mode::Cube;
    if (player->m_isShip) return Mode::Ship;
    if (player->m_isDart) return Mode::Wave;
    if (player->m_isBird) return Mode::Ufo;
    if (player->m_isBall) return Mode::Ball;
    if (player->m_isRobot) return Mode::Robot;
    if (player->m_isSpider) return Mode::Spider;
    if (player->m_isSwing) return Mode::Swing;
    return Mode::Cube;
}

namespace {
    template <class T>
    bool sameValue(T const& a, T const& b) {
        if constexpr (std::is_same_v<T, bool>) {
            return a == b;
        }
        else if constexpr (std::is_floating_point_v<T>) {
            return std::abs(static_cast<double>(a) - static_cast<double>(b)) <= 1e-3;
        }
        else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>) {
            return a == b;
        }
        else if constexpr (std::is_same_v<T, cocos2d::CCPoint>) {
            return std::abs(a.x - b.x) <= 1e-3f && std::abs(a.y - b.y) <= 1e-3f;
        }
        else if constexpr (requires { a.size(); }) {
            return a.size() == b.size();
        }
        else {
            return true;
        }
    }

    template <class T>
    std::string showValue(T const& value) {
        if constexpr (std::is_same_v<T, bool>) {
            return value ? "1" : "0";
        }
        else if constexpr (std::is_floating_point_v<T>) {
            return fmt::format("{:.4f}", static_cast<double>(value));
        }
        else if constexpr (std::is_arithmetic_v<T>) {
            return fmt::format("{}", static_cast<long long>(value));
        }
        else if constexpr (std::is_enum_v<T>) {
            return fmt::format("{}", static_cast<long long>(value));
        }
        else if constexpr (std::is_pointer_v<T>) {
            return value ? "obj" : "null";
        }
        else if constexpr (std::is_same_v<T, cocos2d::CCPoint>) {
            return fmt::format("({:.3f},{:.3f})", value.x, value.y);
        }
        else if constexpr (requires { value.size(); }) {
            return fmt::format("[{}]", value.size());
        }
        else {
            return "?";
        }
    }
}

void PlayerFix::capture(PlayerObject* player) {
    valid = player != nullptr;
    if (!player) return;

    nodePosition = player->getPosition();
    rotation = player->getRotation();
    mode = modeOf(player);
    touchingRings.clear();
    if (auto* rings = player->m_touchingRings) {
        for (unsigned i = 0; i < rings->count(); ++i) {
            touchingRings.push_back(rings->objectAtIndex(i));
        }
    }
    std::memcpy(jumpBuffer.data(), &player->m_jumpRelatedAC2, jumpBuffer.size());
#define AC_CAPTURE_FIELD(name) name = player->name;
    AC_PLAYER_FIELDS(AC_CAPTURE_FIELD)
#undef AC_CAPTURE_FIELD
}

void PlayerFix::apply(PlayerObject* player) const {
    if (!valid || !player) return;

    player->setPosition(nodePosition);
    player->setRotation(rotation);
    if (auto* rings = player->m_touchingRings) {
        rings->removeAllObjects();
        for (auto* ring : touchingRings) {
            rings->addObject(ring);
        }
    }
    std::memcpy(&player->m_jumpRelatedAC2, jumpBuffer.data(), jumpBuffer.size());
#define AC_APPLY_FIELD(name) player->name = name;
    AC_PLAYER_FIELDS(AC_APPLY_FIELD)
#undef AC_APPLY_FIELD
}

std::string PlayerFix::diff(PlayerFix const& other, size_t limit, size_t* count) const {
    std::string out;
    size_t found = 0;
    auto add = [&](char const* name, std::string const& mine, std::string const& theirs) {
        found += 1;
        if (found > limit) return;
        if (!out.empty()) out += ", ";
        out += fmt::format("{} {}/{}", name, mine, theirs);
    };
    if (mode != other.mode) {
        add("mode", std::to_string(static_cast<int>(mode)), std::to_string(static_cast<int>(other.mode)));
    }
    if (!sameValue(nodePosition, other.nodePosition)) {
        add("node", showValue(nodePosition), showValue(other.nodePosition));
    }
    if (jumpBuffer != other.jumpBuffer) {
        // Stored as random, seed, value; the number is random - seed.
        auto decode = [](auto const& bytes) {
            int values[3] = {};
            std::memcpy(values, bytes.data(), std::min(sizeof(values), bytes.size()));
            return values[0] - values[1];
        };
        int const mine = decode(jumpBuffer);
        int const theirs = decode(other.jumpBuffer);
        if (mine != theirs) add("m_jumpRelatedAC2", std::to_string(mine), std::to_string(theirs));
    }
    if (touchingRings != other.touchingRings) {
        add(
            "m_touchingRings", fmt::format("[{}]", touchingRings.size()), fmt::format("[{}]", other.touchingRings.size())
        );
    }
#define AC_DIFF_FIELD(name) \
    if (!sameValue(name, other.name)) add(#name, showValue(name), showValue(other.name));
    AC_PLAYER_FIELDS(AC_DIFF_FIELD)
#undef AC_DIFF_FIELD
    if (count) *count = found;
    return out;
}

} // namespace ac
