#pragma once

// How move triggers move their groups, in physics steps. Pure C++ so it can be tested
// without the game.

#include <algorithm>
#include <cmath>

namespace ac {

// GD's easing curves (the cocos2d formulas), indexed like GD's EasingType. `rate` is the
// exponent for the plain ease types and the period for the elastic ones.
inline double ease(int type, double rate, double t) {
    constexpr double pi = 3.14159265358979323846;
    t = std::clamp(t, 0.0, 1.0);
    auto bounce = [](double x) {
        if (x < 1.0 / 2.75) return 7.5625 * x * x;
        if (x < 2.0 / 2.75) {
            x -= 1.5 / 2.75;
            return 7.5625 * x * x + 0.75;
        }
        if (x < 2.5 / 2.75) {
            x -= 2.25 / 2.75;
            return 7.5625 * x * x + 0.9375;
        }
        x -= 2.625 / 2.75;
        return 7.5625 * x * x + 0.984375;
    };
    double const r = rate > 0.0 ? rate : 1.0;
    switch (type) {
        case 1: { // EaseInOut
            double x = t * 2.0;
            return x < 1.0 ? 0.5 * std::pow(x, r) : 1.0 - 0.5 * std::pow(2.0 - x, r);
        }
        case 2: return std::pow(t, r);         // EaseIn
        case 3: return std::pow(t, 1.0 / r);   // EaseOut
        case 4: { // ElasticInOut
            if (t <= 0.0 || t >= 1.0) return t;
            double const period = rate > 0.0 ? rate : 0.3 * 1.5;
            double const s = period / 4.0;
            double x = t * 2.0 - 1.0;
            if (x < 0.0) return -0.5 * std::pow(2.0, 10.0 * x) * std::sin((x - s) * 2.0 * pi / period);
            return std::pow(2.0, -10.0 * x) * std::sin((x - s) * 2.0 * pi / period) * 0.5 + 1.0;
        }
        case 5: { // ElasticIn
            if (t <= 0.0 || t >= 1.0) return t;
            double const period = rate > 0.0 ? rate : 0.3;
            double const s = period / 4.0;
            double const x = t - 1.0;
            return -std::pow(2.0, 10.0 * x) * std::sin((x - s) * 2.0 * pi / period);
        }
        case 6: { // ElasticOut
            if (t <= 0.0 || t >= 1.0) return t;
            double const period = rate > 0.0 ? rate : 0.3;
            double const s = period / 4.0;
            return std::pow(2.0, -10.0 * t) * std::sin((t - s) * 2.0 * pi / period) + 1.0;
        }
        case 7: // BounceInOut
            return t < 0.5 ? (1.0 - bounce(1.0 - t * 2.0)) * 0.5 : bounce(t * 2.0 - 1.0) * 0.5 + 0.5;
        case 8: return 1.0 - bounce(1.0 - t); // BounceIn
        case 9: return bounce(t);             // BounceOut
        case 10: { // ExponentialInOut
            if (t <= 0.0 || t >= 1.0) return t;
            double const x = t * 2.0;
            return x < 1.0 ? 0.5 * std::pow(2.0, 10.0 * (x - 1.0)) : 0.5 * (2.0 - std::pow(2.0, -10.0 * (x - 1.0)));
        }
        case 11: return t <= 0.0 ? 0.0 : std::pow(2.0, 10.0 * (t - 1.0)) - 0.001; // ExponentialIn
        case 12: return t >= 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * t);         // ExponentialOut
        case 13: return -0.5 * (std::cos(pi * t) - 1.0);                          // SineInOut
        case 14: return 1.0 - std::cos(t * pi / 2.0);                             // SineIn
        case 15: return std::sin(t * pi / 2.0);                                   // SineOut
        case 16: { // BackInOut
            double const s = 1.70158 * 1.525;
            double x = t * 2.0;
            if (x < 1.0) return x * x * ((s + 1.0) * x - s) / 2.0;
            x -= 2.0;
            return x * x * ((s + 1.0) * x + s) / 2.0 + 1.0;
        }
        case 17: { // BackIn
            double const s = 1.70158;
            return t * t * ((s + 1.0) * t - s);
        }
        case 18: { // BackOut
            double const s = 1.70158;
            double const x = t - 1.0;
            return x * x * ((s + 1.0) * x + s) + 1.0;
        }
        default: return t;
    }
}

// One move of a group, as a move trigger starts it. A group's objects have covered
// progress(step) of the offset once physics step `step` has run.
struct MoveCommand {
    int group = 0;
    int start = 0;      // step the move was started in; it moves from the next step on
    int duration = 0;   // steps; 0 moves all at once
    double dx = 0.0;
    double dy = 0.0;
    int easing = 0;
    double rate = 1.0;
    bool lockPlayerX = false;
    bool lockPlayerY = false;
    bool lockCameraX = false;
    bool lockCameraY = false;
    double modX = 1.0;
    double modY = 1.0;
    void const* source = nullptr; // trigger that started it, if known
    bool predicted = false;       // expected from a spawn trigger, not started yet

    int end() const { return start + std::max(duration, 1); }
    bool locked() const { return lockPlayerX || lockPlayerY || lockCameraX || lockCameraY; }

    double progress(int step) const {
        if (step <= start) return 0.0;
        if (step >= this->end()) return 1.0;
        return ease(easing, rate, static_cast<double>(step - start) / static_cast<double>(std::max(duration, 1)));
    }

    // Steps of [from, to) during which the move runs.
    int activeSteps(int from, int to) const {
        return std::max(0, std::min(to, this->end()) - std::max(from, start));
    }

    // How far the group moves between the end of step `from` and the end of step `to`.
    // `playerDx/Dy` is how far the player travels over the same steps (for locked moves; the
    // camera follows the player horizontally).
    void delta(int from, int to, double playerDx, double playerDy, double& outX, double& outY) const {
        if (to <= from) return;
        double const eased = this->progress(to) - this->progress(from);
        double const share = static_cast<double>(this->activeSteps(from, to)) / static_cast<double>(to - from);
        if (lockPlayerX || lockCameraX) outX += playerDx * share * modX;
        else outX += dx * eased;
        if (lockPlayerY) outY += playerDy * share * modY;
        else if (!lockCameraY) outY += dy * eased;
    }
};

} // namespace ac
