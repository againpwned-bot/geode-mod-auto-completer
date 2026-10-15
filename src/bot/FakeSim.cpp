#include "FakeSim.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

namespace ac {

PhysicsModel PhysicsModel::variant(int index, PhysicsModel const& base) {
    PhysicsModel model = base;
    model.inputDelay = index % 2;
    model.jumpVars = static_cast<Place>((index / 2) % 3);
    model.stateVars = static_cast<Place>((index / 6) % 3);
    return model;
}

int PhysicsModel::variantIndex() const {
    return inputDelay % 2 + static_cast<int>(jumpVars) * 2 + static_cast<int>(stateVars) * 6;
}

std::string PhysicsModel::describe() const {
    auto place = [](Place p) {
        switch (p) {
            case Place::Inside: return "in";
            case Place::Start: return "start";
            case Place::End: return "end";
        }
        return "?";
    };
    return fmt::format(
        "delay {} | jump {} | state {} | pre {} | log {}",
        inputDelay, place(jumpVars), place(stateVars), preCollision ? "on" : "in", collisionLog ? "on" : "in"
    );
}

TickRecorder& TickRecorder::get() {
    static TickRecorder recorder;
    return recorder;
}

void TickRecorder::beginFrame(PlayerObject* player, unsigned) {
    m_player = player;
    m_active = player != nullptr;
    m_step = 0;
    m_depth = 0;
    m_segment.clear();
}

void TickRecorder::stepStarted(unsigned progress) {
    if (!m_active) return;
    if (m_step > 0 && progress == m_stepProgress) return;
    m_stepProgress = progress;
    m_step += 1;

    // The player's clock is advanced once per step; measure by how much.
    double const now = m_player->m_totalTime;
    if (m_lastTime >= 0.0 && now > m_lastTime && now - m_lastTime < 0.05) {
        m_timeStep = now - m_lastTime;
    }
    m_lastTime = now;
}

void TickRecorder::endFrame(unsigned) {
    if (m_active && m_step >= 1) {
        bool const moved = std::any_of(m_segment.begin(), m_segment.end(), [](TickOp const& op) {
            return op.kind == TickOp::Kind::Update;
        });
        // A frame with 2+ steps gives a clean first step. With a single step the segment
        // runs to the end of the frame, so it's only used until a clean one is seen.
        bool const strong = m_step >= 2;
        if (moved && (strong || !m_strong)) {
            // Only a different sequence of calls counts as a new template; the step size
            // is just refreshed.
            bool const same = m_template.size() == m_segment.size() &&
                std::equal(m_template.begin(), m_template.end(), m_segment.begin(), [](TickOp const& a, TickOp const& b) {
                    return a.kind == b.kind && a.flag == b.flag;
                });
            m_template = m_segment;
            if (!same) {
                m_revision += 1;
                log::info("Tick calls: {}", this->describe());
            }
            m_strong = m_strong || strong;
        }
    }
    m_active = false;
    m_player = nullptr;
}

void TickRecorder::enter(PlayerObject* player, TickOp::Kind kind, float dt, bool flag) {
    if (!m_active) return;
    if (m_depth == 0 && player == m_player && m_step == 1) {
        TickOp op;
        op.kind = kind;
        op.dt = dt;
        op.flag = flag;
        m_segment.push_back(op);
    }
    m_depth += 1;
}

void TickRecorder::leave() {
    if (m_active && m_depth > 0) {
        m_depth -= 1;
    }
}

std::string TickRecorder::describe() const {
    std::string out;
    for (auto const& op : m_template) {
        if (!out.empty()) out += ' ';
        switch (op.kind) {
            case TickOp::Kind::ResetRings: out += "rings"; break;
            case TickOp::Kind::Internal: out += "internal"; break;
            case TickOp::Kind::Update: out += "update"; break;
            case TickOp::Kind::Collide: out += "collide"; break;
            case TickOp::Kind::Post: out += op.flag ? "post*" : "post"; break;
            case TickOp::Kind::Rotate: out += "rotate"; break;
        }
    }
    return out.empty() ? "none" : out;
}

void TickRecorder::reset() {
    m_player = nullptr;
    m_active = false;
    m_step = 0;
    m_depth = 0;
    m_strong = false;
    m_lastTime = -1.0;
    m_timeStep = 0.0;
    m_segment.clear();
    m_template.clear();
    m_revision += 1;
}

FakeContext& fakeContext() {
    static FakeContext context;
    return context;
}

LayerClock& LayerClock::get() {
    static LayerClock clock;
    return clock;
}

LayerClock::Saved LayerClock::save(GJBaseGameLayer* layer) {
    Saved saved;
    if (!layer) return saved;
    saved.timestamp = layer->m_timestamp;
    saved.currentStep = layer->m_currentStep;
    saved.progress = layer->m_gameState.m_currentProgress;
    saved.levelTime = layer->m_gameState.m_levelTime;
    saved.totalTime = layer->m_gameState.m_totalTime;
    return saved;
}

void LayerClock::restore(GJBaseGameLayer* layer, Saved const& saved) {
    if (!layer) return;
    layer->m_timestamp = saved.timestamp;
    layer->m_currentStep = saved.currentStep;
    layer->m_gameState.m_currentProgress = saved.progress;
    layer->m_gameState.m_levelTime = saved.levelTime;
    layer->m_gameState.m_totalTime = saved.totalTime;
}

void LayerClock::observe(GJBaseGameLayer* layer, int step) {
    if (!layer || step == m_step) return;
    Saved const now = save(layer);
    if (m_step >= 0 && step == m_step + 1) {
        // Smoothed, since the real clock follows the frames.
        auto blend = [this](double& rate, double delta) {
            rate = m_rated ? rate * 0.8 + delta * 0.2 : delta;
        };
        blend(m_rate.timestamp, now.timestamp - m_at.timestamp);
        blend(m_rate.currentStep, static_cast<double>(now.currentStep - m_at.currentStep));
        blend(m_rate.progress, static_cast<double>(now.progress) - static_cast<double>(m_at.progress));
        blend(m_rate.levelTime, now.levelTime - m_at.levelTime);
        blend(m_rate.totalTime, now.totalTime - m_at.totalTime);
        m_rated = true;
    }
    m_step = step;
    m_at = now;
}

void LayerClock::apply(GJBaseGameLayer* layer, int step) const {
    if (!layer || m_step < 0 || !m_rated) return;
    double const d = static_cast<double>(step - m_step);
    layer->m_timestamp = m_at.timestamp + d * m_rate.timestamp;
    layer->m_currentStep = m_at.currentStep + static_cast<int>(std::lround(d * m_rate.currentStep));
    double const progress = static_cast<double>(m_at.progress) + d * m_rate.progress;
    layer->m_gameState.m_currentProgress = static_cast<unsigned>(std::max(0.0, std::round(progress)));
    layer->m_gameState.m_levelTime = m_at.levelTime + d * m_rate.levelTime;
    layer->m_gameState.m_totalTime = m_at.totalTime + d * m_rate.totalTime;
}

void LayerClock::reset() {
    // The rates stay: they don't change between attempts.
    m_step = -1;
    m_at = Saved{};
}

FakeWorld& FakeWorld::get() {
    static FakeWorld world;
    return world;
}

bool FakeWorld::attach(PlayLayer* layer) {
    this->detach(m_layer);
    if (!layer || !layer->m_objectLayer) return false;

    auto* container = CCLayer::create();
    if (!container) return false;
    container->setVisible(false);
    container->setID("simulation"_spr);
    layer->m_objectLayer->addChild(container, -1000);

    auto* player = PlayerObject::create(1, 1, layer, container, true);
    if (!player) {
        container->removeFromParent();
        return false;
    }
    player->setVisible(false);
    player->m_playEffects = false;
    container->addChild(player);

    // Kept alive by us too, so it stays valid even if the level drops it first.
    container->retain();
    m_layer = layer;
    m_container = container;
    m_player = player;
    return true;
}

void FakeWorld::detach(PlayLayer* layer) {
    if (!m_container || layer != m_layer) return;
    m_container->removeFromParent();
    m_container->release();
    m_container = nullptr;
    m_player = nullptr;
    m_layer = nullptr;
}

bool FakeWorld::ensure(PlayLayer* layer) {
    if (!layer) return false;
    bool const attached = m_layer == layer && m_player && m_container &&
        m_container->getParent() == layer->m_objectLayer;
    if (attached) return true;
    log::info("Creating the hidden player for this level again");
    if (m_container && m_layer != layer) {
        // Belongs to a level that is gone; just let go of it.
        m_container->removeFromParent();
        m_container->release();
        m_container = nullptr;
        m_player = nullptr;
        m_layer = nullptr;
    }
    return this->attach(layer);
}

FakeSim::FakeSim(
    PlayLayer* layer, PlayerObject* fake, std::vector<TickOp> ops, double timeStep, PhysicsModel model,
    bool hold, int baseStep, PlayerFix const* from
)
    : m_layer(layer), m_fake(fake), m_ops(std::move(ops)), m_timeStep(timeStep), m_baseStep(baseStep), m_model(model),
      m_held(hold), m_previousWant(hold) {
    m_hasRingReset = std::any_of(m_ops.begin(), m_ops.end(), [](TickOp const& op) {
        return op.kind == TickOp::Kind::ResetRings;
    });
    auto* real = layer ? layer->m_player1 : nullptr;
    if (!real || !m_fake) return;

    m_fake->copyAttributes(real);
    this->ownRingList();
    // An earlier simulation may have left the hidden player in another mode.
    setMode(m_fake, from ? from->mode : modeOf(real));
    if (from) {
        from->apply(m_fake);
    }
    else {
        PlayerFix fix;
        fix.capture(real);
        fix.apply(m_fake);
    }
    m_fake->m_isDead = false;
    m_fake->m_ignoreDamage = false;
    m_fake->m_playEffects = false;
    m_fake->setVisible(false);
    m_triggerX = m_fake->getPositionX();
    this->noteTouchingRings();
}

FakeSim::~FakeSim() {
    this->restoreObjects();
    if (m_inSlice) {
        this->restoreBaseline();
    }
}

FakeSim::Flags FakeSim::read(EnhancedGameObject* object) {
    Flags flags;
    flags.activated = object->m_activated;
    flags.byPlayer1 = object->m_activatedByPlayer1;
    flags.byPlayer2 = object->m_activatedByPlayer2;
    return flags;
}

void FakeSim::write(EnhancedGameObject* object, Flags const& flags) {
    object->m_activated = flags.activated;
    object->m_activatedByPlayer1 = flags.byPlayer1;
    object->m_activatedByPlayer2 = flags.byPlayer2;
}

void FakeSim::restoreBaseline() {
    for (auto const& entry : m_log) {
        write(entry.object, entry.base);
    }
}

void FakeSim::noteObject(GameObject* object) {
    auto* enhanced = typeinfo_cast<EnhancedGameObject*>(object);
    if (!enhanced || !m_logged.insert(enhanced).second) return;
    LogEntry entry;
    entry.object = enhanced;
    entry.base = read(enhanced);
    m_log.push_back(entry);
}

void FakeSim::ownRingList() {
    // The hidden player's list of touched orbs is rewritten on every restore; it must never
    // be the real player's own list.
    auto* real = m_layer ? m_layer->m_player1 : nullptr;
    if (!m_fake || (m_fake->m_touchingRings && (!real || m_fake->m_touchingRings != real->m_touchingRings))) return;
    auto* own = CCArray::create();
    own->retain();
    m_fake->m_touchingRings = own;
}

void FakeSim::noteTouchingRings() {
    auto* rings = m_fake ? m_fake->m_touchingRings : nullptr;
    if (!rings) return;
    for (unsigned i = rings->count(); i-- > 0;) {
        auto* object = typeinfo_cast<GameObject*>(rings->objectAtIndex(i));
        if (!object) continue;
        if (this->spentByRealPlayer(object)) {
            // Used up by the real player: a click must not find it.
            rings->removeObjectAtIndex(i);
            continue;
        }
        this->noteObject(object);
    }
}

bool FakeSim::spentByRealPlayer(GameObject* object) const {
    auto* enhanced = typeinfo_cast<EnhancedGameObject*>(object);
    if (!enhanced || enhanced->m_isMultiActivate || !enhanced->m_activatedByPlayer1) return false;
    // Used in a step this simulation replays (the one-tick checks): not used yet here.
    int const step = ObjectTracker::get().realActivationStep(object);
    return step < 0 || step < m_baseStep;
}

namespace {
    enum class PortalKind { None, Speed, Size, Gravity, Mode, Goal };

    PortalKind portalKind(GameObject* object, float& speed) {
        switch (object->m_objectType) {
            case GameObjectType::NormalGravityPortal:
            case GameObjectType::InverseGravityPortal:
            case GameObjectType::GravityTogglePortal:
                return PortalKind::Gravity;
            case GameObjectType::RegularSizePortal:
            case GameObjectType::MiniSizePortal:
                return PortalKind::Size;
            case GameObjectType::ShipPortal:
            case GameObjectType::CubePortal:
            case GameObjectType::BallPortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::RobotPortal:
            case GameObjectType::SpiderPortal:
            case GameObjectType::SwingPortal:
                return PortalKind::Mode;
            case GameObjectType::DualPortal:
            case GameObjectType::SoloPortal:
            case GameObjectType::TeleportPortal:
                return PortalKind::Goal;
            case GameObjectType::Modifier:
                switch (object->m_objectID) {
                    case 200: speed = 0.7f; return PortalKind::Speed;
                    case 201: speed = 0.9f; return PortalKind::Speed;
                    case 202: speed = 1.1f; return PortalKind::Speed;
                    case 203: speed = 1.3f; return PortalKind::Speed;
                    case 1334: speed = 1.6f; return PortalKind::Speed;
                    default: return PortalKind::None;
                }
            default:
                return PortalKind::None;
        }
    }

    Mode portalMode(GameObjectType type) {
        switch (type) {
            case GameObjectType::ShipPortal: return Mode::Ship;
            case GameObjectType::BallPortal: return Mode::Ball;
            case GameObjectType::UfoPortal: return Mode::Ufo;
            case GameObjectType::WavePortal: return Mode::Wave;
            case GameObjectType::RobotPortal: return Mode::Robot;
            case GameObjectType::SpiderPortal: return Mode::Spider;
            case GameObjectType::SwingPortal: return Mode::Swing;
            default: return Mode::Cube;
        }
    }

    // Whether a mode portal would change anything for this player.
    bool changesMode(GameObjectType type, PlayerObject* player) {
        Mode const mode = modeOf(player);
        switch (type) {
            case GameObjectType::ShipPortal: return mode != Mode::Ship;
            case GameObjectType::CubePortal: return mode != Mode::Cube;
            case GameObjectType::BallPortal: return mode != Mode::Ball;
            case GameObjectType::UfoPortal: return mode != Mode::Ufo;
            case GameObjectType::WavePortal: return mode != Mode::Wave;
            case GameObjectType::RobotPortal: return mode != Mode::Robot;
            case GameObjectType::SpiderPortal: return mode != Mode::Spider;
            case GameObjectType::SwingPortal: return mode != Mode::Swing;
            default: return true;
        }
    }
}

bool FakeSim::isTurnedPortal(GameObject* object) {
    if (!object) return false;
    float speed = 0.f;
    PortalKind const kind = portalKind(object, speed);
    if (kind == PortalKind::None || kind == PortalKind::Speed) return false;
    float const turn = std::fmod(std::abs(object->getRotationX()), 360.f);
    return turn > 0.5f && turn < 359.5f;
}

void FakeSim::notePortal(GameObject* object) {
    if (!object || !m_fake) return;
    float speed = 0.f;
    PortalKind const kind = portalKind(object, speed);
    if (kind == PortalKind::None) return;
    // Switched off (by a toggle, possibly one the simulation expects) or untouchable
    // portals do nothing in GD.
    if (object->m_isGroupDisabled || object->m_isDisabled || object->m_isNoTouch) return;
    if (std::find(m_portalsUsed.begin(), m_portalsUsed.end(), object) != m_portalsUsed.end()) return;
    auto const rect = object->getObjectRect();
    if (!m_fake->getObjectRect().intersectsRect(rect)) return;
    // Straight portals switch the player on first touch; turned ones once its center
    // reaches them (speed portals have their own timing below).
    if (kind != PortalKind::Speed && isTurnedPortal(object) &&
        m_fake->getPositionX() < rect.getMinX() - m_model.portalReach) {
        return;
    }
    m_portalsUsed.push_back(object);

    switch (kind) {
        case PortalKind::Speed:
            // GD changes speed on the step after the player first reaches the portal, after
            // that step's move. If the player already overlapped it before this step's move,
            // that step is now.
            if (m_tickStartRect.intersectsRect(object->getObjectRect())) {
                m_fake->m_playerSpeed = speed;
            }
            else {
                m_pendingSpeed = speed;
            }
            break;
        case PortalKind::Size: {
            bool const mini = object->m_objectType == GameObjectType::MiniSizePortal;
            if (mini != (m_fake->m_vehicleSize != 1.f)) {
                m_fake->togglePlayerScale(mini, true);
                m_fake->updatePlayerScale();
            }
            break;
        }
        case PortalKind::Gravity: {
            // Flipping goes through camera and level state, so a portal that would flip
            // the player ends the look-ahead like a mode portal.
            bool flip = m_fake->m_isUpsideDown;
            if (object->m_objectType == GameObjectType::NormalGravityPortal) flip = false;
            else if (object->m_objectType == GameObjectType::InverseGravityPortal) flip = true;
            else flip = !m_fake->m_isUpsideDown;
            if (flip != m_fake->m_isUpsideDown) {
                m_reachedGoal = true;
                m_goalObject = object;
            }
            break;
        }
        case PortalKind::Mode:
            if (changesMode(object->m_objectType, m_fake)) {
                m_modeSwitchTick = m_tick;
                m_modeSwitchPortal = object;
                setMode(m_fake, portalMode(object->m_objectType));
                // What GD records about the portal on the player.
                m_fake->m_lastPortalPos = ccp(object->m_positionX, object->m_positionY);
                m_fake->m_lastActivatedPortal = object;
                m_fake->m_gameModeChangedTime = m_fake->m_totalTime;
            }
            break;
        case PortalKind::Goal:
            if (changesMode(object->m_objectType, m_fake)) {
                m_reachedGoal = true;
                m_goalObject = object;
            }
            break;
        case PortalKind::None:
            break;
    }
}

void FakeSim::setMode(PlayerObject* player, Mode mode) {
    if (!player || modeOf(player) == mode) return;
    // Leave the current mode, then enter the new one, the way the mode toggles expect.
    switch (modeOf(player)) {
        case Mode::Ship: player->toggleFlyMode(false, true); break;
        case Mode::Ufo: player->toggleBirdMode(false, true); break;
        case Mode::Wave: player->toggleDartMode(false, true); break;
        case Mode::Ball: player->toggleRollMode(false, true); break;
        case Mode::Robot: player->toggleRobotMode(false, true); break;
        case Mode::Spider: player->toggleSpiderMode(false, true); break;
        case Mode::Swing: player->toggleSwingMode(false, true); break;
        case Mode::Cube: break;
    }
    switch (mode) {
        case Mode::Ship: player->toggleFlyMode(true, true); break;
        case Mode::Ufo: player->toggleBirdMode(true, true); break;
        case Mode::Wave: player->toggleDartMode(true, true); break;
        case Mode::Ball: player->toggleRollMode(true, true); break;
        case Mode::Robot: player->toggleRobotMode(true, true); break;
        case Mode::Spider: player->toggleSpiderMode(true, true); break;
        case Mode::Swing: player->toggleSwingMode(true, true); break;
        case Mode::Cube: break;
    }
}

void FakeSim::beginSlice() {
    if (m_inSlice) return;
    m_inSlice = true;
    // The real game may have used some of these objects meanwhile.
    for (auto& entry : m_log) {
        entry.base = read(entry.object);
    }
    if (m_cursor >= 0) {
        this->load(m_cursor);
        this->release(m_cursor);
        m_cursor = -1;
    }
}

void FakeSim::endSlice() {
    if (!m_inSlice) return;
    m_cursor = this->save();
    this->restoreBaseline();
    m_inSlice = false;
}

int FakeSim::save() {
    if (!m_fake) return -1;

    Slot slot;
    slot.fix.capture(m_fake);
    slot.held = m_held;
    slot.previousWant = m_previousWant;
    slot.tick = m_tick;
    slot.portals = m_portalsUsed;
    slot.moves = m_moves;
    slot.toggles = m_toggles;
    slot.pendingSpeed = m_pendingSpeed;
    slot.modeSwitchTick = m_modeSwitchTick;
    slot.modeSwitchPortal = m_modeSwitchPortal;
    slot.mode = modeOf(m_fake);
    slot.flags.reserve(m_log.size());
    for (auto const& entry : m_log) {
        slot.flags.push_back(read(entry.object));
    }
    slot.used = true;

    if (!m_free.empty()) {
        int const handle = m_free.back();
        m_free.pop_back();
        m_slots[handle] = std::move(slot);
        return handle;
    }
    m_slots.push_back(std::move(slot));
    return static_cast<int>(m_slots.size()) - 1;
}

bool& FakeSim::reattributeOnLoad() {
    static bool value = false;
    return value;
}

void FakeSim::load(int handle) {
    if (handle < 0 || handle >= static_cast<int>(m_slots.size()) || !m_fake) return;
    auto const& slot = m_slots[handle];
    if (!slot.used) return;

    if (reattributeOnLoad() && m_layer && m_layer->m_player1) {
        m_fake->copyAttributes(m_layer->m_player1);
        this->ownRingList();
    }
    // Mode first: switching modes changes physics fields the snapshot then overwrites.
    setMode(m_fake, slot.mode);
    slot.fix.apply(m_fake);
    m_fake->m_isDead = false;
    m_held = slot.held;
    m_previousWant = slot.previousWant;
    m_tick = slot.tick;
    m_portalsUsed = slot.portals;
    m_moves = slot.moves;
    m_toggles = slot.toggles;
    m_pendingSpeed = slot.pendingSpeed;
    m_modeSwitchTick = slot.modeSwitchTick;
    m_modeSwitchPortal = slot.modeSwitchPortal;
    m_triggerX = m_fake->getPositionX();
    m_reachedGoal = false;
    m_goalObject = nullptr;
    // Orbs the restored state is touching may be clicked right away; they must be logged
    // (with their flags as they are now) before that can happen.
    this->noteTouchingRings();
    for (size_t i = 0; i < m_log.size(); ++i) {
        write(m_log[i].object, i < slot.flags.size() ? slot.flags[i] : m_log[i].base);
    }
}

void FakeSim::release(int handle) {
    if (handle < 0 || handle >= static_cast<int>(m_slots.size())) return;
    if (!m_slots[handle].used) return;
    m_slots[handle] = Slot{};
    m_free.push_back(handle);
}

void FakeSim::runOp(TickOp const& op) {
    switch (op.kind) {
        case TickOp::Kind::ResetRings:
            m_fake->resetTouchedRings(op.flag);
            break;
        case TickOp::Kind::Internal:
            m_fake->updateInternalActions(op.dt);
            break;
        case TickOp::Kind::Update:
            m_fake->update(op.dt);
            break;
        case TickOp::Kind::Collide:
            m_layer->checkCollisions(m_fake, op.dt, false);
            break;
        case TickOp::Kind::Post:
            m_fake->postCollision(op.dt, op.flag);
            break;
        case TickOp::Kind::Rotate:
            m_fake->updateRotation(op.dt);
            break;
    }
}

ObjectTracker::Context FakeSim::placeContext() const {
    ObjectTracker::Context context;
    context.step = this->absoluteStep();
    context.extra = &m_moves;
    if (auto* real = m_layer ? m_layer->m_player1 : nullptr) {
        // The real player sits where it was at the world step.
        context.playerDx = m_fake->getPositionX() - real->getPositionX();
        context.playerDy = m_fake->getPositionY() - real->getPositionY();
    }
    return context;
}

bool FakeSim::placeObject(GameObject* object) {
    auto& tracker = ObjectTracker::get();
    auto& track = tracker.observe(object);
    if (track.placed == m_stamp) return false;
    track.placed = m_stamp;
    auto const context = this->placeContext();
    tracker.predict(object, track, context, m_moved);

    // Switched on or off by a toggle trigger before this tick.
    int const enabled = tracker.enabledAt(object, context.step, &m_toggles);
    if (enabled >= 0 && object->m_isGroupDisabled != (enabled == 0)) {
        m_switched.emplace_back(object, object->m_isGroupDisabled);
        object->m_isGroupDisabled = enabled == 0;
    }
    return true;
}

void FakeSim::noteToggle(int group, bool on) {
    ObjectTracker::Toggle toggle;
    toggle.group = group;
    toggle.step = this->absoluteStep();
    toggle.on = on;
    m_toggles.add(toggle);
}

void FakeSim::restoreObjects() {
    ObjectTracker::restore(m_moved);
    for (auto it = m_switched.rbegin(); it != m_switched.rend(); ++it) {
        it->first->m_isGroupDisabled = it->second;
    }
    m_switched.clear();
}

void FakeSim::collideMovingObjects(float dt) {
    auto& tracker = ObjectTracker::get();
    int const step = this->absoluteStep();
    int const world = tracker.worldStep();
    if (step <= world) return;

    auto const center = m_fake->getPosition();
    // Nothing a move can reach from farther away needs a look.
    auto const context = this->placeContext();
    double range = std::abs(context.playerDx) + std::abs(context.playerDy);
    double const scale = std::max(1.0, tracker.offsetScale());
    auto addRange = [&](MoveCommand const& command) {
        if (ObjectTracker::movesDuring(command, world, step)) {
            range += (std::abs(command.dx) + std::abs(command.dy)) * scale;
        }
    };
    for (auto const& command : tracker.commands()) addRange(command);
    for (auto const& command : m_moves) addRange(command);

    gd::vector<GameObject*> nearby;
    auto consider = [&](GameObject* object, bool farCheck) {
        if (!ObjectTracker::reachType(object)) return;
        float const reach = 90.f + object->getObjectRadius();
        if (farCheck && (std::abs(object->m_positionX - center.x) > reach + range + 300.0 ||
                         std::abs(object->m_positionY - center.y) > reach + range + 300.0)) {
            return;
        }
        // Already checked this tick at its predicted place.
        if (!this->placeObject(object)) return;
        if (std::abs(object->m_positionX - center.x) < reach && std::abs(object->m_positionY - center.y) < reach) {
            nearby.push_back(object);
        }
    };

    // By index: placing an object may add to the list.
    auto const& moving = tracker.movingObjects();
    for (size_t i = 0; i < moving.size(); ++i) {
        consider(moving[i], false);
    }
    // Hazards, orbs and pads of groups that triggers are moving.
    std::vector<int> groups;
    auto addGroup = [&](MoveCommand const& command) {
        if (!ObjectTracker::movesDuring(command, world, step)) return;
        if (std::find(groups.begin(), groups.end(), command.group) != groups.end()) return;
        groups.push_back(command.group);
    };
    for (auto const& command : tracker.commands()) addGroup(command);
    for (auto const& command : m_moves) addGroup(command);
    // And of groups switched on by then.
    auto addToggled = [&](ObjectTracker::Toggle const& toggle) {
        if (!toggle.on || toggle.step <= world || toggle.step >= step) return;
        if (std::find(groups.begin(), groups.end(), toggle.group) != groups.end()) return;
        groups.push_back(toggle.group);
    };
    for (auto const& toggle : tracker.expectedToggles().list) addToggled(toggle);
    for (auto const& toggle : m_toggles.list) addToggled(toggle);
    for (int group : groups) {
        for (auto* object : tracker.groupObjects(m_layer, group)) consider(object, true);
    }

    if (!nearby.empty()) {
        // Goes through the same hook as the regular check.
        m_layer->collisionCheckObjects(m_fake, &nearby, static_cast<int>(nearby.size()), dt);
    }
}

void FakeSim::passTriggers() {
    double const x = m_fake->getPositionX();
    if (x <= m_triggerX) return;
    auto& tracker = ObjectTracker::get();
    // The real game starts the moves of triggers the real player passes.
    if (this->absoluteStep() > tracker.worldStep()) {
        tracker.eventsBetween(m_layer, m_triggerX, x, this->absoluteStep(), m_moves, m_toggles);
    }
    m_triggerX = x;
}

void FakeSim::stepPhysics() {
    static uint64_t ticks = 0;
    m_stamp = ++ticks;
    this->stepPlayer();
    auto& ctx = fakeContext();
    if (ctx.died && ctx.killer) {
        // Where the killer was in this tick, before moved objects go back.
        m_killerPosition = ccp(ctx.killer->m_positionX, ctx.killer->m_positionY);
        m_killerSwitchedOn = std::any_of(m_switched.begin(), m_switched.end(), [&](auto const& entry) {
            return entry.first == ctx.killer && entry.second;
        });
    }
    if (!ctx.died) {
        this->passTriggers();
    }
    this->restoreObjects();
}

void FakeSim::stepPlayer() {
    using Place = PhysicsModel::Place;
    auto& ctx = fakeContext();
    auto* player = m_fake;

    // GD sets every player's clock at the start of each step.
    if (m_timeStep > 0.0) {
        player->m_totalTime += m_timeStep;
    }
    if (m_model.jumpVars == Place::Start) player->updateJumpVariables();
    if (m_model.stateVars == Place::Start) player->updateStateVariables();
    // GD clears the collision log right after resetting touched rings.
    if (m_model.collisionLog && !m_hasRingReset) player->resetCollisionLog(false);
    m_tickStartRect = player->getObjectRect();

    for (auto const& op : m_ops) {
        if (op.kind == TickOp::Kind::Collide && m_model.preCollision) {
            player->preCollision();
        }
        this->runOp(op);
        if (op.kind == TickOp::Kind::Update && m_pendingSpeed > 0.f) {
            player->m_playerSpeed = m_pendingSpeed;
            m_pendingSpeed = 0.f;
        }
        if (op.kind == TickOp::Kind::ResetRings && m_model.collisionLog) {
            player->resetCollisionLog(false);
        }
        if (op.kind == TickOp::Kind::Collide && !ctx.died) {
            this->collideMovingObjects(op.dt);
        }
        if (ctx.died) return;
    }

    if (m_model.jumpVars == Place::End) player->updateJumpVariables();
    if (m_model.stateVars == Place::End) player->updateStateVariables();
    // GD stores the position after every step (the field is misnamed in the bindings).
    player->m_shipRotation = player->getPosition();
}

InlineProbe FakeSim::probeInlines() {
    InlineProbe result;
    if (!m_fake || !m_layer || m_ops.empty()) return result;

    auto& ctx = fakeContext();
    FakeContext const saved = ctx;
    ctx.active = true;
    ctx.player = m_fake;
    ctx.sim = this;
    ctx.died = false;

    int const base = this->save();
    for (auto const& op : m_ops) {
        this->load(base);
        auto* player = m_fake;

        // Values each inlined function would visibly reset.
        player->m_wasTeleported = true;               // preCollision
        player->m_ringJumpRelated = true;
        player->m_touchedGravityPortal = true;        // updateJumpVariables
        player->m_maybeTouchedBreakableBlock = true;
        player->m_stateBoostX = 1000;                 // updateStateVariables
        player->m_stateScale = 1000;
        player->m_maybeStateForce2 = 1000;
        player->m_lastCollisionBottom = 13579;        // resetCollisionLog(false)
        player->m_unk50C = 24680;

        static uint64_t probes = 0;
        m_stamp = ~++probes;
        this->runOp(op);
        this->restoreObjects();

        if (!player->m_wasTeleported && !player->m_ringJumpRelated) result.preCollision = true;
        if (!player->m_touchedGravityPortal && !player->m_maybeTouchedBreakableBlock) result.jumpVars = true;
        if (player->m_stateBoostX == 999 && player->m_stateScale == 999 && player->m_maybeStateForce2 == 999) {
            result.stateVars = true;
        }
        if (player->m_unk50C == 13579) result.collisionLog = true;
        ctx.died = false;
    }
    this->load(base);
    this->release(base);

    ctx = saved;
    result.done = true;
    return result;
}

RunResult FakeSim::run(int untilTick, InputFn const& input, SampleFn const& sample) {
    return this->runLoop(untilTick, input, sample, nullptr);
}

RunResult FakeSim::runUntil(int untilTick, InputFn const& input, SampleFn const& sample, StopFn const& stop) {
    return this->runLoop(untilTick, input, sample, &stop);
}

uint64_t FakeSim::stateKey() const {
    if (!m_fake) return 0;
    uint64_t hash = 0xcbf29ce484222325ull;
    auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    };
    auto quantize = [](double value, double scale) {
        return static_cast<uint64_t>(static_cast<int64_t>(std::llround(value * scale)));
    };
    mix(static_cast<uint64_t>(m_tick));
    mix(quantize(m_fake->getPositionX(), 64.0));
    mix(quantize(m_fake->getPositionY(), 64.0));
    mix(quantize(m_fake->m_yVelocity, 4096.0));
    mix(quantize(m_fake->getRotation(), 4.0));
    mix(static_cast<uint64_t>(modeOf(m_fake)));
    mix(m_fake->m_isUpsideDown ? 1 : 0);
    mix(m_fake->m_isOnGround ? 1 : 0);
    mix(m_fake->m_isDashing ? 1 : 0);
    mix(m_fake->m_jumpBuffered ? 1 : 0);
    mix(quantize(m_fake->m_vehicleSize, 100.0));
    mix(quantize(m_fake->m_playerSpeed, 1000.0));
    mix(quantize(m_pendingSpeed, 1000.0));
    mix(m_held ? 1 : 0);
    mix(m_previousWant ? 1 : 0);
    mix(m_portalsUsed.size());
    mix(m_moves.size());
    mix(m_toggles.list.size());
    mix(m_fake->m_touchingRings ? m_fake->m_touchingRings->count() : 0);
    // Which orbs and pads this path has used.
    for (auto const& entry : m_log) {
        auto const flags = read(entry.object);
        mix((flags.activated ? 1u : 0u) | (flags.byPlayer1 ? 2u : 0u) | (flags.byPlayer2 ? 4u : 0u));
    }
    return hash == 0 ? 1 : hash;
}

RunResult FakeSim::runLoop(int untilTick, InputFn const& input, SampleFn const& sample, StopFn const* stop) {
    RunResult result;
    if (!m_fake || !m_layer || m_ops.empty()) {
        result.event = Event::Died;
        result.eventTick = m_tick;
        result.endTick = m_tick;
        return result;
    }

    auto& ctx = fakeContext();
    FakeContext const saved = ctx;
    ctx.active = true;
    ctx.player = m_fake;
    ctx.sim = this;
    ctx.died = false;
    ctx.killer = nullptr;
    // The game's clocks move with the simulated steps and are put back at the end.
    auto const clocks = LayerClock::save(m_layer);
    auto const& clock = LayerClock::get();

    auto const begin = std::chrono::steady_clock::now();
    float const levelEnd = m_layer->m_levelLength;

    while (m_tick < untilTick) {
        clock.apply(m_layer, this->absoluteStep());
        Mode const mode = modeOf(m_fake);
        bool const want = input(m_tick, mode);
        // The real game may apply a press one tick late; the bot measures which matches.
        bool const apply = m_model.inputDelay == 0 ? want : m_previousWant;
        m_previousWant = want;
        if (apply != m_held) {
            if (apply) {
                // The game layer marks this on every click before handing it to the player.
                m_fake->m_hasEverJumped = true;
                m_fake->pushButton(PlayerButton::Jump);
            }
            else {
                m_fake->releaseButton(PlayerButton::Jump);
            }
            m_held = apply;
        }

        this->stepPhysics();
        m_ticksRun += 1;

        Sample s;
        s.x = m_fake->getPositionX();
        s.y = m_fake->getPositionY();
        s.hold = want;
        s.mode = mode;
        s.grounded = m_fake->m_isOnGround;
        s.upsideDown = m_fake->m_isUpsideDown;
        // Orbs the player is inside of after this step's collisions (GD's m_touchedRing can
        // stay set long after an orb, which made clicks in mid-air look like orb clicks).
        s.ring = m_fake->m_touchingRings && m_fake->m_touchingRings->count() > 0;
        sample(m_tick, s);

        int const tick = m_tick;
        m_tick += 1;
        if (ctx.died && tick == m_modeSwitchTick && ctx.killer &&
            (ctx.killer->m_objectType == GameObjectType::Hazard ||
             ctx.killer->m_objectType == GameObjectType::AnimatedHazard)) {
            // A hazard in the very step the player goes through a mode portal: GD's portal
            // handling (new mode's floor and ceiling, its order of checks) isn't simulated,
            // and the real game has let the player through where the hidden one died (the
            // ship portal in the spike pit on polarcris). The look-ahead ends at the portal;
            // the bot plans again from the real player's state right after it.
            m_reachedGoal = true;
            m_goalObject = m_modeSwitchPortal;
            result.event = Event::Completed;
            result.eventTick = tick;
            break;
        }
        if (ctx.died) {
            result.event = Event::Died;
            result.eventTick = tick;
            m_lastDeath = DeathInfo{};
            m_lastDeath.valid = true;
            m_lastDeath.tick = tick;
            m_lastDeath.x = s.x;
            m_lastDeath.y = s.y;
            if (auto* killer = ctx.killer) {
                m_lastDeath.objectId = killer->m_objectID;
                m_lastDeath.objectType = static_cast<int>(killer->m_objectType);
                m_lastDeath.objectX = m_killerPosition.x;
                m_lastDeath.objectY = m_killerPosition.y;
                m_lastDeath.object = killer;
                m_lastDeath.switchedOn = m_killerSwitchedOn;
            }
            break;
        }
        if ((levelEnd > 0.f && s.x >= levelEnd) || m_reachedGoal) {
            result.event = Event::Completed;
            result.eventTick = tick;
            break;
        }
        if (stop && (*stop)(tick, s)) break;
    }

    m_secondsRun += std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    LayerClock::restore(m_layer, clocks);
    ctx = saved;
    result.endTick = m_tick;
    return result;
}

} // namespace ac
