#include "ObjectTracker.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace ac {

namespace {

// GD 2.2081 runs 240 physics steps per second.
constexpr double kStepsPerSecond = 240.0;

bool isHazard(GameObject* object) {
    auto const type = object->m_objectType;
    return type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard;
}

// Move triggers whose offset this model understands.
bool plainMove(EffectGameObject* trigger) {
    return trigger->m_objectID == ObjectTracker::kMoveTrigger && trigger->m_targetGroupID > 0 &&
        !trigger->m_useMoveTarget && !trigger->m_isDynamicMode;
}

// Toggle triggers this model understands.
bool plainToggle(EffectGameObject* trigger) {
    return trigger->m_objectID == ObjectTracker::kToggleTrigger && trigger->m_targetGroupID > 0;
}

template <class Fn>
void forEachGroup(GameObject* object, Fn&& fn) {
    if (!object->m_groups || object->m_groupCount <= 0) return;
    int const count = std::min<int>(object->m_groupCount, 10);
    for (int i = 0; i < count; ++i) {
        int const group = (*object->m_groups)[i];
        if (group > 0) fn(group);
    }
}

} // namespace

ObjectTracker& ObjectTracker::get() {
    static ObjectTracker tracker;
    return tracker;
}

void ObjectTracker::setWorldStep(int step) {
    m_worldStep = step;

    // Drop objects that stopped, or weren't seen lately, from the moving list.
    m_moving.erase(
        std::remove_if(
            m_moving.begin(), m_moving.end(),
            [this](GameObject* object) {
                auto const it = m_tracks.find(object);
                if (it == m_tracks.end()) return true;
                Track& track = it->second;
                bool const keep = (track.vx != 0.0 || track.vy != 0.0) && track.step + 16 >= m_worldStep;
                if (!keep) track.listed = false;
                return !keep;
            }
        ),
        m_moving.end()
    );

    // Forget objects not seen for a while, and moves long finished.
    if (m_worldStep > m_lastPrune + 600) {
        m_lastPrune = m_worldStep;
        for (auto i = m_tracks.begin(); i != m_tracks.end();) {
            if (i->second.step + 600 < m_worldStep && !i->second.listed) i = m_tracks.erase(i);
            else ++i;
        }
        size_t const before = m_commands.size();
        m_commands.erase(
            std::remove_if(
                m_commands.begin(), m_commands.end(),
                [this](MoveCommand const& c) { return c.end() + 64 < m_worldStep; }
            ),
            m_commands.end()
        );
        if (m_commands.size() != before) this->indexCommands();
        m_expectedToggles.erase(
            std::remove_if(
                m_expectedToggles.begin(), m_expectedToggles.end(),
                [this](Toggle const& t) { return t.step + 64 < m_worldStep; }
            ),
            m_expectedToggles.end()
        );
    }
}

ObjectTracker::Track& ObjectTracker::observe(GameObject* object) {
    auto [it, inserted] = m_tracks.try_emplace(object);
    Track& track = it->second;
    if (!inserted) {
        if (track.step == m_worldStep) return track;
        int const gap = m_worldStep - track.step;
        track.vx = 0.0;
        track.vy = 0.0;
        if (gap >= 1 && gap <= 16) {
            double const vx = (object->m_positionX - track.x) / gap;
            double const vy = (object->m_positionY - track.y) / gap;
            // Jumps (teleports, toggled spawns) aren't motion.
            if (std::abs(vx) < 30.0 && std::abs(vy) < 30.0) {
                track.vx = vx;
                track.vy = vy;
                if ((vx != 0.0 || vy != 0.0) && !m_byGroup.empty()) {
                    this->calibrate(object, track.step, m_worldStep, vx * gap, vy * gap);
                }
            }
        }
    }
    if ((track.vx != 0.0 || track.vy != 0.0) && !track.listed) {
        track.listed = true;
        m_moving.push_back(object);
    }
    track.x = object->m_positionX;
    track.y = object->m_positionY;
    track.step = m_worldStep;
    return track;
}

void ObjectTracker::observeVisible(GJBaseGameLayer* layer) {
    if (!layer) return;
    int const columns = static_cast<int>(layer->m_sections.size());
    int left = layer->m_leftSectionIndex;
    int right = layer->m_rightSectionIndex;
    int bottom = layer->m_bottomSectionIndex;
    int top = layer->m_topSectionIndex;
    if (!m_loggedSections) {
        m_loggedSections = true;
        log::info("Visible sections: x {}..{} of {}, y {}..{}", left, right, columns, bottom, top);
    }
    // Look a little past the screen edges; skip anything that doesn't look like a window.
    left = std::max(left - 1, 0);
    right = std::min(right + 1, columns - 1);
    bottom = std::max(bottom - 1, 0);
    top += 1;
    if (right < left || top < bottom || right - left > 40 || top - bottom > 40) return;
    if (static_cast<int>(layer->m_sectionSizes.size()) < columns) return;

    for (int x = left; x <= right; ++x) {
        auto* column = layer->m_sections[x];
        auto* sizes = layer->m_sectionSizes[x];
        if (!column || !sizes) continue;
        int const rows = std::min(static_cast<int>(column->size()), static_cast<int>(sizes->size()));
        for (int y = bottom; y <= std::min(top, rows - 1); ++y) {
            auto* objects = (*column)[y];
            if (!objects) continue;
            int const count = std::min((*sizes)[y], static_cast<int>(objects->size()));
            for (int i = 0; i < count; ++i) {
                if (auto* object = (*objects)[i]) this->observe(object);
            }
        }
    }
}

void ObjectTracker::indexCommands() {
    m_byGroup.clear();
    m_sources.clear();
    for (int i = 0; i < static_cast<int>(m_commands.size()); ++i) {
        m_byGroup[m_commands[i].group].push_back(i);
        if (m_commands[i].source) m_sources.insert(m_commands[i].source);
    }
}

void ObjectTracker::onMoveCommand(
    CCPoint offset, int group, float duration, int easing, float rate, bool lockPlayerX, bool lockPlayerY,
    bool lockCameraX, bool lockCameraY, float modX, float modY
) {
    MoveCommand command;
    command.group = group;
    command.start = m_worldStep;
    command.duration = std::max(0, static_cast<int>(std::lround(duration * kStepsPerSecond)));
    command.dx = offset.x;
    command.dy = offset.y;
    command.easing = easing;
    command.rate = rate;
    command.lockPlayerX = lockPlayerX;
    command.lockPlayerY = lockPlayerY;
    command.lockCameraX = lockCameraX;
    command.lockCameraY = lockCameraY;
    command.modX = modX;
    command.modY = modY;
    command.source = m_currentTrigger;
    if (!command.source && m_unknownSources < 3) {
        m_unknownSources += 1;
        log::info("Move of group {} started without a trigger being activated", group);
    }

    if (auto* trigger = m_currentTrigger; trigger && trigger->m_objectID == kMoveTrigger && !command.locked()) {
        // Learn how the trigger's offset maps to the offset GD moves by.
        auto const raw = trigger->m_moveOffset;
        double sum = 0.0;
        int axes = 0;
        if (std::abs(raw.x) > 0.5) {
            sum += offset.x / raw.x;
            axes += 1;
        }
        if (std::abs(raw.y) > 0.5) {
            sum += offset.y / raw.y;
            axes += 1;
        }
        if (axes > 0) {
            double const scale = sum / axes;
            int const unit = trigger->m_smallStep ? 1 : 0;
            if (scale > 0.05 && scale < 20.0) {
                if (!m_scaleKnown[unit] || std::abs(scale - m_scale[unit]) > m_scale[unit] * 0.05) {
                    log::info(
                        "Move trigger offset ({:.1f},{:.1f}){} moves by ({:.1f},{:.1f}): scale {:.3f}",
                        raw.x, raw.y, unit ? " (small step)" : "", offset.x, offset.y, scale
                    );
                }
                m_scale[unit] = scale;
                m_scaleKnown[unit] = true;
            }
        }
    }

    // The real move replaces the one expected from its spawn trigger.
    if (command.source) {
        size_t const before = m_commands.size();
        m_commands.erase(
            std::remove_if(
                m_commands.begin(), m_commands.end(),
                [&](MoveCommand const& c) { return c.predicted && c.source == command.source; }
            ),
            m_commands.end()
        );
        if (m_commands.size() != before) this->indexCommands();
    }
    m_byGroup[group].push_back(static_cast<int>(m_commands.size()));
    if (command.source) m_sources.insert(command.source);
    m_commands.push_back(command);
}

void ObjectTracker::onToggle(int group, bool on) {
    Toggle toggle;
    toggle.group = group;
    toggle.step = m_worldStep;
    toggle.on = on;
    toggle.source = m_currentTrigger;
    if (toggle.source) {
        m_toggleSources.insert(toggle.source);
        // The real toggle replaces the one expected from its spawn trigger.
        m_expectedToggles.erase(
            std::remove_if(
                m_expectedToggles.begin(), m_expectedToggles.end(),
                [&](Toggle const& t) { return t.source == toggle.source; }
            ),
            m_expectedToggles.end()
        );
    }
    m_toggleLog.push_back(toggle);
    if (m_toggleLog.size() > 256) m_toggleLog.erase(m_toggleLog.begin(), m_toggleLog.begin() + 128);
}

int ObjectTracker::enabledAt(GameObject* object, int step, std::vector<Toggle> const* extra) const {
    bool const hasExtra = extra && !extra->empty();
    if (m_expectedToggles.empty() && !hasExtra) return -1;
    // The latest toggle of any of its groups before this step decides.
    int latest = -1000000;
    int state = -1;
    auto consider = [&](Toggle const& toggle, int group) {
        if (toggle.group != group || toggle.step <= m_worldStep || toggle.step >= step) return;
        if (toggle.step >= latest) {
            latest = toggle.step;
            state = toggle.on ? 1 : 0;
        }
    };
    forEachGroup(object, [&](int group) {
        for (auto const& toggle : m_expectedToggles) consider(toggle, group);
        if (hasExtra) {
            for (auto const& toggle : *extra) {
                // The real game did it meanwhile; that one counts.
                if (toggle.source && m_toggleSources.count(toggle.source)) continue;
                consider(toggle, group);
            }
        }
    });
    return state;
}

ObjectTracker::Toggle ObjectTracker::toggleFor(EffectGameObject* trigger, int step) {
    Toggle toggle;
    toggle.group = trigger->m_targetGroupID;
    toggle.step = step;
    toggle.on = trigger->m_activateGroup;
    toggle.source = trigger;
    return toggle;
}

EffectGameObject* ObjectTracker::beginTrigger(EffectGameObject* trigger, GJBaseGameLayer* layer, double playerX) {
    EffectGameObject* const previous = m_currentTrigger;
    m_currentTrigger = trigger;
    int const id = trigger->m_objectID;
    if (id != kMoveTrigger && id != kSpawnTrigger && id != kToggleTrigger) return previous;
    if (trigger->m_isTouchTriggered || trigger->m_isSpawnTriggered) return previous;

    // Learn where position triggers fire relative to the player (not the ones the level
    // start fires all at once).
    double const lead = playerX - trigger->m_positionX;
    if (m_worldStep > 10 && std::abs(lead) < 120.0) {
        m_leadCount += 1;
        m_lead = m_leadCount == 1 ? lead : m_lead + (lead - m_lead) / std::min(m_leadCount, 20);
        if (m_leadCount <= 3) {
            log::info(
                "Position trigger {} at x {:.1f} fired with the player at x {:.1f} (step {})", id,
                trigger->m_positionX, playerX, m_worldStep
            );
        }
    }

    // A spawn trigger's moves start after its delay; expect them until they really start.
    if (id == kSpawnTrigger && layer) {
        int const delay = static_cast<int>(std::lround(
            static_cast<SpawnTriggerGameObject*>(trigger)->m_spawnDelay * kStepsPerSecond
        ));
        bool added = false;
        for (auto* spawned : this->spawnedTriggers(layer, trigger)) {
            if (spawned->m_objectID == kToggleTrigger) {
                Toggle toggle = toggleFor(spawned, m_worldStep + delay);
                toggle.predicted = true;
                m_expectedToggles.push_back(toggle);
                continue;
            }
            MoveCommand command;
            if (!this->commandFor(spawned, m_worldStep + delay, command)) continue;
            command.predicted = true;
            m_commands.push_back(command);
            added = true;
        }
        if (added) this->indexCommands();
    }
    return previous;
}

void ObjectTracker::calibrate(GameObject* object, int from, int to, double dx, double dy) {
    // Only objects moved by exactly one plain move that runs over the whole stretch.
    MoveCommand const* only = nullptr;
    int count = 0;
    forEachGroup(object, [&](int group) {
        auto const it = m_byGroup.find(group);
        if (it == m_byGroup.end()) return;
        for (int index : it->second) {
            auto const& command = m_commands[index];
            if (command.predicted || command.activeSteps(from - 2, to + 2) == 0) continue;
            only = &command;
            count += 1;
        }
    });
    if (count != 1 || only->locked() || only->start > from - 2 || only->end() < to + 2) return;

    double px = 0.0;
    double py = 0.0;
    only->delta(from, to, 0.0, 0.0, px, py);
    double const length = px * px + py * py;
    if (length < 0.25) return;
    double const ratio = (dx * px + dy * py) / length;
    if (ratio < 0.2 || ratio > 5.0) return;

    constexpr size_t kKeep = 64;
    if (m_ratios.size() < kKeep) m_ratios.push_back(ratio);
    else m_ratios[m_ratioCount % kKeep] = ratio;
    m_ratioCount += 1;
    if (m_ratioCount % 16 != 0) return;

    // The median shrugs off objects that something else moves too. GD's factor is a simple
    // one, so anything near 1 is 1.
    std::vector<double> sorted = m_ratios;
    std::sort(sorted.begin(), sorted.end());
    double const median = sorted[sorted.size() / 2];
    double scale = 1.0;
    if (median < 0.8 || median > 1.25) {
        for (double factor : { 1.0 / 3.0, 0.5, 2.0, 3.0 }) {
            if (std::abs(median - factor) <= factor * 0.15) scale = factor;
        }
    }
    if (m_ratioCount == 16 || scale != m_offsetScale) {
        log::info(
            "Moves measured at {:.2f}x their trigger offset ({} samples); using {:.2f}x", median, m_ratioCount, scale
        );
    }
    m_offsetScale = scale;
}

bool ObjectTracker::commandFor(EffectGameObject* trigger, int start, MoveCommand& out) const {
    if (!plainMove(trigger)) return false;
    int const unit = trigger->m_smallStep ? 1 : 0;
    bool const locked = trigger->m_lockToPlayerX || trigger->m_lockToPlayerY || trigger->m_lockToCameraX ||
        trigger->m_lockToCameraY;
    // Until a real move shows the offset's units, only locked moves can be predicted.
    if (!m_scaleKnown[unit] && !locked) return false;
    double const scale = m_scaleKnown[unit] ? m_scale[unit] : 1.0;

    out = MoveCommand{};
    out.group = trigger->m_targetGroupID;
    out.start = start;
    out.duration = std::max(0, static_cast<int>(std::lround(trigger->m_duration * kStepsPerSecond)));
    out.dx = trigger->m_moveOffset.x * scale;
    out.dy = trigger->m_moveOffset.y * scale;
    out.easing = static_cast<int>(trigger->m_easingType);
    out.rate = trigger->m_easingRate;
    out.lockPlayerX = trigger->m_lockToPlayerX;
    out.lockPlayerY = trigger->m_lockToPlayerY;
    out.lockCameraX = trigger->m_lockToCameraX;
    out.lockCameraY = trigger->m_lockToCameraY;
    out.modX = trigger->m_moveModX;
    out.modY = trigger->m_moveModY;
    out.source = trigger;
    return true;
}

std::vector<EffectGameObject*> const& ObjectTracker::spawnedTriggers(GJBaseGameLayer* layer, EffectGameObject* spawn) {
    auto [it, inserted] = m_spawned.try_emplace(spawn);
    if (!inserted) return it->second;

    auto* trigger = static_cast<SpawnTriggerGameObject*>(spawn);
    // Remapped or ordered spawns move other groups or at other times; leave them alone.
    if (trigger->m_targetGroupID <= 0 || !trigger->m_remapObjects.empty() || trigger->m_spawnOrdered) {
        return it->second;
    }
    auto* group = layer->getGroup(trigger->m_targetGroupID);
    if (!group) return it->second;
    for (auto* object : CCArrayExt<GameObject*>(group)) {
        if (!object) continue;
        int const id = object->m_objectID;
        if (id != kMoveTrigger && id != kToggleTrigger) continue;
        auto* spawned = static_cast<EffectGameObject*>(object);
        if (!spawned->m_isSpawnTriggered) continue;
        if (plainMove(spawned) || plainToggle(spawned)) it->second.push_back(spawned);
    }
    return it->second;
}

void ObjectTracker::scanLevel(GJBaseGameLayer* layer) {
    m_scanned = true;
    m_triggers.clear();
    if (!layer || !layer->m_objects) return;

    int moves = 0;
    int toggles = 0;
    int spawns = 0;
    int others = 0;
    // Other triggers that move or hide things, for the log.
    std::unordered_map<int, int> unhandled;
    for (auto* object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (!object) continue;
        int const id = object->m_objectID;
        switch (id) {
            case 1007: case 1346: case 1347: case 1814: case 1815: case 1595: case 1616: case 3016:
                unhandled[id] += 1;
                continue;
            default:
                break;
        }
        if (id != kMoveTrigger && id != kSpawnTrigger && id != kToggleTrigger) continue;
        auto* trigger = static_cast<EffectGameObject*>(object);
        if (trigger->m_isTouchTriggered || trigger->m_isSpawnTriggered || trigger->m_targetGroupID <= 0) {
            others += 1;
            continue;
        }
        if (id == kMoveTrigger) {
            if (!plainMove(trigger)) {
                others += 1;
                continue;
            }
            moves += 1;
        }
        else if (id == kToggleTrigger) {
            toggles += 1;
        }
        else {
            spawns += 1;
        }
        m_triggers.push_back({ object->m_positionX, trigger });
    }
    std::sort(m_triggers.begin(), m_triggers.end(), [](PositionTrigger const& a, PositionTrigger const& b) {
        return a.x < b.x;
    });
    std::string extra;
    for (auto const& [id, count] : unhandled) extra += fmt::format(" {}x{}", count, id);
    log::info(
        "Level triggers: {} move, {} toggle and {} spawn triggers fire by position; {} others (touch, spawned, "
        "move to target); not simulated (count x id):{}",
        moves, toggles, spawns, others, extra.empty() ? " none" : extra
    );
}

void ObjectTracker::eventsBetween(
    GJBaseGameLayer* layer, double fromX, double toX, int step, std::vector<MoveCommand>& moves,
    std::vector<Toggle>& toggles
) {
    if (m_triggers.empty() || toX <= fromX) return;
    double const lead = this->activationLead();
    auto it = std::upper_bound(m_triggers.begin(), m_triggers.end(), fromX - lead, [](double x, PositionTrigger const& t) {
        return x < t.x;
    });
    for (; it != m_triggers.end() && it->x + lead <= toX; ++it) {
        auto* trigger = it->trigger;
        if (trigger->m_objectID == kMoveTrigger) {
            MoveCommand command;
            if (this->commandFor(trigger, step, command)) moves.push_back(command);
            continue;
        }
        if (trigger->m_objectID == kToggleTrigger) {
            toggles.push_back(toggleFor(trigger, step));
            continue;
        }
        int const delay = static_cast<int>(std::lround(
            static_cast<SpawnTriggerGameObject*>(trigger)->m_spawnDelay * kStepsPerSecond
        ));
        for (auto* spawned : this->spawnedTriggers(layer, trigger)) {
            if (spawned->m_objectID == kToggleTrigger) {
                toggles.push_back(toggleFor(spawned, step + delay));
                continue;
            }
            MoveCommand command;
            if (this->commandFor(spawned, step + delay, command)) moves.push_back(command);
        }
    }
}

bool ObjectTracker::movesDuring(MoveCommand const& command, int worldStep, int step) {
    // Also counts moves that ended just now, so a stopped object isn't extrapolated further.
    return command.start < step && command.end() + 16 >= worldStep;
}

void ObjectTracker::predict(GameObject* object, Track const& track, Context const& ctx, std::vector<Moved>& moved) const {
    int const step = ctx.step;
    if (step <= m_worldStep) return;

    double dx = 0.0;
    double dy = 0.0;
    bool commanded = false;
    bool const extra = ctx.extra && !ctx.extra->empty();
    if (!m_byGroup.empty() || extra) {
        double const scale = this->offsetScale();
        auto add = [&](MoveCommand const& command) {
            commanded = true;
            double mx = 0.0;
            double my = 0.0;
            command.delta(m_worldStep, step, ctx.playerDx, ctx.playerDy, mx, my);
            dx += (command.lockPlayerX || command.lockCameraX) ? mx : mx * scale;
            dy += command.lockPlayerY ? my : my * scale;
        };
        forEachGroup(object, [&](int group) {
            if (auto const it = m_byGroup.find(group); it != m_byGroup.end()) {
                for (int index : it->second) {
                    auto const& command = m_commands[index];
                    if (movesDuring(command, m_worldStep, step)) add(command);
                }
            }
            if (extra) {
                for (auto const& command : *ctx.extra) {
                    if (command.group != group || !movesDuring(command, m_worldStep, step)) continue;
                    // The real game started it meanwhile; that one counts.
                    if (m_sources.count(command.source)) continue;
                    add(command);
                }
            }
        });
    }
    if (!commanded) {
        // Nothing known moves it: keep its measured speed, from where it is right now.
        if (track.vx == 0.0 && track.vy == 0.0) return;
        if (track.step != m_worldStep) return;
        // Far out the guess is worthless; hold it there.
        double const ahead = std::min(step - track.step, 240);
        dx = track.vx * ahead;
        dy = track.vy * ahead;
    }
    if (std::abs(dx) < 1e-4 && std::abs(dy) < 1e-4) return;

    Moved undo;
    undo.object = object;
    undo.positionX = object->m_positionX;
    undo.positionY = object->m_positionY;
    undo.node = object->m_obPosition;
    undo.last = object->m_lastPosition;
    moved.push_back(undo);

    // Shift GD's own position, the node position and the previous position alike, so the
    // object keeps its per step motion (platforms carry the player the same way).
    float const fx = static_cast<float>(dx);
    float const fy = static_cast<float>(dy);
    object->m_positionX = undo.positionX + dx;
    object->m_positionY = undo.positionY + dy;
    object->m_obPosition = CCPoint(undo.node.x + fx, undo.node.y + fy);
    object->m_lastPosition = CCPoint(undo.last.x + fx, undo.last.y + fy);
    object->m_isObjectRectDirty = true;
    object->m_isOrientedBoxDirty = true;
}

void ObjectTracker::restore(std::vector<Moved>& moved) {
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
        GameObject* object = it->object;
        object->m_positionX = it->positionX;
        object->m_positionY = it->positionY;
        object->m_obPosition = it->node;
        object->m_lastPosition = it->last;
        object->m_isObjectRectDirty = true;
        object->m_isOrientedBoxDirty = true;
    }
    moved.clear();
}

std::vector<GameObject*> const& ObjectTracker::groupHazards(GJBaseGameLayer* layer, int group) {
    auto [it, inserted] = m_groupHazards.try_emplace(group);
    if (!inserted || !layer) return it->second;
    if (auto* objects = layer->getGroup(group)) {
        for (auto* object : CCArrayExt<GameObject*>(objects)) {
            if (object && isHazard(object)) it->second.push_back(object);
        }
    }
    return it->second;
}

std::string ObjectTracker::describe(GameObject* object) const {
    std::string groups;
    std::string moves;
    forEachGroup(object, [&](int group) {
        groups += fmt::format("{}{}", groups.empty() ? "" : ",", group);
        auto const it = m_byGroup.find(group);
        if (it == m_byGroup.end()) return;
        for (int index : it->second) {
            auto const& c = m_commands[index];
            moves += fmt::format(
                " | group {} {} step {} for {} steps by ({:.1f},{:.1f}) ease {}{}{}", c.group,
                c.predicted ? "expected from" : "moving since", c.start, c.duration, c.dx, c.dy, c.easing,
                c.lockPlayerX || c.lockCameraX ? " lockX" : "", c.lockPlayerY ? " lockY" : ""
            );
        }
    });
    forEachGroup(object, [&](int group) {
        for (auto it = m_toggleLog.rbegin(); it != m_toggleLog.rend(); ++it) {
            if (it->group != group) continue;
            moves += fmt::format(" | group {} switched {} at step {}", group, it->on ? "on" : "off", it->step);
            break;
        }
    });
    std::string out = groups.empty() ? "no groups" : "groups " + groups;
    if (auto const it = m_tracks.find(object); it != m_tracks.end()) {
        out += fmt::format("; speed ({:.2f},{:.2f}) seen at step {}", it->second.vx, it->second.vy, it->second.step);
    }
    return out + moves;
}

void ObjectTracker::resetAttempt() {
    m_tracks.clear();
    m_moving.clear();
    m_commands.clear();
    m_byGroup.clear();
    m_sources.clear();
    m_toggleLog.clear();
    m_expectedToggles.clear();
    m_toggleSources.clear();
    m_currentTrigger = nullptr;
    m_worldStep = -1;
    m_lastPrune = 0;
}

void ObjectTracker::resetLevel() {
    this->resetAttempt();
    m_triggers.clear();
    m_spawned.clear();
    m_groupHazards.clear();
    m_scanned = false;
    m_loggedSections = false;
}

} // namespace ac
