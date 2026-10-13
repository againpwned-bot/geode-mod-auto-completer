#include "FakeSim.hpp"

#include <chrono>

using namespace geode::prelude;

namespace ac {

TickRecorder& TickRecorder::get() {
    static TickRecorder recorder;
    return recorder;
}

void TickRecorder::beginFrame(PlayerObject* player) {
    m_framePlayer = player;
    m_frameActive = player != nullptr;
    m_frameOps.clear();
    m_frameTicks = 0;
    m_depth = 0;
}

void TickRecorder::endFrame() {
    // Only used when the player's physics calls happen outside processCommands: split the
    // frame's calls evenly over the ticks the frame ran.
    if (m_frameActive && !m_tickBased && m_frameTicks > 0 && !m_frameOps.empty() &&
        m_frameOps.size() % static_cast<size_t>(m_frameTicks) == 0) {
        size_t const perTick = m_frameOps.size() / static_cast<size_t>(m_frameTicks);
        m_template.assign(m_frameOps.begin(), m_frameOps.begin() + perTick);
    }
    m_frameActive = false;
    m_framePlayer = nullptr;
}

void TickRecorder::beginTick(PlayerObject* player) {
    m_player = player;
    m_active = player != nullptr;
    m_current.clear();
    if (m_frameActive) m_frameTicks += 1;
}

void TickRecorder::endTick() {
    if (m_active && !m_current.empty()) {
        m_template = m_current;
        m_tickBased = true;
    }
    m_active = false;
    m_player = nullptr;
}

void TickRecorder::enter(PlayerObject* player, TickOp::Kind kind, float dt, bool flag) {
    if (!m_active && !m_frameActive) return;
    if (m_depth == 0) {
        TickOp op;
        op.kind = kind;
        op.dt = dt;
        op.flag = flag;
        if (m_active && player == m_player) {
            m_current.push_back(op);
        }
        else if (!m_active && m_frameActive && player == m_framePlayer) {
            m_frameOps.push_back(op);
        }
    }
    m_depth += 1;
}

void TickRecorder::leave() {
    if ((m_active || m_frameActive) && m_depth > 0) {
        m_depth -= 1;
    }
}

void TickRecorder::reset() {
    m_active = false;
    m_player = nullptr;
    m_frameActive = false;
    m_framePlayer = nullptr;
    m_frameTicks = 0;
    m_tickBased = false;
    m_depth = 0;
    m_current.clear();
    m_frameOps.clear();
    m_template.clear();
}

FakeContext& fakeContext() {
    static FakeContext context;
    return context;
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

    m_layer = layer;
    m_container = container;
    m_player = player;
    return true;
}

void FakeWorld::detach(PlayLayer* layer) {
    if (!m_container || layer != m_layer) return;
    m_container->removeFromParent();
    m_container = nullptr;
    m_player = nullptr;
    m_layer = nullptr;
}

FakeSim::FakeSim(PlayLayer* layer, PlayerObject* fake, std::vector<TickOp> ops, int inputDelay, bool hold)
    : m_layer(layer), m_fake(fake), m_ops(std::move(ops)), m_delay(inputDelay), m_held(hold), m_previousWant(hold) {
    auto* real = layer ? layer->m_player1 : nullptr;
    if (!real || !m_fake) return;

    m_fake->copyAttributes(real);
    PlayerFix fix;
    fix.capture(real);
    fix.apply(m_fake);
    m_fake->m_isDead = false;
    m_fake->m_playEffects = false;
    m_fake->setVisible(false);
}

FakeSim::~FakeSim() {
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

void FakeSim::load(int handle) {
    if (handle < 0 || handle >= static_cast<int>(m_slots.size()) || !m_fake) return;
    auto const& slot = m_slots[handle];
    if (!slot.used) return;

    slot.fix.apply(m_fake);
    m_fake->m_isDead = false;
    m_held = slot.held;
    m_previousWant = slot.previousWant;
    m_tick = slot.tick;
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

void FakeSim::stepPhysics() {
    auto& ctx = fakeContext();
    if (m_fake->m_collisionLogTop) m_fake->m_collisionLogTop->removeAllObjects();
    if (m_fake->m_collisionLogBottom) m_fake->m_collisionLogBottom->removeAllObjects();
    if (m_fake->m_collisionLogLeft) m_fake->m_collisionLogLeft->removeAllObjects();
    if (m_fake->m_collisionLogRight) m_fake->m_collisionLogRight->removeAllObjects();

    for (auto const& op : m_ops) {
        switch (op.kind) {
            case TickOp::Kind::Update:
                m_fake->update(op.dt);
                break;
            case TickOp::Kind::Collide:
                m_layer->checkCollisions(m_fake, op.dt, false);
                break;
            case TickOp::Kind::Post:
                m_fake->postCollision(op.dt, op.flag);
                break;
        }
        if (ctx.died) return;
    }
}

RunResult FakeSim::run(int untilTick, InputFn const& input, SampleFn const& sample) {
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

    auto const begin = std::chrono::steady_clock::now();
    float const levelEnd = m_layer->m_levelLength;

    while (m_tick < untilTick) {
        Mode const mode = modeOf(m_fake);
        bool const want = input(m_tick, mode);
        // The real game may apply a press one tick late; the bot measures which matches.
        bool const apply = m_delay == 0 ? want : m_previousWant;
        m_previousWant = want;
        if (apply != m_held) {
            if (apply) {
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
        sample(m_tick, s);

        int const tick = m_tick;
        m_tick += 1;
        if (ctx.died) {
            result.event = Event::Died;
            result.eventTick = tick;
            break;
        }
        if (levelEnd > 0.f && s.x >= levelEnd) {
            result.event = Event::Completed;
            result.eventTick = tick;
            break;
        }
    }

    m_secondsRun += std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    ctx = saved;
    result.endTick = m_tick;
    return result;
}

} // namespace ac
