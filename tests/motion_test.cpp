// Tests for the move trigger model (src/bot/Motion.hpp).
//   g++ -std=c++20 -O2 -I src tests/motion_test.cpp -o motion_test && ./motion_test

#include "bot/Motion.hpp"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, char const* what, double got = 0.0, double want = 0.0) {
    if (!ok) {
        failures += 1;
        std::printf("FAIL %s (got %.6f, want %.6f)\n", what, got, want);
    }
}

bool near(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) <= eps;
}

void easingEndpoints() {
    for (int type = 0; type <= 18; ++type) {
        double const rate = (type >= 4 && type <= 6) ? 0.4 : 2.0;
        double const start = ac::ease(type, rate, 0.0);
        double const end = ac::ease(type, rate, 1.0);
        char name[64];
        std::snprintf(name, sizeof name, "ease %d starts at 0", type);
        check(near(start, 0.0, 1e-3), name, start, 0.0);
        std::snprintf(name, sizeof name, "ease %d ends at 1", type);
        check(near(end, 1.0, 2e-3), name, end, 1.0);
        // Out of range times are clamped.
        std::snprintf(name, sizeof name, "ease %d clamps", type);
        check(near(ac::ease(type, rate, 5.0), end) && near(ac::ease(type, rate, -5.0), start), name);
    }
}

void easingShapes() {
    // Plain curves move forward only.
    for (int type : { 0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15 }) {
        double previous = ac::ease(type, 2.0, 0.0);
        bool monotonic = true;
        for (int i = 1; i <= 100; ++i) {
            double const value = ac::ease(type, 2.0, i / 100.0);
            if (value < previous - 1e-9 && type != 8 && type != 9) monotonic = false;
            previous = value;
        }
        char name[64];
        std::snprintf(name, sizeof name, "ease %d moves forward", type);
        check(monotonic, name);
    }
    check(near(ac::ease(0, 1.0, 0.25), 0.25), "linear", ac::ease(0, 1.0, 0.25), 0.25);
    check(near(ac::ease(2, 2.0, 0.5), 0.25), "ease in 2", ac::ease(2, 2.0, 0.5), 0.25);
    check(near(ac::ease(3, 2.0, 0.25), 0.5), "ease out 2", ac::ease(3, 2.0, 0.25), 0.5);
    check(near(ac::ease(1, 2.0, 0.5), 0.5), "ease in out middle", ac::ease(1, 2.0, 0.5), 0.5);
    check(near(ac::ease(1, 2.0, 0.25) + ac::ease(1, 2.0, 0.75), 1.0), "ease in out symmetric");
    check(near(ac::ease(13, 1.0, 0.5), 0.5), "sine in out middle", ac::ease(13, 1.0, 0.5), 0.5);
    // Back overshoots.
    check(ac::ease(18, 1.0, 0.7) > 1.0, "back out overshoots", ac::ease(18, 1.0, 0.7), 1.0);
    check(ac::ease(17, 1.0, 0.2) < 0.0, "back in pulls back", ac::ease(17, 1.0, 0.2), 0.0);
}

void commands() {
    ac::MoveCommand move;
    move.group = 5;
    move.start = 100;
    move.duration = 120;
    move.dx = 90.0;
    move.dy = -30.0;

    check(near(move.progress(100), 0.0), "nothing moved at start");
    check(near(move.progress(160), 0.5), "half way", move.progress(160), 0.5);
    check(near(move.progress(220), 1.0), "done at end");
    check(near(move.progress(400), 1.0), "stays done");

    double dx = 0.0;
    double dy = 0.0;
    move.delta(100, 160, 0.0, 0.0, dx, dy);
    check(near(dx, 45.0) && near(dy, -15.0), "first half", dx, 45.0);

    // From the middle on, only what is left.
    dx = dy = 0.0;
    move.delta(160, 1000, 0.0, 0.0, dx, dy);
    check(near(dx, 45.0) && near(dy, -15.0), "rest of the move", dx, 45.0);

    // A finished move doesn't move anything.
    dx = dy = 0.0;
    move.delta(300, 400, 0.0, 0.0, dx, dy);
    check(near(dx, 0.0) && near(dy, 0.0), "finished move", dx, 0.0);

    // A move that starts later only counts from its start.
    dx = dy = 0.0;
    move.delta(40, 130, 0.0, 0.0, dx, dy);
    check(near(dx, 90.0 * 30.0 / 120.0), "later start", dx, 22.5);

    // Instant moves happen on the step after the start.
    ac::MoveCommand instant = move;
    instant.duration = 0;
    check(near(instant.progress(100), 0.0) && near(instant.progress(101), 1.0), "instant move");

    // Locked to the player: follows the player's travel while it runs.
    ac::MoveCommand locked = move;
    locked.lockPlayerX = true;
    locked.modX = 0.5;
    dx = dy = 0.0;
    locked.delta(100, 220, 160.0, 0.0, dx, dy);
    check(near(dx, 80.0), "locked to player x", dx, 80.0);
    check(near(dy, -30.0), "locked x keeps its y move", dy, -30.0);
    // Only the part of the window the move runs for.
    dx = dy = 0.0;
    locked.delta(160, 280, 120.0, 0.0, dx, dy);
    check(near(dx, 0.5 * 120.0 * 60.0 / 120.0), "locked move ends", dx, 30.0);

    check(locked.locked() && !move.locked(), "locked flag");
    check(move.end() == 220 && instant.end() == 101, "end step");
}

} // namespace

int main() {
    easingEndpoints();
    easingShapes();
    commands();
    if (failures > 0) {
        std::printf("%d FAILED\n", failures);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
